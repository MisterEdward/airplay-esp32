#!/bin/zsh
# Reproduce a dropout on purpose: Music on the Mac plays to the speaker (in a
# group with the Mac's own output), then the speaker's reader is told to stop
# reading the data socket for a while (/api/debug/stall), once per MS given.
#
# usage: stalltest.sh TAG MS [MS ...]      e.g. stalltest.sh st1 3000 15000
# Output in $FABLE_RUNS/TAG (default ~/fable-runs/TAG): watch.py's ping.txt,
# status.jsonl (1 s) and journal.log, plus sendq.txt (the Mac's TCP Send-Q
# towards the speaker) and marks.txt (epoch of every action).
# SPEAKER / MAC_OUTPUT override the AirPlay device names; SETTLE is the time
# left after each stall for the speaker to recover (default 30 s).
TAG=${1:?tag}; shift
[ $# -ge 1 ] || { echo "usage: $0 TAG MS [MS ...]"; exit 1; }
IP=${IP:-192.168.68.104}
SPEAKER=${SPEAKER:-Bedroom Speakers}
MAC_OUTPUT=${MAC_OUTPUT:-Edward’s MacBook Pro}
SETTLE=${SETTLE:-30}
HERE=${0:A:h}
D=${FABLE_RUNS:-$HOME/fable-runs}/$TAG; mkdir -p $D
mark() { echo "$(python3 -c 'import time;print(f"{time.time():.2f}")') $*" | tee -a $D/marks.txt; }

python3 $HERE/watch.py --status-s 1 $TAG > $D/watch.out 2>&1 &
WATCH=$!
trap 'kill $WATCH 2>/dev/null' EXIT

total=30
for ms in "$@"; do total=$((total + ms / 1000 + SETTLE)); done
$HERE/sendq.sh $((total * 9)) $IP > $D/sendq.txt &
SENDQ=$!

mark "play: $SPEAKER + $MAC_OUTPUT"
osascript -e "tell application \"Music\"
set selected of AirPlay device \"$SPEAKER\" to true
set selected of AirPlay device \"$MAC_OUTPUT\" to true
play (track 1 of playlist \"Replay All Time\")
end tell" || { mark "Music refused"; exit 1; }
sleep 30
for ms in "$@"; do
  mark "stall $ms ms: $(curl -s -m 4 "http://$IP/api/debug/stall?ms=$ms")"
  sleep $((ms / 1000 + SETTLE))
  mark "after stall $ms: Music $(osascript -e 'tell application "Music" to get {player state, player position}')"
done
osascript -e 'tell application "Music" to pause'
mark "pause"
sleep 6
kill $SENDQ 2>/dev/null
echo "run: $D"
