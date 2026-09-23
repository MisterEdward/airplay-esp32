#!/bin/zsh
# Poll the Mac's TCP send queue towards the speaker's AirPlay data socket.
# usage: sendq.sh ITERATIONS [SPEAKER_IP]   (~0.1 s per iteration)
# A non-zero Send-Q while playing means the speaker is back-pressuring the
# sender: that backlog is what a seek has to wait for.  Healthy: 0.
N=${1:-100}; IP=${2:-192.168.68.104}
for i in $(seq 1 $N); do
  printf "%s " "$(python3 -c 'import time;print(f"{time.time():.2f}")')"
  netstat -anp tcp | grep "$IP" | grep -v "\.7000 " | awk '{print $3}' | tr '\n' ' '
  echo
  sleep 0.08
done
