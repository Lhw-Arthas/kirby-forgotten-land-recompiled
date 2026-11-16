#!/bin/bash
# The checks that need the player's own extracted game files, which CI never has. Run on a PC with:
#   make verify GAME=<folder with exefs/ and romfs/>            (or: tools/ci/verify.sh <folder> [<out folder>])
# Each check is a tool in tools/verify/ (one branch each: static, shaders, tour); a missing one prints "not yet" and the
# run goes on. Output goes to <out folder> (default <dump>/../verify-out), never into the repo: it describes the game.
# Stub: the tools' command lines are filled in as each lands, tools/verify/<name> <dump> <out> is the convention.
cd "$(dirname "$0")/../.." || exit 2
GAME=${1:?usage: tools/ci/verify.sh <folder with exefs/ and romfs/> [<out folder>]}
[ -d "$GAME/exefs" ] && [ -d "$GAME/romfs" ] || { echo "$GAME holds no exefs/ and romfs/"; exit 2; }
OUT=${2:-$GAME/../verify-out}
mkdir -p "$OUT"
PY=${PY:-python}
ran=0 failed=0 missing=""

check() {  # check <name> <tool> <command...>: the tool's presence decides
    local name=$1 tool=$2; shift 2
    if [ ! -f "$tool" ]; then echo "not yet  $name ($tool)"; missing+=" ${tool##*/}"; return; fi
    echo "::group::$name"
    if "$@"; then echo "::endgroup::"; echo "PASS     $name"; else echo "::endgroup::"; echo "FAIL     $name"; failed=$((failed + 1)); fi
    ran=$((ran + 1))
}

check "generator gate (every instruction lifts, every branch resolves)" tools/verify/gen_gate.py \
    $PY tools/verify/gen_gate.py --exefs "$GAME/exefs" --out "$OUT/gen"
check "service check (every service the game names is implemented)" tools/verify/service_check.py \
    $PY tools/verify/service_check.py "$GAME" "$OUT/services"
check "asset decode (every RomFS file reads, every movie and sound decodes)" tools/verify/asset_check.py \
    $PY tools/verify/asset_check.py "$GAME" "$OUT/assets"
check "shader sweep (every shader translates, validates and builds a pipeline)" tools/verify/shader_sweep.sh \
    bash tools/verify/shader_sweep.sh "$GAME/romfs" "$OUT/shaders"
check "stage tour (every stage loads and renders)" tools/verify/stage_tour.sh \
    bash tools/verify/stage_tour.sh "$GAME" "$OUT/tour"

echo; echo "ran $ran, failed $failed, not yet:${missing:- none}; output in $OUT"
# a check that does not exist yet has not passed: green means every one ran and passed
[ "$failed" = 0 ] && [ -z "$missing" ]
