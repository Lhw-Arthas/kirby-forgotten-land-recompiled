#!/bin/bash
# Copies LAS Recomp's files (tools/upstream/paths.txt minus removed.txt) at <rev> of a LAS checkout over this repo and
# stages them:
#   tools/upstream/import.sh <LAS checkout> [rev, default HEAD]
# It overwrites local changes to those files: it made the first import (docs/UPSTREAM.md). A later sync goes file by
# file with tools/upstream/sync.sh instead.
set -euo pipefail
las=${1:?usage: tools/upstream/import.sh <LAS checkout> [rev]}
rev=$(git -C "$las" rev-parse "${2:-HEAD}")
cd "$(dirname "$0")/../.."
up=tools/upstream
mapfile -t roots < <(grep -v -e '^#' -e '^$' $up/paths.txt)
n=0
while IFS=$'\t' read -r meta f; do
    mkdir -p "$(dirname "$f")"
    git -C "$las" cat-file blob "$rev:$f" > "$f"
    git add -- "$f"
    [[ $meta == 100755* ]] && git update-index --chmod=+x -- "$f"
    n=$((n + 1))
done < <(git -C "$las" ls-tree -r "$rev" -- "${roots[@]}" | awk -f $up/filter.awk $up/removed.txt -)
echo "copied $n files from LAS $rev"
