#!/bin/bash
# test_setup_cli.sh <runtime exe>: the launchers' commands (docs/launcher-runtime-contract.md) on synthetic game dumps
# that stop before the build: what they print on stdout and their exit codes. Run it with `make test-setup-cli` in Git Bash.
set -u
EXE=${1:?usage: test_setup_cli.sh <runtime exe>}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export LARECOMP_DATA_ROOT=$(cygpath -w "$T/data")
mkdir "$T/data"

npdm() {  # main.npdm with the game's program id, or $1 when given
    { printf 'META'; head -c 108 /dev/zero; printf '\x80\0\0\0'; head -c 12 /dev/zero
      printf 'ACI0'; head -c 12 /dev/zero; printf "${1:-\x00\xe0\x5a\x0c\x30\x4d\x00\x01}"; } > "$2/main.npdm"
}
exefs() { mkdir -p "$1"; echo code > "$1/main"; npdm "${2:-}" "$1"; }
romfs() { mkdir -p "$1/msg" "$1/gfx"; }

fail=0
expect() {  # expect <what> <wanted stdout> <wanted exit> <command...>
    local what=$1 want=$2 code=$3 got rc
    shift 3
    "$EXE" "$@" > "$T/out" 2>/dev/null; rc=$?
    got=$(tr -d '\r' < "$T/out")
    if [ "$got" = "$want" ] && [ "$rc" = "$code" ]; then echo "pass: $what"
    else echo "FAIL: $what: wanted '$want' exit $code, got '$got' exit $rc"; fail=1; fi
}
w() { cygpath -w "$1"; }

d=$T/update; exefs $d/exefs '\x00\xe8\x5a\x0c\x30\x4d\x00\x01'; romfs $d/romfs
expect "setup of an update dump" $'phase looking 0.000\nfailed update' 1 --setup "$(w $d)"
expect "prepare of an update dump" "progress 0.000 Looking for the game files" 15 --prepare "$(w $d)"
d=$T/package; mkdir $d; : > $d/Kirby.nsp
expect "setup of a package" $'phase looking 0.000\nfailed package' 1 --setup "$(w $d)"
expect "prepare of a package" "progress 0.000 Looking for the game files" 9 --prepare "$(w $d)"
d=$T/empty; mkdir $d
expect "prepare of an empty folder" "progress 0.000 Looking for the game files" 8 --prepare "$(w $d)"
d=$T/other; exefs $d/exefs '\x01\x02\x03\x04\x05\x06\x07\x08'; romfs $d/romfs
expect "prepare of another game" "progress 0.000 Looking for the game files" 10 --prepare "$(w $d)"
d=$T/notnso; exefs $d/exefs; romfs $d/romfs  # (the game's id, but a main that is no program of a version this runtime has)
expect "setup of a main of no known version" $'phase looking 0.000\nfailed version' 1 --setup "$(w $d)"
expect "status of nothing set up" none 0 --status
expect "rebuild with no game files" "failed not_found" 1 --rebuild-code
d=$T/data/extracted; exefs $d/exefs; romfs $d/romfs  # (this install's own copy: not a source for a setup)
expect "check of the install's own copy" "failed not_found" 1 --check "$(w $d)"
expect "setup of the install's own copy" $'phase looking 0.000\nfailed not_found' 1 --setup "$(w $d)"
expect "status with game files and no game code" build 0 --status
exit $fail
