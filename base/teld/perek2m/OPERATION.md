# Perek 2m: rts2-teld-perek2m and rts2-dome-perek2m

These two daemons drive the 2m telescope at Ondřejov through Jan Fuchs'
telescoped (ASCOL over XML-RPC, alhena:9999). The protocol is described in
`PROTOCOL.md`. Both daemons share the `perek2m::Tcs` link in `base_teld`
(`teld/include/perek2m.h`) and the XML-RPC client in the kernel
(`xmlrpcclient.h`).

The 2m is shared with observers who use Fuky's clients. So both daemons:
- **command nothing on their own**;
- report whatever the TCS does, whoever caused it;
- have a read-only mode, which refuses every command. Run that first. It
  is the writable value `tcs_readonly`, switched at any time without a
  restart; `--readonly` only sets it at start. Switching it on stops
  sending at once: a slew already running is not stopped, but a pending
  `TGRA 1` or the next step of a dome sequence is refused, so they fail.

## rts2-teld-perek2m (telescope)

The TCS does its own pointing: it takes **J2000** and applies precession,
aberration, refraction and its pointing model. So a move sends the J2000
origin, plus the RTS2 offsets and astrometric corrections, with
`telescope_set_coordinates ()`. The driver waits until `tsra` shows the
preset, then sends `TGRA 1`. The rest of RTS2's correction chain goes
unused. A move is finished when:
- the TCS is in TRACK with `trrd` within 10" of the target, on the right
  side;
- the dome has stopped too (AUTO_STOP), when it follows the telescope.

A move fails when:
- the telescope stops off target or goes OFF;
- the TCS doesn't take the preset within 15 s;
- `move_timeout` (600 s) passes;
- telescoped stops answering.

Moves are refused while the telescope is off or not ready (oil, TEON). Power
it on with the dome's `open`.

- **Side:** `move_side` is `current` (keep the side the telescope is on),
  `east` or `west`. `TELSIDE` shows the current side.
- **Park:** only goes to the park position: `TETR 0`, `TSHA park_ha
  park_da`, `TGHA 1`. The defaults, 90 / 45, are the final position of
  Fuky's park sequence. The 2m has no mandatory idle position, and RTS2
  issues park on many state changes, so park does nothing more. Power,
  flaps and slit belong to the dome.
- **Horizon:** RTS2's hard horizon stops only RTS2's own slew. The driver
  never stops the TCS for a position it didn't command (`abortMoveTracking`).
- **Tracking:** the base class tracking is off (`hasTracking = false`). The
  TCS tracks by itself after a slew. `TCS_TRACK` shows it, and writing it
  sends `TETR`.
- **Writable values:**

  | value | command |
  |---|---|
  | `TCS_TRACK` | `TETR` |
  | `cor_aberration` | `TSCA` |
  | `cor_precession` | `TSCP` |
  | `cor_refraction` | `TSCR` |
  | `cor_model` | `TSCM` |
  | `guide_mode` | `TSGM` |
  | `TCS_MODEL` | `TSCS` |
  | `TELFOCUS` | `FOSA` + `FOGA` |

- **`ascol <command>`** sends any raw ASCOL command. The answer goes to
  `ascol_answer`.
- **Values recorded in FITS:** `OIL`, `TELSTATE`, `TCS_POS` (J2000 position
  as reported), `TCS_TAR`, `TELSIDE`, `HA_AXPOS`, `DA_AXPOS`, `TCS_TRACK`,
  `TCS_MODEL`, `TELFOCUS`. `TEL` is `trrd` precessed to the date.

## rts2-dome-perek2m (dome)

**open** (the evening):
1. Oil on (`OION 1`), wait for ON.
2. Telescope on (`TEON 1`), wait for STOP.
3. Mirror flap, tube flap and slit open, wait for OPEN.
4. Dome rotation to automatic (`DOAM`).

**close** (the morning, or bad weather):
1. All at once:
   - the slit and both flaps close (`DOSO 0`, `FTOC 0`, `FMOC 0`);
   - the dome stops and turns to `close_az` (default 0, NaN leaves it);
   - the telescope parks (`TETR 0`, `TSHA park_ha park_da`, `TGHA 1`),
     after first stopping any slew.
2. When all of that has finished: telescope off (`TEON 0`).
3. Once the telescope is OFF: oil off (`OION 0`).

`power_off` false (`--keep-power`) skips steps 2 and 3. Nothing else is ever
switched: in particular never the instruments' power.

- **Auto close:** the dome is built with **`auto_close` false**. Until it is
  set, centrald's evening/morning/standby/off states and bad weather do
  nothing; only explicit `open`/`close` commands act. Set `auto_close` when
  RTS2 runs the night.
- **Open, closed, stop:**
  - "Opened" means the slit and both flaps are open and the telescope is
    powered.
  - "Closed" means all three are closed and, with `power_off`, the
    telescope and oil are off.
  - `stop` ends a sequence and sends `DOST`.
- **Writable values:**

  | value | command |
  |---|---|
  | `slit_open` | `DOSO` |
  | `tube_flap_open` | `FTOC` |
  | `mirror_flap_open` | `FMOC` |
  | `dome_auto` | `DOAM` / `DOST` |
  | `dome_goto` | `DOSA` + `DOGA` |

  `sequence` shows the step of a running sequence. `seq_timeout` (900 s)
  fails it.
- **Values recorded in FITS:** `OIL`, `SLIT`, `TUBEFLAP`, `MIRRFLAP`,
  `DOMESTAT`, `DOME_AZ`.

The Dome base class refuses `open` for a short while after the daemon
starts (`next_open`), as it does for every dome.

## Running

    rts2-teld-perek2m -d T0 --readonly
    rts2-dome-perek2m -d DOME --readonly

then, to command the telescope, `X tcs_readonly = 0` on each (and `= 1`
to hand it back to the observers).

Both default to telescoped at 192.168.193.195:9999 (`--tcs-host`,
`--tcs-port`), with a 1 s poll and a 5 s call timeout (`--poll-interval`,
`--rpc-timeout`). The teld reads the site from `/etc/rts2/rts2.ini`.

## Testing without the telescope

    teld/perek2m/telescoped_sim.py 19999 &
    rts2-centrald -i --local-port 16618 --lock-prefix /tmp/p2m/
    rts2-teld-perek2m -d T0 -i --server localhost:16618 --lock-prefix /tmp/p2m/ \
        --tcs-host 127.0.0.1 --tcs-port 19999
    rts2-dome-perek2m -d D0 -i --server localhost:16618 --lock-prefix /tmp/p2m/ \
        --tcs-host 127.0.0.1 --tcs-port 19999

The simulator listens on 127.0.0.1 only. It models the GLST state machine
coarsely: oil start-up, power, RA/Dec and HA/Dec slews, slit and flaps, dome
rotation and focus, each taking a few seconds. The default `--tcs-host` is
the real alhena: every test must override it.

## Not verified on the telescope

- Real timings: the oil start-up, flaps, slit and slews. The timeouts
  (`move_timeout` 600 s, `seq_timeout` 900 s) are guesses.
- Whether `telescope_set_coordinates ()` echoes in `tsra` as the teld
  expects, and to what precision.
- The pier-side convention (0 east / 1 west from the clients) against what
  `telFlip` means to RTS2.
- Whether the TCS takes `TETR 0` / `TSHA` / `TGHA 1` while tracking, as
  Fuky's park sequence implies.
- Fuky's park also calibrates the axes (`TEHC 1`, `TEDC 1`). The dome's
  close doesn't; add that if the 2m needs it.
- The DOSA/DOGA close azimuth.
