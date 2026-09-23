#!/bin/zsh
# Drive the Mac's Music app as the AirPlay sender (speaker + the Mac's own
# output = a multiroom group), seek four times, poll the Mac's send queue,
# then download the speaker's journal.
#
# usage: seektest.sh TAG [rapid]
#   rapid: each seek is followed 3 s later by a second one, which lands in
#          the burst the sender pushes after the first.
# Output: $OUT/TAG.log (journal) and $OUT/sendq-TAG.txt.  OUT defaults to
# /tmp/fable-tests.  SPEAKER / MAC_OUTPUT override the AirPlay device names.
TAG=${1:?tag}; MODE=$2
IP=${IP:-192.168.68.104}
SPEAKER=${SPEAKER:-Bedroom Speakers}
MAC_OUTPUT=${MAC_OUTPUT:-edward’s MacBook Pro}
OUT=${OUT:-/tmp/fable-tests}; mkdir -p $OUT
HERE=${0:A:h}

osascript -e "tell application \"Music\"
set selected of AirPlay device \"$SPEAKER\" to true
set selected of AirPlay device \"$MAC_OUTPUT\" to true
play
end tell"
sleep 20
$HERE/sendq.sh 330 $IP > $OUT/sendq-$TAG.txt &
for P in 120 60 150 45; do
  python3 -c 'import time;print(f"SEEK {time.time():.2f}")'
  osascript -e "tell application \"Music\" to set player position to $P"
  if [ "$MODE" = rapid ]; then
    sleep 3
    osascript -e "tell application \"Music\" to set player position to $((P+20))"
    sleep 7
  else
    sleep 11
  fi
done
wait
osascript -e 'tell application "Music" to pause'
curl -s -m 20 http://$IP/api/logs/download | sed 's/\x1b\[[0-9;]*m//g' > $OUT/$TAG.log
echo "journal: $OUT/$TAG.log"
