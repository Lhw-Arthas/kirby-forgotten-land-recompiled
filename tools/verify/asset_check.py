#!/usr/bin/env python3
"""S4, the asset check: every file of a RomFS reads, every movie and audio stream decodes, and the texture formats the
game holds are ones the runtime can draw.

    asset_check.py --romfs <dump>/romfs --out <work folder> [--av-check build/verify/av_check.exe] [--ffmpeg-bin <dir>]
    asset_check.py --self-test

  - reads every file to the end (a damaged or short copy fails);
  - opens every `.cmp` / `.cmpbin` (a u32 size, then a zstd or LZ4 frame) and checks the size it declares;
  - finds the BNTX texture blocks inside the decompressed files and lists their formats, size and dimension counts,
    then checks each against the formats runtime/nvn/vulkan/textures.c maps (an unmapped one is drawn as RGBA8);
  - decodes every `.webm` and every Nintendo `.opus` with av_check (tools/verify/av_check.c), linked to the FFmpeg the
    runtime ships, and checks the stream parameters hle/movie.c and hle/hwopus rely on;
  - reads the codec of every `.bfstm` from its header and compares it with what the audio renderer decodes.
Everything is read-only. The report goes to --out (the work folder, never git). Exit 1 on any failure.
"""
import argparse
import collections
import concurrent.futures as cf
import ctypes
import ctypes.util
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# BNTX image format -> (name, NVN format code, as runtime/nvn/formats.c numbers it). The Switch Toolbox / nn::gfx
# numbering, type in the high byte and variant in the low one (01 unorm, 02 snorm, 03 uint, 04 sint, 05 float, 06 srgb,
# 0a ufloat), written from memory: a code missing here is listed as unmapped, not guessed.
BNTX = {
    0x0201: ('R8_UNORM', 0x01), 0x0202: ('R8_SNORM', 0x02), 0x0203: ('R8_UINT', 0x03), 0x0204: ('R8_SINT', 0x04),
    0x0701: ('R5G6B5_UNORM', 0x3C), 0x0901: ('R8G8_UNORM', 0x0D), 0x0902: ('R8G8_SNORM', 0x0E),
    0x0A01: ('R16_UNORM', 0x06), 0x0B01: ('R8G8B8A8_UNORM', 0x25), 0x0B02: ('R8G8B8A8_SNORM', 0x26),
    0x0B06: ('R8G8B8A8_SRGB', 0x38), 0x0C01: ('B8G8R8A8_UNORM', 0x99), 0x0C06: ('B8G8R8A8_SRGB', 0x9B),
    0x0D05: ('R9G9B9E5_UFLOAT', 0x40), 0x0E01: ('R10G10B10A2_UNORM', 0x3D), 0x0F05: ('R11G11B10_UFLOAT', 0x3F),
    0x1201: ('R16G16_UNORM', 0x12), 0x1505: ('R16G16B16A16_FLOAT', 0x29), 0x1705: ('R32G32B32A32_FLOAT', 0x2E),
    0x1A01: ('BC1_UNORM', 0x42), 0x1A06: ('BC1_SRGB', 0x46), 0x1B01: ('BC2_UNORM', 0x43), 0x1B06: ('BC2_SRGB', 0x47),
    0x1C01: ('BC3_UNORM', 0x44), 0x1C06: ('BC3_SRGB', 0x48), 0x1D01: ('BC4_UNORM', 0x49), 0x1D02: ('BC4_SNORM', 0x4A),
    0x1E01: ('BC5_UNORM', 0x4B), 0x1E02: ('BC5_SNORM', 0x4C), 0x1F05: ('BC6H_SFLOAT', 0x4F), 0x1F0A: ('BC6H_UFLOAT', 0x50),
    0x2001: ('BC7_UNORM', 0x4D), 0x2006: ('BC7_SRGB', 0x4E),
}
ASTC_SIZES = ['4x4', '5x4', '5x5', '6x5', '6x6', '8x5', '8x6', '8x8', '10x5', '10x6', '10x8', '10x10', '12x10', '12x12']
for _i, _s in enumerate(ASTC_SIZES):
    BNTX[0x2D01 + (_i << 8)] = (f'ASTC_{_s}_UNORM', 0x79 + _i)
    BNTX[0x2D06 + (_i << 8)] = (f'ASTC_{_s}_SRGB', 0x87 + _i)
# bfstm stream codecs and what runtime/services/audio.c decodes (PCM16, DSP ADPCM; the renderer takes both)
BFSTM_CODEC = {0: 'PCM8', 1: 'PCM16', 2: 'DSP-ADPCM', 3: 'IMA-ADPCM'}
BFSTM_SUPPORTED = {1, 2}


def load_zstd(hint=None):
    cands = [hint, os.environ.get('ZSTD_LIB'), ctypes.util.find_library('zstd')]
    for d in os.environ.get('PATH', '').split(os.pathsep):
        for n in ('libzstd.dll', 'libzstd.so.1', 'libzstd.dylib'):
            cands.append(os.path.join(d, n))
    for c in cands:
        if c and os.path.isfile(c) or c and os.sep not in c and '/' not in c:
            try:
                lib = ctypes.CDLL(c)
                lib.ZSTD_decompress.restype = ctypes.c_size_t
                lib.ZSTD_isError.restype = ctypes.c_uint
                lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
                return lib
            except OSError:
                continue
    return None


def decompress(blob, zstd):
    """(bytes, kind) of a .cmp file; raises ValueError."""
    if len(blob) < 8:
        raise ValueError('shorter than its header')
    want = struct.unpack_from('<I', blob)[0]
    body = blob[4:]
    if body[:4] == b'\x28\xb5\x2f\xfd':
        if zstd is None:
            raise ValueError('libzstd not found (--zstd-lib)')
        out = ctypes.create_string_buffer(want)
        r = zstd.ZSTD_decompress(out, want, body, len(body))
        if zstd.ZSTD_isError(r):
            raise ValueError('zstd stream is damaged')
        data, kind = out.raw[:r], 'zstd'
    elif body[:4] == b'\x04\x22\x4d\x18':
        import lz4.frame
        try:
            data, kind = lz4.frame.decompress(body), 'lz4'
        except RuntimeError as e:
            raise ValueError(f'lz4 frame is damaged: {e}')
    else:
        raise ValueError(f'unknown compression, starts {body[:4].hex()}')
    if len(data) != want:
        raise ValueError(f'declares {want} bytes, holds {len(data)}')
    return data, kind


def bntx_textures(data):
    """[(format, width, height, depth, array, mips, dim)] of every BNTX block in data."""
    out, i = [], data.find(b'BNTX')
    while i >= 0:
        if data[i + 0x20:i + 0x24] == b'NX  ':
            count = struct.unpack_from('<I', data, i + 0x24)[0]
            arr = struct.unpack_from('<Q', data, i + 0x28)[0]
            for k in range(min(count, 4096)):
                try:
                    j = i + struct.unpack_from('<Q', data, i + arr + 8 * k)[0]
                    if data[j:j + 4] != b'BRTI':
                        continue
                    dim, mips = data[j + 0x11], struct.unpack_from('<H', data, j + 0x16)[0]
                    fmt, = struct.unpack_from('<I', data, j + 0x1C)
                    w, h, d, a = struct.unpack_from('<4I', data, j + 0x24)
                    out.append((fmt, w, h, d, a, mips, dim))
                except struct.error:
                    break
        i = data.find(b'BNTX', i + 4)
    return out


def bfstm_info(head):
    """(codec, channels, rate) from the first bytes of a .bfstm."""
    if head[:4] != b'FSTM':
        raise ValueError('no FSTM magic')
    nblocks = struct.unpack_from('<H', head, 0x10)[0]
    for k in range(nblocks):
        typ, off, size = struct.unpack_from('<HxxII', head, 0x14 + 12 * k)
        if typ == 0x4000:
            if head[off:off + 4] != b'INFO':
                raise ValueError('INFO block missing')
            ref_off = struct.unpack_from('<I', head, off + 8 + 4)[0]
            base = off + 8 + ref_off
            codec, _loop, ch, _regions, rate = struct.unpack_from('<BBBBI', head, base)
            return codec, ch, rate
    raise ValueError('no INFO block')


def read_file(args):
    path, rel, zstd = args
    r = {'rel': rel, 'size': 0, 'error': None, 'kind': None, 'tex': []}
    try:
        with open(path, 'rb') as f:
            blob = f.read()
        r['size'] = len(blob)
        if rel.endswith(('.cmp', '.cmpbin')):
            data, r['kind'] = decompress(blob, zstd)
            r['tex'] = bntx_textures(data)
        elif rel.endswith('.bfstm'):
            r['bfstm'] = bfstm_info(blob[:0x200])
        elif rel.endswith('.opus') and blob[:4] != b'\x01\x00\x00\x80':
            r['error'] = 'not a Nintendo Opus file'
    except (OSError, ValueError, struct.error) as e:
        r['error'] = str(e)
    return r


def nvn_codes_mapped(textures_c):
    """The NVN format codes runtime/nvn/vulkan/textures.c fmt_of maps (the others fall to RGBA8)."""
    with open(textures_c, encoding='utf-8') as f:
        text = f.read()
    body = text[text.index('static VkFmt fmt_of'):]
    body = body[:body.index('default:')]
    return {int(c, 16) for c in re.findall(r'case (0x[0-9A-Fa-f]+):', body)}


def run_av(av_cmd, mode, files, jobs, env):
    """{path: (ok, line)} for av_check run over files, jobs processes at once."""
    out = {}
    if not files:
        return out
    chunks = [files[i::jobs] for i in range(min(jobs, len(files)))]

    def one(chunk):
        p = subprocess.run(av_cmd + [mode] + chunk, capture_output=True, text=True, env=env)
        return p.stdout, p.returncode, p.stderr
    with cf.ThreadPoolExecutor(len(chunks)) as ex:
        for stdout, rc, err in ex.map(one, chunks):
            for line in stdout.splitlines():
                m = re.match(r'(OK|FAIL) (.*?) (\S+.*)$', line)
                if m:
                    out[m.group(2)] = (m.group(1) == 'OK', m.group(3))
            if rc not in (0, 1):
                raise SystemExit(f'av_check crashed (exit {rc}): {err[-300:]}')
    return out


def check(args):
    romfs = os.path.abspath(args.romfs)
    zstd = load_zstd(args.zstd_lib)
    files = []
    for d, _, fs in os.walk(romfs):
        for f in fs:
            p = os.path.join(d, f)
            files.append((p, os.path.relpath(p, romfs).replace('\\', '/'), zstd))
    files.sort(key=lambda t: t[1])
    failures, warn = [], []
    if zstd is None and any(t[1].endswith(('.cmp', '.cmpbin')) for t in files):
        failures.append('libzstd not found: the .cmp files cannot be opened (--zstd-lib or ZSTD_LIB)')
    ext = collections.Counter()
    nbytes = 0
    fmts = collections.Counter()
    dims = collections.Counter()
    per_fmt_files = collections.defaultdict(set)
    bfstm = collections.Counter()
    kinds = collections.Counter()
    biggest = (0, 0, 0)
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for r in ex.map(read_file, files):
            e = r['rel'].rsplit('.', 2)
            ext['.'.join(e[-2:]) if r['rel'].endswith(('.cmp', '.cmpbin')) else e[-1] if '.' in r['rel'] else '(none)'] += 1
            nbytes += r['size']
            if r['error']:
                failures.append(f'{r["rel"]}: {r["error"]}')
            if r['kind']:
                kinds[r['kind']] += 1
            for fmt, w, h, d, a, mips, dim in r['tex']:
                fmts[fmt] += 1
                per_fmt_files[fmt].add(r['rel'])
                dims[(dim, 'array' if a > 1 else 'single', '3D' if d > 1 else '2D')] += 1
                biggest = max(biggest, (w * h, w, h))
            if 'bfstm' in r:
                bfstm[r['bfstm']] += 1
    mapped = nvn_codes_mapped(args.textures_c) if os.path.isfile(args.textures_c) else None
    tex_rows = []
    for fmt, n in sorted(fmts.items(), key=lambda kv: -kv[1]):
        name, nvn = BNTX.get(fmt, (None, None))
        if name is None:
            status = 'unmapped in this tool'
            warn.append(f'texture format {fmt:#06x}: {n} textures in {len(per_fmt_files[fmt])} files, not in this tool\'s table')
        elif mapped is not None and nvn not in mapped:
            status = 'NOT mapped by the runtime (drawn as RGBA8)'
            failures.append(f'texture format {name} (NVN {nvn:#x}): {n} textures in {len(per_fmt_files[fmt])} files, the runtime does not map it')
        else:
            status = 'supported'
        tex_rows.append(f'{fmt:#06x} {name or "?":18} {n:>7} textures {len(per_fmt_files[fmt]):>6} files  {status}')
    bf_rows = []
    for (codec, ch, rate), n in sorted(bfstm.items()):
        ok = codec in BFSTM_SUPPORTED
        bf_rows.append(f'bfstm {BFSTM_CODEC.get(codec, codec)} {ch}ch {rate}Hz: {n} files' + ('' if ok else '  NOT decoded by the audio renderer'))
        if not ok:
            failures.append(f'bfstm codec {BFSTM_CODEC.get(codec, codec)}: {n} files the audio renderer does not decode')
    movies = [os.path.join(romfs, t[1]) for t in files if t[1].endswith('.webm')]
    opuses = [os.path.join(romfs, t[1]) for t in files if t[1].endswith('.opus')]
    av = {}
    if args.av_check and not args.skip_av:
        env = dict(os.environ)
        if args.ffmpeg_bin:
            env['PATH'] = args.ffmpeg_bin + os.pathsep + env.get('PATH', '')
        cmd = args.av_check if isinstance(args.av_check, list) else [args.av_check]
        av.update(run_av(cmd, 'movie', movies, args.av_jobs, env))
        av.update(run_av(cmd, 'opus', opuses, args.av_jobs, env))
        for p in movies + opuses:
            ok, line = av.get(p.replace('\\', '/'), av.get(p, (False, 'no result from av_check')))
            if not ok:
                failures.append(f'{os.path.relpath(p, romfs)}: {line}')
    elif movies or opuses:
        warn.append(f'{len(movies)} movies and {len(opuses)} opus files not decoded (no --av-check)')
    lines = [f'files {len(files)}, {nbytes / 1e9:.2f} GB read; by type {dict(ext.most_common())}',
             f'compressed files by codec {dict(kinds)}',
             f'textures: {sum(fmts.values())} in {len(set().union(*per_fmt_files.values())) if per_fmt_files else 0} files; largest {biggest[1]}x{biggest[2]}; (dim, array, depth) {dict(dims)}']
    lines += tex_rows + bf_rows
    lines.append(f'movies {len(movies)}, {sum(1 for p in movies if av.get(p, (False,))[0])} decoded; opus {len(opuses)}, '
                 f'{sum(1 for p in opuses if av.get(p, (False,))[0])} decoded')
    lines += ['WARN ' + w for w in warn] + ['FAIL ' + f for f in failures]
    lines.append('S4 ' + ('PASS' if not failures else f'FAIL ({len(failures)} problems)'))
    text = '\n'.join(lines)
    print(text)
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, 's4-report.txt'), 'w', encoding='utf-8') as f:
            f.write(text + '\n')
        with open(os.path.join(args.out, 's4-av.txt'), 'w', encoding='utf-8') as f:
            for p, (ok, line) in sorted(av.items()):
                f.write(f'{"OK" if ok else "FAIL"} {os.path.relpath(p, romfs)} {line}\n')
    return 0 if not failures else 1


FAKE_AV = '''
import sys
mode = sys.argv[1]
for p in sys.argv[2:]:
    bad = p.endswith("bad.webm")
    print(("FAIL" if bad else "OK"), p, mode + " fake" + (" | stream 0: 3 decode errors" if bad else ""))
sys.exit(1 if any(p.endswith("bad.webm") for p in sys.argv[2:]) else 0)
'''


def bntx_blob(formats):
    """A BNTX block holding one BRTI per (format, w, h)."""
    n = len(formats)
    head = bytearray(0x58 + 8 * n)
    head[0:4] = b'BNTX'
    head[0x20:0x24] = b'NX  '
    struct.pack_into('<IQ', head, 0x24, n, 0x58)
    body = bytearray()
    for k, (fmt, w, h) in enumerate(formats):
        brti = bytearray(0x90)
        brti[0:4] = b'BRTI'
        brti[0x11] = 1
        struct.pack_into('<H', brti, 0x16, 1)
        struct.pack_into('<I', brti, 0x1C, fmt)
        struct.pack_into('<4I', brti, 0x24, w, h, 1, 1)
        struct.pack_into('<Q', head, 0x58 + 8 * k, len(head) + len(body))
        body += brti
    return bytes(head + body)


def self_test():
    import io
    import contextlib
    zstd = load_zstd()
    with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()) as cap:
        romfs = os.path.join(tmp, 'romfs')
        for sub in ('gfx', 'mov', 'snd'):
            os.makedirs(os.path.join(romfs, sub))
        def put(rel, data):
            with open(os.path.join(romfs, rel), 'wb') as f:
                f.write(data)

        def cmp_file(data, declared=None):
            """A .cmp as the game ships them: its size, then a zstd frame (an LZ4 frame without libzstd)."""
            head = struct.pack('<I', len(data) if declared is None else declared)
            if zstd:
                comp = ctypes.create_string_buffer(len(data) + 64)
                zstd.ZSTD_compress.restype = ctypes.c_size_t
                n = zstd.ZSTD_compress(comp, len(comp), data, len(data), 1)
                return head + comp.raw[:n]
            import lz4.frame
            return head + lz4.frame.compress(data)
        put('gfx/ok.bfres.cmp', cmp_file(b'FRES' + bntx_blob([(0x1A06, 64, 64), (0x2006, 32, 32)])))
        put('mov/a.webm', b'x')
        put('snd/a.opus', b'\x01\x00\x00\x80' + b'\0' * 40)
        hdr = bytearray(0x200)
        hdr[0:4] = b'FSTM'
        struct.pack_into('<H', hdr, 0x10, 1)
        struct.pack_into('<HxxII', hdr, 0x14, 0x4000, 0x40, 0x100)
        hdr[0x40:0x44] = b'INFO'
        struct.pack_into('<I', hdr, 0x48 + 4, 0x18)
        struct.pack_into('<BBBBI', hdr, 0x60, 2, 1, 2, 0, 48000)
        put('snd/a.bfstm', bytes(hdr))
        tex_c = os.path.join(tmp, 'textures.c')
        with open(tex_c, 'w') as f:
            f.write('static VkFmt fmt_of(uint32_t f) { switch (f) { case 0x46: case 0x4E: return x; default: return y; } }')
        av = os.path.join(tmp, 'av.py')
        with open(av, 'w') as f:
            f.write(FAKE_AV)
        ns = argparse.Namespace(romfs=romfs, out=os.path.join(tmp, 'out'), av_check=[sys.executable, av], ffmpeg_bin=None,
                                zstd_lib=None, jobs=2, av_jobs=2, skip_av=False, textures_c=tex_c)
        assert check(ns) == 0, cap.getvalue()
        assert 'BC1_SRGB' in cap.getvalue() and 'DSP-ADPCM' in cap.getvalue(), cap.getvalue()
        # a damaged .cmp, a format the runtime does not map, an unknown format, a movie that fails to decode
        put('gfx/short.bfres.cmp', cmp_file(b'abcdefgh', declared=99))
        put('gfx/new.bfres.cmp', cmp_file(b'FRES' + bntx_blob([(0x1D01, 8, 8), (0x7777, 8, 8)])))
        put('mov/bad.webm', b'x')
        cap.truncate(0)
        assert check(ns) == 1
        out = cap.getvalue()
        assert 'short.bfres.cmp: declares 99 bytes' in out, out
        assert 'BC4_UNORM' in out and 'does not map it' in out, out
        assert '0x7777' in out and 'WARN' in out, out
        assert 'bad.webm: ' in out and 'decode errors' in out, out
    print('asset_check self-test: ok' + ('' if zstd else ' (no libzstd: the .cmp files were LZ4)'), file=sys.__stdout__)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--romfs')
    ap.add_argument('--out')
    ap.add_argument('--av-check', help='tools/verify/av_check.exe, built by make build/verify/av_check.exe')
    ap.add_argument('--ffmpeg-bin', help="the FFmpeg DLL folder the runtime ships (put on PATH for av_check)")
    ap.add_argument('--zstd-lib')
    ap.add_argument('--textures-c', default=os.path.join(REPO, 'runtime', 'nvn', 'vulkan', 'textures.c'))
    ap.add_argument('--jobs', type=int, default=4, help='files read at once')
    ap.add_argument('--av-jobs', type=int, default=2, help='av_check processes at once (movie decoding is the slow part)')
    ap.add_argument('--skip-av', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.romfs:
        ap.error('--romfs is required')
    sys.exit(check(args))


if __name__ == '__main__':
    main()
