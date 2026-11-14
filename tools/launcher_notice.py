# usage: launcher_notice.py <launcher dir> <out file>: the third-party notice of the launcher (make dist): every crate
# built into it for Windows (cargo metadata's resolved graph, normal dependencies only: no build scripts, no tests) and
# the npm packages bundled into its page, each with its licence and licence files; identical texts are written once.
import json
import os
import subprocess
import sys

launcher, out = sys.argv[1:3]
LICENSE_NAMES = ('LICENSE', 'LICENCE', 'COPYING', 'NOTICE', 'UNLICENSE')


def licence_files(d):
    return sorted(os.path.join(d, f) for f in os.listdir(d)
                  if f.upper().startswith(LICENSE_NAMES) and os.path.isfile(os.path.join(d, f)))


meta = json.loads(subprocess.check_output(
    ['cargo', 'metadata', '--format-version', '1', '--locked', '--filter-platform', 'x86_64-pc-windows-msvc',
     '--manifest-path', os.path.join(launcher, 'src-tauri', 'Cargo.toml')]))
pkgs = {p['id']: p for p in meta['packages']}
nodes = {n['id']: n for n in meta['resolve']['nodes']}
root = meta['resolve']['root']
seen, todo = set(), [root]
while todo:
    i = todo.pop()
    if i in seen:
        continue
    seen.add(i)
    todo += [d['pkg'] for d in nodes[i]['deps'] if any(k['kind'] is None for k in d['dep_kinds'])]
seen.discard(root)

entries = []  # (name, version, licence, licence file paths)
for i in seen:
    p = pkgs[i]
    entries.append((p['name'], p['version'], p.get('license') or p.get('license_file') or 'unknown',
                    licence_files(os.path.dirname(p['manifest_path']))))
for pkg in ('@tauri-apps/api', '@tauri-apps/plugin-dialog'):
    d = os.path.join(launcher, 'node_modules', *pkg.split('/'))
    pj = json.load(open(os.path.join(d, 'package.json'), encoding='utf-8'))
    entries.append((pkg + ' (npm)', pj['version'], pj.get('license', 'unknown'), licence_files(d)))
entries.sort(key=lambda e: (e[0].lower(), e[1]))
if not any(e[0] == 'tauri' for e in entries):
    sys.exit('launcher_notice.py: tauri is not in the dependency graph')

texts = {}  # licence text -> the packages that carry it
for name, ver, _, files in entries:
    for f in files:
        texts.setdefault(open(f, encoding='utf-8', errors='replace').read().strip(), []).append(f'{name} {ver}')

with open(out, 'w', encoding='utf-8', newline='\r\n') as o:
    o.write('Third-party software in ForgottenLandLauncher.exe\n\n'
            f'{len(entries)} packages: the Rust crates compiled into it and the JavaScript bundled into its page.\n'
            'Licence (SPDX expression) of each, then every licence text the packages ship, each written once.\n\n')
    for name, ver, lic, files in entries:
        o.write(f'{name} {ver}: {lic}{"" if files else "  (no licence file in the package)"}\n')
    for text, users in texts.items():
        o.write('\n' + '=' * 100 + '\nUsed by: ' + ', '.join(users) + '\n' + '-' * 100 + '\n' + text + '\n')
print(f'{out}: {len(entries)} packages, {len(texts)} licence texts, '
      f'{sum(1 for e in entries if not e[3])} without a licence file')
