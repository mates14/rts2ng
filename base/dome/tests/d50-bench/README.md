# D50 dome bench

The real `rts2-dome-d50` driven against a simulated roof - no hardware, no
root, nothing touched outside `$BENCH_DIR`:

- `fordsim.py` - the Ford dome board on a pseudo-terminal: two motor outputs,
  two end switches, 6 s end to end; `bench.sh jam` stops the roof half-way.
- a private `rts2-centrald` (port 16170), `rts2-teld-dummy` as the mount `T0`
  (its `CAL_NUT` stands in for the D50 mount's `inside_dome_limit`), and
  `rts2-bopwatch` printing every device's state and BOP bits on change.

`./run.sh` runs the scenarios (about 9 minutes) and prints PASS/FAIL:
opening blocks exposures and telescope moves until the roof is open; closing
waits for a slewing mount to stop; a jammed roof is retried 3 times and then
reported; stopping an opening lifts the blocks; with the mount daemon dead the
roof does not close, says so as a critical, and closes by itself once the
mount is back; a mount moving during a close stops the roof, which closes
once the mount is at rest; a dome daemon killed mid-move stops the motor when
restarted (`bench.sh dome-restart`).

`./bench.sh start`, `./bench.sh cmd DOME open`, ... for poking by hand; logs
(`DOME.log`, `T0.log`, `fordsim.log`, `bop.log`, `centrald.log`) are in
`$BENCH_DIR` (default `/tmp/rts2-d50-bench-$USER`). Build first: the scripts
use the binaries from `base/build` (override with `BUILD=`).

The mount side of the interlock (D50 `teld`) needs raw ethernet to the REMOTES
boards and real servos, so it is not on the bench.
