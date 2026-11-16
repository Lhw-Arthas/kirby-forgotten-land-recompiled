"""Where tools/gen.py cuts each module's generated C into files, keyed by the module's build ID (16 hex digits).

File k holds the functions from offset k (from the module base) up to offset k + 1. With cuts that stay put, a change
to one function changes only its own file, so the start screen's setup recompiles just that file after an update
(runtime/gamebuild.c reuses every file whose C is unchanged). Cutting by running size instead could move every cut
after a changed function.

Made with gen.py --new-chunk-bounds (written by save() below; don't edit by hand). Make new cuts only on purpose, for
example when the files have drifted far from even sizes: every file of that module compiles again once.
"""

BOUNDS = {
}


def save(key, what, offsets):
    """Rewrite this file with `key` set to (what, offsets), keeping the other entries."""
    import os
    import re
    path = os.path.abspath(__file__)
    BOUNDS[key] = (what, tuple(offsets))
    with open(path) as f:
        text = f.read()
    head, tail = text.split('\nBOUNDS = {\n', 1)
    tail = tail[re.search(r'^}\n', tail, re.M).end():]
    out = [head, '\nBOUNDS = {\n']
    for k in sorted(BOUNDS):
        w, offs = BOUNDS[k]
        out.append(f'    {k!r}: ({w!r}, (\n')
        for i in range(0, len(offs), 8):
            out.append('        ' + ' '.join(f'{o:#x},' for o in offs[i:i + 8]) + '\n')
        out.append('    )),\n')
    out += ['}\n', tail]
    with open(path, 'w') as f:
        f.write(''.join(out))
