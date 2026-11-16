#!/bin/bash
# stage_run.sh <seconds> <log> <game folder>: one game run of the stage tour (stage_tour.sh sets its environment and
# runs it from the work folder), killed after <seconds>. Its own script so a queue (TOUR_WRAP) holds the PC for one run.
SECS=$1 LOG=$2 GAME=$3
./build/ForgottenLandRecomp.exe "$GAME" --play > "$LOG" 2>&1 &
pid=$!
( sleep "$SECS"; kill "$pid" 2>/dev/null && echo "[tour] killed after ${SECS}s" >> "$LOG" ) &
watchdog=$!
wait "$pid"
echo "[tour] exit $?" >> "$LOG"
kill "$watchdog" 2>/dev/null
exit 0
