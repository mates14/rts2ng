#!/bin/bash
# Run the D50 dome bench scenarios (about 9 minutes) and report PASS/FAIL.
# Same environment as bench.sh. Exit status is the number of failed scenarios.

HERE=$(dirname "$(readlink -f "$0")")
B="$HERE/bench.sh"
export BENCH_DIR=${BENCH_DIR:-${TMPDIR:-/tmp}/rts2-d50-bench-$USER}
# slow enough roof for a stop to arrive while it still moves (rts2-sendcmd needs 1-2 s to connect)
export FORDSIM_TRAVEL=${FORDSIM_TRAVEL:-12}
failed=0

# mark: remember where a log ends, so checks only see what a scenario wrote
mark () { for f in DOME bop fordsim T0; do eval "M_$f=$(wc -l < "$BENCH_DIR/$f.log")"; done; }
new () { eval "tail -n +\$((M_$1 + 1)) \"$BENCH_DIR/$1.log\""; }

# wait_for LOG TEXT SECONDS - TEXT appears in the new part of LOG
wait_for ()
{
	for i in $(seq $(($3 * 10))); do
		new "$1" | grep -qF "$2" && return 0
		sleep 0.1
	done
	return 1
}

result ()
{
	if [ "$2" = 0 ]; then echo "PASS  $1"; else echo "FAIL  $1 - $3"; failed=$((failed + 1)); fi
}

# The dome's standby ()/off () (centrald day/evening/off states) reset ignore_time,
# after which an open is refused as bad weather - so set it again before each open.
open_roof () { "$B" cmd DOME X ignore_time = $(( $(date +%s) + 36000 )) > /dev/null; "$B" cmd DOME open > /dev/null; }

lst () { python3 -c "
import time; d = time.time () / 86400 + 2440587.5 - 2451545.0
print (((280.46061837 + 360.98564736629 * d) + 14.7838 - $1) % 360)"; }

"$B" start || exit 100
trap '"$B" stop > /dev/null' EXIT

# 1. opening blocks exposures and telescope moves, and lifts both when open
mark
open_roof
if ! wait_for DOME "dome opened" 20; then
	result "open: roof opens" 1 "no 'dome opened'"
elif ! new bop | grep -q "^[0-9.]* T0 .*BLOCK_EXPOSURE BLOCK_TEL_MOVE"; then
	result "open: T0 sees BLOCK_EXPOSURE and BLOCK_TEL_MOVE while opening" 1 "never seen on T0"
elif new bop | grep "^[0-9.]* T0 " | tail -1 | grep -q BLOCK; then
	result "open: blocks lifted when open" 1 "T0 still blocked"
else
	result "open: T0 blocked while opening, unblocked when open" 0
fi

# 2. close with the mount at rest and inside
mark
"$B" cmd T0 X CAL_NUT = 1 > /dev/null
"$B" cmd DOME close > /dev/null
wait_for DOME "dome closed" 20
result "close: mount at rest and inside" $? "no 'dome closed'"

# 3. close requested while the mount slews, inside flag already set: wait for it to stop
open_roof
wait_for DOME "dome opened" 20
"$B" cmd T0 move "$(lst 0)" 49.9 > /dev/null	# first dummy move lands at once
sleep 1
mark
"$B" cmd T0 move "$(lst 30)" 49.9 > /dev/null &	# 30 deg at 2 deg/s
sleep 2
"$B" cmd DOME close > /dev/null
# the slew (~15 s) and the close (12 s) take ~33 s: 40 s failed once (2026-10-17)
if ! wait_for DOME "dome closed" 60; then
	result "close during slew" 1 "no 'dome closed'"
else
	# TEL_MOVING is bit 0x1 of the state, i.e. an odd last hex digit
	last_moving=$(new bop | awk '$2 == "T0" && $4 ~ /[13579bdf]$/ { t = $1 } END { print t }')
	motor=$(new fordsim | awk '/motors B=0x02/ { print $1; exit }')
	if ! new DOME | grep -q "mount is moving, waiting"; then
		result "close during slew: dome waits for the mount" 1 "no 'mount is moving, waiting'"
	elif [ -z "$last_moving" ] || [ -z "$motor" ] || ! python3 -c "import sys; sys.exit (0 if $motor > $last_moving else 1)"; then
		result "close during slew: roof motor only after the mount stopped" 1 "motor at $motor, mount moving until $last_moving"
	else
		result "close during slew: roof motor only after the mount stopped" 0
	fi
fi
wait

# 4. a jammed roof is retried 3 times, then reported
open_roof
wait_for DOME "dome opened" 20
mark
"$B" jam
"$B" cmd DOME close > /dev/null
if ! wait_for DOME "giving up" 180; then
	result "jammed roof: retries, then gives up" 1 "no 'giving up'"
else
	n=$(new DOME | grep -c "retrying the close")
	on=$(new fordsim | grep -c "motors B=0x02")
	[ "$n" = 3 ] && [ "$on" = 4 ]
	result "jammed roof: 3 retries, 4 attempts, then gives up" $? "$n retries, $on motor starts"
fi

# 5. after the jam the roof closes again; stopping an opening lifts both blocks
"$B" unjam
mark
"$B" cmd DOME close > /dev/null
wait_for DOME "dome closed" 20
result "unjammed roof closes" $? "no 'dome closed'"
mark
open_roof
"$B" cmd DOME stop > /dev/null
sleep 2
# stopped = the opening motor went off before the open end switch was reached
if ! new fordsim | grep -q "motors B=0x00" || new fordsim | grep -q "switches A=0x02"; then
	result "stop while opening lifts the blocks" 1 "the roof was not stopped while opening"
elif new bop | grep "^[0-9.]* T0 " | tail -1 | grep -q BLOCK || new bop | grep "^[0-9.]* DOME " | tail -1 | grep -q BLOCK; then
	result "stop while opening lifts the blocks" 1 "still blocked"
else
	result "stop while opening lifts the blocks" 0
fi

# 6. mount daemon dead: the roof must not close blind, must say so loudly, and
#    must close by itself once the mount is back (retry every 30 s)
open_roof
wait_for DOME "dome opened" 20
"$B" mount-down
sleep 2
mark
"$B" cmd DOME close > /dev/null
if ! wait_for DOME "cannot start closing" 10; then
	result "mount down: close refused, reported" 1 "no 'cannot start closing'"
elif new DOME | grep "cannot start closing" | grep -qv " 1048592 "; then
	result "mount down: close refused, reported as critical" 1 "not logged as REPORTIT|CRITICAL"
elif new fordsim | grep -q "motors B=0x02"; then
	result "mount down: roof must not move" 1 "closing motor started"
else
	result "mount down: close refused, reported as critical" 0
fi
mark
"$B" mount-up
wait_for DOME "dome closed" 60
result "mount back: roof closes by itself" $? "no 'dome closed' within 60 s"

# 7. the mount starts to move while the roof closes: a close request then (in bad
#    weather one comes at every idle) stops the roof until the mount is at rest
open_roof
wait_for DOME "dome opened" 20
"$B" cmd T0 move "$(lst 0)" 49.9 > /dev/null	# the dummy restarted in 6: first move lands at once
sleep 1
"$B" cmd T0 X CAL_NUT = 1 > /dev/null
mark
"$B" cmd DOME close > /dev/null
sleep 2
"$B" cmd T0 move "$(lst 60)" 49.9 > /dev/null &	# 60 deg away
sleep 2
"$B" cmd DOME close > /dev/null
if ! wait_for DOME "stopping the closing roof" 10; then
	result "mount moving during a close stops the roof" 1 "no 'stopping the closing roof'"
elif ! new fordsim | grep -q "motors B=0x00"; then
	result "mount moving during a close stops the roof" 1 "the closing motor was not switched off"
elif ! wait_for DOME "dome closed" 80; then
	result "mount moving during a close: roof closes after the mount stops" 1 "no 'dome closed'"
else
	result "mount moving during a close stops the roof, closes after" 0
fi
wait

# 8. the dome daemon restarted while the roof moves: the motor is stopped at start
#    (classic left it running without a timeout)
open_roof
sleep 3
mark
"$B" dome-restart
if ! wait_for DOME "a roof motor was running at start" 15; then
	result "dome restart mid-move stops the motor" 1 "no 'a roof motor was running at start'"
elif ! new fordsim | grep -q "motors B=0x00"; then
	result "dome restart mid-move stops the motor" 1 "motor still on"
else
	result "dome restart mid-move stops the motor" 0
fi
"$B" cmd DOME close > /dev/null
wait_for DOME "dome closed" 30
result "after the restart the roof closes" $? "no 'dome closed'"

echo "$failed failed, logs in $BENCH_DIR"
exit $failed
