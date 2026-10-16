# BART roof bench [MJ 2026-10-17]

The real `rts2-dome-bart` driven against a simulated Ford board and roof, no
hardware, no root. Logs in `$BENCH_DIR` (default `/tmp/rts2-bart-bench-$USER`).
Build first; the script uses `base/build` (override with `BUILD=`).

- `fordsim.py`: the board on a pseudo-terminal, echo + state replies as the
  real one, a roof 15 s end to end. Flags: `o` the open switch never makes
  (the state at BART in 2026-10: openings end on the 20 s timeout), `c` the
  closed switch never makes. It can inject a stray byte (`bench.sh stray`) or
  an extra byte inside a reply (`bench.sh inreply`).
- `bench.sh`: start/stop, commands, centrald ON/OFF, bad weather (centrald
  stopped), `log` prints motor moves and DOME's messages.

Scenarios checked on 2026-10-17 (see `HISTORY.md`, "BART roof"):

```
./bench.sh start; ./bench.sh cmd open; ./bench.sh stray; ./bench.sh cmd close
    roof closes (classic: close drove the motor open, then on/off every second)
./bench.sh start o; ./bench.sh cmd open; ./bench.sh inreply
    one bad reply logged, opening goes on to the timeout
./bench.sh start c; ./bench.sh cmd open; ./bench.sh cmd close
    "roof did not reach the closed switch", motor stopped, 3 retries, EMERGENCY
./bench.sh start o; ./bench.sh state on; sleep 4; ./bench.sh state off
    close refused while opening, roof closes ~10 s after the open timeout
```
