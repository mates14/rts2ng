# base: status

Read this before working in `base/`. It holds the current state and the
things that must stay true. The dated log of how it got here is
`HISTORY.md`. Grep that for the reason behind anything below; don't read
it front to back.

At the end of a session: update this file if a must-stay-true fact, a
deferral or an open item changed, and append the session to `HISTORY.md`
with the date and `[MJ]`/`[JS]`.

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
- Tests: `kernel/tests`, `teld/tests`, `dome/tests`. Most drivers are
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
| `sensord/` | `system`, `nut`, `cloud4`, `bart-rain`, `aws-mlab`, `d50-wfunit`, `makak-shutter`, `external`, `fram-weather`, `windrs`, `avelab` |
| `script/`, `scriptexec/` | `rts2script` engine, `rts2-scriptexec` |
| `monitor/`, `focusc/`, `sendcmd/` | `rts2-mon`, `rts2-focusc`, `rts2-sendcmd` |
| `packaging/`, `debian/` | `rts2-start`/`rts2-stop`, systemd units, rsyslog/logrotate, `/etc/rts2` defaults |
| `external/` | bundled libfli + fliusb, libgxccd (see their README.md) |

## Where the drivers run

| Site | Drivers |
| --- | --- |
| D50 (Ondřejov) | `camd-andor`, `focusd-tefo`, `teld-d50`, `dome-d50`, sensors `system`, `nut`, `cloud4`, `bart-rain`, `aws-mlab`, `d50-wfunit`. Still on classic RTS2; switch-over checklist in `D50_READINESS.md` |
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
- `Connection::isName()` accepts `nullptr` (returns false). Callers rely on
  it; `getOpenConnection(nullptr)` used to crash `rts2-focusc`.
- `Client::Client()` takes the user name from `getpwuid(geteuid())`, so
  clients started without a login session (cron, systemd) can log in.

- Every drop of a `Connection` goes through `dropConnection (size, reason)`,
  which logs who and why; do not call `connectionError ()` directly from
  Connection's error paths. `ConnFork` opts out via `logsDrops ()`.
  [JS 2026-10-15]
- DEBUG messages reach neither syslog nor centrald unless the daemon runs
  with `--debug` (`Daemon::sendMessage`, `Device::sendMessage`).
  [JS 2026-10-15]

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
- `LTV1`/`LTV2` (and through them `CRPIX1`/`CRPIX2`) include the window
  offset, `(x)/bin`, consistent with `DATASEC`.
- FLI: `nflush` defaults to 1, and `idle_flush_period` (300 s) dumps the
  CCD while idle. Both are deliberate.
- Optional per-camera config (`instrume`, `telescop`, `origin`, `template`,
  `environment`) is read with the quiet lookups. Missing keys must not warn.

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
- The dome raises `BOP_TEL_MOVE` (not only `BOP_EXPOSURE`) while opening.
- A close that does not reach the end switch is retried 3 times
  (`Dome::closeFailed()`), then reported as critical.
- `ConnREMOTES::init()` returns the real result and `D50::init()` checks it.
  A failed raw socket used to segfault.

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
  to a slow peer; a lost authorization handshake leaves the connection in
  `CONN_AUTH_PENDING` for ever (warned after 60 s, never retried). Data:
  `/var/log/rts2.log`, `~torman/rts2diag/` on lascaux. HISTORY,
  "Lost inter-daemon connections". [JS 2026-10-15]
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
