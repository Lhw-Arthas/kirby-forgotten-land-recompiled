#!/usr/bin/env python3
"""S3, the service check: which system services and commands the game needs, and which of them the runtime answers.

    service_check.py --exefs <dump>/exefs --logs <log or folder>... [--runtime runtime] [--out report.txt]
    service_check.py --self-test

Three sources, compared:
  - the game's binaries: service names the NSOs hold as strings and reach from code (an ADRP+ADD of the string's
    address). That is what the game CAN ask for. Command ids live in code, not in strings, so they come from logs only.
  - logs of runs: the services the game opened (`[sm] GetService`) and every command a handler answered without
    doing it (`STUB`, `unhandled`, `(ok)`), with how often.
  - the runtime's source (services/*.c, ipc.c): the SERVICE() names, the interfaces they return and the `case N`
    labels of each handler, so a command seen in an old log can be checked against the runtime as it is now.
It reads the dump and the logs and writes the report only where --out says (the work folder, never git).
"""
import argparse
import collections
import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
# Horizon services without a ':' in their name (the ones with one are found by shape). Public knowledge, not the game's.
PLAIN = set('apm appletAE appletOE audctl btdrv btm caps clkrst csrng es fgm fsp-ldr fsp-pr fsp-srv gpio grc hid '
            'hwopus lbl lm ncm nim nvdrv ro ssl sfdnsres sm spl set psm pdm pm friend mig usb'.split())
NAME_RE = re.compile(rb'(?<![\x20-\x7e])([a-zA-Z][a-zA-Z0-9\-_]{1,7}(?::[a-z0-9]{1,3})?)\x00')


def binary_refs(exefs):
    """{module: {service name: number of code references}}"""
    sys.path.insert(0, os.path.join(REPO, 'tools'))
    sys.dont_write_bytecode = True
    from nso import Nso
    out = {}
    for mod in sorted(os.listdir(exefs)):
        path = os.path.join(exefs, mod)
        if mod in ('rtld', 'main.npdm') or not os.path.isfile(path):
            continue
        try:
            n = Nso(path)
        except (AssertionError, struct.error):
            continue
        img = bytes(n.image)
        names = {m.start(1): m.group(1).decode() for m in NAME_RE.finditer(img)
                 if b':' in m.group(1) or m.group(1).decode() in PLAIN}
        lo, size = n.text[0], n.text[1]
        words = struct.unpack(f'<{size // 4}I', img[lo:lo + size])
        refs, adrp = collections.Counter(), {}
        for i, w in enumerate(words):
            if (w & 0x9F000000) == 0x90000000:  # ADRP
                imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
                imm -= (1 << 21) if imm & (1 << 20) else 0
                adrp[w & 31] = (((lo + i * 4) & ~0xFFF) + (imm << 12), i)
            elif (w & 0xFFC00000) == 0x91000000:  # ADD (immediate)
                rn = (w >> 5) & 31
                if rn in adrp and i - adrp[rn][1] <= 6:
                    t = adrp[rn][0] + ((w >> 10) & 0xFFF)
                    if t in names:
                        refs[names[t]] += 1
        out[mod] = dict(refs)
    return out


# ---------------------------------------------------------------- the runtime's source

def strip_c(text):
    """Comments out and braces in strings blanked, so braces count right."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            i = text.find('\n', i)
            i = n if i < 0 else i
        elif text.startswith('/*', i):
            i = text.find('*/', i) + 2
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(re.sub(r'[{}]', ' ', text[i:j + 1]))  # (braces inside a string must not count)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def body_at(text, start):
    """The {...} block whose opening brace is the first one at or after start."""
    i = text.index('{', start)
    depth, j = 0, i
    while j < len(text):
        depth += (text[j] == '{') - (text[j] == '}')
        j += 1
        if depth == 0:
            return text[i:j]
    return text[i:]


REG_SERVICE = re.compile(r'\bSERVICE\(\s*"([^"]+)"\s*,\s*(\w+)\s*\)')
REG_IFACE = re.compile(r'service_new\(\s*"([^"]+)"\s*,\s*(\w+)')
REG_TABLE = re.compile(r'\{\s*\d+\s*,\s*"(I\w+)"\s*,\s*(\w+)\s*\}')
CASE = re.compile(r'\bcase\s+(\d+)\s*:|req->cmd\s*==\s*(\d+)')


def runtime_handlers(runtime):
    """{name: {'cases': set of command ids, 'file': path}} for every service and interface the runtime registers."""
    out = {}
    for d, _, files in os.walk(runtime):
        for fn in files:
            if not fn.endswith(('.c', '.cpp')):
                continue
            path = os.path.join(d, fn)
            with open(path, encoding='utf-8', errors='replace') as f:
                text = strip_c(f.read())
            defs = {}  # handler function name -> its body
            for m in REG_SERVICE.finditer(text):
                defs[m.group(2)] = body_at(text, m.end())
            for m in re.finditer(r'\bvoid\s+(\w+)\(\s*Service \*\w+,\s*IpcReq \*\w+,\s*IpcResp \*\w+\)\s*\{', text):
                defs.setdefault(m.group(1), body_at(text, m.end() - 1))

            def cases(fnname, depth=0):
                b = defs.get(fnname, '')
                found = {int(a or b_) for a, b_ in CASE.findall(b)}
                if not found and depth < 2:  # a handler that only calls another one
                    call = re.fullmatch(r'\{\s*(?:\(void\)\w+;\s*)*(\w+)\(\s*\w+,\s*\w+,\s*\w+\);\s*\}', b.strip())
                    if call:
                        return cases(call.group(1), depth + 1)
                return found
            for rx in (REG_SERVICE, REG_IFACE, REG_TABLE):
                for m in rx.finditer(text):
                    name, fnname = m.group(1), m.group(2)
                    if fnname in defs:
                        out.setdefault(name, {'cases': set(), 'file': os.path.relpath(path, runtime)})
                        out[name]['cases'] |= cases(fnname)
    return out


# ---------------------------------------------------------------- logs

L_GET = re.compile(r'^\[sm\] GetService (\S+)')
L_NOIMPL = re.compile(r"^\[ipc\] no HLE implementation for service '([^']+)'")
L_STUB = re.compile(r'^\[ipc\] STUB (\S+) cmd (\d+)')
L_IFACE = re.compile(r'^\[(\w+)\] ([\w:\-]+) cmd (\d+) \((unhandled, ok|ok|not implemented)')
L_TAG = re.compile(r'^\[(\w+)\] cmd (\d+) \((ok|not implemented)')


def scan_logs(paths):
    r = {'opened': collections.Counter(), 'stub_services': set(), 'cmds': collections.Counter(), 'logs': 0}
    files = []
    for p in paths:
        if os.path.isdir(p):
            for d, _, fs in os.walk(p):
                files += [os.path.join(d, f) for f in fs if f.endswith('.log')]
        else:
            files.append(p)
    for p in files:
        r['logs'] += 1
        with open(p, encoding='utf-8', errors='replace') as f:
            for line in f:
                if not line.startswith('['):
                    continue
                m = L_GET.match(line)
                if m:
                    r['opened'][m.group(1)] += 1
                    continue
                m = L_NOIMPL.match(line)
                if m:
                    r['stub_services'].add(m.group(1))
                    continue
                m = L_STUB.match(line)
                if m:
                    r['cmds'][(m.group(1), int(m.group(2)), 'service has no implementation')] += 1
                    continue
                m = L_IFACE.match(line)
                if m:
                    how = {'unhandled, ok': 'unhandled', 'ok': 'answered ok', 'not implemented': 'not implemented'}[m.group(4)]
                    r['cmds'][(m.group(2), int(m.group(3)), how)] += 1
                    continue
                m = L_TAG.match(line)
                if m:
                    r['cmds'][(m.group(1), int(m.group(2)), 'answered ok')] += 1
    return r


def report(exefs, runtime, logs):
    refs = binary_refs(exefs) if exefs else {}
    handlers = runtime_handlers(runtime)
    lg = scan_logs(logs)
    lines, gaps = [], collections.defaultdict(list)
    all_refs = collections.Counter()
    for mod, d in refs.items():
        all_refs.update(d)
        lines.append(f'binary {mod}: {len(d)} service names reached from code')
    lines.append(f'runtime registers {len([n for n in handlers if not n.startswith("I")])} services '
                 f'and {len([n for n in handlers if n.startswith("I")])} interfaces; {lg["logs"]} logs read')
    lines.append('')
    lines.append(f'{"service":10} {"code refs":>9} {"opened in logs":>14}  runtime')
    for name in sorted(set(all_refs) | set(lg['opened'])):
        h = handlers.get(name)
        state = ('none (logging stub)' if not h else 'implemented, no command switch' if not h['cases']
                 else f'implemented, {len(h["cases"])} commands')
        lines.append(f'{name:10} {all_refs.get(name, 0):>9} {lg["opened"].get(name, 0):>14}  {state}')
        if not h and lg['opened'].get(name):
            gaps['A'].append(f'{name}: opened {lg["opened"][name]}x in the logs, the runtime only logs it (a stub that answers ok)')
        elif not h and name in all_refs:
            gaps['B'].append(f'{name}: the binaries reach it ({all_refs[name]} references), never opened in these logs, no runtime service')
        elif h and not h['cases'] and lg['opened'].get(name):
            gaps['D'].append(f'{name}: opened in the logs, its handler has no command switch (every command gets an empty reply)')
    for (iface, cmd, how), n in sorted(lg['cmds'].items(), key=lambda kv: -kv[1]):
        h = handlers.get(iface)
        if iface in lg['opened'] and not h:
            now = 'still no service'
        elif h is None:
            now = 'interface not in the runtime now'
        else:
            now = 'now has a case' if cmd in h['cases'] else 'still no case'
        gaps['C'].append(f'{iface} cmd {cmd}: {how} x{n}; runtime source: {now}')
    titles = {'A': 'services the game opened that have no runtime implementation',
              'B': 'services the binaries can reach, not opened in these logs, no implementation',
              'C': 'commands the logs show answered without doing the work',
              'D': 'implemented services whose handler has no command switch'}
    for k in 'ABCD':
        lines.append('')
        lines.append(f'[{k}] {titles[k]} ({len(gaps[k])})')
        lines += ['  ' + g for g in gaps[k]]
    return '\n'.join(lines) + '\n', gaps


def self_test():
    import io
    with tempfile.TemporaryDirectory() as tmp:
        rt = os.path.join(tmp, 'rt')
        os.makedirs(rt)
        with open(os.path.join(rt, 'a.c'), 'w') as f:
            f.write('''
static void sub(Service *s, IpcReq *req, IpcResp *resp) {
    switch (req->cmd) { case 1: break; case 7: break; default: LOG("[am] %s cmd %u (unhandled, ok)\\n"); }
}
SERVICE("appletOE", ao) { if (req->cmd == 0) ipc_return_object(resp, service_new("ISub", sub, NULL)); }
static void root(Service *s, IpcReq *req, IpcResp *resp) { switch (req->cmd) { case 3: break; } }
SERVICE("pctl", pctl) { root(s, req, resp); }
SERVICE("hid:sys", hs) { (void)s; resp_size(resp, 0x10); }
''')
        log = os.path.join(tmp, 'r.log')
        with open(log, 'w') as f:
            f.write("[sm] GetService appletOE\n[sm] GetService prepo:u\n[sm] GetService hid:sys\n"
                    "[ipc] no HLE implementation for service 'prepo:u', using logging stub\n"
                    "[ipc] STUB prepo:u cmd 10105 (in 8 bytes, x0 a0 b0 c0) -> ok\n"
                    "[am] ISub cmd 7 (unhandled, ok)\n[am] ISub cmd 9 (unhandled, ok)\n[ssl] cmd 5 (ok)\n")
        h = runtime_handlers(rt)
        assert h['ISub']['cases'] == {1, 7} and h['appletOE']['cases'] == {0}, h
        assert h['pctl']['cases'] == {3}, 'a handler that only calls another one'
        assert h['hid:sys']['cases'] == set()
        text, gaps = report(None, rt, [log])
        assert any('prepo:u' in g and 'logs it' in g for g in gaps['A']), gaps
        assert any('ISub cmd 7' in g and 'now has a case' in g for g in gaps['C']), gaps
        assert any('ISub cmd 9' in g and 'still no case' in g for g in gaps['C']), gaps
        assert any('hid:sys' in g for g in gaps['D']), gaps
    print('service_check self-test: ok')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exefs')
    ap.add_argument('--logs', nargs='*', default=[])
    ap.add_argument('--runtime', default=os.path.join(REPO, 'runtime'))
    ap.add_argument('--out')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    text, gaps = report(args.exefs, args.runtime, args.logs)
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(text)
    print(text)


if __name__ == '__main__':
    main()
