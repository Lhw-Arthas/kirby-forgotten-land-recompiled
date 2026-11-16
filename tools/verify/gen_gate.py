#!/usr/bin/env python3
"""S1, the generator gate: run the generator's lift over every module of a dump and fail on code the runtime cannot run.

    gen_gate.py --exefs <dump>/exefs --out <work folder> [--gen tools/gen.py] [--python <python with unicorn>]
    gen_gate.py --self-test

It runs the generator as the Makefile's `gen` does (any generator that takes `name nso base outdir`: v0.2.0's, LAS's,
this repo's), reads its own counters from stderr and scans the C it wrote. It fails (exit 1) on
  - any instruction the lifter does not support (a trap the generator emits in its place: rt_udf, fatal when run),
  - a direct branch or call whose target is in no module's function table (the runtime stops with "no function at"),
  - a direct branch, call or jump table case that lands on a trap word (data or an unsupported instruction).
It reports, without failing, what only the runtime can settle: trap words that no direct branch reaches (padding,
literal pools), brk sites, and indirect branches the generator could not resolve to a table.
The output stays in --out (never in git). Nothing is read from or written to the game's folders.
"""
import argparse
import ast
import concurrent.futures as cf
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MODULES_RE = re.compile(r'X\(\s*(\w+)\s*,\s*(0x[0-9a-fA-F]+)\s*\)')
STATS_RE = re.compile(r'^(\w+): (\d+) boundaries, (\d+) functions, (\d+) rounds, (\{.*\}), \d+s$')
CALL_RE = re.compile(r'\bf_([0-9a-f]+)\(c\);')
PC_LIT_RE = re.compile(r'c->pc = 0x([0-9a-f]+)ull;')
GOTO_RE = re.compile(r'goto L_([0-9a-f]+);')
TRAP_RE = re.compile(r'^\s*rt_udf\(c, 0x([0-9a-f]+)ull\);(?: /\* (.*) \*/)?')
FN_RE = re.compile(r'^void f_([0-9a-f]+)\(Cpu \*c\) \{')
ADDR_RE = re.compile(r'0x([0-9a-f]+)')


def parse_modules(text):
    return [(n, int(a, 16)) for n, a in MODULES_RE.findall(text)]


def scan_module(args):
    """The facts one module's C holds. Runs in a worker process."""
    name, outdir = args
    r = {'unsupported': Counter(), 'data_traps': 0, 'brk': 0, 'literal_pcs': set(), 'calls': set(),
         'trap_entries': set(), 'trap_gotos': [], 'files': 0}
    for fn in sorted(os.listdir(outdir)):
        if not re.fullmatch(re.escape(name) + r'_\d{3}\.c', fn):
            continue
        r['files'] += 1
        trap_labels, gotos, label = set(), [], None
        fn_addr, first, in_dispatch = None, True, False

        def end_fn():
            for g, at in gotos:
                if g in trap_labels:
                    r['trap_gotos'].append((fn_addr, g, at))
        with open(os.path.join(outdir, fn), encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('void f_'):
                    m = FN_RE.match(line)
                    if m:
                        if fn_addr:
                            end_fn()
                        fn_addr, first, in_dispatch, trap_labels, gotos, label = m.group(1), True, False, set(), [], None
                    continue
                if line.startswith('L_dispatch'):
                    in_dispatch = True
                    continue
                if line.startswith('L_'):
                    label = line[2:line.index(':')]
                    continue
                if 'rt_udf(' in line or 'rt_brk(' in line:
                    m = TRAP_RE.match(line)
                    if m:
                        if m.group(2):
                            r['unsupported'][m.group(2)] += 1
                        else:
                            r['data_traps'] += 1
                        if label is not None:
                            trap_labels.add(label)
                        if first and fn_addr:
                            r['trap_entries'].add(fn_addr)
                    elif 'rt_brk(' in line:
                        r['brk'] += 1
                        if label is not None:
                            trap_labels.add(label)
                        if first and fn_addr:
                            r['trap_entries'].add(fn_addr)
                    label = None
                    first = False
                    continue
                if line.startswith('    FN_ENTER'):
                    continue
                label = None
                first = False
                if 'f_' in line and '(c);' in line:
                    r['calls'].update(CALL_RE.findall(line))
                if 'c->pc = 0x' in line:
                    for a in PC_LIT_RE.findall(line):
                        r['literal_pcs'].add(a)
                if 'goto L_' in line and not in_dispatch and 'case ' not in line:
                    for g in GOTO_RE.findall(line):
                        gotos.append((g, 'direct'))
                elif 'goto L_' in line and not in_dispatch:  # a resolved jump table's cases
                    for g in GOTO_RE.findall(line):
                        gotos.append((g, 'jump table'))
        if fn_addr:
            end_fn()
    r['unsupported'] = dict(r['unsupported'])
    return name, r


def table_addrs(outdir, name):
    path = os.path.join(outdir, f'{name}_table.c')
    with open(path, encoding='utf-8') as f:
        text = f.read()
    i = text.index(f'{name}_fn_addrs[]')
    j = text.index('};', i)
    return {a for a in ADDR_RE.findall(text[i:j])}


WORDS_PY = '''
import sys
from nso import Nso
n = Nso(sys.argv[1]); base = int(sys.argv[2], 16)
for a in sys.argv[3:]:
    o = int(a, 16) - base
    print(a, int.from_bytes(bytes(n.image[o:o + 4]), 'little'))
'''
COMPILER_TRAP = 0xE7FFDEFE  # udf #0xdefe: what the Switch toolchain puts after a call that never returns, and on unreachable/UB paths


def words_at(py, gendir, nso, base, addrs):
    """The instruction words at addrs, read with the generator's own NSO loader."""
    if not addrs:
        return {}
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONPATH=gendir)
    p = subprocess.run([py, '-c', WORDS_PY, nso, hex(base)] + sorted(addrs), env=env, capture_output=True, text=True)
    if p.returncode:
        sys.exit('gen_gate: could not read the NSO: ' + p.stderr[-300:])
    return {a: int(w) for a, w in (l.split() for l in p.stdout.splitlines())}


def run_gen(py, gen, name, nso, base, outdir, hle):
    cmd = [py, gen, name, nso, hex(base), outdir]
    if hle and name == 'sdk':  # (the Makefile's gen passes the HLE list to the sdk only)
        cmd += ['--hle', hle]
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    t = time.time()
    p = subprocess.run(cmd, cwd=os.path.dirname(os.path.abspath(gen)), env=env, capture_output=True, text=True)
    return name, p.returncode, p.stderr, time.time() - t


def gate(args):
    if args.modules:
        modules = [(n, int(a, 16)) for n, a in (m.split(':') for m in args.modules.split(','))]
    else:
        modules = parse_modules(next(l for l in open(args.game_h) if l.startswith('#define GAME_MODULES')))
    if not modules:
        sys.exit('gen_gate: no modules (game/game.h GAME_MODULES or --modules name:0xbase)')
    gen = os.path.abspath(args.gen)
    gendir = os.path.join(args.out, 'gen')
    results = {m: {'base': hex(b)} for m, b in modules}
    t0 = time.time()
    if not args.reuse:
        with cf.ThreadPoolExecutor(len(modules)) as ex:
            futs = [ex.submit(run_gen, args.python, gen, n, os.path.join(args.exefs, n), b, os.path.join(gendir, n),
                              args.hle and os.path.abspath(args.hle)) for n, b in modules]
            # (the modules are independent: the generator runs once per module, as the Makefile's gen does)
            for fu in futs:
                name, rc, err, secs = fu.result()
                os.makedirs(gendir, exist_ok=True)
                with open(os.path.join(gendir, name + '.stderr'), 'w') as f:
                    f.write(f'exit {rc} after {secs:.0f} s\n{err}')
    for n, _ in modules:  # (also what --reuse reads)
        res = results[n]
        with open(os.path.join(gendir, n + '.stderr')) as f:
            head, err = f.read().split('\n', 1)
        res['gen_rc'], res['gen_secs'] = int(head.split()[1]), int(head.split()[3])
        res['gen_notes'] = [l for l in err.splitlines() if ' progress ' not in l]
        for l in err.splitlines():
            m = STATS_RE.match(l)
            if m:
                res['functions'] = int(m.group(3))
                res['stats'] = ast.literal_eval(m.group(5))
        if res['gen_rc'] == 0 and 'stats' not in res:
            sys.exit(f'gen_gate: no "N boundaries, N functions" line from the {n} generator: its output format changed, fix this tool')
    with cf.ProcessPoolExecutor(len(modules)) as ex:
        scans = dict(ex.map(scan_module, [(n, os.path.join(gendir, n)) for n, _ in modules]))
    addrs = {n: table_addrs(os.path.join(gendir, n), n) for n, _ in modules if results[n].get('gen_rc', 0) == 0}
    known = set().union(*addrs.values()) if addrs else set()
    failures = []
    for n, base in modules:
        res, sc = results[n], scans[n]
        if res.get('gen_rc', 0) != 0:
            failures.append(f'{n}: the generator failed (exit {res["gen_rc"]}): ' + ' | '.join(res['gen_notes'][-3:]))
            continue
        res['unsupported_instructions'] = sum(sc['unsupported'].values())
        res['unsupported_by_reason'] = sc['unsupported']
        res['data_trap_words'] = sc['data_traps']
        res['brk_sites'] = sc['brk']
        res['jump_tables'] = res['stats'].get('jumptables', 0)
        res['indirect_unresolved'] = {k: v for k, v in res['stats'].items() if k.startswith('br_')}
        bad_lit = sorted(a for a in sc['literal_pcs'] if a not in known)
        res['dispatch_targets_without_function'] = bad_lit
        res['direct_out_of_text_targets_checked'] = len(sc['literal_pcs'])
        # A branch onto the compiler's own trap word is the game's own abort path (a call that never returns falls
        # through into it; a failed check branches to it): reported. Onto any other word it is a defect.
        trap_calls = sorted(a for a in sc['calls'] if a in sc['trap_entries'])
        targets = set(trap_calls) | {g for _, g, _ in sc['trap_gotos']}
        w = words_at(args.python, os.path.dirname(gen), os.path.join(args.exefs, n), base, targets)
        res['calls_to_compiler_trap'] = [a for a in trap_calls if w[a] == COMPILER_TRAP]
        res['branches_to_compiler_trap'] = sum(1 for _, g, _ in sc['trap_gotos'] if w[g] == COMPILER_TRAP)
        res['calls_to_other_trap'] = [f'{a} (word {w[a]:08x})' for a in trap_calls if w[a] != COMPILER_TRAP]
        res['branches_to_other_trap'] = [f'f_{f}: {kind} branch to {g} (word {w[g]:08x})'
                                         for f, g, kind in sc['trap_gotos'] if w[g] != COMPILER_TRAP]
        # the generator's own counter must agree with what is in the C: a mismatch means a changed output format
        counted = res['stats'].get('unsupported', 0)
        if counted != res['unsupported_instructions']:
            failures.append(f'{n}: the generator counted {counted} unsupported instructions, the C has '
                            f'{res["unsupported_instructions"]} (output format changed? fix this tool)')
        if res['unsupported_instructions']:
            failures.append(f'{n}: {res["unsupported_instructions"]} unsupported instructions: '
                            + ', '.join(f'{k} x{v}' for k, v in sorted(sc['unsupported'].items())[:8]))
        if bad_lit:
            failures.append(f'{n}: {len(bad_lit)} direct branch targets in no function table (no function at): '
                            + ', '.join(bad_lit[:8]))
        if res['calls_to_other_trap']:
            failures.append(f'{n}: {len(res["calls_to_other_trap"])} direct calls into a word that is no instruction: '
                            + ', '.join(res['calls_to_other_trap'][:8]))
        if res['branches_to_other_trap']:
            failures.append(f'{n}: {len(res["branches_to_other_trap"])} branches onto a word that is no instruction: '
                            + '; '.join(res['branches_to_other_trap'][:4]))
    out = {'ok': not failures, 'failures': failures, 'secs': round(time.time() - t0), 'modules': results}
    with open(os.path.join(args.out, 's1-report.json'), 'w') as f:
        json.dump(out, f, indent=1, default=sorted)
    for n, _ in modules:
        r = results[n]
        if 'unsupported_instructions' not in r:
            continue
        print(f'{n}: {r["functions"]} functions, unsupported {r["unsupported_instructions"]}, '
              f'unresolved direct targets {len(r["dispatch_targets_without_function"])}, '
              f'branches into non-instruction words {len(r["calls_to_other_trap"]) + len(r["branches_to_other_trap"])}; '
              f'info: branches to the compiler trap {len(r["calls_to_compiler_trap"]) + r["branches_to_compiler_trap"]}, trap words {r["data_trap_words"]}, brk {r["brk_sites"]}, jump tables {r["jump_tables"]}, '
              f'indirect {r["indirect_unresolved"]}')
    for f in failures:
        print('FAIL', f)
    print('S1', 'PASS' if not failures else 'FAIL', f'({out["secs"]} s, report {os.path.join(args.out, "s1-report.json")})')
    return 0 if not failures else 1


FAKE_GEN = r'''
import os, sys
name, nso, base, outdir = sys.argv[1:5]
mode = open(nso).read().strip()
os.makedirs(outdir, exist_ok=True)
base = int(base, 16)
fns = [base, base + 0x10, base + 0x20]
body = ['#include "cpu.h"', f'void f_{fns[0]:x}(Cpu *c) {{', f'    FN_ENTER(0x{fns[0]:x}ull);', f'L_{fns[0]:x}:;',
        f'    c->x[30] = 0x{fns[0]+4:x}ull; f_{fns[1]:x}(c);', f'    return;', '}',
        f'void f_{fns[1]:x}(Cpu *c) {{', f'    FN_ENTER(0x{fns[1]:x}ull);', f'L_{fns[1]:x}:;', '    return;', '}',
        f'void f_{fns[2]:x}(Cpu *c) {{', f'    FN_ENTER(0x{fns[2]:x}ull);', f'L_{fns[2]:x}:;',
        f'    rt_udf(c, 0x{fns[2]:x}ull);', '}']
unsup = 0
if mode == 'unsupported':
    body[-2] = f'    rt_udf(c, 0x{fns[2]:x}ull); /* ldst 12345678 */'
    unsup = 1
if mode == 'badpc':
    body.insert(4, '    c->pc = 0x1234ull; __attribute__((musttail)) return rt_dispatch(c);')
if mode in ('traptarget', 'compilertrap'):
    body[4] = f'    c->x[30] = 0x{fns[0]+4:x}ull; f_{fns[2]:x}(c);'
if mode == 'padding':  # a trap word no branch reaches: only reported
    body.insert(4, '    rt_udf(c, 0x7ull);')
open(os.path.join(outdir, f'{name}_000.c'), 'w').write('\n'.join(body) + '\n')
open(os.path.join(outdir, f'{name}_table.c'), 'w').write('const uint64_t %s_fn_addrs[] = {\n%s};\n' % (
    name, ',\n'.join(f'0x{a:x}ull' for a in fns)))
print(f'{name}: 3 boundaries, 3 functions, 2 rounds, {{\'unsupported\': {unsup}, \'jumptables\': 1}}, 0s', file=sys.stderr)
'''


FAKE_NSO = '''
class Nso:
    def __init__(self, path):
        mode = open(path).read().strip()
        self.image = bytearray(0x100)
        if mode == 'compilertrap':
            self.image[0x20:0x24] = (0xE7FFDEFE).to_bytes(4, 'little')
'''


def self_test():
    import io
    with tempfile.TemporaryDirectory() as tmp:
        gen = os.path.join(tmp, 'fakegen', 'gen.py')
        os.makedirs(os.path.dirname(gen))
        open(gen, 'w').write(FAKE_GEN)
        open(os.path.join(os.path.dirname(gen), 'nso.py'), 'w').write(FAKE_NSO)
        exefs = os.path.join(tmp, 'exefs')
        os.makedirs(exefs)
        cases = [('clean', 0, 'S1 PASS'), ('padding', 0, 'S1 PASS'), ('unsupported', 1, '1 unsupported'),
                 ('badpc', 1, 'no function at'), ('traptarget', 1, 'no instruction'), ('compilertrap', 0, 'S1 PASS')]
        for mode, rc, want in cases:
            open(os.path.join(exefs, 'main'), 'w').write(mode)
            open(os.path.join(exefs, 'sdk'), 'w').write('clean')
            out = os.path.join(tmp, 'out-' + mode)
            os.makedirs(out)
            ns = argparse.Namespace(exefs=exefs, out=out, gen=gen, python=sys.executable, game_h=None,
                                    modules='main:0x7000000000,sdk:0x7004000000', hle=None, reuse=False)
            old, sys.stdout = sys.stdout, io.StringIO()
            try:
                got = gate(ns)
                text = sys.stdout.getvalue()
            finally:
                sys.stdout = old
            assert got == rc and want in text, f'{mode}: exit {got}, output {text!r}'
        print('gen_gate self-test: ok (%d cases)' % len(cases))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exefs')
    ap.add_argument('--out')
    ap.add_argument('--gen', default=os.path.join(REPO, 'tools', 'gen.py'))
    ap.add_argument('--python', default=sys.executable, help='a Python with unicorn (the generator needs it)')
    ap.add_argument('--game-h', default=os.path.join(REPO, 'game', 'game.h'))
    ap.add_argument('--modules', help='name:0xbase,... instead of game.h (a checkout from before game.h)')
    ap.add_argument('--hle', default=os.path.join(REPO, 'runtime', 'hle', 'hle_symbols.txt'))
    ap.add_argument('--reuse', action='store_true', help='scan the C already in <out>/gen again')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not (args.exefs and args.out):
        ap.error('--exefs and --out are required')
    os.makedirs(args.out, exist_ok=True)
    sys.exit(gate(args))


if __name__ == '__main__':
    main()
