# base: status

Read this before working in `base/`. It holds the current state and the
things that must stay true. The dated log of how it got here is
`HISTORY.md`. Grep that for the reason behind anything below; don't read
it front to back.

At the end of a session: update this file if a must-stay-true fact, a
deferral or an open item changed, and append the session to `HISTORY.md`
with the date and `[MJ]`/`[JS]`.

## In progress

- 2026-11-07 [JS] D50 Sun watchdog (`D50::checkSunApproach ()`, teld/d50):
  not yet seen on the mount - a standing mount (tracking off, `block_move`
  0, dome open) that the Sun nears should park ~10 min before the 15 deg;
  with `block_move` 1 a critical report every 5 min. Also unverified there:
  leaving the Sun's proximity. gxccd stop: C2 untested. Morning flats:
  D50 seen 2026-11-08 (dome closed after the flats, 187 C0 darks by
  `python/sites/d50/flatC0.py`, no `BrokenPipeError`); SBT C3 darks still
  to see - its target 2 ran the old `/etc/rts2/flatC3.py` until 2026-11-08
  (python/STATUS.md).
- 2026-11-08 [JS] D50 evening flats (script/devscript.cpp, camd): released
  (4b55578, D50 restarted 12:12 UTC, Andor rebuilt). To see: the first clear
  D50 dusk - C0 flats despite a refused first slew, no "disabling target 2
  on device C0".
- 2026-11-08 [JS] D50 PEC (teld/d50/wormencoder.cpp): stage 1 (the worm
  encoder read and logged, nothing corrected) on D50 since 19:42 UTC
  (822fe8b, T0 `--worm-encoder`, `--worm-log /var/log/rts2-worm`); tracked
  under the roof: one worm turn 179.50 s. Next: a night of C1 guiding, the
  error fitted against the worm angle (HISTORY, "D50 periodic error").

## What base is

The RTS2 backend: wire protocol/kernel, `centrald`, every device driver and
the DB-free clients (`rts2-mon`, `rts2-scriptexec`, `rts2-focusc`,
`rts2-sendcmd`, `rts2-state`). Ported from the classic autotools tree
(`~/src/rts2`, `../include`, `../lib`, `../src` in older notes) file by file,
reviewed and lightly modernized, not rewritten. Source of truth for a ported
file is the classic file of the same name. Every deliberate deviation is
marked with a `// base note:` comment (grep for it).

No database here, by design - that is `db/`.

## How to build

```
cd base
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Or `../buildme.sh base` (uses `base/build`, `-d` builds the .debs).

- libnova is found with `find_path`/`find_library` (no `.pc` file); cfitsio,
  ncurses and libusb through pkg-config.
- FLI (`external/fli`) and gxccd (`external/gxccd`, x86-64 only) are bundled
  and build by default. `BASE_FLI_SDK_DIR`/`BASE_GXCCD_SDK_DIR` override them.
- Andor (`BASE_ANDOR_SDK_DIR`) and Paramount's `libmks3`
  (`BASE_PARACL_DIR`) are opt-in and not packaged: Andor ships only a `.so`,
  `libmks3` is under an informal non-redistribution understanding.
- Debian packages: `rts2-base`, `rts2-drivers-fli`, `rts2-drivers-gxccd`,
  `fliusb-dkms`. There are no CMake `install()` rules; `debian/rules`
  copies binaries from the build tree, so a new binary must be added to
  `RTS2_BASE_BINARIES` (or the FLI/gxccd list) there.
- `fliusb-dkms` is how the FLI kernel module gets built - no hand builds.
  Its postinst builds it for every installed kernel with headers, not only
  the running one: `dkms remove --all` drops the build of a kernel waiting
  for its reboot too (HISTORY, "fliusb missing after a kernel update").
  [JS 2026-10-22]
- Tests: `kernel/tests`, `teld/tests`, `dome/tests` (with the D50 and
  BART roof benches, run by hand). Most drivers are
  verified by running them, not by ctest; there is no automated
  centrald + device integration test yet.

## Layout

| Directory | Contents |
| --- | --- |
| `kernel/` | `base_kernel`: values, connections (serial/udp/tcp/ethernet/modbus/fork), `Block`/`Daemon`/`Device`, `Client`, FITS (`Image`, `DevClientCameraImage`) |
| `centrald/` | `rts2-centrald`, `rts2-state` |
| `camd/` | `Camera` base + `dummy`, `andor`, `gxccd`, `fli`, `v4l` |
| `teld/` | `Telescope`, `Fork`, `GEM`, `TelLX200`, `perek2m::Tcs` + `dummy`, `d50`, `gemini`, `gemini-udp`, `lx200`, `paramount`, `perek2m` |
| `dome/` | `Dome`, `Ford` + `d50`, `bart`, `zelio`, `perek2m` (links `base_teld` for `perek2m::Tcs`) |
| `focusd/` | `dummy`, `tefo`, `fli`, `aaf`, `optec`, `nstep` |
| `filterd/` | `dummy`, `fli` |
| `sensord/` | `system`, `nut`, `cloud4`, `bart-rain`, `aws-mlab`, `d50-wfunit`, `makak-shutter`, `external`, `fram-weather`, `windrs`, `avelab`, `scheduler` |
| `script/`, `scriptexec/` | `rts2script` engine, `rts2-scriptexec` |
| `monitor/`, `focusc/`, `sendcmd/` | `rts2-mon`, `rts2-focusc`, `rts2-sendcmd` |
| `packaging/`, `debian/` | `rts2-start`/`rts2-stop`, systemd units, rsyslog/logrotate, `/etc/rts2` defaults |
| `external/` | bundled libfli + fliusb, libgxccd (see their README.md) |

## Where the drivers run

| Site | Drivers |
| --- | --- |
| D50 (Ondřejov) | `camd-andor`, `focusd-tefo`, `teld-d50`, `dome-d50`, sensors `system`, `nut`, `cloud4`, `bart-rain`, `aws-mlab`, `d50-wfunit`. On the rts2ng packages (`/usr/bin`, no classic in `/usr/local/bin`, seen 2026-10-17 [MJ]); the hardware checks of `D50_READINESS.md` still apply |
| SBT/BART | `dome-bart`, `teld-gemini` / `teld-gemini-udp`, `focusd-aaf`, `sensor-bart-rain`, `sensor-aws-mlab` |
| FRAM Los Leones | `focusd-aaf`, `sensor-fram-weather`, `dome-zelio`, `teld-lx200`, `sensor-nut`, `camd-gxccd` |
| FRAM Paranal (brown, white) | `teld-paramount`, `dome-zelio`, `camd-gxccd`, `focusd-nstep`, `sensor-windrs`, `sensor-external` |
| FLORES / Perek 2m | `camd-fli` (with a filter wheel), `teld-perek2m`, `dome-perek2m` (both run `--readonly` until RTS2 takes over the 2m); TOPTEC focuser, spectrograph and calibration lamps are rtspy drivers, not in this repo |
| makak | `camd-gxccd`, `sensor-makak-shutter` |
| AVE lab | `sensor-avelab` |

## Must stay true

A change that breaks one of these is a real regression even if everything
compiles. Each has more detail in `HISTORY.md` and, for classic-tree bugs,
`UPSTREAM_BUGS.md`.

**Wire protocol**
- A device sends its full property dump to a new connection *before* login
  completes. Do not change this ordering: every client and device ever
  written depends on it. Clients must cope with it (see `gui/` viewer's
  cache-and-replay). `UPSTREAM_BUGS.md`, "PROTOCOL DESIGN NOTE".
- `Connection::sendMsg ()` writes nothing while the link is
  `CONN_INPROGRESS` (non-blocking connect not finished): the write would
  fail with EAGAIN and drop the link for 60 s (UPSTREAM_BUGS, "Writes
  before a non-blocking connect completes"). [MJ 2026-11-03]
- `Connection::isName()` accepts `nullptr` (returns false). Callers rely on
  it; `getOpenConnection(nullptr)` used to crash `rts2-focusc`.
- `Client::Client()` takes the user name from `getpwuid(geteuid())`, so
  clients started without a login session (cron, systemd) can log in.
- A string value is sent as `"..."` without escaping, and
  `ValueString::setValue` takes the text up to the last quote on the
  line, so SCRIPT can contain quotes (`exe "..."`, 189 of SBT's
  scripts). Keep the wire format as classic's. [JS 2026-10-26]

- Every drop of a `Connection` goes through `dropConnection (size, reason)`,
  which logs who and why; do not call `connectionError ()` directly from
  Connection's error paths. `ConnFork` opts out via `logsDrops ()`.
  [JS 2026-10-15]
- When a device's link to centrald breaks, and again before the register on
  a reconnect, a running command that was already sent is failed
  (`Connection::failSentCommand ()`). Otherwise the register waits behind
  it for ever and the device stays off that centrald. [JS 2026-10-18]
- DEBUG messages reach neither syslog nor centrald unless the daemon runs
  with `--debug` (`Daemon::sendMessage`, `Device::sendMessage`).
  [JS 2026-10-15]
- While any value change is queued, `Daemon::setCondValue ()` queues a
  change the current state would queue even when it equals the current
  value. A queued change can rewrite other values when applied (Andor
  ADCMODE -> `setTiming ()` -> `exposure`); skipping the "equal" set gave
  0 s frames at every script repeat on D50. `kernel/tests/test_value_que`.
  [MJ 2026-10-18]

**Device values** [MJ 2026-11-03]
- Right after `infotime`/`uptime`, every device sends its identity:
  `DRIVER` (executable), `BUILD` (the package version, as `dpkg-query -W`
  shows it, from `RTS2_BUILD_ID` set by `buildme.sh -d`; `git describe` at
  configure time in a plain build),
  `LINKED` (the executable's mtime), and `SDK_VER`/`HW_VER` when the driver
  calls `setSdkVersion ()`/`setHwVersion ()` from `initHardware ()` (gxccd,
  fli and andor do). They are constant values (`Device::Device`), not part
  of classic, and carry `RTS2_VALUE_DEVPREFIX`, so FITS has `C0.BUILD`,
  `T0.BUILD`, ... instead of one device's overwriting the other's. A new
  driver on a vendor SDK should set both.
- Values an operator does not need in normal work carry `RTS2_VALUE_DEBUG`
  (camd: stats beyond average/min/max, `center_*`, `sep_*`, WCS, channel
  geometry, temperature history knobs). rts2-mon hides them unless the
  Debug menu item or `--show-debug` is on, and its Ctrl-F searches only
  what is shown. The flag is display only: FITS, scripts and `-s` are
  unaffected. Mark the internals of a new driver the same way.
- `Device::beforeRun ()` reorders the value list once, before any client
  gets it (`Daemon::sortValuesForDisplay`): values marked with
  `markEssential ()` first, in the order marked, then the rest in creation
  order, then the debug ones. No wire flag, nothing in classic or here
  reads values by position. A driver that overrides `beforeRun ()` must
  call `Device::beforeRun ()`. Camera marks exposure, IMAGETYP, OBJECT,
  binning, WINDOW, exposure_end, que_exp_num, last_image, average, and
  where they are created filter/FILTx, SHUTTER, CCD_TEMP, COOLING, CCD_SET.
  Telescope, focuser and filter wheel mark theirs in the base class (TEL,
  TEL_, ORI, OFFS, TRACKING, target_distance, MNT_FLIP; FOC_POS, FOC_TAR,
  FOC_DEF, FOC_FOFF, FOC_TOFF, FOC_TEMP; filter), domes and sensors in the
  driver (roof/limit switches, slit; the headline readings). A value
  created after `beforeRun ()` (cloud4's TEMP_AMB) cannot be marked.
  [MJ 2026-11-03]

**Event loop**
- Nothing that runs inside `Block::pollSuccess ()` (a command handler, a
  script line) may call `oneRunLoop ()`. A nested loop leaves the outer one
  with stale poll results and an invalidated `connections` iterator. Script
  `waitmask`/`waitidle` used to do this, and it dropped the SBT executor's
  centrald connection and once crashed it (`UPSTREAM_BUGS.md`). They are
  answered from `ConnExe::idle ()` now.

**Logging**
- Every daemon logs to syslog under the ident `rts2`, always, whether or
  not centrald is up. `Daemon::doDaemonize()` must not call `openlog()`
  again (it reset the ident to the program name).
- centrald does not write a log file. It only relays messages to live
  monitors. Persistence is rsyslog + logrotate from `packaging/`.

**Times**
- A time `double` never goes to an ostream without an explicit mode
  (`timeDisplay_t`: ISO, ctime, JD). Default precision prints
  `1.78818e+09`. `rts2-httpd` JSON always emits ctime with 6 decimals,
  whatever the daemon's `--jd`/`--ctime`.

**Cameras and FITS**
- `ClientFilterCamera`/`ClientFocusCamera` must exist for `--wheeldev`/
  `--focdev`. Without them `FILTA` records filter index 0 whatever the
  wheel is at, and per-filter focus offsets never reach the focuser.
- `Camera::postEvent()` must handle `EVENT_FOCUSER_END_MOVE` and
  `EVENT_FILTER_MOVE_END`. `offsetForFilter()` raises `focuserMoving`, and
  without those handlers the first filter change wedges the camera.
- `Camera::createOtherType()` null-checks `focuserDevice`.
- `script_ends` while the chip exposes or reads out only sets
  `scriptEndsPending`; `Camera::checkQueChanges ()` runs `scriptEnds ()`
  once the chip is idle, before the queued values. A reset mid-exposure
  ruins the Andor's frame (HISTORY, "D50 evening flats lost"). [JS 2026-11-08]
- `LTV1`/`LTV2` (and through them `CRPIX1`/`CRPIX2`) include the window
  offset, `(x)/bin`, consistent with `DATASEC`.
- FLI: `nflush` defaults to 1, and `idle_flush_period` (300 s) dumps the
  CCD while idle. Both are deliberate.
- Optional per-camera config (`instrume`, `telescop`, `origin`, `template`,
  `environment`) is read with the quiet lookups. Missing keys must not warn.
- Andor: a kinetic series (`ACQMODE=3`) saves the same kind of frame as
  an accumulation (`ACQMODE=2`): the sum of its `KINNUM * ACCNUM` scans,
  32-bit when that is more than one, `exposure_total` (EXPTIME) the scans'
  total; plus the raw series in `KINFILE`, 16-bit frames, 32-bit when
  `ACCNUM > 1`. `coaddedScans ()` is the one place that counts the scans;
  it is also `COADDS` in the header. Classic averaged the series into 16
  bits; **`COADDS` present = the new frame**, absent with `ACQMODE` 3 = an
  old averaged one. Processing forks on that keyword, not on BUILD.
  [MJ 2026-11-03]
- Andor: `scriptEnds ()` does not reset `intExposure` (hidden state that
  `setTiming ()` later copies into `exposure`). [MJ 2026-10-18]
- An exposure whose client disconnects ends at once
  (`Camera::deleteConnection ()`), aborted mid-exposure: aborted at its
  end it ran into the camera's own readout, and an exposure waiting behind
  it was refused for the whole readout (2.6 s on C3) or, with the reinit,
  wedged the camera (HISTORY, "An exposure whose client left ends at
  once"). [JS 2026-10-27]
- A camera without a cooler has only `CCD_TEMP` and `CCD_AIR`; the
  cooling set exists only where the driver creates a `Cooler`, which it
  may do as late as `initHardware ()` (gxccd: when `GBP_COOLER`; makak's
  G1-2000 and D50's C1 have none). `-c` and `--no-autocooling` belong to
  Camera and are applied whenever the cooling values appear; on a camera
  without them `-c` is ignored with a warning. fli always has a cooler
  (libfli cannot tell). [MJ 2026-11-03]
- Cooled cameras regulate through `Cooler` (`camd/include/cooler.h`,
  `Camera::createCooler ()`); the driver implements only
  `CoolerHardware`: `coolerSetPoint ()`, optionally `coolerPower ()` and
  `coolerBusy ()`. dummy, gxccd, fli and andor use it. Cooling off ramps
  to the driver's off temperature (gxccd 50 C, fli 40 C: no cooler switch,
  they stay regulated warm; andor 20 C, then `CoolerOFF ()`) and then
  calls `coolerPower (false)`; it does not touch
  `CCD_SET`, so cooling on returns to it (gxccd used to overwrite it with
  50, and COOLING on by hand then cooled to 50). `TERAMP` defaults to
  100 C/min, practically immediate both ways; it is autosaved, so a site
  that set a gentler one keeps it. `temp_stable`: cooling on, the ramp at
  `CCD_SET`, `CCD_TEMP` within `temp_tol` for 120 s; checked wherever
  `CCD_TEMP` is refreshed (`Camera::info ()`, `addTempCCDHistory ()`), not
  only on `info ()`, which nobody may call (flores has no httpd). Cooler power is
  `TEMPPWR`, in percent, on every camera (fli's was `COOL_PWR`; gxccd's
  was the SDK's 0-1 fraction until 2026-11-03, older FITS have that).
  [MJ 2026-11-03]
- gxccd: a failed filter move is retried every 0.5 s for up to 15 s and
  never answered with `reinitCamera ()` (classic did not reinit either).
  The driver sends nothing of its own to a busy camera (`cameraBusy ()`:
  not `isIdle ()`, or an exposure or readout aborted under 15 s ago - an
  aborted exposure too since 2026-10-27 [JS]; a refused exposure start is
  retried every 50 ms for 3 s after an abort) - no reinit, no
  window heating, no temperature-ramp step; only stopping an exposure.
  A client's filter change during a readout is queued by RTS2 and runs the
  moment the readout ends (tested on C2); keep `createFilter ()` as it is,
  waiting longer only loses time. Commands make one attempt, as classic
  (HISTORY, "gxccd: back to classic error handling"). [JS 2026-10-24] After a NOW's `killall`
  aborts a readout the camera is still busy, the move fails, and
  reinitializing it then wedged C1 and C2 until power-cycled (HISTORY,
  "gxccd: no reinit on a failed filter move"). [JS 2026-10-20]

**Telescope**
- A correction whose move fails is dropped and `corr` restored
  (`CorrectionRollback` in `startResyncMove ()`). Classic left `wcorr`
  pending and `setFullBopState ()` re-added it on every retry; at SBT it
  reached 5.6 deg. A failed move still calls `stopTracking ()`, as classic.

**D50 mount and roof** (`teld/d50`, `dome/d50`, `dome/src`; full list in
`UPSTREAM_BUGS.md` "D50 mount/roof interlock" and `D50_READINESS.md`)
- Motor-vs-absolute-encoder checks use `fabs` and wrapped RA
  (`checkAbsEncDifference()`). An unreadable encoder refuses inside-dome
  moves, but only warns at end of park: it must never keep the roof open.
- The mount sends `inside_dome_limit` the moment it changes. The dome waits
  while the mount is moving or parking. The mount refuses any move but park
  while the dome is closing or waiting to close.
- A close waiting for the mount sends `park` at once, then at most every
  30 s while the mount stays at rest outside the dome limit, and reports a
  wait of over 90 s once as critical (`D50::waitingForMount ()`): the roof
  still never closes onto a mount outside the limit, but a roof held open
  in bad weather calls a human. Classic waited silently and sent `park`
  at every idle. [JS 2026-10-28]
- Every move goes by `D50Path::findPath ()` (`teld/d50/d50path.cpp`):
  the classic straight / HA-first / Dec-first tries, then a 5 deg grid of
  intermediate targets. Every target, intermediate ones included, must be
  safe to stop at (horizon, ceiling with the roof not open, 15 deg from the
  Sun while it is above the horizon - classic 10); a target given early is only one a
  moving axis can reach without reversing. A passage is sampled as densely
  as its distance to the nearest limit requires (1 s to 5 ms, bounded by
  the drives' speeds and the horizon slopes), so no limit is crossed
  between samples. A mount already within 15 deg of the Sun may only move
  away from it until out (sampled every 5 ms).
  A mount standing still (tracking off) that the Sun will reach within
  10 min, or has, parks (checked once a minute), unless `block_move` is set
  (then a critical report). `teld/tests/test_d50path`
  replays the found paths independently. [JS 2026-11-07]
- The HA worm encoder (`--worm-encoder`, `teld/d50/wormencoder.cpp`) is
  read in its own thread and only published and logged: no move, no
  tracking and no guide pulse waits for it or depends on it, and a missing
  or failing board only leaves `worm_encoder` false. [JS 2026-11-08]
- The dome raises `BOP_TEL_MOVE` (not only `BOP_EXPOSURE`) while opening.
- A close that does not reach the end switch is retried 3 times
  (`Dome::closeFailed()`), then reported as critical.
- `ConnREMOTES::init()` returns the real result and `D50::init()` checks it.
  A failed raw socket used to segfault.
- `D50::isClosed ()` is also Dome's "closed already?" probe, at every idle
  in bad weather: -2 only with the closed switch made and the opening motor
  off, separate open/close timeouts cleared first thing in `endOpen ()` /
  `endClose ()`, a close timeout is -1. A leftover timeout used to answer
  every close in the rain with "dome closed". A mount found moving or
  outside stops a running close. Motor writes are read back, motors running
  at start are stopped. Timeout 40 s (travel 26-27 s). [MJ 2026-10-17]

**Ford board and BART roof** (`dome/src/connford.cpp`, `dome/bart`;
`UPSTREAM_BUGS.md` "Ford board replies are never resynchronised" and
"BART roof: a close that times out ...", bench in `dome/tests/bart-bench`)
- Every Ford query flushes stale input first, checks the echo and rejects
  bytes left after the reply. A pin write rewrites the whole port, so it
  happens only after two reads in a row agree. One stray byte used to
  desync the board until a restart and drive a closing roof open.
- A BART close that does not reach the closed switch in 20 s is an error
  (`isClosed ()` -1), never "closed". The end of every move switches the
  motor off and checks it; the direction is checked before and after the
  motor starts.
- An opening that times out counts as opened: the BART open switch rarely
  makes (2026-10), so this is how the roof opens. [MJ 2026-10-17]
- The box does not report its manual/PC selector (no port A bit moves), so
  a BART roof found open while it should be closed (good weather, dome
  state closed) is reported once as a warning and closed quietly every
  120 s: a failure is no alarm, so a visitor working it by hand causes no
  panic, and it closes soon after PC control is back. The quiet close stays
  out while an ordinary close is failing (it would cancel the retries and
  the EMERGENCY). Bad weather closes as always. [MJ 2026-10-17]

**Gemini UDP** (`teld/gemini-udp`, SBT; operator notes in its `OPERATION.md`)
- The startup handshake (boot menu, `:hW#`, clock, limits) runs in the
  caring loop and is re-checked periodically, so a mount that reboots under
  the driver is brought back up. `initValues()` does no mount I/O.
- `handleGoto()` refuses while startup is incomplete.
- `setTracking()` drives the worm (native 131/135). `Telescope` alone only
  changes RTS2's own state.
- Safety checks count *mount polls* (gated on `GeminiStatus::timestamp`),
  not `idle()` ticks, and need 3 in a row. The wrong-way check stands down
  for the rest of a move that changed pier side.
- Recovery is stop, park, cold start (65533). After `max_recoveries` the
  mount stays blocked with `safety_locked` for a human. An unwritable
  incident log must never stop a recovery.
- `abortMoveTracking()` returns 1 until there is a valid position.
- While the mount does not answer (`st.connected` false), `applyStatus ()`
  logs the outage once and stops after `checkStartup ()`: the held-over
  status must not drive anything. `gotoRaDec ()` and `startPark ()` refuse at once.
- `carryPersistentFields()` keeps fields that do not come from the ENQ reply
  (`parking`, `trackingSecToWestLimit`). `pollStatus()` used to reset them.
- Every goto takes `tel_target` (`getTelTargetRaDec ()`): the framework's
  astrometric corrections (`CORR_`) are only in it. The goto, the move
  recovery and the move after a re-zero took `getTarget ()` until
  2026-10-26, and every `correct` was lost (HISTORY, "Gemini UDP: gotos
  without the corrections"). [JS 2026-10-26]
- A Gemini goto waits `GeminiCaringLoop::GOTO_WAIT_SEC` (15 s) for the
  mount loop: three loop commands at their worst (1 s x 5 attempts), the
  one in flight, the stop the framework sends before every goto, and the
  slew. 3 s refused a goto behind a slow mid-slew stop (HISTORY, "Gemini
  goto wait"). [JS 2026-10-26]
- A `correct` is taken only for a move that reached its target
  (`move_completed`, set when `endMove ()` succeeds, cleared when a move or
  a park starts). The error is measured against the target (asarina,
  classic), so from a failed move it is the miss, not a pointing error.
  `MOVE_NUM` only says the image is fresh. (HISTORY, "Corrections only from
  completed moves") [JS 2026-10-26]

**Perek 2m** (`teld/perek2m`, `dome/perek2m`; `teld/perek2m/OPERATION.md`)
- The 2m is shared with observers using Fuky's clients. Neither daemon
  commands anything on its own, and both have a read-only mode
  (`tcs_readonly`, writable at run time; `--readonly` sets it at start).
- The dome is built with `inhibit_auto_close`: `auto_close` starts false,
  so centrald states and bad weather do nothing until it is set.
- The teld's `abortMoveTracking ()` stops only RTS2's own slew; it never
  stops the TCS for a position RTS2 did not command.
- Park is the park position only. The full power-down (park, flaps and slit
  closed, telescope off, then oil off) is the dome's close. Neither ever
  switches the instruments' power.
- The default `--tcs-host` is the real alhena: a test must point at
  `teld/perek2m/telescoped_sim.py` on 127.0.0.1.

**Scheduler trigger** (`sensord/scheduler`)
- One run at a time. A trigger during a run (ready, `run`, interval) is
  held and starts one new run when the current one ends; a run is never
  killed for a newer trigger.
- Each telescope's centrald state is kept apart in `setMasterState ()`:
  `Block` keeps only one master state. A lost centrald counts as not ready.
- `endRunLoop ()` kills the running scheduler: `Daemon::endRunLoop ()`
  exits from the signal handler and no destructor runs.
- A telescope getting ready while nothing runs waits `ready_hold` (30 s)
  so that the other, switched on a moment later, joins the same run; the
  `run` command and the interval never wait, and take a held trigger along.
- The run closes every fd above 2 before `exec` (`closefrom (3)`): a
  ConnFork child otherwise holds all the daemon's connections open.

**Scripts**
- `[scriptexec] script_path` is a `:`-separated search list, default
  `/etc/rts2/scripts:/usr/share/rts2/scripts` (`ConnFork`). The first is
  the site's own and not packaged; the second holds rts2-python's
  site-independent scripts. Site-specific scripts are never packaged
  (`python/STATUS.md`). [MJ 2026-10-26]
- `DevScript::deleteScript ()` drops the script's exposures still waiting
  in the connection's queue for a block (`Connection::queDropUnsent ()`) and
  clears the camera client's `waitForExposure`. Left there, they went out
  when the block ended, behind the next script's `script_ends`. [JS 2026-11-08]

**Other drivers**
- `makak-shutter` keeps the value names the makak scripts use (`shutter`,
  `htSwitch`, `heating`, `temp`, `humidity`) and re-sends the heating mode
  after every USB reset (the arduino forgets it).
- `teld-dummy` parks to the current zenith, re-locked on every poll.
- `Block::~Block()` deletes pending timers.
- `[observatory]` longitude/latitude/altitude are mandatory. This is a
  decision, not something to work around.

## Deliberately not ported

Each is stubbed or falls through cleanly. Port it when a real user needs
it, and say which classes you added.
- ~44 `Command` subclasses and 11 `DevClient` subclasses (`Dome`, `Cupola`,
  `Mirror`, `Rotator`, ...). Check `command.h`/`devclient.h` first; many
  were added later for real consumers.
- SEP star finding (`Camera::findSepStars` is a no-op).
- TLE/SGP4 tracking (`move_tle` is rejected), concrete pointing models
  (`GPointModel`/`TPointModel`; `--rts2-model` logs an error),
  `ClientCupola`/`ClientRotator`.
- ERFA code paths (only the libnova path exists).
- `SimbadTarget` in `rts2-mon`; JPEG previews in `Image`; dummy camd's
  direct-FITS readout.
- `ElementAcquire` and `after_exposure_cmd` (need the DB; live in `db/`).
- `connnotify`, `rts2-cmon`, `rts2-talker`.

## Agreed but not done

- **GEM/Fork/AltAz.** Flip handling should be a property of one equatorial
  class (add `never` to `flipping`), with `AltAz` as its sibling. Deferred
  on purpose until all mounts in use are ported; `GEM` and `Fork` are
  faithful classic ports for now.
- **Shared device parts.** [MJ 2026-11-03] Components instead of copies
  in every driver. Done: `Cooler` for dummy, gxccd, fli, andor. Left:
  connection health for serial/TCP/UDP
  devices; night-scheduled switches (gxccd's `WINDOW_HEATING_NIGHT`).

## Open items

- `Camera::updateStatistics` mode histogram is UB for 64-bit/float pixel
  types (`1 << 64`). Only hit if `data_type` is changed; needs a decision
  on what "mode" means for those types.
- `exec_device` in `rts2-scriptexec` casts a `Client` to a `Device`
  (`known-bugs.md`). makak's script works around it.
- Not verified on hardware: Perek 2m teld/dome (only against the
  simulator; list in `teld/perek2m/OPERATION.md`); FLI idle flush; Gemini UDP recovery (does the
  UDP listener come back after 65533, is the boot menu reachable over UDP);
  D50 mount-side interlock fixes.
- Lost inter-daemon connections at SBT after days of running (the daily
  restart cron is gone since 2026-10-15). Now logged, not fixed. Two
  suspects: `sendMsg ()` drops the connection on EAGAIN or a short write
  to a slow peer; a lost authorization handshake left the connection in
  `CONN_AUTH_PENDING` for ever - now retried by the side that started it,
  on a new socket, after 30 s and then up to every 300 s (HISTORY,
  "Handshake retry") [JS 2026-10-20]. A dropped link to another device
  forgot its address, and nothing connected again until centrald announced
  the device anew; now the address is kept and the link reconnected after
  10 s, up to every 300 s, until centrald's `delete_device` (fixed, it
  matched nothing) says the device left (HISTORY, "Dropped device links
  reconnect") [JS 2026-10-20]. One
  stuck authorization at BART was an ID collision between two centralds
  (also possible after a centrald restart), fixed (HISTORY, "Authorization replies matched on the ID alone"). Devices
  that never registered again with a second centrald, after a reply lost
  with the link, fixed (HISTORY, "A lost reply keeps a device off a second
  centrald"). The key for a device that also connects to us (D50 T0 and
  DOME) given to the other's connection of the same name, fixed
  (HISTORY, "Two devices connecting to each other") [JS 2026-10-20]. Data:
  `/var/log/rts2.log`, `~torman/rts2diag/` on lascaux. HISTORY,
  "Lost inter-daemon connections". [JS 2026-10-15]
- BART roof, on site [MJ 2026-10-17]: the south open switch almost never
  makes and the closed switch sometimes drops out (HISTORY, "BART roof").
  With the fix a bad closed switch gives CRITICAL/EMERGENCY instead of a
  silent "dome closed". To test there: whether the roof can reverse during
  an opening (a close then is refused until it is open, classic, retried
  every 30 s), what the north switch bits mean, the real travel time vs
  the 20 s timeout; that manual really bypasses the board's motor relays
  (the quiet close drives them while a visitor works the roof).
- Wheel moves the camera did not start (filterd `--daytime-filter` on DAY,
  `--default-filter`, homing, a manual move) do not hold the camera, and its
  `FILTA` follows only at its next `info ()`: frames in that window go
  through the wrong filter with the old name in the header. Classic. Seen in
  the past as dark-filter frames failing astrometry, not recently; left
  alone on purpose. [MJ 2026-10-18]
- Gemini UDP, for MJ: each reset of the mount's clock (`endPark ()` out of
  night, and at startup when it is off) leaves Gemini's RA relation off by
  the reset; the clock loses 3-4 s a day, so the RA offset grows until the
  next cold start (0.362 deg on 2026-10-26, shown as "mount's own offsets"
  in every sky sample; a cold start at CWD cleared it). Options: no clock
  reset between cold starts; compensate the offset the driver measures;
  reset the clock only with a cold start. Needs a daytime check first
  (set the clock 30 s off, see the own offset move 0.125 deg).
  `teld/gemini-udp/OPERATION.md` 5, "The mount's own RA offset".
  [JS 2026-10-26]
- A script value the operand parser cannot read (`WINDOW=50:40:100:80`,
  or `WINDOW="50 40 100 80"`) throws `rts2script::UnknowOperantMultiplier`
  out of `OperandsSet::parseOperand` (script/src/operands.cpp:118), and
  nothing up through `Script::parseBuf` catches it: rts2-scriptexec aborts
  with SIGABRT. Anything parsing a target's script the same way would too.
  Classic has the same code and no catch. Found 2026-10-28, not fixed.
  [JS 2026-10-28]
- `LTM1_1`/`LTM2_2` ignore binning.
- No CMake `install()` rules.
- No automated integration test (centrald + dummy + scriptexec, assert on
  the FITS file).
- `template`/`environment` camera config has no documented example.

## Conventions

- C++17, `#pragma once`, `nullptr`, `<cstdint>`/`<cstring>` over C headers.
  Minimal change per file: correctness first, no blanket `override` passes.
- Drop dead legacy-platform branches (`sun`, `__CYGWIN__`, old GCC) outright.
  Modern Linux/glibc only.
- Keep headers diffable against classic in structure and naming. Mark every
  deliberate deviation with `// base note:`.
- Drop an unneeded `#include` (often `block.h`) and add what the file
  really uses. Verify with grep every time.
- A clean static-library build does not prove a declared method has a body.
  Only a linked executable or a grep does.
- When porting part of a class family, write down exactly which classes
  were left out and why.
- A genuine classic-tree bug goes to `UPSTREAM_BUGS.md`; a bug of the port
  itself goes to `known-bugs.md`.

## Other files

- `HISTORY.md` - dated session log.
- `UPSTREAM_BUGS.md` - classic-tree bugs found and fixed here, written to be
  reported upstream.
- `known-bugs.md` - open bugs in rts2ng itself.
- `D50_READINESS.md` - D50 switch-over review and checklist (2026-10-09).
- `teld/gemini-udp/OPERATION.md`, `teld/perek2m/OPERATION.md` and
  `PROTOCOL.md`, `external/*/README.md`.
