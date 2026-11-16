#!/bin/bash
# What changed in LAS Recomp since the commit this repo's copy came from (docs/UPSTREAM.md), for the files this repo
# takes (tools/upstream/paths.txt minus removed.txt). Prints; merges nothing.
#   tools/upstream/sync.sh <LAS checkout> [rev, default HEAD]   upstream's changes, each with what to do here
#   tools/upstream/sync.sh <LAS checkout> --local               the taken files this repo changed after copying
# Each line: <upstream status> <path>  <what>. After taking the changes, set docs/UPSTREAM.md's Commit to the new rev.
set -euo pipefail
las=${1:?usage: tools/upstream/sync.sh <LAS checkout> [rev | --local]}
cd "$(dirname "$0")/../.."
up=tools/upstream
base=$(sed -n 's/^Commit: \([0-9a-f]\{40\}\).*/\1/p' docs/UPSTREAM.md)
[ -n "$base" ] || { echo "no 'Commit: <sha>' line in docs/UPSTREAM.md" >&2; exit 1; }
git -C "$las" cat-file -e "$base^{commit}" 2>/dev/null || { echo "$las has no commit $base: fetch it first" >&2; exit 1; }
mapfile -t roots < <(grep -v -e '^#' -e '^$' $up/paths.txt)

# here: how this repo's copy of $1 stands against LAS at $base
here() {
    if [ ! -e "$1" ]; then echo missing
    elif ! git -C "$las" cat-file -e "$base:$1" 2>/dev/null; then echo ours
    elif cmp -s <(git -C "$las" cat-file blob "$base:$1" 2>/dev/null | tr -d '\r') <(tr -d '\r' < "$1"); then echo same
    else echo changed; fi
}

if [ "${2:-}" = --local ]; then
    git -C "$las" ls-tree -r --name-only "$base" -- "${roots[@]}" | awk -f $up/filter.awk $up/removed.txt - |
        while read -r f; do h=$(here "$f"); [ "$h" = same ] || printf '%-8s %s\n' "$h" "$f"; done
    exit 0
fi

rev=$(git -C "$las" rev-parse "${2:-HEAD}")
echo "LAS $base..$rev"
git -C "$las" diff --no-renames --name-status "$base" "$rev" -- "${roots[@]}" | awk -f $up/filter.awk $up/removed.txt - |
    while IFS=$'\t' read -r st f; do
        case $st$(here "$f") in
            Asame|Amissing) what='new upstream: engine (copy it) or a feature of their game (add it to removed.txt)' ;;
            A*) what='new upstream, and this repo has its own file there: merge by hand' ;;
            Msame) what='unchanged here: copy theirs' ;;
            Dsame) what='unchanged here: delete it here too' ;;
            Mmissing|Dmissing) what='deleted here: decide whether it comes back' ;;
            M*|D*) what='changed here too: merge by hand' ;;
            *) what='?' ;;
        esac
        printf '%s %s  %s\n' "$st" "$f" "$what"
    done
