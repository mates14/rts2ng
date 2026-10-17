#!/bin/bash
# D50 dome bench - the real rts2-dome-d50 against a simulated roof, no hardware.
#
#   bench.sh start          private centrald + dummy mount T0 + rts2-dome-d50 on a
#                           simulated Ford board (fordsim.py, on a pty) + rts2-bopwatch
#   bench.sh stop
#   bench.sh cmd DEV TEXT   send a command, e.g.  bench.sh cmd DOME open
#                                                 bench.sh cmd T0 X CAL_NUT = 1
#   bench.sh jam | unjam    roof stops half-way / moves normally
#   bench.sh mount-down | mount-up   kill / restart the mount daemon
#   bench.sh dome-restart   kill -9 the dome daemon and start it again
#
# Environment: BUILD (rts2ng base build dir), BENCH_DIR (logs, pids, pty link),
# BENCH_PORT (centrald port), FORDSIM_TRAVEL (roof travel end to end, default
# 6 s). Nothing outside BENCH_DIR is written, the system /etc/rts2/rts2.ini is
# only read (by the dome, which has no --config).
#
# The dummy mount has no inside_dome_limit; CAL_NUT (a harmless writable bool)
# stands in for it. Logs: centrald.log T0.log DOME.log fordsim.log bop.log

HERE=$(dirname "$(readlink -f "$0")")
BUILD=${BUILD:-$(readlink -f "$HERE/../../../build")}
BENCH_DIR=${BENCH_DIR:-${TMPDIR:-/tmp}/rts2-d50-bench-$USER}
PORT=${BENCH_PORT:-16170}
INSIDE_VAR=CAL_NUT

start ()
{
	stop > /dev/null
	mkdir -p "$BENCH_DIR"
	cd "$BENCH_DIR" || exit 1
	rm -f ford.tty fordsim.ctl ./*.log lock_*
	cat > rts2.ini <<-EOF
	[observatory]
	longitude = 14.7838
	latitude = 49.9094
	altitude = 528
	EOF
	python3 "$HERE/fordsim.py" "$BENCH_DIR" & echo $! > sim.pid
	for i in $(seq 50); do [ -e ford.tty ] && break; sleep 0.1; done
	local common="--lock-prefix $BENCH_DIR/lock_ --debug"
	"$BUILD/centrald/rts2-centrald" -i --config "$BENCH_DIR/rts2.ini" $common --local-port $PORT > centrald.log 2>&1 & echo $! > centrald.pid
	sleep 2
	"$BUILD/teld/dummy/rts2-teld-dummy" -i --config "$BENCH_DIR/rts2.ini" $common -d T0 --server localhost:$PORT > T0.log 2>&1 & echo $! > T0.pid
	start_dome > DOME.log
	stdbuf -oL "$BUILD/dome/tests/rts2-bopwatch" --port $PORT > bop.log 2>&1 & echo $! > bopwatch.pid
	sleep 4
	for p in sim centrald T0 DOME bopwatch; do
		kill -0 "$(cat $p.pid)" 2> /dev/null || { echo "$p did not start, see $BENCH_DIR"; exit 1; }
	done
	echo "bench running in $BENCH_DIR (centrald port $PORT)"
}

start_dome ()
{
	"$BUILD/dome/d50/rts2-dome-d50" -i --lock-prefix "$BENCH_DIR/lock_" --debug -d DOME -f "$BENCH_DIR/ford.tty" --server localhost:$PORT \
		--ignore-timeout 36000 --mount-indome-variable $INSIDE_VAR >> DOME.log 2>&1 & echo $! > DOME.pid
}

start_mount ()
{
	cd "$BENCH_DIR" || exit 1
	"$BUILD/teld/dummy/rts2-teld-dummy" -i --config "$BENCH_DIR/rts2.ini" --lock-prefix "$BENCH_DIR/lock_" --debug -d T0 --server localhost:$PORT >> T0.log 2>&1 & echo $! > T0.pid
	sleep 3
	# the dummy forgets its values when restarted - it is "inside" again
	"$BUILD/sendcmd/rts2-sendcmd" --port $PORT T0 X $INSIDE_VAR = 1 > /dev/null 2>&1
}

stop ()
{
	[ -d "$BENCH_DIR" ] || return 0
	cd "$BENCH_DIR" || return 0
	for p in bopwatch DOME T0 centrald sim; do
		[ -f $p.pid ] && kill "$(cat $p.pid)" 2> /dev/null
		rm -f $p.pid
	done
	sleep 1
	echo stopped
}

case "$1" in
	start) start ;;
	stop) stop ;;
	cmd) shift; "$BUILD/sendcmd/rts2-sendcmd" --port $PORT "$@" 2>&1 | grep -v "^$" ;;
	mount-down) kill "$(cat "$BENCH_DIR/T0.pid")" ;;
	mount-up) start_mount ;;
	dome-restart) cd "$BENCH_DIR" && kill -9 "$(cat DOME.pid)" && sleep 1 && start_dome ;;
	jam) echo stuck > "$BENCH_DIR/fordsim.ctl" ;;
	unjam) echo ok > "$BENCH_DIR/fordsim.ctl" ;;
	*) sed -n '2,19p' "$0"; exit 1 ;;
esac
