#!/bin/zsh
# Record the MacBook mic while Music plays the sweep on the speaker only.  The
# Mac's own output is deselected for the take: it would be in the measurement.
#
# usage: measure.sh TAG [VOLUME]   -> work/rec-TAG.wav
# SPEAKER / MAC_OUTPUT override the AirPlay device names.
TAG=${1:?tag}; VOL=${2:-60}
SPEAKER=${SPEAKER:-Bedroom Speakers}
MAC_OUTPUT=${MAC_OUTPUT:-Edward’s MacBook Pro}
HERE=${0:A:h}
WORK=$HERE/work
cd "$WORK" || { echo "run gen_sweep.py in $WORK first"; exit 1; }
SWEEP="$WORK/Fable sweep test.wav"
cp sweep.wav "$SWEEP" || exit 1
DUR=$(( 75 + 12 ))

# The track must point at a file that exists: Music keeps an entry whose file
# was deleted, and it cannot play it.
TRACK_ID=$(osascript -e "tell application \"Music\"
pause
set selected of AirPlay device \"$SPEAKER\" to true
set selected of AirPlay device \"$MAC_OUTPUT\" to false
set sound volume to 100
set sound volume of AirPlay device \"$SPEAKER\" to $VOL
set t to missing value
repeat with c in (every file track of library playlist 1 whose name is \"Fable sweep test\")
  if location of c is not missing value then
    set t to c
    exit repeat
  end if
end repeat
if t is missing value then set t to add POSIX file \"$SWEEP\" to library playlist 1
return persistent ID of t
end tell") || exit 1

"$HERE/venv/bin/python" "$HERE/rec.py" "rec-$TAG.wav" $DUR &
REC=$!
sleep 2
osascript -e "tell application \"Music\"
set shuffle enabled to false
set song repeat to off
play (first track of library playlist 1 whose persistent ID is \"$TRACK_ID\") with once
end tell" >/dev/null
wait $REC
osascript -e "tell application \"Music\"
pause
set selected of AirPlay device \"$MAC_OUTPUT\" to true
end tell" >/dev/null
echo "recorded $WORK/rec-$TAG.wav"
