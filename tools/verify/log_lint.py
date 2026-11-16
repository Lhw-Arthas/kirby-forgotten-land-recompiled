#!/usr/bin/env python3
"""S3, the log gate: fail a run whose log shows the runtime stubbing, ignoring or crashing on something.

    log_lint.py <log>... [--allow shapes.txt] [--write-baseline shapes.txt] [--runtime <repo>/runtime] [--source-gaps]
    log_lint.py --self-test

Every log line that matches a pattern below is reduced to a "shape" (addresses and counters removed; command ids kept,
so "cmd 62" and "cmd 63" are two shapes). A run passes when every shape is in the --allow file. Exit 1 otherwise.
--write-baseline writes the shapes of known-good runs to a file (keep it in the work folder, not in git).
--source-gaps reads the runtime's own LOG() strings and lists the ones that look like a stub, an unimplemented path or
a failure but match no pattern: run it after an engine merge, so a stub that upstream added is not missed.
"""
import argparse
import contextlib
import io
import os
import re
import sys
import tempfile

# (name, regex). Failures, stubs and "answered ok without doing it" lines the runtime writes (runtime/ipc.c,
# services/*.c, dispatch.c, hle/*.c, nvn/**). Lines from the game's own logging (lm) are excluded in shape().
PATTERNS = [
    ('crash', r'^FATAL:|\[throw\] uncaught|\[hang\] =====|\[vk\] device fault|^host backtrace|device lost'),
    ('missing code', r'no function at|undefined instruction / trap at|brk #\d+ at'),
    ('hle unimplemented', r'\[hle\] UNIMPLEMENTED|unimplemented virtual'),
    ('service stub', r'\[ipc\] STUB |\[ipc\] no HLE implementation|ConnectToNamedPort.*unsupported|\[sm\] unknown cmd'),
    ('ipc unhandled', r'\[ipc\] (control cmd|message type|bad SFCI|SendSyncRequest on bad|domain object .* missing)'),
    ('service cmd unhandled', r'\(unhandled, ok\)'),
    ('service cmd answered ok', r'cmd \d+ \(ok\)'),
    ('not implemented', r'not implemented|splitter section present \(ignored\)|GetInfo unknown id'),
    ('shader', r'translate failed|compile failed|shader module creation failed|pipeline (creation )?failed|'
               r'pipelines? failed|a pipeline failed to build|unknown blend factor|prepared \d+ \+ \d+ .*\([1-9]\d* failed;|'
               r'pipeline binaries: (capture|creating).* failed|\[vk\] vertex format \d+ (unknown|unsupported)'),
    ('gpu resource', r'image creation failed|view failed|texture creation failed|guest heap .* failed|'
                     r'heap as driver memory: .* failed|present failed|acquire failed|NV12 pipeline failed|'
                     r'\[view\] FAILED|gmem copies: no slot|\[vk\] saving .* failed'),
    ('assets', r'romfs: \d+ of the game\'s files missing|storage read failed|\[save\] .*(FAILED|failed)|'
               r'\[heaps\] .* FAILED|\[gamebuild\] .*failed|SDL audio \w+ failed'),
    ('draws dropped', r'draws have been missing for|its draws are dropped|\[skip:[^\]]*?(?:noprog|failed|noobj|mode) [1-9]'),
]
RX = [(n, re.compile(p)) for n, p in PATTERNS]
# volatile parts of a line: hex addresses and counters. Command and effect type ids are kept.
VOLATILE = [
    (re.compile(r'\(in \d+ bytes[^)]*\)'), '(args)'),
    (re.compile(r'\(lr=[^)]*\)'), '(lr)'),
    (re.compile(r'\bhandle [0-9a-f]+'), 'handle N'),
    (re.compile(r'0x[0-9a-fA-F]+|\b[0-9a-f]{5,}\b'), 'N'),
    (re.compile(r'(?<!cmd )(?<!type )\b\d+\b'), 'N'),
]


def classify(line):
    for name, rx in RX:
        if rx.search(line):
            return name
    return None


def shape(line):
    s = line.strip()
    for rx, sub in VOLATILE:
        s = rx.sub(sub, s)
    return s[:160]


def scan(paths):
    """{shape: [pattern name, count, set of logs, first line]}"""
    found = {}
    for p in paths:
        with open(p, encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('[lm]'):  # the game's own log output, not the runtime's
                    continue
                name = classify(line)
                if name:
                    e = found.setdefault(shape(line), [name, 0, set(), line.strip()[:200]])
                    e[1] += 1
                    e[2].add(os.path.basename(p))
    return found


def read_allow(path):
    if not path:
        return set()
    with open(path, encoding='utf-8') as f:
        return {l.rstrip('\n') for l in f if l.strip() and not l.startswith('#')}


def fmt_to_regex(fmt):
    parts = re.split(r'%[-+# 0]*\d*(?:\.\d+)?(?:ll|l|h|z|j)?[a-zA-Z]', fmt)
    return re.compile('.*'.join(re.escape(p) for p in parts))


KEYWORDS = re.compile(r'stub|unimplemented|not implemented|unsupported|unhandled|unknown|ignored|failed|missing|'
                      r'\(ok\)|no HLE|fatal|lost', re.I)
BENIGN = re.compile(r'imports bound to host stubs|scripted run: keyboard|i18n\] .*missing|gpl\] probe|window surface lost|'
                    r'of the game code at the root, ignored|pipeline binaries: functions missing|ImGui|pipelines from stored binaries|one import of')
LOG_FMT = re.compile(r'\bLOG\(\s*"((?:[^"\\]|\\.)*)"')


def source_gaps(runtime):
    gaps = []
    for d, _, files in os.walk(runtime):
        for fn in files:
            if not fn.endswith(('.c', '.cpp', '.m', '.mm')):
                continue
            path = os.path.join(d, fn)
            with open(path, encoding='utf-8', errors='replace') as f:
                for i, line in enumerate(f, 1):
                    m = LOG_FMT.search(line)
                    if not m or not KEYWORDS.search(m.group(1)):
                        continue
                    text = m.group(1).replace('\\n', '').replace('\\"', '"')
                    sample = re.sub(r'%[-+# 0]*\d*(?:\.\d+)?(?:ll|l|h|z|j)?[a-zA-Z]', '1', text)
                    if classify(sample) is None and not BENIGN.search(sample):
                        gaps.append(f'{os.path.relpath(path, runtime)}:{i}: {text}')
    return gaps


def lint(args):
    found = scan(args.logs)
    if args.write_baseline:
        with open(args.write_baseline, 'w', encoding='utf-8') as f:
            f.write('# shapes of the log lines log_lint.py accepts (from known-good runs)\n')
            for s in sorted(found):
                f.write(s + '\n')
        print(f'wrote {len(found)} shapes to {args.write_baseline}')
        return 0
    allow = read_allow(args.allow)
    new = {s: e for s, e in found.items() if s not in allow}
    for s, (name, n, logs, first) in sorted(new.items(), key=lambda kv: (kv[1][0], kv[0])):
        print(f'NEW [{name}] x{n} in {len(logs)} log(s): {first}')
    stale = sorted(allow - set(found)) if args.report_stale else []
    for s in stale:
        print(f'note: allowed shape not seen in these logs: {s}')
    if args.source_gaps:
        gaps = source_gaps(args.runtime)
        for g in gaps:
            print('source: no pattern covers', g)
        print(f'{len(gaps)} runtime log strings look like a stub or failure and match no pattern')
    print(f'{len(found)} flagged shapes in {len(args.logs)} log(s), {len(new)} not allowed:', 'FAIL' if new else 'PASS')
    return 1 if new else 0


def self_test():
    with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
        def w(name, text):
            p = os.path.join(tmp, name)
            with open(p, 'w') as f:
                f.write(text)
            return p
        good = w('good.log', '[sm] GetService hid\n[hid] SetSupportedNpadStyleSet 0x1f\n[lm] [ipc] STUB x cmd 1\n'
                             '[vk] t=2.0 frame 120 fps 60 skipped 0\n')
        stub = ('[ipc] no HLE implementation for service \'prepo:u\', using logging stub\n'
                '[ipc] STUB prepo:u cmd 10105 (in 24 bytes, x0 a1 b0 c0) -> ok\n'
                '[am] ISelfController cmd 62 (unhandled, ok)\n[bsd] bsd:u cmd 0 (ok)\n')
        a = w('a.log', stub + '[am] IStorage cmd 7 (ok)\n')
        b = w('b.log', stub.replace('10105 (in 24', '10105 (in 32').replace('x0 a1', 'x1 a2') +
                       '[hle] UNIMPLEMENTED nn_foo (x0=1 x1=2 lr=7000123456) -> returning 0\n')
        c = w('c.log', stub.replace('cmd 62', 'cmd 63'))
        d = w('d.log', 'FATAL: no function at 7000123456 (lr=7000000010 ?+0)\n[shader] FS translate failed: unknown instruction 5\n')
        base = os.path.join(tmp, 'base.txt')
        ns = lambda **k: argparse.Namespace(**{**dict(logs=[], allow=None, write_baseline=None, runtime=None,
                                                      source_gaps=False, report_stale=False), **k})
        assert lint(ns(logs=[good])) == 0, 'a clean log passes'
        assert lint(ns(logs=[a])) == 1, 'stubs fail without an allow list'
        assert lint(ns(logs=[a], write_baseline=base)) == 0
        assert lint(ns(logs=[a], allow=base)) == 0, 'the baseline run passes against its baseline'
        assert lint(ns(logs=[b], allow=base)) == 1, 'a new unimplemented call fails (a changed byte count does not make a new shape)'
        assert lint(ns(logs=[c], allow=base)) == 1, 'a new command id is a new shape'
        assert lint(ns(logs=[d], allow=base)) == 1, 'a fatal line fails'
        e = w('e.log', '[vk] t=4.7 frame 42 18.9 fps (245 draws, 88 skipped [skip: noprog 0 compiling 35 failed 0 noobj 0 '
                       'mode 0 pipe-building 53 pipe-failed 0])\n')
        assert lint(ns(logs=[e])) == 0, 'draws skipped while a pipeline builds are transient'
        f_ = w('f.log', '[vk] t=4.7 frame 42 (245 draws, 88 skipped [skip: noprog 0 compiling 0 failed 3 noobj 0 '
                        'mode 0 pipe-building 0 pipe-failed 0])\n')
        assert lint(ns(logs=[f_])) == 1, 'draws skipped for good fail'
        rt = os.path.join(tmp, 'rt')
        os.makedirs(rt)
        w('rt/x.c', 'LOG("[ipc] STUB %s cmd %u (in %u bytes)\\n", a, b, c);\nLOG("[foo] frobnicate unsupported %d\\n", 1);\n'
                    'LOG("[foo] all fine %d\\n", 1);\n')
        gaps = source_gaps(rt)
        assert len(gaps) == 1 and 'frobnicate' in gaps[0], gaps
    print('log_lint self-test: ok')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='*')
    ap.add_argument('--allow')
    ap.add_argument('--write-baseline')
    ap.add_argument('--report-stale', action='store_true', help='also list allowed shapes these logs do not show')
    ap.add_argument('--source-gaps', action='store_true')
    ap.add_argument('--runtime', default=os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), 'runtime'))
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.logs and not args.source_gaps:
        ap.error('give at least one log')
    sys.exit(lint(args))


if __name__ == '__main__':
    main()
