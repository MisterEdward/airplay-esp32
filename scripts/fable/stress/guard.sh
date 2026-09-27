#!/bin/zsh
# Soak guard: recover the SENDER the way a user would, and log it.
# PHANTOM: Music "playing", speaker has no session for 45 s -> reselect speaker.
# FROZEN:  Music "playing", position unchanged for 45 s -> relaunch Music.
# Also logs one ping per 2 s.  usage: guard.sh RUN_ID
RUN=$1; D=${FABLE_RUNS:-$HOME/fable-runs}/$RUN; LOG=$D/guard.log; PING=$D/ping.txt
IP=192.168.68.104; MAC='edward’s MacBook Pro'; SPK='Bedroom Speakers'
ph=0; fr=0; lastpos=""
reselect() {
  osascript -e "tell application \"Music\" to set selected of AirPlay device \"$MAC\" to true" \
            -e "tell application \"Music\" to set selected of AirPlay device \"$SPK\" to false" 2>/dev/null
  sleep 3
  osascript -e "tell application \"Music\" to set selected of AirPlay device \"$SPK\" to true" \
            -e "tell application \"Music\" to set selected of AirPlay device \"$MAC\" to false" 2>/dev/null
}
( while pgrep -f "bin/run.py $RUN " >/dev/null; do r=$(ping -c 1 -t 2 $IP 2>/dev/null | grep -oE 'time=[0-9.]+' | cut -d= -f2); echo "$(date +%s) ${r:-lost}"; sleep 2; done ) > $PING &
sleep 40
while pgrep -f "bin/run.py $RUN " >/dev/null; do
  st=$(osascript -e 'tell application "Music" to get player state' 2>/dev/null)
  pos=$(osascript -e 'tell application "Music" to get player position' 2>/dev/null | cut -d. -f1)
  c=$(curl -s -m4 http://$IP/api/status | python3 -c 'import json,sys;print(json.load(sys.stdin)["airplay"].get("connected"))' 2>/dev/null)
  if [ "$st" = "playing" ] && [ "$c" = "False" ]; then ph=$((ph+1)); else ph=0; fi
  if [ "$st" = "playing" ] && [ -n "$pos" ] && [ "$pos" = "$lastpos" ]; then fr=$((fr+1)); else fr=0; fi
  lastpos=$pos
  if [ $ph -ge 3 ]; then echo "$(date +%s000) PHANTOM: Music playing, speaker has no session -> reselect" >> $LOG; reselect; ph=0; fi
  if [ $fr -ge 3 ]; then echo "$(date +%s000) FROZEN: Music position stuck at $pos -> relaunch Music" >> $LOG
    osascript -e 'tell application "Music" to quit' 2>/dev/null; sleep 5; open -a Music; sleep 8; reselect
    osascript -e 'tell application "Music" to play' 2>/dev/null; sleep 10
    p2=$(osascript -e 'tell application "Music" to get player position' 2>/dev/null | cut -d. -f1)
    if [ "$p2" = "0" ]; then echo "$(date +%s000) still stuck at 0 -> next track" >> $LOG
      osascript -e 'tell application "Music" to next track' -e 'tell application "Music" to play' 2>/dev/null; fi
    fr=0; fi
  sleep 15
done
echo "$(date +%s000) guard ended" >> $LOG
