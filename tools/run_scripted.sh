#!/bin/bash
# run_scripted.sh <name> <seconds> <shot frames> [VAR=value ...]: run the game from the work folder with a fresh scratch
# save, a hidden window and scripted input, then copy the frame captures (the game image at each of <shot frames>,
# comma-separated: frame_NNNNN.png) to $WORK/runs/<name>/ (log: runs/<name>.log).
# LARECOMP_INPUT defaults to the script that reaches the first stage on a new save and walks Kirby forward:
#   880:LEFT:8,950:A:8,1400:A:8,1800:A:8,2600:A:8,2700:PLUS:8,2800:B:8,3400:A:8,3550:A:8,3700:A:8,3850:A:8,
#   4000:A:8,4150:A:8,4300:A:8,4450:A:8,4750:STICK_UP:300
# ("Do Not Use" online, difficulty, opening movie skipped with PLUS, the cutscenes and tutorial with A, then the stick).
# LARECOMP_INPUT=guide: the same start, then to the first star blocks, walking at them and inhaling until one is caught
# and spat: the Jump, Inhale and Spit guide clips play in turn (around frames 5000-6100; give it 500 s or more). Kirby
# lands on either side of the gap depending on the frame rate (at ~12 fps a block is caught before frame 5820, at ~28 fps
# the 315-degree steps reach the left one): check the log has "GuideMovie/InhaleAndSpit_Spit".
# LARECOMP_DEBUG_FRAME=<frame> also saves every render pass of that frame as $WORK/build/pass_NNN.png (the largest is
# the g-buffer: the scene stays visible there while the final image is still white). Run from the repository root in
# Git Bash; WORK defaults to ../work/kfl. Never point WORK inside the repository.
set -u
WORK=${WORK:-../work/kfl}
NAME=$1 SECS=$2 SHOTS=$3
shift 3
export PATH="/c/msys64/ucrt64/bin:$PATH"
cd "$WORK" || exit 2
rm -rf test-save build/frames/*.png build/pass_*.png
mkdir -p test-save runs/"$NAME"
export LARECOMP_NO_LAUNCHER=1 LARECOMP_GAME_LIB=build/lasgame.dll LARECOMP_SAVE_DIR=test-save LARECOMP_NO_AUDIO=1 LARECOMP_NO_USER_INPUT=1 LARECOMP_HIDDEN=1
export LARECOMP_INPUT="880:LEFT:8,950:A:8,1400:A:8,1800:A:8,2600:A:8,2700:PLUS:8,2800:B:8,3400:A:8,3550:A:8,3700:A:8,3850:A:8,4000:A:8,4150:A:8,4300:A:8,4450:A:8,4750:STICK_UP:300"
[ -n "$SHOTS" ] && export LARECOMP_SHOT_AT="$SHOTS"
for kv in "$@"; do export "$kv"; done
[ "$LARECOMP_INPUT" = guide ] && export LARECOMP_INPUT="880:LEFT:8,950:A:8,1400:A:8,1800:A:8,2600:A:8,2700:PLUS:8,2760:PLUS:8,2800:B:8,3400:A:8,3550:A:8,3700:A:8,3850:A:8,4000:A:8,4150:A:8,4300:A:8,4450:A:8,4750:STICK_UP:700,5150:A:8,5300:A:8,5450:STICK_RIGHT:215,5560:A:10,5585:A:10,5610:A:10,5635:A:10,5700:B:10,5730:STICK_200:25,5760:B:50,5820:STICK_315:25,5850:B:50,5910:STICK_315:25,5940:B:50,6000:STICK_315:25,6030:B:50,6090:STICK_315:25,6120:B:50,6180:STICK_200:25,6210:B:50,6270:STICK_200:25,6300:B:50,6360:STICK_RIGHT:2000"
timeout "$SECS" ./build/ForgottenLandRecomp.exe extracted --play > runs/"$NAME".log 2>&1
echo "exit $?"
cp build/frames/*.png build/pass_*.png runs/"$NAME"/ 2>/dev/null
ls runs/"$NAME"
grep -a -m1 -A3 '^FATAL' runs/"$NAME".log | cut -c1-200
