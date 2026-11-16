#!/bin/sh
# Fails if the git tree holds anything of the game: files with a Switch game extension, romfs / exefs / generated
# folders, the game library, keys, or a file over 1 MiB outside third_party/. The repo never holds game data and CI
# never sees any, so a hit is a mistake to fix before it is pushed. Checks what git tracks (what a push publishes).
cd "$(dirname "$0")/../.." || exit 2
fail=0
hits=$(git ls-files | grep -E -i \
  '(^|/)(romfs|exefs|generated[^/]*|extracted[^/]*)/|(^|/)(lasgame\.[^/]*|romfs\.bin|[^/]*\.keys)$|\.(nso|nca|nsp|xci|bfsha|bfres|zs|cmp)$')
if [ -n "$hits" ]; then
  echo "game data in the tree:"; echo "$hits" | sed 's/^/  /'; fail=1
fi
big=$(git ls-files -z | grep -z -v '^third_party/' | xargs -0 wc -c | grep -v ' total$' | awk '$1 > 1048576')
if [ -n "$big" ]; then
  echo "files over 1 MiB outside third_party/:"; echo "$big" | sed 's/^/  /'; fail=1
fi
[ "$fail" = 0 ] && echo "no game data in the tree ($(git ls-files | wc -l) files checked)"
exit $fail
