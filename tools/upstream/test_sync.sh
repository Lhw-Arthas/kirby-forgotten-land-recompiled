#!/bin/bash
# tools/upstream/sync.sh on a synthetic upstream and a synthetic copy of it (scratch folders only).
set -euo pipefail
src=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
g() { git -C "$t/las" -c core.autocrlf=false -c user.name=t -c user.email=t@t "$@"; }

mkdir -p "$t/las/runtime" "$t/las/docs"
for f in same changed gone_here deleted_up deleted_up_changed; do echo "$f v1" > "$t/las/runtime/$f.c"; done
echo feature > "$t/las/runtime/feature.c"
echo readme > "$t/las/docs/readme.md"
git init -q "$t/las" && g add -A && g commit -qm base
base=$(g rev-parse HEAD)

mkdir -p "$t/ours/tools/upstream" "$t/ours/docs" "$t/ours/runtime"
cp "$src/sync.sh" "$src/filter.awk" "$t/ours/tools/upstream/"
printf 'runtime/\n' > "$t/ours/tools/upstream/paths.txt"
printf '# their game\nruntime/feature.c\n' > "$t/ours/tools/upstream/removed.txt"
echo "Commit: $base" > "$t/ours/docs/UPSTREAM.md"
cp "$t/las/runtime/"{same,changed,deleted_up,deleted_up_changed}.c "$t/ours/runtime/"
printf 'changed v1\r\nours\n' > "$t/ours/runtime/changed.c"
printf 'deleted_up_changed v1\nours\n' > "$t/ours/runtime/deleted_up_changed.c"
printf 'same v1\r\n' > "$t/ours/runtime/same.c"  # (a CRLF checkout of the same text)
echo ours > "$t/ours/runtime/ours.c"

for f in same changed gone_here feature; do echo "$f v2" >> "$t/las/runtime/$f.c"; done
g rm -q runtime/deleted_up.c runtime/deleted_up_changed.c
echo new > "$t/las/runtime/new.c"
echo ours-upstream > "$t/las/runtime/ours.c"
echo readme2 > "$t/las/docs/readme.md"
g add -A && g commit -qm next

out=$("$t/ours/tools/upstream/sync.sh" "$t/las")
expect="LAS $base..$(g rev-parse HEAD)
M runtime/changed.c  changed here too: merge by hand
D runtime/deleted_up.c  unchanged here: delete it here too
D runtime/deleted_up_changed.c  changed here too: merge by hand
M runtime/gone_here.c  deleted here: decide whether it comes back
A runtime/new.c  new upstream: engine (copy it) or a feature of their game (add it to removed.txt)
A runtime/ours.c  new upstream, and this repo has its own file there: merge by hand
M runtime/same.c  unchanged here: copy theirs"
[ "$out" = "$expect" ] || { echo "FAIL: sync.sh printed"; echo "$out"; exit 1; }

local_out=$("$t/ours/tools/upstream/sync.sh" "$t/las" --local)
expect_local="changed  runtime/changed.c
changed  runtime/deleted_up_changed.c
missing  runtime/gone_here.c"
[ "$local_out" = "$expect_local" ] || { echo "FAIL: sync.sh --local printed"; echo "$local_out"; exit 1; }
echo "PASS: sync.sh reports upstream changes against local ones; removed.txt and untaken paths stay out"
