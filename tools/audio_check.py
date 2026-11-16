"""Check a capture of the game's sound: is it there, is it clean, does it sound like music.

  python tools/audio_check.py <file.wav | file.raw> [--rate 48000 --channels 2 --format f32|s16] [--from S --to S]
  python tools/audio_check.py --selftest

Capture the mixed output of a run with SDL's disk driver (what a player would hear, every stream mixed by SDL):
  SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=runs/<name>.raw LARECOMP_NO_AUDIO=0
in the run's environment (the format is the device's: the runtime logs it as "[audren] device ..."), or only the
audio renderer's mix with LARECOMP_AUDIO_WAV=<file.wav>. Never commit a capture: it is the game's sound.

Reports, per second and overall: level (RMS, dBFS), peak, clipped samples, digital silence (gaps of exact zeros: an
underrun or nothing playing), blocks repeated back to back (a stuck or replayed buffer), and the spectral flatness
from ffmpeg (0 = tones, 1 = white noise; music is well under 0.5).
"""
import array
import math
import struct
import subprocess
import sys


def read_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError('not a WAV file')
    pos, fmt, rate, ch, bits, body = 12, None, None, None, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack('<I', data[pos + 4:pos + 8])[0]
        if cid == b'fmt ':
            fmt, ch, rate = struct.unpack('<HHI', data[pos + 8:pos + 16])
            bits = struct.unpack('<H', data[pos + 22:pos + 24])[0]
        elif cid == b'data':
            body = data[pos + 8:pos + 8 + size] if size else data[pos + 8:]
            break
        pos += 8 + size + (size & 1)
    kind = 'f32' if fmt == 3 and bits == 32 else 's16' if fmt == 1 and bits == 16 else None
    if kind is None or body is None:
        raise ValueError('unsupported WAV (format %s, %s bits)' % (fmt, bits))
    return samples(body, kind), rate, ch, kind


def samples(raw, kind):
    a = array.array('f' if kind == 'f32' else 'h')
    a.frombytes(raw[:len(raw) - len(raw) % a.itemsize])
    if kind == 's16':
        a = array.array('f', (v / 32768.0 for v in a))
    return a


def analyse(a, rate, ch):
    """Per-second rows and overall findings for interleaved float samples."""
    frames = len(a) // ch
    secs = []
    for s in range(0, frames, rate):
        seg = a[s * ch:min(frames, s + rate) * ch]
        n = len(seg) or 1
        sq = sum(v * v for v in seg)
        peak = max((abs(v) for v in seg), default=0.0)
        clip = sum(1 for v in seg if abs(v) >= 0.999)
        secs.append((s // rate, 10 * math.log10(sq / n) if sq > 0 else -120.0, peak, clip))
    # digital silence: runs of frames where every channel is exactly 0
    gaps, run_start = [], None
    for i in range(frames):
        zero = all(a[i * ch + c] == 0.0 for c in range(ch))
        if zero and run_start is None:
            run_start = i
        elif not zero and run_start is not None:
            gaps.append((run_start, i))
            run_start = None
    if run_start is not None:
        gaps.append((run_start, frames))
    gaps = [(x / rate, (y - x) / rate) for x, y in gaps if y - x >= rate // 20]  # >= 50 ms
    # a buffer played twice: samples equal to those one buffer length earlier, for the usual buffer lengths. A repeat
    # of length L always covers an L/2 window starting at a multiple of L/2, so those are enough to compare.
    repeats, mv = set(), memoryview(a)
    for lag in (240, 256, 480, 512, 960, 1024, 2048):
        half, step = lag // 2 * ch, lag * ch
        for b in range(step, len(a) - half + 1, half):
            if mv[b:b + half] == mv[b - step:b - step + half] and any(mv[b:b + half]):
                repeats.add(round(b // ch / rate, 2))
    return secs, gaps, sorted(repeats)


def flatness(path, rate, ch, kind, raw):
    """Mean spectral flatness per second from ffmpeg's aspectralstats (None if ffmpeg is missing)."""
    src = ['-f', 'f32le' if kind == 'f32' else 's16le', '-ar', str(rate), '-ac', str(ch)] if raw else []
    cmd = ['ffmpeg', '-hide_banner', '-nostats'] + src + ['-i', path, '-af',
           'aspectralstats=measure=flatness:win_size=2048,ametadata=print:file=-', '-f', 'null', '-']
    try:
        out = subprocess.run(cmd, capture_output=True, text=True).stdout
    except OSError:
        return None
    per, t = {}, 0.0
    for line in out.splitlines():
        if line.startswith('frame:'):
            t = float(line.split('pts_time:')[1]) if 'pts_time:' in line else t
        elif '.flatness=' in line and line.startswith('lavfi.aspectralstats.1.'):
            per.setdefault(int(t), []).append(float(line.split('=')[1]))
    return {s: sum(v) / len(v) for s, v in per.items()}


def report(path, a, rate, ch, kind, raw, t0=0, t1=None):
    if t1 is not None or t0:
        a = a[int(t0 * rate) * ch:(int(t1 * rate) * ch if t1 is not None else len(a))]
    secs, gaps, repeats = analyse(a, rate, ch)
    flat = flatness(path, rate, ch, kind, raw) or {}
    print('%s: %d Hz, %d channels, %s, %.1f s' % (path, rate, ch, kind, len(a) / ch / rate))
    print('  sec   dBFS   peak  clipped  flatness')
    for s, db, peak, clip in secs:
        f = flat.get(s + int(t0))
        print('  %4d %6.1f  %5.3f  %7d  %s' % (s + t0, db, peak, clip, '%.3f' % f if f is not None else '-'))
    loud = [r for r in secs if r[1] > -60]
    print('audible seconds: %d of %d' % (len(loud), len(secs)))
    print('clipped samples: %d' % sum(r[3] for r in secs))
    print('digital silence >= 50 ms: %d gaps%s' % (len(gaps), ''.join('\n  at %.2f s, %.0f ms' % (t + t0, d * 1000)
                                                                      for t, d in gaps[:40])))
    print('stretches equal to the buffer before them (not silent): %d%s' % (len(repeats), ''.join('\n  at %.2f s' % (t + t0)
                                                                                           for t in repeats[:20])))
    if flat:
        vals = [flat[s + int(t0)] for s, db, _, _ in secs if db > -50 and (s + int(t0)) in flat]
        if vals:
            print('spectral flatness of the audible seconds: median %.3f, max %.3f' % (sorted(vals)[len(vals) // 2], max(vals)))


def selftest():
    rate, ch = 8000, 2
    a = array.array('f')
    for i in range(rate * 3):
        v = 0.5 * math.sin(2 * math.pi * 440 * i / rate) if not (rate <= i < rate + rate // 5) else 0.0
        a.extend((v, v))
    blk = a[2 * rate * ch + 37 * ch:2 * rate * ch + 37 * ch + 480 * ch]  # a block played twice
    a[2 * rate * ch + 517 * ch:2 * rate * ch + 997 * ch] = blk
    a[100] = 1.0
    secs, gaps, repeats = analyse(a, rate, ch)
    assert len(secs) == 3 and secs[0][3] == 1, secs
    assert len(gaps) == 1 and abs(gaps[0][0] - 1.0) < 1e-3 and abs(gaps[0][1] - 0.2) < 1e-3, gaps
    assert any(abs(t - (2 * rate + 517) / rate) < 0.07 for t in repeats), repeats
    print('selftest ok')


def main(argv):
    if argv[1:] == ['--selftest']:
        return selftest()
    path, opts = argv[1], dict(zip(argv[2::2], argv[3::2]))
    raw = not path.lower().endswith('.wav')
    if raw:
        kind, rate, ch = opts.get('--format', 'f32'), int(opts.get('--rate', 48000)), int(opts.get('--channels', 2))
        with open(path, 'rb') as f:
            a = samples(f.read(), kind)
    else:
        a, rate, ch, kind = read_wav(path)
    report(path, a, rate, ch, kind, raw, float(opts.get('--from', 0)),
           float(opts['--to']) if '--to' in opts else None)


if __name__ == '__main__':
    main(sys.argv)
