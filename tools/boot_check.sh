#!/bin/bash
# boot_check.sh [seconds] [name]: start the game from the work folder without the launcher, stop it after <seconds>
# (default 60), and print how far it got: the runtime's milestones and its FATAL line, if any. The full log is
# $WORK/boot-<name>.log. Run from the repository root in Git Bash; WORK defaults to ../work/kfl.
set -u
WORK=${WORK:-../work/kfl}
SECS=${1:-60}
LOG="boot-${2:-last}.log"
export PATH="/c/msys64/ucrt64/bin:$PATH"
cd "$WORK" || exit 2
LARECOMP_GAME_LIB=build/lasgame.dll timeout "$SECS" ./build/ForgottenLandRecomp.exe extracted --play > "$LOG" 2>&1
code=$?
echo "exit $code ($([ $code = 124 ] && echo 'still running at the time limit' || echo 'stopped by itself')), $(wc -l < "$LOG") log lines"
grep -cE '^\[kernel\] CreateThread' "$LOG" | sed 's/^/threads created: /'
grep -E '^init |^\[nvn\]|^\[vk\]|^\[gpu\]|first frame|^\[audio\]' "$LOG" | cut -c1-160 | head -20
grep -m1 -A3 '^FATAL' "$LOG" | cut -c1-200
