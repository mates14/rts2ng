#!/bin/bash
# BART roof bench: the real rts2-dome-bart against fordsim.py. See README.md.
#
#   bench.sh start [flags]     fordsim (flags: o, c), centrald, DOME
#   bench.sh cmd <command>     send a command to DOME (open, close, stop ...)
#   bench.sh state on|off      switch centrald ON / OFF
#   bench.sh stray|inreply     inject a stray byte / an extra byte in a reply
#   bench.sh badweather        stop centrald (DOME then closes as in bad weather)
#   bench.sh manual on|off     the box's manual/PC selector
#   bench.sh hand open|close|stop  the push buttons (manual only)
#   bench.sh log               roof and DOME messages
#   bench.sh stop

HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-$HERE/../../../build}
DIR=${BENCH_DIR:-/tmp/rts2-bart-bench-$USER}
PORT=16180
mkdir -p "$DIR"

centrald_conf ()
{
	cat > "$DIR/rts2.ini" <<EOF
[observatory]
longitude = 14.7813631
latitude = 49.9093889
altitude = 530
EOF
}

start_centrald ()
{
	setsid "$BUILD/centrald/rts2-centrald" -i --config "$DIR/rts2.ini" --local-port $PORT --lock-prefix "$DIR/lock_" > "$DIR/centrald.log" 2>&1 < /dev/null &
}

case "$1" in
	start)
		centrald_conf
		setsid python3 -I "$HERE/fordsim.py" "$DIR/ford" 15 "$2" > "$DIR/fordsim.log" 2>&1 < /dev/null &
		sleep 1
		start_centrald
		sleep 2
		setsid "$BUILD/dome/bart/rts2-dome-bart" -i -d DOME -f "$DIR/ford" --server localhost:$PORT --local-port $((PORT + 1)) --lock-prefix "$DIR/lock_" --debug > "$DIR/DOME.log" 2>&1 < /dev/null &
		sleep 15
		;;
	cmd)
		shift
		timeout 25 "$BUILD/sendcmd/rts2-sendcmd" --port $PORT DOME "$@"
		;;
	state)
		python3 -I - "$PORT" "$2" <<'EOF'
import socket, sys, time
s = socket.create_connection (("127.0.0.1", int (sys.argv[1])))
for l in ("login bench bench", "password bench", sys.argv[2]):
	s.sendall ((l + "\n").encode ())
	time.sleep (0.5)
EOF
		;;
	stray)
		pkill -USR1 -f "fordsim.py $DIR/ford"
		;;
	inreply)
		pkill -USR2 -f "fordsim.py $DIR/ford"
		;;
	badweather)
		pkill -f "rts2-centrald -i --config $DIR/rts2.ini"
		;;
	manual|hand)
		echo "$1 $2" >> "$DIR/ford.cmd"
		;;
	log)
		grep -v "roof closed, travel" "$DIR/fordsim.log" | grep -E "motor|roof at|\*\*\*"
		echo "-- DOME"
		grep -avE "^$| A 0x|dropped|closeDomeWeather|domeCloseStart|EVENT_DOMECLOSE" "$DIR/DOME.log" | sed -E 's/^[0-9-]+T([0-9:.]+) \S+ \S+ /\1 /' | uniq -c -f1 | tail -${2:-30}
		;;
	stop)
		pkill -f -- "--lock-prefix $DIR/lock_"
		pkill -f "fordsim.py $DIR/ford"
		;;
	*)
		sed -n '2,14p' "$0"
		exit 1
		;;
esac
