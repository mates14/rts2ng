# base porting status

Working notes for the base effort so it can be picked up cold. Update this
file at the end of each work session; don't rely on chat history surviving.

## What base is

A from-scratch CMake build of the RTS2 backend (wire protocol + device
drivers), living next to the classic autotools tree (`../lib`, `../src`,
`../include`). Not a mechanical port - files are reviewed and lightly
modernized while they're ported (dead legacy-platform code stripped,
`NULL`->`nullptr`, raw owning pointers -> `unique_ptr` where safe, real bugs
fixed when found, dead/unnecessary `#include`s dropped). Started because the
classic tree has no way to configure out unused drivers, and no clean
separation between the backend (network + drivers, no DB) and the frontend
(executor/scheduler/httpd, DB-bound).

Design context (why this shape) is in two artifacts from earlier in this
initiative - not reproduced here, just linked:
- Driver inventory + tier model (kernel/standard/vendor/site/legacy) + backend/frontend split: https://claude.ai/code/artifact/8cc1c68b-cf02-41fc-aed9-45438e4e44e9

Working method agreed with the user: build `base/` piece by piece, bottom of
the dependency graph first. Get centrald running on the new kernel, *then*
port drivers. When a file turns out to declare a big family of concrete
subclasses that are conceptually driver/client-tier (not core bus plumbing -
e.g. `command.h`'s ~50 `Command` subclasses, `devclient.h`'s 15 `DevClient`
subclasses), only the base class is ported and the subclasses are deferred
until a real consumer needs them - same tier-model judgment as the driver
inventory artifact above, just applied one level deeper into the kernel.

## How to build

```
cd base
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Requires libnova dev headers (found via `find_path`/`find_library` in
`kernel/CMakeLists.txt`, not pkg-config - this system's libnova ships no
`.pc` file).

`rts2-camd-andor`/`rts2-camd-gxccd`/`rts2-camd-fli`/`rts2-focusd-fli`/
`rts2-filterd-fli` are skipped by default (proprietary vendor SDKs). Add
`-DBASE_ANDOR_SDK_DIR=/path/to/andor/sdk`,
`-DBASE_GXCCD_SDK_DIR=/path/to/gxccd/sdk` (each a tree containing
`include/` + `lib/`), and/or `-DBASE_FLI_SDK_DIR=/path/to/built/libfli`
(a tree with `libfli.h` + `libfli.a` built at its top level - see "FLI"
section for the two small patches libfli itself needed first) to the
`cmake` invocation to build them - see the "Andor"/"GXCCD"/"FLI" sections
below.

## Progress

1. DONE - skeleton + hoststring proof-of-concept
2. DONE - base utility layer (`status`, `error`, `radecparser`, `utilsfunc`,
   `riseset`, `rts2format`, `timestamp`, `libnova_cpp`)
3. DONE - config layer (`proto`, `message`, `logstream`, `pid`, `objectcheck`,
   `iniparser`, `configuration`, `value.h`)
4. DONE - Value type system fully wired (`value.cpp`, `data.cpp`, `valuestat`,
   `valueminmax`, `valuerectangle`, `valuearray`), Command (trimmed)
5. DONE (core) - Connection + Block + DevClient (base) all ported, compiling,
   linking, **and passing a real behavioral test**
   (`test_block_connection.cpp`: connection registration/lookup, value
   storage/retrieval). Transport variants ported later, as real drivers
   needed them: `connnosend`/`connudp` for TEFO (task 17), `connserial`/
   `connethernet` for D50 (task 18, also not on the original task 5 list -
   turned out needed once TGDrive/ConnREMOTES showed up), `conntcp` for
   the `nut` sensor (task 20), `connfork` for the upcoming `scriptexec`
   port (task 22, needed by `ConnExe`/the `exe`/`hex` script elements).
   Still remaining: `connnotify` - not yet needed for anything ported so
   far.
6. DONE - `client.h`/`client.cpp` (`Client`/`ConnClient`/`ConnCentraldClient`/
   `CommandLogin`) ported into the kernel - the CLI-client side of `Block`,
   needed by `rts2-mon` (task 11) and any future CLI tool. Small (~500
   lines total), compiled clean first try.
7. DONE - Device/Daemon framework: `daemon`, `device`, `multidev` all ported,
   compiling, linking. `centrald` (task 9) can now extend `Daemon` directly.
   See "Daemon networked-logging decision" below for the one open design
   question that was explicitly deferred, not resolved, during this task.
8. DONE - real App/CLI framework (moved up ahead of schedule - see below)
9. DONE - centrald ported and **smoke-tested as a running process**: builds
   as `rts2-centrald` in its own `base/centrald/` directory (linking
   against `base_kernel`, matching the "own directory, compiles against
   the kernel" layout from the original design artifact). Ran it against a
   real (patched) `conf/rts2.ini` with `-i --local-port 18617` and
   confirmed with `ss -ltnp` that it actually binds and listens - the full
   `Daemon::init()` -> `reloadConfig()` -> `initValues()` -> run loop path
   works end to end on the ported kernel. First real daemon milestone hit.
10. DONE - first standard driver ported end-to-end: `dummy` camd. This is the
    biggest single chunk ported so far - `scriptdevice` (into the kernel,
    it's generic `rts2core` device-scripting infra, not camera-specific),
    then `camd.h`/`camd.cpp` (the shared `Camera` base class used by every
    camera driver, ~2300 lines) in a new `base/camd/` directory building a
    `base_camd` static library, then `base/camd/dummy/dummy.cpp` building
    the `rts2-camd-dummy` executable. See "Camd (base/camd/)" below for
    what was deferred/dropped and why. **Smoke-tested for real**: ran
    `rts2-centrald` + `rts2-camd-dummy` together, confirmed the device
    registration handshake over a real TCP connection (`ss -tnp` showed the
    established socket), then opened a raw `/dev/tcp` connection to the
    camera port and drove a full `exposure 0.01` / `expose` cycle by hand -
    got back real wire-protocol traffic: metadata (`E`/`V`/`F` lines),
    progress (`R`), command ack (`+0 OK`), binary data channel setup (`B`),
    actual generated image pixel data (`D`), post-readout statistics
    (`average`/`max`/`min`/`sum`/`readout_time`/`pixels_second`), and final
    status (`S`). The full expose -> readout -> data-transfer ->
    statistics -> status cycle works end to end on the ported kernel.
11. DONE - `rts2-mon` ported in full (user explicitly chose the complete
    interactive ncurses TUI over a lighter view-only alternative when
    asked). ~5600 lines across 12 files in a new `base/monitor/`
    directory, linking against `base_kernel` + system ncurses (found via
    `pkg_check_modules`). Needed `displayvalue`/`ProgressIndicator` added
    to the kernel and `CommandChangeValue` added to `command.h`/`.cpp`
    first (see "Kernel additions made for the monitor" below). Two classic
    dependencies deliberately dropped (see "Monitor" section below):
    `rts2fits/devcliimg.h` (grep-verified dead include) and `SimbadTarget`
    (network SIMBAD lookup for a mount target argument - pulls in libxml2 +
    xmlrpc++ for a peripheral feature). **Smoke-tested for real**: ran it
    against a live `rts2-centrald` + `rts2-camd-dummy` pair non-interactively
    (`timeout 3 ./rts2-mon --port ... --server ...`) and captured genuine
    ncurses terminal output - menu bar, status bar
    (`HARD OFF day | next in 7:53:43 evening`), and the centrald device
    panel streaming real live values (`infotime`, `uptime`, `longitude`,
    `latitude`, `morning_off`, `open_close`, ...), refreshing correctly
    over the run before the timeout ended it cleanly (exit 124, not a
    crash).
12. DONE - `rts2core::Telescope` base class (`teld.h`/`teld.cpp`, the shared
    base every telescope driver extends) plus the `Dummy` telescope driver,
    in a new `base/teld/` directory building `base_teld` +
    `rts2-teld-dummy`, mirroring the `camd`/`dummy` layout. Three
    subsystems deliberately deferred (TLE/SGP4 satellite propagation,
    concrete GPointModel/TPointModel pointing models, ClientCupola/
    ClientRotator) - see "Teld (base/teld/)" below for the full writeup.
    **Smoke-tested for real**: ran `rts2-centrald` + `rts2-teld-dummy`
    together for ~30 minutes, confirmed device registration (`ss -ltnp`
    showed both listening sockets), pulled a full `info` dump over a raw
    `/dev/tcp` connection (every `Value` the constructor creates came back
    correctly over the wire), and let the idle loop run unattended - it hit
    `infoUTCLST`/hard-horizon-check/`checkTracking` every `refresh_idle`
    tick (60s) with zero crashes, and correctly handled a live
    `changeMasterState()` callback triggered by centrald's own weather-state
    broadcast. Confirmed authenticated commands (`nc_altaz`, `info` sent
    without going through centrald's login handshake) are correctly
    rejected as unauthorized - expected protocol behavior, not a bug.
13. DONE - `rts2focusd::Focusd` base class (`focusd.h`/`focusd.cpp`, ~330
    lines) plus the `Dummy` focuser driver, in a new `base/focusd/`
    directory building `base_focusd` + `rts2-focusd-dummy`, same
    kernel -> driver-family-base -> driver layout as `camd`/`teld`. **No
    deferrals** - `Focusd` is small and self-contained, no vendor SDK or
    optional subsystem to defer (unlike camd/teld). Ported verbatim with
    `NULL`->`nullptr`. **Smoke-tested for real**: ran `rts2-centrald` +
    `rts2-focusd-dummy` together, confirmed both listening sockets, watched
    `initValues()` drive a real `setPosition(0)` -> `isFocusing()` cycle to
    completion in the log (`changing focuser position to 0.000000` /
    `focuser moved to 0.000000`), and pulled a full `info` dump over the
    raw wire protocol confirming `FOC_TYPE`/`FOC_POS`/`FOC_TEMP`/`focstep`
    etc. all came back with the values `Dummy::initValues()` sets.
14. DONE - `rts2filterd::Filterd` base class (`filterd.h`/`filterd.cpp`,
    ~210 lines) plus the `Dummy` filter wheel driver, in a new
    `base/filterd/` directory building `base_filterd` +
    `rts2-filterd-dummy`. **No deferrals** - same reasoning as `focusd`:
    small, self-contained, no vendor SDK. Ported verbatim with
    `NULL`->`nullptr`. **Smoke-tested for real**: ran `rts2-centrald` +
    `rts2-filterd-dummy -F "clear:B:V:R:I"` together, confirmed both
    listening sockets, and pulled a full `info` dump over the raw wire
    protocol confirming the `filter` selection value correctly parsed the
    `-F` argument into 5 selection options (`clear`/`B`/`V`/`R`/`I`) with
    `clear` (index 0) as the active value. Ran for several minutes with
    periodic unauthorized-command probes correctly rejected, no crash.
15. DONE - Andor CCD driver (`base/camd/andor/`), built for real against
    the actual proprietary vendor SDK (available on this system, gated
    behind `BASE_ANDOR_SDK_DIR` - see "Andor" section below for the full
    writeup, including two vendor-SDK-specific CMake build wrinkles that
    had to be solved). **Smoke-tested further than any other vendor item
    in this port**: real compile+link against the vendor `.so`, and a real
    (if hardware-less) run that reaches `Initialize()` and fails cleanly
    with the expected SDK error code instead of crashing.
16. DONE - GXCCD (Moravian Instruments) CCD driver (`base/camd/gxccd/`),
    same treatment as Andor - built for real against the actual vendor SDK
    (also available on this system, gated behind `BASE_GXCCD_SDK_DIR` -
    see "GXCCD" section below). Cleaner to wire up than Andor (no
    SONAME/symlink wrinkle). **Smoke-tested**: real compile+link, and a
    real hardware-less run that reaches `gxccd_initialize_usb()` and fails
    cleanly (`cannot find device with id 0`) instead of crashing.
17. DONE - TEFO focuser driver (`base/focusd/tefo/`), the focuser
    actually in use at D50 - see "Focusd-tefo" section below. First driver
    needing a Connection transport variant beyond plain TCP
    (`rts2core::ConnUDP`/`ConnNoSend`, ported into the kernel for this),
    and **smoke-tested further than any other driver in this port**: the
    UDP wire protocol was fully exercised end-to-end against a small
    script simulating the real TEFO daemon, not just a hardware-less
    failure path.
18. DONE - D50 mount driver (`base/teld/d50/`), the mount actually in
    use at Ondrejov/Astrolab D50 - see "Teld-d50" section below for the
    full dependency chain (~4850 lines: `ConnSerial`/`ConnEthernet` in the
    kernel, `TGDrive`/`ConnREMOTES`/`Fork` in `base/teld/`, then the
    driver itself). Biggest single port in this initiative so far.
    **Found and fixed a real crash bug** in the classic tree while
    smoke-testing (uninitialized-buffer segfault on raw-socket init
    failure - see that section for the full writeup).
19. DONE - FLI focuser + filter wheel drivers (`base/focusd/fli/`,
    `base/filterd/fli/`) - see "FLI" section below. Two libfli copies
    existed on this system; the first one tried (`libfli-1.32`, an old
    raw-usbfs-based snapshot) needed vendor-library patches to build at
    all and was missing an API the classic driver needs
    (`FLIGetSerialString`) - the user then pointed at the real one
    (`~/src/fliusb/libfli`, using FLI's own `fliusb` kernel-module USB
    driver), which has the complete API, so the port ended up completely
    faithful with no workarounds once switched. **Smoke-tested**: both
    compile and link against the real SDK, and both correctly report "No
    device found!" and exit cleanly with no FLI hardware attached.
20. DONE - D50 weather/sensor stack, all six sensors the user listed
    (`base/sensord/`) - see "Sensord" section below. Also closed the
    `conntcp` piece of task 5, needed by `nut`. **Two genuine classic-tree
    bugs found and fixed while porting**: an uncaught-exception crash in
    `nut.cpp` when the NUT daemon is unreachable, and a fan-power
    readback/stack-overflow pair of bugs in `d50-wfunit.cpp` - see
    `UPSTREAM_BUGS.md`. **Smoke-tested**: `system` builds and links; the
    four serial-based sensors (`bart-rain`, `cloud4`, `aws-mlab`,
    `d50-wfunit`) each fail cleanly with a logged error against a
    nonexistent `/dev` path; `nut` (after the fix) fails cleanly with a
    logged error against an unreachable host:port instead of crashing.
21. DONE - dome-d50, the last driver actually running at D50
    (`base/dome/`) - see "Dome-d50" section below. **One more genuine
    classic-tree bug found and fixed**: `Ford::processOption()` discarded
    `Dome::processOption()`'s return value - see `UPSTREAM_BUGS.md`.
    **Smoke-tested**: builds and links; fails cleanly with a logged error
    against a nonexistent serial device (no crash); `--help` and normal
    option parsing both verified interactively.

**Everything currently in the tree builds and all tests pass** (`ctest`:
7/7 at last check - hoststring, libnova_cpp, config, value_header,
connection_prereqs, app, block_connection). No new automated test was added
for Daemon/Device/MultiDev/Centrald/Camera/Dummy - they were instead
verified by actually running the daemons and driving them over the real
wire protocol (see tasks 9 and 10 above). **A real ctest-based integration
test (spawn centrald + dummy, connect, exchange a few protocol commands,
assert on the response) would be a genuinely valuable thing to add now** -
none exists yet, everything past task 6 has been verified by manual smoke
test rather than an automated one.

## Files ported (base/kernel/include + base/kernel/src)

Utility/config layer: `hoststring`, `status`, `error`, `radecparser`,
`utilsfunc`, `riseset`, `rts2format`, `timestamp`, `libnova_cpp`, `proto`,
`message`, `logstream`, `pid`, `objectcheck`, `iniparser`, `configuration`.

App/CLI: `app`, `option`, `askchoice` (all real, not placeholders).

Value system: `value`, `valuestat`, `valueminmax`, `valuerectangle`
(unique_ptr-owned members), `valuearray`.

Bus core: `event`, `object`, `serverstate`, `centralstate`, `valuelist`,
`data`, `command` (trimmed), `connection`, `networkaddress`, `connuser`,
`devclient` (base only), `block`, `connnosend`, `connudp` (added for
`focusd-tefo`), `connserial`, `connethernet` (added for `teld-d50` - see
those sections).

Daemon/device framework: `daemon`, `device` (`DevConnection`,
`DevConnectionMaster`, `CommandRegister`, `CommandDeviceStatusInfo`,
`Device`), `multidev` (`MultiDev`, `MultiBase`), `scriptdevice`
(`ScriptDevice` - generic script-progress-tracking `Device` subclass, used
as `Camera`'s base; lives in the kernel rather than `base/camd/` because
it's namespaced `rts2core` and not camera-specific in the classic tree).

CLI-client infra: `client` (`Client`, `ConnClient`, `ConnCentraldClient`,
`CommandLogin`) - the client-side counterpart to `Device`, used by
`rts2-mon` and any future CLI tool, not by centrald or device daemons.

Display/formatting: `displayvalue` (`getDisplayValue`), `ProgressIndicator`
(appended to `utilsfunc.h`, where it lived in the classic tree too) - both
added for `rts2-mon`, both generic enough to belong in the kernel rather
than the monitor itself.

Source of truth for each: the classic-tree file of the same name in
`../include/*.h` and `../lib/rts2/*.cpp`.

## Centrald (base/centrald/ - separate from kernel)

`base/centrald/include/centrald.h` + `base/centrald/src/centrald.cpp`
(`Centrald`, `ConnCentrald`) - source of truth is
`../src/centrald/centrald.h`/`.cpp` in the classic tree (note: classic
centrald lives under `src/centrald/`, not `include/` + `lib/rts2/` like the
kernel pieces). Builds as the `rts2-centrald` executable via its own
`CMakeLists.txt`, linking against the `base_kernel` static library - this
is the first piece of base that lives in its own directory and links
against the kernel rather than being part of it, matching the layout from
the original design artifact.

Dropped the classic tree's dead `#ifndef RTS2_HAVE_FLOCK` branch in
`Centrald::init()` (same reasoning as `daemon.cpp`'s lockfile handling -
`RTS2_HAVE_FLOCK` is always defined on modern Linux, so the branch was
already dead code in the classic build too) - `init()` now just calls
`lockFile()` directly after `reloadConfig()`.

## Camd (base/camd/ - shared Camera base + first driver)

`base/camd/include/camd.h` + `base/camd/src/camd.cpp` (`Binning2D`,
`DataType`, `FilterVal`, `Camera`) build a `base_camd` static library that
any camera driver links against, alongside `base_kernel` - source of truth
is `../include/camd.h` + `../lib/rts2/camd.cpp` in the classic tree.
`base/camd/dummy/dummy.cpp` builds the `rts2-camd-dummy` executable
(source of truth `../src/camd/dummy.cpp`), linking against both. This
mirrors the centrald layout: kernel (bus) -> driver-family base (camd) ->
specific driver (dummy), matching the tier model from the design artifact
(`dummy` is a standard/kernel-tier driver - no vendor SDK needed).

Three things were deliberately deferred/dropped porting `camd`/`dummy`,
each because they're genuinely optional and off by default, not because
they were hard:

- **SEP star-finding** (`Camera::findSepStars`): the classic implementation
  links unconditionally against the vendored `sep` (source-extractor-as-
  library) headers at `../include/sep/sep.h`. `sepFind` defaults to false -
  it's a convenience feature, not needed for exposure/readout - and SEP is
  a vendor-tier dependency by the same driver-tier model used for the
  original driver inventory. `findSepStars` is declared in `camd.h` (so the
  public API shape matches the classic tree) but stubbed to a no-op in
  `camd.cpp`. Port for real if/when a driver actually needs star-finding.
- **Filter wheel / focuser `DevClient` subclasses** (classic
  `ClientFilterCamera`/`ClientFocusCamera` in `lib/rts2/cliwheel.*`/
  `clifocuser.*`, ~290 lines total): deferred the same way as the rest of
  the `DevClient` family (see below) - `Camera::createOtherType()` falls
  through to the base implementation for `DEVICE_TYPE_FW`/`DEVICE_TYPE_FOCUS`
  instead of instantiating them, exactly like `Device::createOtherType()`
  already does for every device type. **Important nuance**: the event
  codes and structs those two classes communicate through
  (`EVENT_FILTER_START_MOVE`, `EVENT_FOCUSER_GET`, `struct filterStart`,
  `struct focuserMove`, etc.) are *not* deferred - `Camera`'s own
  `setFilterNum`/`getFilterNum`/`setFocuser`/`getFocPos` use them
  unconditionally regardless of whether a wheel/focuser is configured, so
  they're declared directly in `camd.h` with a note explaining why. Without
  a wheel/focuser device configured (dummy never configures one), these
  `postEvent()` calls simply find no listener and return early - verified
  this is dummy's actual code path by grepping `dummy.cpp` for
  `wheelDevices`/`focuserDevice`/`createFilter` (no hits).
- **Direct-FITS-write readout path** (`dummy.cpp`'s `fitsTransfer` option,
  `#include "rts2fits/image.h"`): pulls in the whole rts2fits/cfitsio
  image-writing subsystem, a separate frontend-adjacent library, not part
  of the backend this port is building. Defaulted to false in the classic
  tree anyway. Dropped the value and the `image != NULL` branches in
  `doReadout()` - the real path being proven (TCP/IP readout over the wire
  protocol) is fully preserved and is what was actually smoke-tested.
  `LONGLONG` (a cfitsio typedef pulled in transitively by that header) was
  replaced with `int64_t`, its actual underlying type on this platform.

**A latent bug was found, not fixed, in classic `Camera::updateStatistics`**
(ported as-is into `camd.h`): the mode-histogram array is sized
`1 << (sizeof(t)*8)` bytes, which is fine for byte/short pixel types but is
undefined behavior (shift by >= type width) for `int64_t`/`double` pixel
types - the compiler warns `left shift count >= width of type` at
`camd.h:1115`/`1118`. This only triggers if a user explicitly switches
`data_type` away from the default (`USHORT`) to `LONG`/`LONGLONG`/`FLOAT`/
`DOUBLE`, which the smoke test didn't do. Not fixed here because a correct
fix means redesigning what "mode" means for float/double/wide-int pixel
data (clamp a range? skip mode calc for those types?), which is a real
design decision, not a one-line correctness fix - flagging it rather than
silently carrying it forward or silently patching around it.

## Kernel additions made for the monitor

Two things were added to the kernel specifically because `rts2-mon` needed
them - both generic, not monitor-specific, so they live in `base/kernel/`:

- **`displayvalue.h`/`.cpp`** (`rts2core::getDisplayValue`) - pretty-prints
  a `Value` using its display-type flags (RA/Dec, degrees, percents, hex,
  byte/KMG sizes, time intervals, on/off). Ported verbatim from
  `../include/displayvalue.h` + `../lib/rts2/displayvalue.cpp`.
- **`ProgressIndicator`**, added to the end of `utilsfunc.h` (it lived
  there in the classic tree too, not in its own file) - renders a `#`-filled
  progress bar with a rotating spinner character to an `ostream`. One
  deliberate behavior change: the classic header declared the spinner
  position/glyph table as file-scope `static` globals, so every
  translation unit that included the header got its *own* independent
  counter. Moved both to function-local `static`s inside the (already
  inline, header-defined) friend `operator<<` - now one counter shared
  program-wide, which is what a rotating "busy" spinner is presumably
  meant to be. Purely cosmetic either way.
- **`CommandChangeValue`**, added to `command.h`/`command.cpp` (see the
  updated deferral note at the top of `command.h`) - builds a
  `PROTO_SET_VALUE` wire command from a value name/operator/operand(s). No
  `DevClient` dependency (unlike the ~45 deferred `Command` subclasses), so
  it didn't need to wait for a specific driver - `nvaluebox.cpp`'s
  interactive value-editing dialogs need it directly.

## Monitor (base/monitor/ - rts2-mon, full ncurses TUI)

Ported in full: `nlayout`, `nwindow`, `daemonwindow` (`NSelWindow`,
`NDevListWindow`, `NCentraldWindow`), `nstatuswindow`, `nmsgwindow`,
`ncomwin`, `nmenu`, `nmsgbox`, `nwindowedit`, `nvaluebox` (the value-editing
dialogs - one class per `Value` subtype), `ndevicewindow` (the
per-connection value list + search/filter), and `nmonitor` (the `NMonitor`
app class itself, `main()`). Source of truth for each is the identically-
named file in `../src/monitor/`. Builds as `rts2-mon` via
`base/monitor/CMakeLists.txt`, linking against `base_kernel` and system
ncurses (`pkg_check_modules(NCURSES REQUIRED ncurses)` - found cleanly on
this system, unlike libnova there's a working `.pc` file).

Two classic dependencies were deliberately dropped, both documented inline
in `base/monitor/include/nmonitor.h`:

- **`rts2fits/devcliimg.h`** (`DevClientCameraImage`) - grep-verified dead
  include in the classic tree: `#include`d in `nmonitor.h` but
  `DevClientCameraImage` is never instantiated anywhere in
  `src/monitor/*.cpp`. Dropping it avoids pulling in the whole
  rts2fits/cfitsio image-writing subsystem for nothing - same category of
  finding as the `iniparser.h`/`value.h` and `block.cpp`/`client.h` dead
  includes found earlier in this project.
- **`SimbadTarget`** (`../include/simbadtarget.h` +
  `../lib/rts2/simbadtarget.cpp`) - resolves a target name typed as a
  positional command-line argument against the network SIMBAD service, so
  `rts2-mon <target-name>` can auto-slew a mount on startup. Requires
  libxml2 + RTS2's own bundled `xmlrpc++` HTTP client - a real but
  peripheral feature, unrelated to bus monitoring, and squarely a
  vendor/optional-tier dependency by the same driver-tier reasoning used
  throughout this port. Dropped along with it: `NMonitor::processArgs()`
  (positional args now fall through to `App::processArgs()`'s default,
  which cleanly errors instead of attempting a lookup), the `tarArg`
  member, and the `DEVICE_TYPE_MOUNT` + `CommandMove` special case in
  `NMonitor::createOtherType()`.

Also dropped as genuinely dead legacy-platform branches (same convention as
elsewhere in this port): the classic tree's autoconf-detected
`RTS2_HAVE_CURSES_H`/`RTS2_HAVE_NCURSES_CURSES_H` choice between
`<curses.h>` and `<ncurses/curses.h>` (this system always has `<curses.h>`
at the top include level - used directly), the `RTS2_HAVE_XCURSES` block
in `nmonitor.cpp` (`XCursesProgramName` - PDCurses/X11 curses backend, not
applicable on Linux ncurses), the `__CYGWIN__` guard around
`ESCDELAY = 0;` (Cygwin isn't a target), and an `#ifdef DEBUG`/`#else` pair
in `ndevicewindow.cpp`'s `printState()` (kept only the non-debug branch -
`DEBUG` is never defined in this build). `RTS2_PACKAGE_VERSION` in the
About box became `BASE_VERSION` from the generated config header.

## Teld (base/teld/ - Telescope base + Dummy driver)

`base/teld/include/teld.h` + `base/teld/src/teld.cpp` (`rts2teld::Telescope`,
~1480 header lines / ~2000 impl lines) build a `base_teld` static library,
source of truth `../include/teld.h` + `../lib/rts2tel/teld.cpp`.
`base/teld/dummy/dummy.cpp` builds `rts2-teld-dummy` (source of truth
`../src/teld/dummy.cpp`). Same three-tier layout as camd: kernel -> driver-
family base (teld) -> specific driver (dummy).

Three subsystems deliberately deferred, all documented inline with
`// base note:` comments at their point of use:

- **TLE/satellite propagation** (NORAD SGP4/SDP4/SGP8/SDP8, the `pluto`
  library, ~3100 lines). `base/teld/include/pluto/norad.h` ports only the
  `tle_t` struct + extern "C" declarations (needed for the `Telescope::tle`
  member's type) - no implementation is linked. `pluto/observe.h`/`.cpp`
  **were** ported for real (`lat_alt_to_parallax`, `observer_cartesian_coords`,
  `get_satellite_ra_dec_delta`) since they're self-contained geometry with
  no dependency on the rest of `pluto`, and `initValues()` calls
  `lat_alt_to_parallax()` unconditionally regardless of whether TLE tracking
  is ever used. `calculateTLE()`/`moveTLE()`/`parseTLE()` in `teld.cpp` are
  stubbed to log an error and fail safely (`DEVDEM_E_PARAMSVAL`) instead of
  calling into SGP4 - a `move_tle` command against `rts2-teld-dummy` will be
  rejected cleanly, not crash.
- **Concrete pointing models** (`GPointModel`/`TPointModel`, ~1090 lines).
  `base/teld/include/telmodel.h` ports the abstract `rts2telmodel::TelModel`
  interface in full (pure virtual, no `.cpp` needed) - `Telescope::model`
  stays real-typed and `applyModel`/`computeModel`/`setModel()` all work
  against it. The two concrete implementations aren't ported; `init()`
  and `signaledHUP()` log an error instead of silently ignoring
  `--rts2-model`/`--t-point-model` if a user actually passes one.
  `loadModelStream()` is stubbed the same way. `Dummy` never uses a model
  file, so this doesn't affect the smoke-tested driver.
- **Cupola/rotator `DevClient` subclasses** (`ClientCupola`/`ClientRotator`) -
  deferred like camd's `ClientFilterCamera`/`ClientFocusCamera`.
  `createOtherType()` falls through to the base `Device::createOtherType()`
  for `DEVICE_TYPE_CUPOLA`/`DEVICE_TYPE_ROTATOR` (still tracks the
  connection in `cupolas`/`rotators`). `CommandCupolaSyncTel` (added to
  kernel `command.h`/`.cpp` - see below) has no `DevClient` dependency, so
  `startCupolaSync()`'s core command-sending logic is fully intact even
  with no `ClientCupola` connected; only the client-side sync-confirmation
  event callback is lost, which is inert without a real cupola anyway.

**Kernel addition made for teld**: `CommandCupolaSyncTel` (`command.h`/
`.cpp`) - builds the `synctel <ra> <dec>` wire command via
`Block::queueCommandForType(DEVICE_TYPE_CUPOLA, ...)`, no `DevClient`
dependency (unlike `CommandCupolaNotMove`, which stays deferred).

**Not a deferral, a build-target decision**: the classic `teld.h`/`teld.cpp`
carry the whole class in two flavors selected by `#ifdef RTS2_LIBERFA` - an
ERFA/SOFA-based coordinate-transform path and a manual libnova-based
fallback. liberfa isn't installed on this system and base's CMake has no
detection for it, so only the non-ERFA path is ported. This is a complete,
correct implementation on its own, not a stub - add ERFA `find_package`
detection to `kernel/CMakeLists.txt` and restore the `#ifdef` split if a
build with it is ever needed. Same reasoning applied to `dummy.cpp`'s
`runTracking()` (`getEraUTC()` vs. manual `ln_get_julian_from_sys()`).

Also dropped: the dead `useErfa` (`ValueBool*`) member - grep-verified
declared but never `createValue()`'d or read anywhere in classic
`teld.cpp`.

**Design decision for GEM/Fork/AltAz (not yet implemented - no driver needs
it yet, `base/teld/` only has `Dummy`, which extends `Telescope`
directly)**: the classic tree has `GEM`, `Fork`, `AltAz` (`../include/
gem.h`/`fork.h`/`altaz.h`, `../lib/rts2tel/gem.cpp`/`fork.cpp`/`altaz.cpp`)
as three parallel siblings of `Telescope`. This conflates two independent
properties - coordinate frame (HA/Dec "equatorial" vs. Az/Alt "alt-az") and
meridian-flip handling (whether/how the mount flips) - into one flat
three-way class choice that only actually varies along the first axis.
`GEM` and `Fork` are both HA/Dec equatorial mounts doing near-identical
core math (`Fork::sky2counts`, 206 lines total, vs. `GEM::sky2counts`,
957 lines total - the extra ~750 is genuine new flip-preference/
trajectory-safety logic, but the ~150-line HA/Dec-to-counts skeleton
itself is duplicated, not shared); `AltAz`'s own doc comment calls it an
"abstract AltAz **fork** mount", i.e. the original author already
recognized alt-az mounts are mechanically fork-like, yet `AltAz` shares
nothing with the `Fork` class. Concrete cost: `src/teld/sitech-gem.cpp` and
`src/teld/sitech-altaz.cpp` are two entirely separate driver files for the
identical SiTech controller hardware, forced to duplicate all
protocol-handling code because `GEM` and `AltAz` share no common ancestor
below `Telescope` to hang it on.

User's call on the fix (2026-07-11): flip is **not** a fork-vs-GEM
mechanical fact - "nobody says there cannot be flip in a fork mount, it's
just that LX200 didn't use that option, and some GEMs don't flip either" -
so it should be a **property, not a subclass**. Confirmed in the code:
`GEM` already has exactly this as a `flipping` `ValueSelection`
(`gem.cpp:362-370` - `shortest`/`same`/`opposite`/`west`/`east`/`longest`/
`cw down`/`cw up`), just missing a `never`/`none` entry - `Fork` exists
only to stand in for that one missing enum value (and does it
incompletely, since `Fork::sky2counts` still computes and applies a flip).
**Planned shape when this is next touched**: collapse `GEM`+`Fork` into
one `Equatorial`-ish class (name TBD) with flip capability/strategy as a
property (add `never`/`none` to the `flipping` selection), keep `AltAz` as
the true sibling on the other side of the equatorial/alt-az split. Port
whichever of `GEM`/`Fork`/`AltAz` is needed first under this corrected
shape rather than reproducing the classic three-way split as-is.

## Focusd (base/focusd/ - shared Focusd base + first driver)

`base/focusd/include/focusd.h` + `base/focusd/src/focusd.cpp`
(`rts2focusd::Focusd`) build a `base_focusd` static library, source of
truth `../include/focusd.h` + `../lib/rts2/focusd.cpp`.
`base/focusd/dummy/dummy.cpp` builds `rts2-focusd-dummy` (source of truth
`../src/focusd/dummy.cpp`). Same three-tier layout as `camd`/`teld`.

Unlike `camd`/`teld`, **nothing was deferred** - `Focusd` is a small
(~330-line) self-contained class with no vendor SDK, network service, or
optional-subsystem dependency to defer. Ported essentially verbatim,
`NULL`->`nullptr` only. This is the first driver family in base ported
with zero deferrals, confirming the tier model correctly predicts driver
complexity (focuser < camera/telescope in dependency footprint).

## Filterd (base/filterd/ - shared Filterd base + first driver)

`base/filterd/include/filterd.h` + `base/filterd/src/filterd.cpp`
(`rts2filterd::Filterd`) build a `base_filterd` static library, source of
truth `../include/filterd.h` + `../lib/rts2/filterd.cpp`.
`base/filterd/dummy/dummy.cpp` builds `rts2-filterd-dummy` (source of
truth `../src/filterd/dummy.cpp`). Same layout as `camd`/`teld`/`focusd`.

**No deferrals** - same reasoning as `focusd`: `Filterd` is small
(~210 lines), self-contained, no vendor SDK or optional-subsystem
dependency. This is the third driver family in a row (after `focusd`) to
port with zero deferrals - the tier model's "simple peripheral" drivers
(focus/filter wheels) are consistently lightweight compared to
camera/telescope base classes. The classic `DevClientFilter` (used by
cameras to auto-select filters) is still deferred - see "Deliberately
incomplete ports" below - `Filterd` itself has no `DevClient` dependency.

## Andor (base/camd/andor/ - real vendor SDK, SDK-gated build)

`base/camd/andor/andor.cpp` (source of truth `../src/camd/andor.cpp`, the
only git-tracked of four andor-related files in the classic tree - the
other three, `andor-bak.cpp`/`andor-newkin.cpp`/`andor.bak.cpp`, are the
user's own untracked WIP scratch files and were not touched or consulted).

Unlike every other vendor-tier item in this port (SEP, ERFA, TLE/pluto,
GPointModel/TPointModel), the Andor SDK is **not deferred** - a real
precompiled copy (headers + x86_64 `.so`, copied from the D50 telescope's
install, not installed system-wide) exists on this system at
`/home/mates/src/andor`, and its `.so` links cleanly (`ldd` resolves
against only standard system libs). So this was ported and **built for
real** against the actual vendor SDK, gated behind an explicit CMake cache
variable (`BASE_ANDOR_SDK_DIR`, empty/skipped by default) added to
`camd/CMakeLists.txt` - deliberately *not* auto-searched, matching the
classic tree's opt-in `--with-andor=<path>` and the user's own reasoning:
nobody has a $40k CCD and its vendor SDK without knowing it, so there's no
point scanning default install prefixes for it.

Two build wrinkles specific to this vendor SDK, both handled in
`camd/CMakeLists.txt`/`camd/andor/CMakeLists.txt`:
- The SDK ships only fully-versioned library filenames (e.g.
  `libandor-stdc++6-x86_64.so.2.98.30010.0`) with no unversioned symlink -
  normally created by the vendor's `install_andor` script under
  `/usr/lib`, which this port deliberately does not run (root-only, and
  would touch system directories for a symlink). `file(GLOB ...)` finds
  the real file directly instead of using `find_library`.
- The `.so`'s embedded SONAME is `libandor.so.2`, which nothing on this
  system provides - `ldd` reported `libandor.so.2 => not found` even with
  a correct `RPATH` pointing at the SDK's `lib/` dir, because RPATH
  resolution needs a file matching the SONAME exactly. Fixed by having
  CMake recreate that missing symlink itself inside the build tree
  (`camd/andor_lib/libandor.so.2 -> <real SDK .so>`, via `file(CREATE_LINK
  ... SYMBOLIC)`) and pointing `RPATH` there instead - reproduces what the
  vendor installer would have done, scoped to the build directory instead
  of `/usr/lib`.

Modernization applied while porting (all documented inline with
`// base note:` at the top of the file): dropped the pre-GCC3
`_GNUC3_`/`<iostream.h>` fallback and the unused `<fstream>` include
(grep-verified: no `ifstream`/`ofstream` anywhere in the file);
`NULL`->`nullptr`; bare `isnan`->`std::isnan` (matching convention used
elsewhere in base); dropped six methods that were declared in the class
but never defined and never called anywhere (`setADChannel`, `printInfo`,
`printCapabilities`, `printNumberADCs`, `printHSSpeeds`, `updateHSSpeeds`,
`printVSSpeeds`) - confirmed dead in the classic tree too, since
`rts2-camd-andor` links from this one source file alone (no other
translation unit could supply definitions).

**Smoke-tested for real, further than any other vendor-tier item in this
port**: compiles and links against the actual vendor `.so`; running it
with `-i` (no camera attached) reaches the real `Initialize()` SDK call,
gets back a real error code (`20003` = `DRV_VXDNOTINSTALLED` - no
Andor kernel driver/hardware present, exactly as expected on this
machine), and the daemon framework correctly propagates that failure and
exits cleanly (`cannot init daemon, exiting`) instead of crashing or
hanging. What was **not** and cannot be verified here: any actual data
acquisition, temperature control, or other hardware-dependent behavior -
that needs the physical camera.

## GXCCD (base/camd/gxccd/ - real vendor SDK, SDK-gated build)

`base/camd/gxccd/gxccd.cpp` (source of truth `../src/camd/gxccd.cpp`, the
only git-tracked of three gxccd-related files in the classic tree - like
Andor, the other two, `gxccd.cpp.old`/`gxccd.cpp.zaloha`, are the user's
own untracked scratch files, not touched or consulted). Driver for
Moravian Instruments cameras via their closed-source `gxccd` library.

Same situation as Andor - the vendor SDK is genuinely present on this
system (`/home/mates/libgxccd-0.9.0`: headers + both a static `libgxccd.a`
and a shared `libgxccd.so`), so this was built for real rather than
deferred/stubbed, gated behind an `BASE_GXCCD_SDK_DIR` CMake cache
variable (empty/skipped by default, matching the classic tree's
`--with-gxccd=<path>` and the same "nobody has this without knowing it"
reasoning as Andor). Also depends on `libusb-1.0`, found via a working
system `pkg-config` file (unlike libnova).

**Update (2026-07-16, Debian-packaging pass):** originally wired up with
plain `find_library()`, which resolves `.so` before `.a` by default -
despite this section's own earlier note that the classic tree links the
static `.a`, the port was actually linking `libgxccd.so` + an `RPATH` into
the SDK tree (same wrinkle as Andor). Fixed by scoping
`CMAKE_FIND_LIBRARY_SUFFIXES` to `.a` around that one `find_library()`
call (`camd/CMakeLists.txt`), so `GXCCD_LIBRARY` now resolves to the
static archive and links straight into the binary - no RPATH, no vendor
`.so` to ship alongside it (`camd/gxccd/CMakeLists.txt`'s RPATH block
removed accordingly). Verified with `ldd`/`readelf -d`: the built
`rts2-camd-gxccd` has no RPATH and no `libgxccd.so` dependency, only
ordinary system libraries (`libusb-1.0`, `libnova`, libc/libstdc++), with
37 `gxccd_*` symbols confirmed statically embedded (`nm`). `libgxccd.a`'s
license explicitly permits binary redistribution, so this is a real,
packageable static link - motivated by wanting `rts2-camd-gxccd` in a
"statically-linked control library, no bundled vendor blob" base package
(see the Debian-packaging discussion, [[base-porting-initiative]]).

Modernization applied while porting (documented inline with an
`// base note:` at the top of the file): `NULL`->`nullptr` throughout.
`GXCCD_HAS_RESET_FILTERS` (guarding the `reset_filters` command) is never
defined anywhere in the classic tree's `configure.ac`/`Makefile.am`, and
this SDK version's `gxccd.h` doesn't declare `gxccd_reset_filters()`
either - that whole branch is dead in both trees. Kept as-is (same
treatment as Andor's `TEMP_SAFETY`) in case a build ever targets a newer
SDK version that has the function.

**Smoke-tested for real, same caliber as Andor**: compiles and links
against the actual vendor library; running it with `-i -p 0` (no camera
attached) reaches the real `gxccd_initialize_usb()` SDK call, gets back a
real null (no USB camera found), and the daemon framework correctly
reports `cannot find device with id 0` and exits cleanly instead of
crashing. What was **not** and cannot be verified here: actual data
acquisition, temperature control, or filter-wheel control - that needs a
physical camera.

## Focusd-tefo (base/focusd/tefo/ - real D50 focuser, first Connection-transport port)

`base/focusd/tefo/tefo.cpp` (source of truth `../src/focusd/tefo.cpp`) -
driver for the TEFO focuser (Universal Scientific Technologies) actually
in use at D50. Talks a plain UDP text protocol to a separate TEFO daemon,
no vendor SDK involved - but needed `rts2core::ConnUDP`, which didn't
exist in base yet (part of task 5's still-unfinished "Connection
transport variants": `connnosend`/`connnotify`/`connudp`/`conntcp`/
`connfork`).

Ported into the kernel first, ahead of the driver that needed them:
- `kernel/include/connnosend.h` + `src/connnosend.cpp` (`ConnNoSend`) -
  source of truth `../include/connnosend.h` + `../lib/rts2/connnosend.cpp`.
  Trivial (~40 lines): a `Connection` with `sendMsg()` disabled, used for
  connections that only ever receive.
- `kernel/include/connudp.h` + `src/connudp.cpp` (`ConnUDP`) - source of
  truth `../include/connection/udp.h` + `../lib/rts2/connudp.cpp`,
  flattened out of the classic tree's `connection/` subdirectory to match
  this port's flat `kernel/include/` convention (no other header lives in
  a subdirectory here either). One modernization: classic `bzero()` ->
  `memset()` in `init()`.

`connnotify`/`conntcp`/`connfork` remain deferred - still nothing ported
needs them. Task 5 stays open until all five transport variants exist,
but is no longer blocking any specific driver.

**Smoke-tested further than any other driver in this port** thanks to the
network protocol being fully simulable without hardware: ran the built
`rts2-focusd-tefo` three ways -
1. without `-t` - clean rejection ("You must specify IP:port...").
2. with `-t` pointing at a UDP port nobody listens on - real socket
   created, real `sendto`/`recvfrom` retry loop ran for the full
   `UDP_RETRY_ATTEMPTS`/`UDP_RESPONSE_TIMEOUT` window, then failed
   cleanly ("Didn't get response from focuser daemon...").
3. with `-t` pointing at a small Python script faking the TEFO daemon's
   wire protocol (`<ID> <cmd> <status> <motor-status> <last-result>
   <actual-pos> <set-pos> <time-to-moveend>`) - `initHardware()` and
   `initValues()` both succeeded, `sscanf`-based response parsing and the
   `idle`/`moving`/`calibrating` state-string matching all worked
   correctly, and the daemon ran normally (no error output) until killed.
   This exercises real network I/O and real protocol-parsing logic more
   thoroughly than any hardware-dependent driver so far, since the "far
   end" was fully reproducible.

## Teld-d50 (base/teld/d50/ - the D50 mount, ~4850 lines across 4 layers)

`base/teld/d50/d50.cpp` (source of truth `../src/teld/d50.cpp`, the sole
git-tracked file among the D50 mount sources - untracked scratch files in
the classic tree were not touched or consulted) - the actual mount
currently running at Ondrejov/Astrolab D50. By far the largest single
port in this initiative: needed four new dependency layers on top of the
already-ported kernel/teld, ~4850 lines total. User-directed pacing: all
four layers in one session (no pause between them), and no `sudo` for the
raw-socket smoke test (see the `ConnEthernet`/`ConnREMOTES` writeup
below) - test what's testable without privilege escalation, document the
rest.

Dependency chain, bottom to top:

- **`kernel/include+src/connserial.{h,cpp}`** (`ConnSerial`) - generic
  RS-232 connection (`ConnNoSend` subclass: opens a device node,
  `termios` configuration, `writePort`/`readPort` with optional end-char
  matching). Flattened from classic `include/connection/serial.h`, same
  convention as `connudp.h`. No dependency wrinkles - compiled clean.
- **`kernel/include+src/connethernet.{h,cpp}`** (`ConnEthernet`) - raw
  `AF_PACKET`/`SOCK_RAW` IEEE 802.3 ethernet connection, used by REMOTES
  units (custom I/O boards on D50's axes). Flattened from classic
  `include/connection/ethernet.h`. One build wrinkle: an added
  `#include <net/if.h>` (for `struct ifreq`) collided with `<linux/if.h>`
  already pulled in transitively by `<linux/if_packet.h>` (glibc vs.
  kernel-header `IFF_*`/`ifreq`/`ifmap`/`ifconf` redefinition errors) -
  removed the extra include, matching what the classic file relied on
  transitively. **Opens a raw socket, which needs `CAP_NET_RAW` (root) -
  this cannot be exercised for real on this development machine.**
- **`base/teld/include+src/tgdrive.{h,cpp}`** (`TGDrive`, extends
  `ConnSerial`) - RS-232 protocol to TGDrives servo motor controllers
  (D50's RA/DEC axes). Placed in `base/teld/` rather than the kernel
  since it's D50-specific despite the `rts2teld`-namespaced classic
  location. Compiled clean.
- **`base/teld/include+src/connremotes.{h,cpp}`** (`ConnREMOTES`, extends
  `ConnEthernet`) - custom checksummed request/response protocol to
  REMOTES I/O boards (power/pressure/stepper-generator control, absolute
  encoder readout). Placed in `base/teld/` for the same reason as
  `TGDrive`, despite being namespaced `rts2core` in the classic tree.
  **Found and fixed a real crash bug while smoke-testing** (see below).
- **`base/teld/include+src/fork.{h,cpp}`** (`Fork`, extends `Telescope`)
  - HA/Dec equatorial mount base class, ported faithfully per the user's
    explicit call (see the GEM/Fork/AltAz hierarchy discussion further up
    this file) - no redesign attempted, since D50 is the only Fork/GEM
    consumer in base so far. Compiled clean.
- **`base/teld/d50/d50.cpp`** (`D50`, extends `Fork`) - the driver
  itself: REMOTES-based servo power/backlash/worm-tracking control,
  "Successive Safe Targets" path-finding around the hard horizon and
  optional dome ceiling, dome-open interlock via a live connection to a
  `DEVICE_TYPE_DOME` device, RA/DEC autoguiding via worm-frequency
  modulation. Modernization: `NULL`->`nullptr`; dropped the
  `#ifndef RTS2_LIBERFA`/`#endif` guard pairs, keeping their body
  unconditional (same ERFA-removal decision as `teld.h`/`teld.cpp` -
  `RTS2_LIBERFA` is never defined in base, so these were always-true
  branches anyway); `DEBUG_EXTRA`/`DEBUG_BRUTAL` kept as opt-in developer
  logging toggles, same as classic (both commented out by default).

**Bug found and fixed while smoke-testing** (not a porting mistake - a
genuine latent bug in the classic tree, reproducible there too): running
`rts2-teld-d50` without `CAP_NET_RAW` correctly failed at
`ConnEthernet::init()`'s `socket()` call with `EPERM` - but then
segfaulted immediately after, because:
1. `ConnREMOTES::init()` discarded `ConnEthernet::init()`'s return value
   and unconditionally returned 0.
2. `D50::init()` never checked `remotesRA->init()`/`remotesDec->init()`'s
   result (unlike `raDrive->init()`/`decDrive->init()`, which *are*
   checked a few lines below).

   So a failed raw-socket open left `ConnEthernet`'s buffer pointers
   (`ethOutBuffer`/`ethInBuffer`/`ethOutData`/`ethInData`/`eh`) never
   allocated, and the very next register read/write on that connection
   (`remotesSetMCRegister`, called unconditionally right after) dereferenced
   them uninitialized. In real production use at D50 (root, real network
   interface) `ConnEthernet::init()` always succeeds, so this path was
   presumably never exercised there - it only surfaces when the raw
   socket fails to open, which is exactly what happens in this sandboxed,
   non-root environment. Fixed by propagating `ConnEthernet::init()`'s
   return value out of `ConnREMOTES::init()`, and checking
   `remotesRA->init()`/`remotesDec->init()` in `D50::init()` the same way
   `raDrive`/`decDrive` already were.

**Smoke-tested**: full chain compiles and links clean on the first
attempt (~4850 lines). Ran `rts2-teld-d50` with no RA/DEC serial devices
and no root: reaches the real `ConnEthernet::init()` call, gets a real
`EPERM` from `socket()`, and (after the bug fix above) the daemon
correctly reports the failure and exits cleanly instead of crashing. What
was **not** and cannot be verified here without root: any real REMOTES
register I/O, TGDrive serial communication, or actual mount movement -
that needs the physical D50 hardware and/or `CAP_NET_RAW`.

## FLI (base/focusd/fli/, base/filterd/fli/, base/camd/fli/ - real vendor SDK)

`base/focusd/fli/fli.cpp` (source of truth `../src/focusd/fli.cpp`),
`base/filterd/fli/fli.cpp` (source of truth `../src/filterd/fli.cpp`),
and `base/camd/fli/fli.cpp` (source of truth `../src/camd/fli.cpp`) -
drivers for Finger Lakes Instrumentation focusers/filter wheels/cameras.
Ported **completely faithfully**, `NULL`->`nullptr` only - no API gaps, no
workarounds needed, once pointed at the right SDK copy (see below).

`rts2-camd-fli` (2026-07-16) was added after the focuser/filter-wheel
pair, motivated by a real FLI CCD becoming available at FLORES - the
first live piece of hardware this cmake-based base/db tree may
actually run against. Same `camd.h`/`Camera` base as `dummy`/`andor`/
`gxccd`; every FLI symbol the classic driver calls (`FLIOpen`,
`FLIGetSerialString`, `FLIList`, `FLISetVBin`/`FLISetHBin`/
`FLISetImageArea`, `FLIGrabRow`, etc.) is present, unchanged, in the same
`/home/mates/src/fliusb/libfli` SDK already wired up for the other two
FLI drivers - ported line-for-line, no adaptation needed.

**Two libfli copies were on this system; the first one tried was the
wrong pick.** `/home/mates/libfli-1.32` looked like the obvious candidate
(a plain libfli source snapshot) but is an old one: its Linux USB backend
talks directly to the deprecated raw `usbfs`/`<linux/usb.h>` interface,
doesn't build against modern kernel headers, and - once patched to
build - turned out to be missing `FLIGetSerialString()` entirely (only
has the older `FLIGetSerialNum()`, a `long`). Patched it anyway (swapped
`<linux/usb.h>` for `<linux/usb/ch9.h>` + already-included
`<linux/usbdevice_fs.h>`, dropped the same dead include from
`libfli-filter-focuser.c`, added a missing `<unistd.h>` there) and got it
building, then wrote the driver ported against a `FLIGetSerialNum()`-based
workaround for the missing string variant, plus a `const_cast` for a
non-const `FLIOpen()` signature. **The user then pointed out the real SDK
was actually already sitting at `/home/mates/src/fliusb/libfli`** - a
complete, modern, already-built libfli using FLI's own `fliusb` Linux
kernel-module driver for USB I/O (custom ioctls via `fliusb_ioctl.h`/
`/dev/fliusb*`, not raw usbfs) - matching what the classic RTS2 driver
was actually written against: it has the real `FLIGetSerialString()` and
a const-correct `FLIOpen()`. Switched to it and **reverted both
workarounds** - the driver code is now a completely faithful port with no
adaptations. The `libfli-1.32` patches were left in place (harmless,
possibly still useful for the "older cameras" archaeology the user was
originally doing) but are no longer what base builds against.

Gated behind `BASE_FLI_SDK_DIR` (now pointed at
`/home/mates/src/fliusb/libfli`), detected once in the top-level
`base/CMakeLists.txt` (shared by `focusd/fli`, `filterd/fli`, and
`camd/fli` rather than duplicating the same `find_path`/`find_library`
three times) - empty/skipped by default, same opt-in reasoning as
Andor/gxccd (classic tree: `--with-fli=<path>`). Like the old snapshot,
this libfli's own `Makefile` puts `libfli.h` and `libfli.a` both at the
SDK root (no separate `include/`/`lib/`), so detection looks there
directly. No external link dependency beyond libc (`nm`-verified).

**Smoke-tested**: all three drivers compile and link clean against the
real SDK; running any of them with no FLI hardware attached reaches the
real `FLIList()` enumeration call (via the `fliusb`-kernel-module code
path this time), correctly reports "No device found!", and exits cleanly
- no crash. `rts2-camd-fli` specifically: `--help` lists all FLI-specific
options (`-D`/`-R`/`-B`/`-n`/`-N`/`-b`/`-l`/`-f`) correctly; run with
`-i --debug` and an unreachable `--server`, it logs
`Fli::init No device found!` then `cannot init daemon, exiting` and exits
0 - no crash, same shape as the focuser/filter-wheel smoke tests. (Note:
without `-i` the daemon logs to syslog rather than stdout/stderr and
produces no visible console output even though it ran and exited cleanly
- this is normal daemon behavior, not a bug; use `-i` for interactive
smoke-testing.)

## Sensord (base/sensord/ - Sensor/SensorWeather base + the six D50 sensors)

`base/sensord/include/sensord.h` + `src/sensord.cpp` - the `rts2sensord::
Sensor`/`SensorWeather` base classes (`DEVICE_TYPE_SENSOR`, weather-timeout
handling via `nextGoodWeather`). Completely faithful port, no changes.

The user asked for the six sensors actually deployed at D50: `system`,
`nut`, `cloud4`, `bart-rain`, `aws-mlab`, `d50-wfunit`. Each lives in its
own `base/sensord/<name>/` directory, building `rts2-sensor-<name>`:

- **`system`** (disk free space + load average) - no new Connection
  dependency, faithful port.
- **`bart-rain`** (Bart dome rain detector, raw `termios`/`ioctl` on a
  serial port) - no Connection class involved at all, faithful port,
  copied verbatim.
- **`cloud4`** (Kakona mrakomer cloud sensor, versions 4.0/4.1) and
  **`aws-mlab`** (mlab.cz Automated Weather Station) - both use the
  already-ported `ConnSerial`, faithful ports (only the `#include` path
  changed, `connection/serial.h` -> `connserial.h`).
- **`d50-wfunit`** (D50 WF-camera arduino unit: shutter/fans/temp/hum) -
  also `ConnSerial`. **Two genuine bugs found and fixed** (see
  `UPSTREAM_BUGS.md` for full writeups): (1) the fan-power readback stored
  the shutter-state character instead of the parsed fan-state integer;
  (2) `sscanf`'s `"%s"` conversion wrote into a bare `char` lvalue instead
  of a buffer - a stack buffer overflow on every call. Fixed both.
- **`nut`** (NUT UPS daemon client) needed a brand-new connection class:
  `rts2core::ConnTCP` (flattened from classic `include/connection/tcp.h` +
  `lib/rts2/conntcp.cpp` into `base/kernel/include/conntcp.h` + `kernel/
  src/conntcp.cpp`, same convention as `connserial`/`connudp`/
  `connethernet` - this also closes the `conntcp` piece of task 5).
  Faithful port of `ConnTCP` itself. **One genuine bug found and fixed**
  in `nut.cpp` (see `UPSTREAM_BUGS.md`): `initHardware()` only checked
  `connNUT->init()`'s return value, but `ConnTCP::init()` throws
  `ConnCreateError` on most connect failures (e.g. connection refused)
  rather than returning nonzero - an unreachable UPS daemon crashed the
  whole process with an uncaught exception. Fixed by wrapping the call in
  a `try`/`catch`, matching every other failure path in that function.

**Smoke-tested**: everything builds and links clean (`ctest`: still 7/7,
unaffected - these are new tests, not changes to existing ones). Ran each
driver standalone (no centrald, `--lock-prefix` pointed at a scratch dir):
the four serial-based sensors (`bart-rain`, `cloud4`, `aws-mlab`,
`d50-wfunit`) each correctly log "cannot open ..." against a nonexistent
`/dev` path and exit cleanly (`initHardware`/`init` failure -> daemon init
abort, no crash); `nut` (post-fix) correctly logs a connection-refused
error and exits cleanly against an unreachable `host:port` instead of
crashing; `system` needs no hardware at all and was run far enough to
confirm it doesn't crash (it blocks trying to reach a central server, as
expected for any daemon run standalone - same behavior verified for other
driver families in earlier tasks).

## Dome-d50 (base/dome/ - Dome base + FordConn/Ford + the D50 dome driver)

The last driver actually running at D50, per the user: "last driver to be
running on D50 is dome-d50" - a nonrotating enclosure (roof), not a
rotating cupola, hence the plain `Dome` base class rather than a `Cupola`
one.

`base/dome/include/dome.h` + `src/dome.cpp` - the `rts2dome::Dome` base
class (open/close state machine, weather-driven auto-close, `DOME_*`
status bits, `close_for`/`reset_next` commands). Faithful port, no changes.

`base/dome/include/connford.h` + `src/connford.cpp` - `rts2core::FordConn`,
the serial protocol for Martin Nekola's "Ford" dome-control board (`ZAP`/
`VYP` pin set/clear over `ConnSerial`, already-ported). Like `ConnREMOTES`
in the teld family, this is `rts2core`-namespaced in the classic tree but
specific to one hardware family, so it lives in `base/dome/include/`
rather than the kernel, not `kernel/include/connford.h`. Faithful port.

`base/dome/include/domeford.h` + `src/domeford.cpp` - `rts2dome::Ford`,
the abstract dome class wrapping `FordConn` with `Dome`'s open/close
lifecycle. **One genuine bug found and fixed** (see `UPSTREAM_BUGS.md`):
`Ford::processOption()`'s `default:` case called
`Dome::processOption(in_opt)` without returning its result, so any
option-parsing failure from the `Dome`/`Device`/`Daemon`/`App` chain was
silently discarded and the function always reported success. Fixed by
returning the actual result.

`base/dome/d50/d50.cpp` (source of truth `../src/dome/d50.cpp`) - the D50
enclosure driver itself: two output pins (open/close motor), two end-switch
inputs, plus a safety interlock that queries the mount (`teld`) device over
the wire protocol before allowing a close, parking it first if it's not
already in the dome-safe zone. Ported completely faithfully, `NULL`->
`nullptr` only.

**Smoke-tested**: full chain (`Dome`/`FordConn`/`Ford`/`D50`) compiles and
links clean on the first attempt. Ran `rts2-dome-d50` with no serial
device present: reaches the real `ConnSerial::init()` call, gets a clean
"cannot open serial port" error, and exits gracefully instead of crashing
- same failure-path pattern verified for every other serial-based driver
in this port. `--help` and normal option parsing (including the
`Ford::processOption` fix) were also exercised interactively. What is
**not** and cannot be verified here without the physical hardware: any
real Ford-board pin I/O, or the mount-interlock round trip (needs a real
`teld` connection).

## Dome-bart (base/dome/bart/ - the BART/SBT dome driver)

User-directed (2026-07-12): "let's fix drivers that are active at SBT:
dome-bart" - the second real-site dome deployment, at BART/SBT rather
than D50. Built entirely on the `Ford`/`FordConn`/`Dome` base already
ported for dome-d50 above, so this was a small, mechanical, one-file
port with zero new kernel/dome-tier work needed.

`base/dome/bart/bart.cpp` (source of truth `../src/dome/bart.cpp`,
~394 lines) - the `Bart:public Ford` driver. Same open/close-motor +
end-switch pattern as D50, plus BART-specific extras D50 doesn't have:
six controllable wall sockets (`socket1`-`socket6`, mapped to
`ZASUVKA_1`-`ZASUVKA_6` Ford output pins - cameras, mount, anti-dew
heating) that get set to fixed on/off patterns on `off()`/`standby()`/
`observing()` state transitions (`handle_zasuvky()`), plus per-socket
manual override via `setValue()`. Faithful port, `NULL`->`nullptr`
only - no bugs found, compiled and linked clean on the first attempt
(every symbol it needs - `ZAP`/`VYP`/`isOn`/`zjisti_stav_portu`/
`getPortState`/`setTimeout`/`maskState`/`DOME_DOME_MASK`/`DOME_CLOSED`/
`DOME_OPENED`/`USEC_SEC`/`RTS2_DT_HEX`/`RTS2_DT_ONOFF`/
`RTS2_VALUE_WRITABLE`/`ValueSelection::addSelVal` - was already present
from the dome-d50/kernel work).

`base/dome/bart/CMakeLists.txt` - new `add_executable(rts2-dome-bart
bart.cpp)` linked against `base_dome` + `base_kernel`, wired into
`base/dome/CMakeLists.txt` via `add_subdirectory(bart)` alongside the
existing `add_subdirectory(d50)`.

**Smoke-tested**: full tree builds clean, `ctest` still 7/7, `--help`
produces the expected `Ford`/`Dome`-inherited option list (`-f` serial
device, `--ignore-timeout`, `--notclose`, `--state-master`,
`--weather-can-open`, etc.) Not verified here (no physical hardware):
real Ford-board pin I/O or the wall-socket relay behavior.

## Focusd-aaf + confirming sensor-bart-rain/sensor-aws-mlab (base/focusd/aaf/ - SBT push continues)

User-directed (2026-07-12): "next is then sensor-bart-rain and
sensor-aws-mlab, focusd-aaf - no point in testing 3x if it builds,
these should be straightforward." Two of the three were already done:

- **`sensor-bart-rain`, `sensor-aws-mlab`**: both already ported and
  built as part of the D50 sensor stack (task 20, `base/sensord/`) -
  same generic sensord drivers, just deployed at a different site via
  CLI config, no new code needed. Rebuilt as part of this pass to
  confirm - still build clean, still part of `ctest`'s 7/7.
- **`focusd-aaf`** (new): `base/focusd/aaf/aaf.cpp` (source of truth
  `../src/focusd/aaf.cpp`, ~436 lines) - `AAF:public Focusd`, a
  reverse-engineered serial protocol driver for the ASA AAF focuser
  (`#`/`$`-delimited ASCII commands over `ConnSerial`, no vendor SDK).
  Faithful port, `NULL`->`nullptr` and `"connection/serial.h"`->
  `"connserial.h"` only - no bugs found, compiled and linked clean on
  the first attempt (every symbol it needs - `setFocusExtent`/
  `createTemperature`/`setIdleInfoInterval`/`focType`/`position`
  (`ValueDouble`, whose `setValueInteger` override correctly forwards
  to `setValueDouble` - checked given the mixed int/double usage in
  this file, not a bug)/`temperature`/`ConnSerial`/`DEVDEM_E_HW`/
  `DEVDEM_OK`/`DEVICE_ERROR_HW` - was already present from the
  dummy/tefo/FLI focuser work). `base/focusd/aaf/CMakeLists.txt` -
  `add_executable(rts2-focusd-aaf aaf.cpp)`, wired into
  `base/focusd/CMakeLists.txt` via `add_subdirectory(aaf)`.

**Smoke-tested**: per explicit user instruction, no per-driver manual
smoke test this time (all three were expected to be, and were,
straightforward) - just confirmed the full tree builds clean and
`ctest` stays 7/7 after adding `focusd-aaf`.

## Teld-gemini (base/teld/gemini/ - the Losmandy Gemini mount, SBT/BART)

User-directed (2026-07-12, "so it down to us and gemini2ser :)") - the
last actively-deployed SBT driver. Substantially bigger than the other
SBT ports this push (~2900 lines total): the classic `Gemini` driver
(`src/teld/gemini.cpp`, 2125 lines) extends `TelLX200`
(`include/rts2lx200/tellx200.h` + `lib/rts2lx200/tellx200.cpp`, 444
lines), a wholly new abstract base for this port - the first
LX200-protocol telescope, as opposed to D50's TGDrive/ConnREMOTES
protocol. Scoped before writing anything (grepped the whole chain for
`rts2db`/DB includes - none; checked whether `gemini.cpp` used the
sibling `pier-collision.h`/`tellx200gps.h` classic files - it doesn't,
so only `tellx200`+`hms` were needed, not the full `rts2lx200`
directory) - confirmed portable against the existing `Telescope` base
before committing to the full 2125-line port.

- **`base/teld/include/hms.h` + `src/hms.cpp`** (flattened from
  classic `include/rts2lx200/hms.h` + `lib/rts2lx200/hms.c`) - two
  free functions, `hmstod`/`dtoints`, for the LX200 wire format.
  Dropped the `extern "C"` wrapper (pointless in an all-C++ tree with
  no C callers) and `#include "nan.h"` (using `<cmath>`'s `NAN`
  directly, matching the established convention). **One genuine bug
  found and fixed**: classic `hmstod()` `strdup()`s its input into
  `locptr`, then reassigns `locptr` while walking the string
  (`locptr = endptr + 1`) - every return path frees nothing, and by the
  time any return executes, the pointer actually returned by `strdup()`
  has already been overwritten, so there's no way to recover it to
  free even if someone tried. Genuine unconditional memory leak on
  every single call - and this function is called on every RA/Dec/Alt/
  Az/lat/long/time read from the mount, so on a running telescope
  daemon this leaks continuously (one small allocation per axis read,
  every poll cycle). Fixed by keeping the original `strdup()` pointer
  in a separate `buf` variable, freeing it on every return path, and
  using `locptr` only for the sliding parse position - see
  `UPSTREAM_BUGS.md`.
- **`base/teld/include/tellx200.h` + `src/tellx200.cpp`**
  (`rts2teld::TelLX200:public Telescope`, flattened from classic
  `include/rts2lx200/tellx200.h` + `lib/rts2lx200/tellx200.cpp`) - the
  abstract LX200 serial-protocol layer (`tel_read_ra`/`_dec`/
  `_altitude`/`_azimuth`/`_local_time`/`_sidereal_time`/`_latitude`/
  `_longitude`, `tel_write_ra`/`_dec`/`_altitude`/`_azimuth`,
  `tel_set_slew_rate`, `tel_start_slew_move`/`tel_stop_slew_move`,
  `matchTime`/`matchTimeZone`/`setTimeZone`/`getTimeZone`), built on
  `ConnSerial` (already ported) and `Telescope` (already ported for
  D50 - matched the exact same base-class method set: `setTelRa`/
  `setTelDec`, `telAltAz`, `lst`, `telLatitude`/`telLongitude`,
  `ValueTime`, all already present, no gaps). Faithful port, no bugs
  found.
- **`base/teld/gemini/gemini.cpp`** (`rts2teld::Gemini:public
  TelLX200`, source of truth `../src/teld/gemini.cpp`, 2125 lines) -
  the actual Losmandy Gemini-2 mount-control driver: full gemini
  register read/write protocol with two different XOR-based checksum
  variants (firmware bug compensation), model save/load to a local
  config file, HA-limit-aware meridian-flip logic in `startResync()`,
  guiding/centering-rate "change" moves, BOOTES optical home-sensor
  parking, cold/warm/restart reset handling. Confirmed every
  `Telescope`-base symbol it needs - including the pointing-model
  application methods `computeModel`/`applyModel`/
  `applyModelPrecomputed`/`applyCorrRaDec`/`zeroCorrRaDec` and the
  `calculateAberation`/`calculatePrecession`/`calculateRefraction`/
  `setCorrections` correction-toggle methods - were already present on
  the `Telescope` base from earlier work (these are abstract framework
  methods on `Telescope` itself, distinct from the *concrete*
  GPointModel/TPointModel pointing-model subclasses that were
  deliberately deferred during teld-d50 - `Gemini` never needed those
  concrete subclasses). One naming mystery resolved during scoping:
  `change_ra`/`change_dec` call a `telescope_start_move()` method that
  `Gemini` never declares and no base class provides - but that whole
  `#else` branch (guarding on `#ifdef L4_GUIDE`, which this file
  `#define`s at the top) is dead code, never compiled, exactly as in
  the classic tree - so it's carried forward unchanged rather than
  fixed, matching the existing precedent of leaving inactive
  `#ifdef`-guarded branches alone (e.g. `ElementAcquire`'s
  `RTS2_HAVE_PGSQL` guard in task 25). **One genuine bug found and
  fixed**: `startResync()` declares `ra_diff`/`ra_diff_flip`/
  `dec_diff`/`dec_diff_flip` uninitialized; if neither mount flip is
  reachable in the HA-limit check loop, none of the four ever get
  assigned before an unconditional debug log line prints all of them -
  an uninitialized read (real UB, not just a style nit; caught by
  `-Wmaybe-uninitialized` during the build). Fixed by zero-initializing
  at declaration - behavior is unchanged in the normal case where at
  least one flip is reachable, and the pathological all-flips-blocked
  case now logs 0 instead of garbage. See `UPSTREAM_BUGS.md`.
  Faithful port otherwise, `NULL`->`nullptr` and include-path
  flattening only.
- `base/teld/CMakeLists.txt` - added `src/hms.cpp` + `src/tellx200.cpp`
  to `base_teld`'s source list, `add_subdirectory(gemini)` for the new
  `rts2-teld-gemini` executable.

**Smoke-tested**: full tree builds clean, `ctest` still 7/7, `--help`
produces the full expected option list (Gemini-specific options like
`--evening-reset`/`--force-latlon`/`--bootes`/`--gemini-model`, plus
the inherited `TelLX200` and `Telescope` options). Not verified here
(no physical hardware): real Gemini-2 serial protocol I/O, checksum
validation against a live unit, or the meridian-flip decision logic
against real sky positions.

## FRAM installations (dome-zelio, teld-lx200, focusd-optec, focusd-nstep, sensor-external)

User-directed (2026-07-12): "so lets do FRAM installations" - five
drivers active at the FRAM sites, all small-to-medium and, per the
user, expected to be straightforward. All five confirmed DB-free and
built clean on the first attempt; no bugs found in any of them.

- **`base/kernel/include/connmodbus.h` + `src/connmodbus.cpp`**
  (`rts2core::ConnModbus`/`ConnModbusTCP`/`ConnModbusRTUTCP`, flattened
  from classic `include/connection/modbus.h` +
  `lib/rts2/connmodbus.cpp`, ~467 lines) - a new generic kernel-tier
  connection class (family-agnostic like `ConnTCP`/`ConnSerial`, hence
  placed in `kernel/` even though only `zelio` uses it so far), needed
  by `dome-zelio`. Built directly on the already-ported `ConnNoSend`/
  `ConnTCP`. Faithful port, no changes.
- **`base/dome/zelio/zelio.cpp`** (`rts2dome::Zelio:public Dome`,
  source of truth `../src/dome/zelio.cpp`, ~1486 lines) - driver for
  Martin Nekola's Zelio PLC-based dome controller (Modbus TCP/IP
  register read/write, with model auto-detection from register bits -
  `ZELIO_SIMPLE`/`BOOTES3`/`COMPRESSOR`/`FRAM`/`ELYA` variants, each
  with different available switches/sensors/battery/humidity outputs).
  Needed `base/kernel/include/valueminmax.h`+`.cpp`
  (`ValueDoubleMinMax`/`ValueIntegerMinMax`) for `domeTimeout` - this
  turned out to already exist in the kernel (silently ported during an
  earlier task, e.g. teld-d50, and never removed), so no new work
  there. Faithful port, no bugs found.
- **`base/teld/include/tellx200.h`/`.cpp`+`hms.h`/`.cpp`** (already
  ported for teld-gemini above) meant **`base/teld/lx200/lx200.cpp`**
  (`rts2teld::LX200:public TelLX200`, source of truth
  `../src/teld/lx200.cpp`, ~719 lines) - the generic/reference
  LX200-protocol driver (as opposed to Gemini's Losmandy-specific
  register protocol layered on top of the same `TelLX200` base) - was
  a small, mechanical add: no new kernel/teld-tier work needed at all,
  every symbol (`setTelLongLat`, `getTelTargetRa`/`Dec`,
  `TEL_MASK_TRACK`/`TEL_TRACKING`/`TEL_NOTRACK`, `getInfoTime`,
  `getIdleInfoInterval`, `paramNextString`) was already present.
  Faithful port, no bugs found. Includes optional 10micron
  ("AstroPhysics extensions") support, detected at runtime from the
  `:GVP#` product-name query.
- **`base/focusd/optec/optec.cpp`** (`rts2focusd::Optec:public
  Focusd`, source of truth `../src/focusd/optec.cpp`, ~287 lines) -
  simple serial ASCII-protocol driver for the Optec TCF focuser.
  Faithful port, no bugs found.
- **`base/focusd/nstep/nstep.cpp`** (`rts2focusd::NStep:public
  Focusd`, source of truth `../src/focusd/nstep.cpp`, ~351 lines) -
  serial ASCII-protocol driver for the GCUSB-nStep focuser (temperature
  compensation registers, coil-power control). Faithful port, no bugs
  found.
- **`base/sensord/external/external.cpp`** (`rts2sensord::External:
  public SensorWeather`, source of truth `../src/sensord/external.cpp`,
  ~154 lines) - trivial pseudo-sensor exposing a `good_weather`
  boolean and arbitrary externally-created int/bool/double/string
  values, meant to be driven by an external script rather than reading
  real hardware. Faithful port, no bugs found.

All five wired into their respective family `CMakeLists.txt` as new
`add_subdirectory()`s, each building its own executable
(`rts2-dome-zelio`, `rts2-teld-lx200`, `rts2-focusd-optec`,
`rts2-focusd-nstep`, `rts2-sensor-external`) linked against the
relevant family static lib + `base_kernel`.

**Smoke-tested**: full tree builds clean, `ctest` still 7/7, `--help`
runs clean on all five executables. Not verified here (no physical
hardware): real Modbus/serial protocol I/O against any of the five
devices.

## Sensor-fram-weather (Los Leones FRAM site - last new driver in this wave)

User-directed (2026-07-12): "FRAM at Los Leones runs: focusd-aaf,
sensor-fram-weather, dome-zelio, teld-lx200, sensor-nut and gxccd - so
only fram-weather is new in this wave." The other five were already
ported in earlier tasks (28, 30, 16); only `sensor-fram-weather` needed
new work.

- **`base/sensord/fram-weather/framudp.h`+`.cpp`** (`rts2sensord::
  ConnFramWeather:public rts2core::ConnNoSend`, flattened from classic
  `src/sensord/framudp.h`+`.cpp`, ~178 lines) - a small raw UDP
  connection (bind + non-blocking `recvfrom`/`sendto`, no framing
  beyond a `sscanf`-parsed text line) that receives weather broadcasts
  from a Pierre Auger-style weather station. Built directly on the
  already-ported `ConnNoSend`. Faithful port, no changes. One cosmetic
  oddity carried forward as-is (not a functional bug - noted here for
  the record, not worth "fixing" since the actual `wtimeout` value used
  is identical either way): in `receive()`'s `sscanf` failure path,
  `master->setWeatherTimeout()` is called once inside the nested
  `if (ret != 1)` block and then unconditionally again right after
  with a different message string, regardless of whether the nested
  parse succeeded - so on the success path the final call logs
  "cannot parse packet from weather station" even though a
  `weatherTimeout=N` packet did parse. Purely a redundant call /
  misleading log message, not a behavior change (same `wtimeout` value
  both times).
  - This directory-local placement (rather than the shared
    `sensord/include/`) matches the classic tree's own structure -
    `framudp.h` is used by exactly one driver, not shared infrastructure.
- **`base/sensord/fram-weather/fram-weather.h`+`.cpp`**
  (`rts2sensord::FramWeather:public SensorWeather`, source of truth
  `../src/sensord/fram-weather.cpp`, ~230 lines total) - windspeed/
  rain/watch-mode weather sensor, all data driven by
  `ConnFramWeather`'s UDP callback (`setWeather()`), no polling of its
  own (`info()` just checks how long ago the last UDP packet arrived).
  Faithful port, no bugs found.
- Both files build into a single new executable,
  `rts2-sensor-fram-weather`, via `base/sensord/fram-weather/
  CMakeLists.txt`, wired into `base/sensord/CMakeLists.txt` via
  `add_subdirectory(fram-weather)`.

**Smoke-tested**: full tree builds clean, `ctest` still 7/7, `--help`
runs clean. Not verified here (no physical hardware): real UDP packets
from an actual Pierre Auger-style weather station.

## Sensor-avelab (AVE lab CCD-testing computer, LED control) - user-supplied source, not in the classic tree

User-directed (2026-07-12): "avelab CCD-testing computer uses this LED
control: sensor-avelab, I'm not sure if the source is identical to
what we have here in the tree or if it is there at all. ~/avelab.cpp."
Checked - **`avelab` does not exist anywhere in the classic `~/rts2`
tree at all** (`find` across `src/`, `include/`, `lib/` found nothing);
the only copy is the user's own `~/avelab.cpp` (152 lines,
Copyright Petr Kubanek 2011 + Martin Jelinek 2014). This is presumably
a locally-written driver that was never merged back upstream, not a
file this session had simply not reached yet.

`base/sensord/avelab/avelab.cpp` - `rts2sensord::Colamp:public
Sensor` (the class itself is named `Colamp`, not `Avelab` - "Colamp"
board, controlling the AVE lab's LED light source over a trivial
one-byte serial protocol: write a single byte 0-255 as the requested
light intensity, no response expected). Built on `Sensor`+`ConnSerial`,
both already ported - no new kernel/sensord-tier work needed. Ported
verbatim from the user-supplied source (only include-path flattening:
`"connection/serial.h"` -> `"connserial.h"`), no bugs found. Note: the
default device path baked into the constructor is `/dev/collamp`
(double-L, inconsistent with the single-L "Colamp" class name/board
name) - left as-is since it's presumably a real udev symlink on the
actual AVE lab machine, not a typo safe to silently "fix".

`base/sensord/avelab/CMakeLists.txt` - new `add_executable
(rts2-sensor-avelab avelab.cpp)`, wired into
`base/sensord/CMakeLists.txt` via `add_subdirectory(avelab)`.

**Smoke-tested**: full tree builds clean, `ctest` still 7/7, `--help`
runs clean. Not verified here (no physical hardware): real serial I/O
against the Arduino-based LED controller.

## Sensor-windrs (Papouch WindRS wind sensor, Paramount/Brown FRAM at Paranal)

User-directed (2026-07-12): "brown FRAM at Paranal runs: teld-paramount,
dome-zelio, camd-gxccd, focusd-nstep, sensor-windrs, sensor-external" -
four of the six were already ported (`dome-zelio`, `camd-gxccd`,
`focusd-nstep`, `sensor-external`); `sensor-windrs` was new,
`teld-paramount` is a substantial new architectural item (see next
section).

`base/sensord/windrs/windrs.cpp` (`rts2sensord::WindRS:public
SensorWeather`, source of truth `../src/sensord/windrs.cpp`,
~334 lines) - Papouch WindRS wind sensor over the "Spinel" serial
protocol (custom checksummed packet framing, hand-rolled in this same
file as free functions `spinel_checksum`/`spinel_make_packet`, not a
shared library). Built on `SensorWeather`+`ConnSerial`, both already
ported.

**One genuine bug found and fixed**: `WindRS::openConnection()`
allocates a `ConnSerial` into `WindRSConn`, and on `init()` failure
sets `WindRSConn = NULL` directly without freeing the object first -
an unconditional memory leak on every failed connection attempt.
Since `getData()` (called every 10s via `EVENT_LOOP`) calls
`openConnection()` again whenever `WindRSConn == NULL`, a sensor with
a bad/missing serial port leaks one `ConnSerial` object every 10
seconds indefinitely for the life of the daemon. Fixed by adding
`delete WindRSConn;` before the `= NULL` assignment - see
`UPSTREAM_BUGS.md`.

`base/sensord/windrs/CMakeLists.txt` - new `add_executable
(rts2-sensor-windrs windrs.cpp)`, wired into
`base/sensord/CMakeLists.txt`.

**Smoke-tested**: full tree builds clean, `ctest` still 7/7, `--help`
runs clean. Not verified here (no physical hardware): real Spinel
protocol I/O against the actual WindRS unit.

## Teld-paramount (Software Bisque Paramount ME/MYT, brown FRAM at Paranal)

User-directed (2026-07-12), part of the same brown-FRAM-at-Paranal
request above. Initially scoped as blocked on two things - a deferred
`GEM`/`Fork` architectural redesign, and a missing vendor SDK - but
the user resolved both: **`libmks3` lives at `~/paracl`** (the user's
own C library, real and complete, not the classic `Fork`-vs-`GEM`
situation of a truly external vendor SDK - "this driver is 'jelinek
style' as opposed to the usual Kubanek/Strobl style"), and the
GEM/Fork unification is explicitly **deferred again, on purpose**:
port `GEM` faithfully as classic has it now, and revisit unifying
`GEM`/`Fork`/`AltAz` later once all actively-used mounts are ported and
the real common denominators are visible - not before.

- **`base/teld/include/gem.h` + `src/gem.cpp`** (`rts2teld::GEM:public
  Telescope`, source of truth `../include/gem.h` +
  `../lib/rts2tel/gem.cpp`, ~1107 lines) - the abstract German
  Equatorial Mount class: `sky2counts`/`counts2sky` (RA/Dec <-> HA/Dec
  axis-count conversion with full meridian-flip decision logic - 8
  flipping strategies: shortest/same/opposite/west/east/longest/cw
  down/cw up), `checkTrajectory`/`calculateMove` (horizon-aware
  multi-step path planning, splitting moves across RA/Dec axes to
  avoid pole/horizon issues). Confirmed every single `Telescope`-base
  symbol it needs (`getLstDeg`/`applyCorrections`/`setTelTarget`/
  `setTarTelRaDec`/`useParkFlipping`/`parkFlip`/`flip_move_start`/
  `flip_longest_path`/`calculateTarget`/`hardHorizon`/
  `TRAJECTORY_CHECK_LIMIT`/`peekFlip`/`updateMetaInformations`, the
  `virtual int sky2counts(...)` signature `calculateTarget` dispatches
  through) was already present from the D50/Gemini teld work - **zero
  new kernel work needed**, confirmed by a clean first-try build.
  Faithful port, no bugs found, no behavior changes - the redesign
  decision above means this is intentionally the classic shape, not
  the planned unified one.
- **`libmks3`** (Martin Jelinek/Petr Kubanek's C library for the
  Paramount ME/MYT MKS3 servo controller protocol, real source at
  `~/paracl/libmks3.c`+`.h`, ~2166 lines total) - built from source
  like FLI (not a prebuilt vendor `.so` like Andor/gxccd), gated behind
  a new opt-in CMake cache variable `BASE_PARACL_DIR` (empty by
  default, matching the Andor/gxccd/FLI opt-in convention) added to
  the top-level `CMakeLists.txt`, which also now declares `LANGUAGES
  CXX C` (needed since `libmks3.c` is plain C). Only `libmks3.c`/`.h`
  are built - the rest of the `paracl` tree (`drv.c`/`main.c`/`maps.c`/
  `tpmodel.c`/`simplc.c`) belongs to a separate standalone `paracl` CLI
  utility that `rts2-teld-paramount` doesn't need. Compiles clean
  (only pre-existing warnings in the vendor's own C source - not
  touched, it's the user's own external library, same non-interference
  policy as Andor/gxccd's precompiled binaries). `libmks3.c` itself
  needs only `libm` (`-lnova` in its own `Makefile` is for the
  standalone tool's other files, not `libmks3.c`).
- **`base/teld/paramount/paramount.cpp`** (`rts2teld::Paramount:
  public GEM`, source of truth `~/paracl` referenced classic
  `src/teld/paramount.cpp`, 1667 lines) - full Paramount ME/MYT driver:
  MKS3 register read/write protocol, homing/park/slew state machine
  (`doPara()`, driven from `idle()`/`isMoving()`/`isParking()`/
  `startResync()`), per-axis constant save/load to a local config file
  (mirroring Gemini's model-register save/load pattern), north/south
  hemisphere sign inversion, per-major-version tick-per-revolution
  tables (ME/MEII/MYT). Needed one explicit `#include "configuration.h"`
  the classic file doesn't have (pulled in transitively by something
  in the classic tree's looser include graph - same recurring gotcha
  documented earlier in this port, this time in the other direction:
  missing rather than superfluous). **One genuine bug found and
  fixed**: `saveFlash()` calls `open("/etc/rts2/flash", O_CREAT |
  O_TRUNC)` with no third `mode_t` argument - undefined permission bits
  at runtime on classic glibc, and a hard compile error on this system
  (`_FORTIFY_SOURCE`'s `open()` wrapper refuses to compile `O_CREAT`
  without an explicit mode). Fixed by adding `0666`, matching the mode
  already used for equivalent lock-file/flash-dump `open()` calls
  elsewhere in this port (`daemon.cpp`, `gxccd.cpp`) - see
  `UPSTREAM_BUGS.md`.

**Smoke-tested for real**: full tree builds clean, `ctest` still 7/7,
`--help` produces the complete option list (Paramount-specific plus
inherited `GEM`/`Telescope` options). Ran it against a nonexistent
serial device (`-f /dev/nonexistent-paramount-port -i`) and confirmed
it fails cleanly through the *real* vendor `MKS3Init()`/`commInit()`
(actual `termios` open, not a stub) - "Can't open
/dev/nonexistent-paramount-port" followed by a clean daemon-init abort,
not a crash. Not verified here (no physical hardware): real MKS3
register I/O against a live Paramount controller.

## Deliberately incomplete ports (by design, not oversight)

- **`command.h`/`command.cpp`**: the base `Command` class plus
  `Rts2CentraldCommand`, `CommandSendKey`, `CommandAuthorize`, `CommandKey`,
  `CommandStatusInfo`, `CommandDeviceStatus`, `CommandMessageMask` (all
  genuinely core-protocol, used directly by `Connection`/`Block`),
  `CommandChangeValue` (added for the monitor - no `DevClient` dependency),
  and `CommandCupolaSyncTel` (added for teld - no `DevClient` dependency,
  see "Teld" section above). The classic tree's other ~44 concrete `Command`
  subclasses (`CommandMove`,
  `CommandFilter`, `CommandExposure`, ...) are tied to specific `DevClient*`
  types and deferred - port alongside whichever driver/client needs them.
- **`devclient.h`/`devclient.cpp`**: only the base `DevClient` class.
  `Block::createOtherType()` returns a plain `DevClient` for every device
  type instead of switching to 15 concrete subclasses
  (`DevClientCamera`/`DevClientTelescope`/...) - deferred the same way. The
  monitor and dummy camd both exercise this fallback path directly (neither
  configures a device that would need one of the 15).
- These are all genuinely large chunks of code (comparable in size to what
  *has* been ported) - don't be surprised if resuming this project involves
  porting one of them; check whether the specific class you need is already
  declared before assuming a fresh read-and-port is required.

## Load-bearing findings (architecture, not just cleanup)

**`logStream()`/`sendLog` needs an `App*` singleton**, and separately,
**`Block` directly extends `App`** (constructor, run loop, timers - not just
logging). Together these meant App had to be ported for real much earlier
than planned (originally staged as "last, after centrald exists"). Done -
see "App: DONE" below.

**The disputed networked-logging behavior is NOT in `App`.**
`App::sendMessage()` just writes to `stderr`, which is correct either way.
The real "route log messages over the RTS2 wire protocol instead of stderr"
behavior lives in `Daemon::sendMessage()` (`base/kernel/src/daemon.cpp`).
See memory `rts2-logging-architecture-flaw` for the full background
(user-flagged upstream concern that networked logging can overload the
messaging channel under load).

**Update (2026-07-16, Debian-packaging pass): implemented the syslog-first
redesign for real**, motivated by wanting a working, self-contained
`rts2-base` package with sane log rotation and no dependency on centrald
being reachable for persistence. Confirmed by code reading that the
flaw was already faithfully ported (not just inherited on paper):
`Daemon::sendMessage()` only called `syslog()` when no centrald connection
was up; otherwise `Device::sendMessage()` relayed the message over the wire
instead, and `Centrald::processMessage()` was the *sole* writer of one
shared log file (`fileLog`, from `[centrald] logfile`/`RTS2_LOG_FILE`).
Changed:
- `Daemon::Daemon()` now calls `openlog ("rts2", LOG_PID, LOG_DAEMON)`
  once, so every daemon has an open syslog channel under one shared ident
  from the start.
- `Daemon::sendMessage()` now *always* `syslog()`s locally (dropped the
  `someCentraldRunning()` gate entirely - `Block::someCentraldRunning()`
  itself is consequently unused now, left in place as a small, still
  possibly-useful utility rather than removed as unrelated cleanup),
  additionally echoing to stderr only when not-yet-daemonized/interactive
  (`DO_DAEMONIZE`/`DONT_DAEMONIZE` states - `IS_DAEMONIZED`/`CENTRALD_OK`
  stay syslog-only).
- `Centrald::processMessage()` no longer writes to any file - it now only
  `sendMessageAll()`s for live `rts2-mon`-style monitoring. All of
  `Centrald`'s `fileLog`/`openLog()`/`logFile`/`logFileSource`/
  `OPT_LOGFILE`/`--logfile` machinery removed (`centrald.cpp`/
  `centrald.h`) - persistence is syslog's job now, not centrald's.

**Bonus bug found while smoke-testing**: `Daemon::doDaemonize()`
(`kernel/src/daemon.cpp`, the actual single-fork-into-background routine)
had its own `openlog (nullptr, LOG_PID, LOG_DAEMON)` call right after
forking - re-opening syslog with a null ident falls back to the program's
own default name (e.g. `rts2-camd-fli`), silently overriding the
constructor's `openlog ("rts2", ...)` for every daemon that actually
daemonizes (the normal case - only `-i`/interactive runs skip
`doDaemonize()` entirely, which is why this wasn't caught by the first
round of interactive-only smoke tests). Removed - glibc's syslog state,
including the ident string pointer, survives `fork()` correctly, so no
re-open was ever needed. Verified by actually daemonizing `rts2-centrald`
(no `-i`) on a non-privileged port and provoking a real log message (a
bogus wire command) - confirmed landing under `journalctl -t rts2`, not
`rts2-centrald`.

Smoke-tested end-to-end: full clean rebuild; `rts2-centrald -i --debug`
run interactively (bind-permission error on the privileged default port
lands in `journalctl -t rts2` immediately); a real daemonized
`rts2-centrald` on a non-privileged port confirmed logging under the same
`rts2` ident post-fork. This closes out the `rts2-logging-architecture-flaw`
memory's core concern - see that memory for the packaging-side
follow-through (rsyslog/logrotate config for the `rts2-base` Debian
package).

**`iniparser.h` included `value.h` for no reason** - verified by grep,
dropped. IniParser/Configuration/ObjectCheck do not depend on the Value
type system at all.

**`rts2format.cpp` used to reach into a global
`getMasterApp()->usesLocalTime()`.** Decoupled via `setLocalTimeDefault(bool)`,
which `App`'s constructor/option-processing now calls to stay in sync
(see `app.cpp`), instead of formatting code reaching upward into the CLI
framework.

**`block.cpp` had a dead `#include "client.h"`** - grep-verified nothing in
the file uses `ConnClient`/`ConnCentraldClient`/`CommandLogin`/`Client`.
Dropped, same pattern as the `iniparser.h`/`value.h` and
`timestamp.cpp`/`block.h` dead includes found earlier.

**`devclient.h` and `block.h` circularly `#include`d each other** in the
classic tree (worked there only by include-guard ordering luck). `DevClient`
only ever needs `Block*` as a pointer return type, so `devclient.h` now
forward-declares `class Block;` instead - breaks the cycle cleanly.

## Bugs found and fixed during the port (not just carried forward)

**See also `base/UPSTREAM_BUGS.md`** - the subset of these that are genuine
classic-tree bugs (not porting mistakes or stylistic modernization) is
written up there with full reproduction/fix detail, in a form suitable for
actually reporting upstream (e.g. to Jan Strobl for the D50/`ConnREMOTES`
one below). Keep both in sync when a new one turns up.

- `LibnovaRaDec`/`LibnovaHrz` (libnova_cpp.h): copy constructor + destructor
  but no copy-assignment operator -> compiler-generated shallow copy of
  owning raw pointers -> double-free on `a = b`. Fixed with `unique_ptr` +
  explicit deep-copy `operator=`. Covered by `test_libnova_cpp.cpp`.
- `IniSection::createBlockedBy`: leaked the last parsed device name when the
  list didn't end in a trailing space.
- `LibnovaDegDist operator>>`: leaked `os`/`is` on the `default:` exit path.
- `ValueDoubleStat::getDisplayValue`: `memcpy(buf, os.str().c_str(), sizeof(buf))`
  unconditionally copied 200 bytes regardless of the actual string length -
  read past the string's end. Replaced with bounded `strncpy` + explicit
  null termination.
- `App::askForBoolean`: returned `-1` from a `bool`-returning function on
  EOF (silently becomes `true` via int-to-bool conversion). Changed to
  `return false;`.
- Dropped `nan.c`/`nan.h` entirely - on Linux the whole file compiled to
  nothing; standard `<cmath>` `NAN` used directly instead.
- Dropped `block.cpp`'s file-scope `type_names[]` array - grep-verified
  unused anywhere in the tree, and it had a real bug anyway (a missing comma
  between `"UNKNOWN"` and `"EXEC"` silently string-concatenates them into
  `"UNKNOWNEXEC"`, so the array didn't even line up with the
  `DEVICE_TYPE_*` constants it's named after).
- Stripped Solaris/Cygwin/ancient-GCC compat shims from `utilsfunc`
  (`isinf`/`strcasestr`/`getline` fallbacks, `HUGE_VALF`/`INFINITY` macro
  ladders) - dead weight on a modern glibc/C++17 target.
- `timestamp.cpp` `#include`d `block.h` for no reason anything in the file
  used it - dropped.
- The classic `connection.cpp` currently has active in-progress debug
  scratch in `sendMsg()` (commented-out `clock_gettime`/`debugtime` lines,
  a temporary `std::cout` swapped in for the usual `logStream` call) -
  that's someone's uncommitted WIP on the classic tree (visible in
  `git diff`), not settled behavior, so it was not carried over.
- `daemon.cpp`'s `checkLockFile()` dropped the classic `#ifndef
  RTS2_HAVE_FLOCK` / `lockf()` fallback path - always uses `flock()` now,
  which glibc always provides. Also dropped the local `#define LOCK_SH /
  LOCK_EX / LOCK_NB / LOCK_UN` compat guards at the top of the file -
  `<sys/file.h>` always defines these on modern Linux.
- `device.cpp`/`device.h`/`multidev.cpp`/`multidev.h` ported with no
  functional changes found or needed - both compiled and linked clean on
  the first attempt once wired into `kernel/CMakeLists.txt`.
- `centrald.cpp`'s `Centrald::init()` had the same dead
  `#ifndef RTS2_HAVE_FLOCK` branch as `daemon.cpp` - dropped, see the
  "Centrald" section above.
- `ConnREMOTES::init()` (D50) discarded `ConnEthernet::init()`'s return
  value and always returned 0; `D50::init()` never checked
  `remotesRA->init()`/`remotesDec->init()`'s result either (unlike
  `raDrive`/`decDrive`, which *are* checked). A failed raw-socket open
  left `ConnEthernet`'s buffer pointers unallocated, and the very next
  register read/write segfaulted. Found by smoke-testing `rts2-teld-d50`
  without `CAP_NET_RAW` (see "Teld-d50" section) - a real bug in the
  classic tree too, just never triggered there since production always
  runs as root. Fixed by propagating the real return values.
- `NUT::initHardware()` (sensord/nut.cpp) only checked `connNUT->init()`'s
  return value, but `ConnTCP::init()` throws `ConnCreateError` (not a
  return code) for most connect failures - an unreachable NUT daemon
  crashed the whole process with an uncaught exception. Found by
  smoke-testing `rts2-sensor-nut` against an unreachable host:port. Fixed
  by wrapping the call in `try`/`catch (rts2core::ConnError &)`.
- `D50WFUnit::wfunitCommand()` (sensord/d50-wfunit.cpp): two bugs -
  `wfFanPower->setValueInteger(shutState)` stored the shutter-state
  character instead of the parsed `fanState` integer; and `sscanf`'s
  `"%s"` conversion wrote into a bare `char` lvalue (`&shutState`) rather
  than a buffer, a stack buffer overflow on every call. Found by code
  inspection while porting (not smoke-testable without real hardware).
  Fixed both - see "Sensord" section and `UPSTREAM_BUGS.md`.
- `Ford::processOption()` (dome/domeford.cpp): `default:` case called
  `Dome::processOption()` without returning its result, so option-parsing
  errors from the base class chain were silently discarded. Found by code
  inspection while porting. Fixed by returning the actual result - see
  "Dome-d50" section and `UPSTREAM_BUGS.md`.
- `Rts2Target`'s constructor (rts2target.h) never initialized `epochId` -
  `getEpoch()` could return garbage before the first `setEpoch()` call.
  Found by code inspection while porting. Fixed by defaulting to `-1`,
  matching the same convention already used by `rts2core::Expander::epochId`.
- `DevClientCameraImage::allImageDataReceived()` (kernel/src/devcliimg.cpp):
  `LTV1`/`LTV2`/`CRPIX1`/`CRPIX2` never incorporated the actual per-exposure
  windowed-readout position (`x`/`y`) - only the optional multi-channel
  `CHAN1_OFFSETS`/`CHAN2_OFFSETS` config, unused by any camera actually
  deployed, so every windowed exposure silently recorded LTV as if it
  weren't windowed at all. Found live by the user (a separate pipeline had
  resorted to guessing the window position from `NAXIS1`/`NAXIS2` alone).
  See "Window position fix" below and `UPSTREAM_BUGS.md` for the full
  write-up and empirical verification.

## Scriptexec push (task 22-26): all six D50 devices are ported, but nothing
## can command them yet - the user asked to jump straight to the "other
## side": a client that actually drives scripted observations

After finishing dome-d50 (task 21), the user pointed out the obvious next
problem: every D50 device driver now exists, but there's still no way to
actually *run* an observation against them - RTS2 needs a client that
parses an observation script (expose, filter/focus change, telescope move,
etc.) and drives it against the running devices over the wire protocol.

The classic candidate is `rts2-executor` (`src/plan/executor.cpp`), but it
extends `rts2db::DeviceDb` - it's tightly coupled to the Postgres/ecpg-backed
database layer (`Target`/`Plan`/`Constraints`/`TargetGRB`, ~40 files) that
base has deliberately never ported (stated "no database" design principle
in the top-level `CMakeLists.txt`). Committing to the full Executor means
committing to a real database layer first - a huge, separate multi-session
effort.

**Decision (user-confirmed):** port `rts2-scriptexec` instead first
(`src/plan/scriptexec.cpp`) - a standalone, non-DB CLI tool built on the
same script-parsing/execution engine (`rts2script`), but using a plain
`Rts2Target` (non-DB target base, `include/rts2target.h`) instead of
`rts2db::Target`. It connects to already-running devices and drives a
script directly - exactly the "let's actually run something at D50"
unblock, without the DB detour. Scoped via a research agent: **~18,500
lines across ~60 files** (bigger than any single task so far). Only one
real DB dependency was found reachable in the whole closure - the
`ACQUIRE` script element (`ElementAcquire`/`ConnImgProcess`, needs
`rts2db::Observation` for astrometry.net + archive logic) -
**user-confirmed: drop it** (every other script command - expose,
filter/focus change, telescope move, for/if/repeat, exe/hex, waitfor,
dither - has no DB dependency).

Broken into five ordered tasks (22-26), bottom-up through the dependency
graph:

- **Task 22 (DONE): `ConnFork`** - the last unported Connection transport
  variant (`kernel/include/connfork.h` + `src/connfork.cpp`, flattened
  from classic `include/connection/fork.h`). Forks a subprocess, treats
  its stdout as a command stream (needed by `ConnExe`, the `exe`/`hex`
  script elements), stderr separately, optional stdin. Faithful port, no
  bugs found. Compiles clean, `ctest` still 7/7 (not yet smoke-tested
  standalone - nothing links against it until task 25's `ConnExe`).
- **Task 23 (DONE): `rts2fits` non-DB tier** - real CFITSIO-backed
  FITS writing, a wholly new subsystem for base (gated via
  `find_package(PkgConfig)` + `pkg_check_modules(CFITSIO REQUIRED cfitsio)`
  in `kernel/CMakeLists.txt` - CFITSIO is a normal system package here,
  not vendor-gated like Andor/gxccd/FLI, found at version 4.6.3 on this
  system). All pieces ported, building clean, `ctest` still 7/7:
  - `kernel/include/expander.h` + `src/expander.cpp` (`rts2core::Expander`,
    `%`/`@` filename-expansion mechanism, flattened from classic
    `include/expander.h` + `lib/rts2/expander.cpp`) - a prerequisite,
    `FitsFile` extends it. Faithful port; dropped an unnecessary
    `#include "block.h"` (nothing in the file uses `Block`), which meant
    adding explicit `#include "error.h"` and `<climits>` that block.h had
    been pulling in transitively.
  - `kernel/include/counted_ptr.h` (header-only reference-counted pointer,
    used later by `rts2script::ScriptPtr` in task 25) - copied verbatim,
    dropped the always-inactive Sun/`NO_MEMBER_TEMPLATES` compat branch
    (the macro was unconditionally defined right above it, so that code
    path never compiled anyway).
  - `kernel/include/fitsfile.h` + `src/fitsfile.cpp` (`rts2image::FitsFile`
    + `ColumnData`/`TableData`, flattened from classic
    `include/rts2fits/fitsfile.h` + `lib/rts2fits/fitsfile.cpp`) - the
    CFITSIO wrapper `Image` builds on. Faithful port; same
    dropped-`block.h`-include situation as `expander.cpp`, this time
    needing `#include "app.h"` added explicitly (`logStream()` the free
    function is declared there, not in `logstream.h` as might be
    expected - `block.h` was pulling it in transitively via `App`).
  - `kernel/include/channel.h` + `src/channel.cpp` (`rts2image::Channel`/
    `Channels`, per-channel pixel data + statistics, flattened from
    classic `include/rts2fits/channel.h` + `lib/rts2fits/channel.cpp`).
    Faithful port, no changes.
  - `kernel/include/image.h` + `src/image.cpp` (`rts2image::Image`, the
    biggest single file in this port - flattened from classic
    `include/rts2fits/image.h` (831 lines) + `lib/rts2fits/image.cpp`
    (2805 lines)). The optional ImageMagick/libjpeg preview path
    (`getMagickImage`/`writeLabel`/`writeAsJPEG`/`writeAsBlob`, classic
    `#if RTS2_HAVE_LIBJPEG`) is dropped entirely - cosmetic JPEG preview
    generation, no Magick++/libjpeg anywhere in base, not needed by
    execcli/scriptexec. The classic `#if !RTS2_HAVE_DECL_LN_GET_HELIOCENTRIC_TIME_DIFF`
    fallback implementation is also dropped - the libnova installed on
    this system already provides `ln_get_heliocentric_time_diff()`
    natively. `kernel/include/imgdisplay.h` (`DISPLAY_*` constants) ported
    alongside it, needed by `Image::print()`.
  - `kernel/src/imageastrometry.cpp` (the `Image::getCoord*()` family -
    `getCoordObject`/`Target`/`Astrometry`/`Mount`/`Best`/`MountRawUn`/
    `MountRawComputed`/`MountRawBest`, flattened from classic
    `lib/rts2fits/imageastrometry.cpp`, kept as its own file matching the
    classic tree's own separation) - confirmed genuinely DB-free (only
    needs `image.h` + libnova), unlike its sibling `imageastrometry.cpp`
    neighbors in the classic `rts2fits` directory (`imagedb.cpp`/
    `dbfilters.cpp`/`imageprocess.cpp`, all real DB code, correctly
    excluded from this port).
  - `kernel/include/cameraimage.h` + `src/cameraimage.cpp`
    (`CameraImage`/`CameraImages`/`ImageDeviceWait`, flattened from
    classic `include/rts2fits/cameraimage.h` + `lib/rts2fits/cameraimage.cpp`).
    Faithful port. **Process note**: on a first pass this file's methods
    were declared in the header but the `.cpp` was never actually
    written - the gap wasn't caught by the build because nothing yet
    links an executable calling them (they're only used from within
    `devcliimg.cpp`, itself not yet linked into anything until task 26).
    Caught by re-checking the header against grep results for the
    methods' definitions before moving on, and fixed by writing the real
    `cameraimage.cpp`. Worth remembering for the rest of this push: a
    clean `base_kernel` static-library build does **not** prove every
    declared method has a body - only an executable that actually calls
    it (or an explicit grep check) will catch a missing implementation.
  - `kernel/include/devcliimg.h` + `src/devcliimg.cpp`
    (`DevClientCameraImage`/`TelescopeImage`/`FocusImage`/`WriteImage`,
    `CommandQueImage`/`CommandQueObs`, flattened from classic
    `include/rts2fits/devcliimg.h` + `lib/rts2fits/devcliimg.cpp`) -
    same libjpeg-preview-path drop as `image.h` (the preview-saving block
    in `processCameraImage()` and the constructor's preview config
    loading). Needed three new `Command` subclasses not yet in base's
    trimmed `command.h`: `CommandFitsStat`, `CommandChange`,
    `CommandInfo` - added them (each is a thin wire-protocol string
    builder, no DB dependency), following the same "port only what's
    needed" pattern already used for `CommandChangeValue`/
    `CommandCupolaSyncTel`.
  - `kernel/include/devclifoc.h` + `src/devclifoc.cpp`
    (`DevClientCameraFoc`/`DevClientFocusFoc`/`ConnFocus`/
    `DevClientPhotFoc`, flattened from classic `include/rts2fits/devclifoc.h`
    + `lib/rts2fits/devclifoc.cpp`) - needed by `element.h` (task 25).
    Needed two more `Command` subclasses: `CommandChangeFocus`,
    `CommandIntegrate` - added alongside the task-23 three, same
    reasoning.
  - **Found-and-fixed while wiring this all together** (not a classic-tree
    bug, an base-internal gap from an earlier task): `devclient.h`'s
    forward-declaration block was missing `class ServerState;` - every
    `DevClient` subclass declares `virtual void stateChanged (ServerState
    *)`, and this had silently worked so far only because every previous
    consumer of `devclient.h` happened to also transitively include
    `serverstate.h` via something else first. `image.h` was the first
    file where `devclient.h` got included *before* anything else pulled
    `ServerState` in, surfacing the gap. Fixed by adding the forward
    declaration directly to `devclient.h`, which is the robust fix (not
    just an include-order workaround).
- **Task 24 (DONE): widen `devclient.h` + `Rts2Target` + leaf utils** -
  pulled forward ahead of finishing task 23, since `Image`'s constructor
  needs both `Rts2Target` and `rts2core::DevClientCamera`.
  - `kernel/include/rts2target.h` + `src/rts2target.cpp` (`Rts2Target`,
    the plain non-DB target base class - position + script-fetching
    interface - plus the free functions `getEventMaskName`/
    `printEventMask`, flattened from classic `include/rts2target.h` +
    `lib/rts2/rts2target.cpp`). **One bug found and fixed**: the
    constructor never initialized `epochId` - see "Bugs found and fixed"
    above and `UPSTREAM_BUGS.md`.
  - Widened `kernel/include/devclient.h` + `src/devclient.cpp` with
    `DevClientCamera`, `DevClientTelescope`, `DevClientFocus` (real
    consumers in `execcli`/task 25) and `DevClientPhot` (referenced only
    in a virtual-signature declaration, never instantiated by
    `scriptexec`). The other 11 classic subclasses
    (`DevClientDome`/`Cupola`/`Filter`/`Mirror`/`Rotator`/
    `AugerShooter`/`Executor`/`Selector`/`Imgproc`/`Grb`/`BB`) remain
    deferred, per the header's existing note (updated accordingly).
    Faithful port, no bugs found.
  - `nan.h` was already a non-issue (see the entry for it from an earlier
    task - `<cmath>`'s `NAN` is used directly throughout base);
    `expander.h`/`.cpp` landed under task 23 instead (it turned out to be
    `FitsFile`'s prerequisite).
- **Task 25 (DONE): `rts2script` library** - `Script`/`Element` hierarchy
  (minus `ElementAcquire`), `DevScript`, `execcli`. The actual
  script-parsing and device-driving engine, ~9290 lines. New top-level
  `base/script/` directory (its own tier, not folded into kernel, given
  the size), building `base_script` as a static lib linked against
  `base_kernel`:
  - `rts2spiral.h`/`.cpp` (`Rts2Spiral`, dither step generator).
  - `operands.h`/`.cpp` (`rts2operands` namespace - `Operand`/`Number`/
    `SystemValue`/`String`/`RandomNumber`/`OperandsSet`/
    `OperandsLREquation`).
  - `element.h`/`.cpp` (`Element` base + `ElementNone`/`ElementSequence`/
    `ElementImage`/`ElementExpose`/`ElementDark`/`ElementShiftStore*`/
    `ElementBox`/`ElementCenter`/`ElementChange`/`ElementWait`/
    `ElementWaitAcquire`/`ElementPhotometer`/`ElementSendSignal`/
    `ElementWaitSignal`/`ElementChangeValue`/`ElementComment`/
    `ElementCommand`/`ElementLoopDisable`).
  - `scriptcommands.h` (pure `COMMAND_*` string-constant defines,
    flattened from classic `include/rts2db/scriptcommands.h` - despite
    the misleading classic path, confirmed zero DB linkage, just string
    literals).
  - `elementblock.h`/`.cpp` (`ElementBlock`, `ElementSignalEnd`,
    `ElementAcquired`, `ElementElse`, `ElementFor`, `ElementWhileSod`,
    `ElementWhile`, `ElementDo`, `ElementOnce`).
  - `elementtarget.h`/`.cpp` (`ElementTarget`, `ElementDisable`,
    `ElementTempDisable`, `ElementTarBoost`) - dropped the classic
    `#include "elementacquire.h"` (unused in this file, and that header
    is the DB-touch point being avoided).
  - `connexe.h`/`.cpp` (`ConnExe:rts2core::ConnFork` - the generic
    script-control wire protocol: value-creation commands, `waitidle`,
    `log`, `run_device`/`exec_device`, randevous commands, etc).
  - `elementexe.h`/`.cpp` (`ConnExecute:ConnExe`, `Execute:Element` -
    adds image/exposure/target-move-specific commands: `exposure`,
    coordinate moves, `dark`/`flat`/`archive`/`trash`/`rename`, target
    disable/loop control, `requeue`).
  - `elementhex.h`/`.cpp` (`Rts2Path`, `ElementHex`, `ElementFxF`,
    `ElementSpiral` - dither-pattern movement elements; `ElementSpiral`
    implements its own inline spiral logic rather than using the
    separately-ported `Rts2Spiral` class, matching classic).
  - `elementwaitfor.h`/`.cpp` (`ElementWaitFor`, `ElementSleep`,
    `ElementWaitForIdle`).
  - `script.h`/`.cpp` (`Script` - the centerpiece parser/engine:
    `parseScript`/`setTarget`/`parseBuf`/`parseBlock`/`parseOperand`/
    `postEvent`/`nextCommand`, `ParsingError`/`UnknowOperantMultiplier`
    exceptions). Dropped `#include "rts2db/camlist.h"` and the free
    function `getMaximalScriptDuration()` (only called from
    `httpd`/`printtarget`/`executorque`, none in this closure - the
    confirmed avoidable DB touch). The classic `#ifdef RTS2_HAVE_PGSQL`
    guard around `ElementAcquire` handling in `parseBuf()` is kept
    completely intact - base never defines `RTS2_HAVE_PGSQL`, so it
    compiles out naturally with no manual code stripping needed.
  - `devscript.h`/`.cpp` (`DevScript` - per-connection script state
    machine: `startTarget`/`postEvent`/`nextPreparedCommand`/
    `haveNextCommand`/`deleteScript`/`setNextTarget`/`scriptBegin`).
    Faithful port, no bugs found.
  - `execcli.h`/`.cpp` (`DevClientCameraExec`/`DevClientTelescopeExec`,
    `GuidingParams` - the actual device-side script-driving clients).
    **Second real DB dependency found in this push, beyond the
    `ElementAcquire` one already scoped**: classic `execcli.cpp` includes
    `rts2script/connimgprocess.h` and `rts2db/target.h`.
    `connimgprocess.h` genuinely needs `rts2db/observation.h`
    (`rts2plan::ConnImgProcess`, used only for the optional
    `after_exposure_cmd` feature inside `DevClientCameraExec::queImage()`
    - run an arbitrary external command after each exposure via a
    DB-backed connection). **Dropped**, same reasoning as `ElementAcquire`:
    cut the whole `if (run_after && Configuration::instance()->getString
    (..., "after_exposure_cmd", ...))` block, keeping only the
    "find image processor with lowest queue number" / `CommandQueImage`
    path that was already there - see `UPSTREAM_BUGS.md`. The
    `rts2db::Target *` casts on `currentTarget` (itself declared as a
    plain `Rts2Target *` in the header) were simply re-cast to
    `Rts2Target *`, dropping the `#include "rts2db/target.h"` - only the
    `Rts2Target` interface (`moveNotStarted`/`beforeMove`/`startSlew`/
    `moveStarted`/`moveEnded`/`moveFailed`/`moveWasStarted`) is ever
    used, so no behavior changed.
  - Needed several new `Command` subclasses added to
    `kernel/include/command.h`+`src/command.cpp`:
    `CommandCameraSettings`, `CommandExposure`, `CommandShiftStart`/
    `Progress`/`End`, `CommandBox`, `CommandCenter`, `CommandQueueAt`,
    `CommandKillAll`, `CommandKillAllWithoutScriptEnds`,
    `CommandScriptEnds` (for `element.cpp`/`elementexe.cpp`/
    `devscript.cpp`), and finally the whole `CommandMove` family
    (`CommandMove`, `CommandMoveUnmodelled`, `CommandMoveMpec`,
    `CommandMoveTle`, `CommandMoveFixed`, `CommandMoveHaDec`,
    `CommandMoveAltAz`, `CommandResyncMove`) plus `CommandStartGuide`/
    `CommandStopGuideAll` (for `execcli.cpp`'s telescope-move/guiding
    wire protocol) - all thin faithful ports, no DB dependency in any of
    them.
  - `base_script` builds clean (only pre-existing benign warnings:
    `setValueInteger` overload-hiding, one `catch by value`), full
    `base` tree builds clean, `ctest` still 7/7.
- **Task 26 (DONE): `scriptexec` CLI** - the actual client that drives
  scripted observations against running devices, closing out the whole
  scriptexec push (tasks 22-26). New files, all added to `base_script`
  (in `base/script/`, since they're still library-side, non-DB parts of
  `rts2script`) except the CLI executable itself:
  - `scriptinterface.h`/`.cpp` (`ScriptForDevice`, `ScriptForDeviceStream`,
    `ScriptInterface` - trivial device-name-to-script-text lookup
    interface). Faithful port; dropped the classic `#include "nan.h"` in
    favour of `<cmath>`'s `NAN` directly, matching the convention already
    established elsewhere in this port (see task 24 notes - `nan.h` was
    never actually needed).
  - `scripttarget.h`/`.cpp` (`ScriptTarget:public Rts2Target` - a
    do-nothing target implementation, `save`/`saveWithID`/
    `setNextObservable`/`setTargetBonus` are all no-ops, `startSlew`
    always fails - it exists purely to satisfy `Rts2Target`'s interface
    so the script engine has *a* target object to hang script-fetching
    and position-reporting off of, with the real behavior delegated back
    to `ScriptInterface`). Faithful port, no changes needed.
  - New top-level `base/scriptexec/` directory (its own tier, an
    executable, not a library) with `scriptexec.h`/`.cpp` - the actual
    `main()`, `ScriptExec:public rts2core::Client, public
    rts2script::ScriptInterface`. Two things dropped, both purely
    cosmetic terminal-UI, not behavior:
    - The classic `ClientCameraScript`'s live ncurses progress bar
      (exposure/readout percentage redrawn on a timer via `curses.h`/
      `term.h`, `setupterm`/`curs_set`/`SIGWINCH` handling) - this drives
      the same `processImage()` that prints each written image's
      filename to stdout, which is kept (it's what the `xpaset`-piping
      examples in `usage()` actually depend on for automation). This is
      the same class of drop already used for JPEG-preview generation in
      `rts2image::Image` - real vendor/terminal-cosmetic paths, not
      functional behavior.
    - The unused `rts2core::ConnFork *afterImage;` field (and its
      `#include "connection/fork.h"`) - declared in the classic header
      but never referenced anywhere in `scriptexec.cpp`, a dead leftover
      in the classic tree. Dropped along with the now-unnecessary
      `connfork.h` include.
  - **Smoke-tested for real, full end-to-end pipeline**: ran
    `rts2-centrald` + `rts2-camd-dummy` (device `C0`), then
    `rts2-scriptexec --server localhost --port <port> --config rts2.ini
    -d C0 -s 'for 3 { E 1 }' -e '%f'` against them. Produced three real
    FITS files (confirmed with `astropy.io.fits` - valid `PrimaryHDU`,
    `200x100 uint16`, genuine non-zero dummy-camera pixel data, plus the
    `C0.shiftstore` binary table extension), printed each filename to
    stdout as designed (the automation contract the `usage()` examples
    rely on), and logged the expected "cannot find IMGP connection for
    image" message (no image-processing daemon was running in this
    smoke test - `queImage()`'s `getMinConn("queue_size")` correctly
    returns NULL and logs, exactly as ported from classic). First
    genuine end-to-end proof that a D50 device (well, dummy camd here -
    the six real D50 sensors/dome/mount/focuser/camera are all ported
    but weren't all wired up for this particular smoke test) can be
    actually commanded to run an observation script through base,
    without any database layer.
  - One CLI option-parsing gotcha hit while smoke-testing (not a bug in
    the port, a usage mistake): classic `Client` splits `--server`
    (hostname only) and `--port` (separate option) rather than accepting
    a combined `host:port` string the way device-side `--server` does -
    passing `--server localhost:18717` produces a "Name or service not
    known for localhost:18717:617" error (it appends the *default*
    centrald port to whatever string `--server` was given). Worth
    remembering if anyone reaches for this CLI again.

**Build plumbing added this push**: `kernel/CMakeLists.txt` now does
`find_package(PkgConfig REQUIRED)` + `pkg_check_modules(CFITSIO REQUIRED
cfitsio)` and links `${CFITSIO_LIBRARIES}` into `base_kernel` - CFITSIO is
a normal system package (found via pkg-config, version 4.6.3 on this
system), not vendor-SDK-gated like Andor/gxccd/FLI, so no opt-in cache
variable was needed.

**Recurring porting gotcha this push**: several classic `rts2fits`/
`rts2script`-family files `#include "block.h"` even though nothing in them
actually uses `Block` - it was just the easiest way to transitively pull in
`logstream.h`/`app.h`/`error.h`/`<climits>` in the classic tree's less
disciplined include graph. Each time, base drops the `block.h` include (it
would pull in the entire kernel unnecessarily), and instead adds back
whatever the file actually needs directly. Worth checking this again for
every remaining file in tasks 23-26.

## Site coverage checkpoint (2026-07-12)

Tasks 1-34 are all done. Following the scriptexec push (tasks 22-26,
which gave base a working end-to-end script-driving client), tasks
27-34 ported every driver the user could identify as actively deployed
at a real site:

- **SBT/BART**: `dome-bart`, `sensor-bart-rain`, `sensor-aws-mlab`,
  `focusd-aaf`, `teld-gemini` (Losmandy Gemini-2).
- **FRAM generic/shared drivers**: `dome-zelio` (+ new kernel
  `ConnModbus`), `teld-lx200`, `focusd-optec`, `focusd-nstep`,
  `sensor-external`.
- **Los Leones (FRAM)**: `sensor-fram-weather`; the rest of its stack
  (`focusd-aaf`/`dome-zelio`/`teld-lx200`/`sensor-nut`/`gxccd`) was
  already covered by earlier tasks.
- **Brown FRAM at Paranal**: `sensor-windrs`; `teld-paramount` (the
  big one - needed porting `GEM` for real and building the real
  `libmks3` vendor library from the user's own `~/paracl` source, see
  the "Teld-paramount" section above); the rest
  (`dome-zelio`/`camd-gxccd`/`focusd-nstep`/`sensor-external`) already
  covered.
- **White dome at Paranal**: user-confirmed identical driver stack to
  the brown dome - nothing new.
- **AVE lab CCD-testing computer**: `sensor-avelab` (user-supplied
  source, not in the classic tree at all).
- **Makak (zenith camera)**: user-confirmed uses `gxccd` + `d50-wfunit`
  (identical shutter hardware to D50) - both already ported, nothing
  new needed.

User's own assessment (2026-07-12): "everything I can reach is
supported" - the only unknowns are whether Markus Wildi or other RTS2
users/sites are still active, which isn't something this port can
determine (no usage-analytics/ping-back mechanism exists in classic
RTS2 either - see the user's own aside about why upstream developers
like having each install occasionally phone home).

**Update (2026-07-16): FLORES.** A real FLI CCD became available at the
FLORES site - potentially the first live piece of hardware this
cmake-based base/db tree actually runs against. Ported
`rts2-camd-fli` (`base/camd/fli/`, see the "FLI" section above) as
task 35: completely faithful port, same real `/home/mates/src/fliusb/
libfli` SDK already used by `focusd-fli`/`filterd-fli`, no adaptations
needed. Smoke-tested clean (no hardware attached, see "FLI" section for
details).

Candidates for what comes next, roughly in order of likely value:

1. **Wire scriptexec against the real D50 stack**, not just dummy camd -
   all six D50 drivers (camd/teld/focusd/dome/sensord) are ported and the
   client that can drive them now exists too; nothing has actually
   combined them in one live run yet.
2. **Add a real automated integration test** (spawn centrald + dummy +
   scriptexec, drive a script, assert on the produced FITS file) -
   flagged several times now, still hasn't been done, and the manual
   smoke test just performed for task 26 would be a natural template.
3. **Investigate the user's suspicion that a large fraction of the
   script language is unused/broken** (see memory
   `rts2-script-language-underused`) - `ACQUIRE`/`ElementAcquire` is
   confirmed broken/unused (three-short-exposures is used instead in
   practice), and the user suspects there may be more. A dedicated
   bugfixing pass over the `Element*` hierarchy could be high-value now
   that the whole engine actually runs.
4. Finish task 5's remainder (`connnotify`), port a driver that needs a
   deferred subsystem, or one of the other older options listed below -
   none of these are blockers, pick based on what's actually wanted next.

**Tasks 1-21 are all done**, including task 6 (`client.h`, folded in once
the monitor needed it), task 11 (`rts2-mon`, ported in full per explicit
user choice), task 12 (`Telescope` base + `Dummy` teld driver - see "Teld"
section above for what was deferred and why), task 13 (`Focusd` base +
`Dummy` focuser driver - no deferrals needed, see "Focusd" section above),
tasks 15-16 (Andor and GXCCD camera drivers, both built for real against
their actual vendor SDKs, not deferred - see the "Andor"/"GXCCD" sections
above), task 17 (TEFO focuser - see "Focusd-tefo" section, also added
`ConnUDP`/`ConnNoSend` to the kernel), task 18 (D50 mount - the biggest
single port so far, ~4850 lines across `ConnSerial`/`ConnEthernet`
(kernel) and `TGDrive`/`ConnREMOTES`/`Fork` (teld) - see "Teld-d50",
including a real crash bug found and fixed along the way), task 19
(FLI focuser + filter wheel - see "FLI" section, the first vendor SDK in
this port built from source, needing small patches to the vendor library
itself plus a vendor-API-version adaptation in the driver code), task 20
(D50 weather/sensor stack - all six sensors the user named: `system`,
`nut`, `cloud4`, `bart-rain`, `aws-mlab`, `d50-wfunit` - see "Sensord"
section, also closing the `conntcp` piece of task 5, and fixing two more
genuine classic-tree bugs along the way), and task 21 (dome-d50 - the
last driver actually running at D50, see "Dome-d50" section, one more
genuine bug fixed). Centrald, dummy camd, rts2-mon, dummy teld, dummy
focusd, dummy filterd, the real Andor/GXCCD/FLI camera-family drivers,
the real TEFO focuser, the real D50 mount, all six D50 sensord drivers,
and the D50 dome driver have each been smoke-tested as real running
processes/sessions, not just compiled.

**D50 push (user-directed, 2026-07-11) status: every device driver
actually running at D50 is now ported** - camd (Andor), focusd (TEFO),
teld (D50 mount), all six sensord drivers, and dome-d50. A live
end-to-end test at D50 itself (the user's stated goal for this push) is
now fully unblocked. Only remaining open item from the original task-5
scope is `connnotify`/`connfork` - not needed by anything ported so far,
still genuinely open if a future driver needs them.

Beyond the D50 push, other options, roughly in order of likely value:

1. **Add a real automated integration test** (spawn centrald + dummy,
   connect, drive a command, assert on the response) - flagged as valuable
   several times now and still hasn't been done. Everything past task 6
   has only been verified by hand (manual smoke test), which won't survive
   as regression coverage.
2. **Finish task 5's remainder**: `connnotify`/`connfork` (Connection
   transport variants) - still not needed by anything ported so far
   (`connnosend`/`connudp`/`connserial`/`connethernet`/`conntcp` are done).
3. **Port a driver that actually needs a deferred subsystem** - all four
   driver families ported so far (camd/teld/focusd/filterd) either had no
   deferrals or the deferrals were cleanly optional. A real consumer of
   `DevClientFilter`/`CommandFilter` (filter wheel) or `DevClientFocus`/
   `CommandChangeFocus`/`CommandSetFocus` (focuser) - see "Deliberately
   incomplete ports" - or a teld driver needing a real pointing model, or a
   camd driver needing SEP, would pressure-test whether those deferrals
   still hold up under real use. Note: filter wheel and focuser are two
   separate device types (`DEVICE_TYPE_FW`/`DEVICE_TYPE_FOCUS`) with two
   separate `DevClient`/`Command` families each - there's no single
   combined "focus wheel" device.
4. Revisit the SEP star-finding stub and the direct-FITS-write readout
   path in `camd`/`dummy`, the `SimbadTarget`/`rts2fits/devcliimg.h`
   deferrals in the monitor, or the TLE/GPointModel/TPointModel/
   ClientCupola/ClientRotator deferrals in teld, if a real use case needs
   any of them (see the relevant sections above for exactly what was
   dropped and why).
5. Port the smaller `rts2-cmon`/`rts2-talker` companion tools from
   `src/monitor/cmonitor.cpp`/`talker.cpp` (~120 and ~110 lines) - both
   trivial now that `client.h` exists; `rts2-talker` in particular is a
   nice lightweight "just watch the messages" companion to the full
   `rts2-mon` TUI.

No single one of these is a hard blocker on the others - pick based on
what the user actually wants next.

## Debian packaging: rts2-base (2026-07-16)

First real Debian package built from this tree, `rts2-base`: the DB-free
core (`rts2-centrald`, `rts2-mon`, `rts2-scriptexec`) plus every driver
whose control library links statically - no bundled proprietary shared
library. Scoped deliberately narrow per the user's framing: "no database,
no complex python, no extra scripts, just pure base."

**What changed to make this possible** (each also documented in its own
section above/in memory):
- `Configuration::getSpecialValues()` - left `[observatory]` longitude/
  latitude/altitude mandatory (user's explicit call: requiring these is a
  reasonable one-time setup step, not something to engineer around).
- `camd/gxccd` flipped from linking the vendor SDK's `.so` (needing an
  RPATH into the SDK tree) to its static `.a` - see "GXCCD" section update
  above. Verified via `ldd`: no RPATH, no vendor `.so`, only ordinary
  system libraries.
- Logging made syslog-first (`Daemon::sendMessage`/`Centrald`) - see
  "Load-bearing findings" above and the `rts2-logging-architecture-flaw`
  memory. Includes a real bug fix: `Daemon::doDaemonize()` was silently
  resetting the syslog ident back to the program's own name right after
  forking, defeating the fix for every actually-daemonized (non-`-i`) run.
- New `base/packaging/` directory: `rts2-start.in`/`rts2-stop.in`
  (adapted from classic `scripts/`, minus the old shared-logfile
  redirection), `rts2.service.in`/`rts2@.service.in` (adapted from
  classic `conf/`, minus `After=postgresql.service`), a trimmed
  `rts2.ini` (just `[observatory]`/`[centrald]` - other sections belong
  to future rts2-db/rts2-httpd packages), empty/comment-only
  `centrald`/`devices`/`services` (a freshly-installed package shouldn't
  autostart a network-listening daemon with site-specific options before
  the admin configures it), and an rsyslog/logrotate pair routing the
  `rts2` syslog ident to its own rotated file. All wired into the
  top-level `CMakeLists.txt` via `configure_file()` - useful independent
  of Debian packaging.
- `debian/` directory (native package, `debhelper-compat 13`): since no
  `CMakeLists.txt` in this tree has `install()` rules yet (touching all
  ~30 driver `CMakeLists.txt` files is a separate future cleanup, not
  folded into this pass), `debian/rules` overrides `dh_auto_install` to
  copy binaries directly from the build tree - a standard technique for
  upstream trees without polished install support. `BASE_GXCCD_SDK_DIR`/
  `BASE_FLI_SDK_DIR` point at this build host's SDK copies; adjust for a
  different builder. Explicitly excludes `rts2-camd-andor` (vendor SDK is
  `.so`-only, no static option despite earlier assumption otherwise -
  checked directly, see the GXCCD/Andor sections) and
  `rts2-teld-paramount` (`libmks3` source is under an informal
  non-redistribution understanding, not vendored into the build).
- **Three real format-string vulnerabilities found and fixed** in
  `rts2-mon` (`NWindowEdit::setValue`, `AbstractBoxSelection::drawRow`,
  `NComWin`'s history display) - all passed a data string directly as a
  `wprintw`/`mvwprintw` format argument with no format string of its own.
  Invisible under a plain manual `cmake`/`make` build; caught immediately
  by Debian's default hardening flags (`-Wformat -Werror=format-security`).
  See `UPSTREAM_BUGS.md` - identical bugs exist in the classic tree,
  unfixed there.

**Built and verified for real** (`dpkg-buildpackage -us -uc -b`, this
build host's `cmake` happens to be a snap install so `-d` was needed to
skip `dpkg-checkbuilddeps` - a real builder with `cmake` from `apt`, as
declared in `Build-Depends`, wouldn't need that):
- `rts2-base_0.1.0-1_amd64.deb` produced clean, no lintian available on
  this host but `dpkg-deb -c`/`-e`/`-I` inspected directly: all 30
  binaries + `rts2-start`/`rts2-stop` + `/etc/rts2/*` + systemd units +
  rsyslog/logrotate snippets present, `conffiles` list correct (six
  entries, no duplicates - initially duplicated because `dh_installdeb`
  already auto-detects everything under `/etc` as a conffile, and a
  manual `debian/rts2-base.conffiles` listing the same paths was
  redundant - removed).
- Auto-detected runtime `Depends:` are all ordinary system libraries
  (`libcfitsio`, `libncurses`, `libnova`, `libusb-1.0`, libc/libstdc++) -
  confirms no proprietary vendor blob leaked into the dependency chain.
- Extracted the built package and ran `rts2-start --help`/
  `rts2-centrald --help` straight from the installed-layout paths
  (`/usr/bin`, `/etc/rts2`) to confirm the real `/usr` prefix wiring
  works, not just the dev-tree defaults.

**Deliberately not done in this pass** (flagged for later, not blockers):
proper CMake `install()` rules for all driver binaries (would let
`cmake --install` work standalone, not just via `debian/rules`'
build-tree-copy override); a `rts2-andor` package bundling the vendor
`.so` (user's plan: "ship together with the library" - separate task);
`rts2-teld-paramount` packaging (blocked on the informal NDA question
about `libmks3` redistribution, not a technical blocker); `rts2-db`/
`rts2-httpd` packages (separate, DB-bound, out of scope for `rts2-base`).

### `rts2-drivers-fli` split out of `rts2-base` (0.1.0-3)

User wanted a standalone `.deb` for just the FLI (Finger Lakes
Instrumentation) drivers, given a built `libfli` tree is available -
same idea as the `rts2-andor` plan above, but for FLI. Rather than a new
top-level sibling project (which would re-nest and recompile all of
`base` again, adding to the redundant-rebuild count the user had just
flagged), this reuses the *same* `base/debian/` source package: added a
second binary stanza (`Package: rts2-drivers-fli`) to `debian/control`
and split `rts2-camd-fli`/`rts2-focusd-fli`/`rts2-filterd-fli` out of
`RTS2_BASE_BINARIES` into their own `RTS2_FLI_BINARIES` list in
`debian/rules`, installed into `debian/rts2-drivers-fli/usr/bin/`
instead. No extra compilation - one `dpkg-buildpackage` run now produces
both `.deb`s from the one build tree. `Depends: rts2-base (=
${binary:Version})` since the FLI binaries need rts2-base's config/user/
systemd-unit scaffolding to actually run as a device.

The FLI install step guards each binary with `[ -f ... ]` so a host
without `BASE_FLI_SDK_DIR` set still builds `rts2-base` cleanly -
`rts2-drivers-fli` just comes out empty there rather than failing the
whole build. Found one real bug while testing this: `dh_installsystemd`
defaults to acting on *every* binary package when called without `-p`,
so it started failing on `rts2-drivers-fli` ("does not install unit
'rts2.service'") the moment a second package existed - fixed by scoping
both calls in `override_dh_installsystemd` to `-prts2-base` explicitly.

Verified via a real `dpkg-buildpackage -us -uc -b -d` run (bumped to
0.1.0-3): `rts2-drivers-fli_0.1.0-3_amd64.deb` contains exactly the three
FLI binaries (`dpkg-deb -c`), `rts2-base_0.1.0-3_amd64.deb` no longer
contains any of them, and the auto-detected `Depends:` on
`rts2-drivers-fli` correctly resolved to `rts2-base (= 0.1.0-3)`. gxccd
was deliberately left in `rts2-base` at the time - not asked to split it
yet (see below, this changed the very next round).

Two follow-ups landed right after, both small: (1) `buildme.sh` and
`debian/rules` had two different hardcoded `BASE_FLI_SDK_DIR` paths that
had drifted apart (one per session/machine) - fixed by having
`buildme.sh` `export BASE_FLI_SDK_DIR=...` once and dropping the
hardcoded default in `debian/rules` (which already used `?=`, so the
exported env var flows straight into the Make variable, then into the
`-D` flag passed to `cmake` - verified with a real build using the
export instead of the old default). (2) The user asked whether gxccd and
`rts2-teld-paramount` could get the same split - see the next section
for gxccd; paramount was explicitly deferred (the user's own call: "skip
it for now" when asked whether a compiled `rts2-drivers-paramount` .deb
would be okay to publish given the informal non-redistribution
understanding around `libmks3` - see that section above for the
authorship context). Not revisited without the user raising it again.

### `rts2-drivers-gxccd` split out of `rts2-base` (0.1.0-4)

Same exact pattern as the FLI split above, requested right after it:
second binary stanza in `debian/control`, `rts2-camd-gxccd` moved out of
`RTS2_BASE_BINARIES` into its own `RTS2_GXCCD_BINARIES` (one binary,
unlike FLI's three - gxccd has no separate focuser/filter-wheel driver),
installed into `debian/rts2-drivers-gxccd/usr/bin/` guarded the same
`[ -f ... ]` way. Also dropped `BASE_GXCCD_SDK_DIR`'s hardcoded default
(was a stale personal path, same problem `BASE_FLI_SDK_DIR` had) - both
vendor SDK paths are now environment-sourced with no default in
`debian/rules`, matching each other.

Verified via a real `dpkg-buildpackage -us -uc -b -d` run with both
`BASE_FLI_SDK_DIR` and `BASE_GXCCD_SDK_DIR` exported (bumped to 0.1.0-4):
all three packages built in one run, `rts2-drivers-gxccd_0.1.0-4_amd64.deb`
contains exactly `/usr/bin/rts2-camd-gxccd`, `rts2-base_0.1.0-4_amd64.deb`
contains neither the gxccd nor FLI binaries, `rts2-drivers-fli` still
correct alongside it, and `rts2-drivers-gxccd`'s auto-detected `Depends:`
correctly resolved to `rts2-base (= 0.1.0-4)`. No new bugs found this
time - the `dh_installsystemd` scoping fix from the FLI split already
covers any number of extra binary packages, not just one.

`buildme.sh`'s plain-`cmake` loop still doesn't pass
`BASE_GXCCD_SDK_DIR` at all (only `BASE_FLI_SDK_DIR` was ever wired
in there) - not changed here since the actual gxccd SDK path on the
user's other build machines isn't known; add `export
BASE_GXCCD_SDK_DIR=/path/to/libgxccd` there the same way if/when needed.

### Window position (LTV1/LTV2/CRPIX) never recorded for windowed exposures - fixed

User reported hitting this as a real, high-priority bug: RTS2 never
recorded where a windowed (non-full-frame) exposure's readout window
actually was on the detector. They already had a workaround in a
separate pipeline (`asarina/pipeline/patch_window.py`) that *guesses*
the window position post-hoc from `NAXIS1`/`NAXIS2` alone, assuming it
was centered on the chip - exactly the kind of thing that should never
be necessary if RTS2 wrote the real value at capture time, which was the
ask: fix it at the point of writing the FITS file.

Traced to `kernel/src/devcliimg.cpp`'s `allImageDataReceived()`: the
`mods[2]`/`mods[3]` array that becomes both the literal `LTV1`/`LTV2`
FITS header values and (via `writeWCS()`) the `CRPIX1`/`CRPIX2` sky-WCS
adjustment is built only from the optional per-channel
`CHAN1_OFFSETS`/`CHAN2_OFFSETS` config (multi-amplifier geometry) and
never incorporates the window's own per-exposure `x`/`y` position
(`imgh->x`/`imgh->y`, decoded a few lines above and already used
correctly by the neighboring `DATASEC`/`DETSEC`/`TRIMSEC` computations).
Since no camera actually deployed anywhere uses `CHAN1_OFFSETS`,
`mods[2]`/`mods[3]` stayed exactly `0` for every real windowed exposure,
regardless of where the window actually was. **Confirmed identical in
classic RTS2** (`lib/rts2fits/devcliimg.cpp`, byte-for-byte) - a
genuine, long-standing upstream bug, not something this port introduced.
Full write-up in `UPSTREAM_BUGS.md`.

**Fix**: fold the window offset into the same `mods[2]`/`mods[3]`
computation, using the identical `(detector_pixel - x) / bin` convention
`DATASEC` already uses, so a windowed image's `LTV1`/`LTV2` are now
self-consistent with its own `DATASEC`:

```diff
-			if (bin1 != 0)
-			{
-				mods[2] /= bin1;
-			}
-
-			if (bin2 != 0)
-			{
-				mods[3] /= bin2;
-			}
+			if (bin1 != 0)
+			{
+				mods[2] = mods[2] / bin1 - ((double) x) / bin1;
+			}
+
+			if (bin2 != 0)
+			{
+				mods[3] = mods[3] / bin2 - ((double) y) / bin2;
+			}
```

**Verified empirically**, not just by inspection - stood up a real,
throwaway `centrald` + `rts2-camd-dummy` + `rts2-scriptexec` chain
(dummy camera configured with `--detsize 0:0:1000:1000 --datasec
20:20:960:960`, a `BOX x y w h` script command to command a real
windowed readout, `E 0.1` to expose) and inspected the resulting FITS
headers directly:
- Window at `x=0,y=0,w=100,h=100` (overlapping the chip's own overscan
  edge): `DATASEC=[21:100,21:100]`, `LTV1=LTV2=-0` - consistent (window
  starts exactly at the detector origin, so no shift).
- Window at `x=500,y=500,w=200,h=200`: `LTV1=LTV2=-500`,
  `DATASEC=[1:200,1:200]` (window fully inside the good-data region) -
  the `-500` exactly matches the window's own offset.
- Repeated the second case with `--wcs` set on the dummy camera:
  `CRPIX1`/`CRPIX2` shifted by the same amount through the untouched
  `writeWCS()` code, confirming the one fix correctly propagates into
  both the literal `LTV1`/`LTV2` header and the sky-WCS reference pixel.

Deliberately left alone: `LTM1_1`/`LTM2_2` (a separate, pre-existing gap
- they don't reflect `bin1`/`bin2` either, only matters for binned data,
out of scope for this specifically-reported window-position bug).

### FLI camera driver never flushes the CCD before exposure by default - fixed, plus a new idle-flush mitigation

Second real bug the user reported right after the window-position one:
`camd/fli/fli.cpp`'s `nflush` ("number of flushes before exposure")
defaults to `-1`, which means `FLISetNFlushes()` is never called unless
`-l <N>` is passed on the command line - the camera is left at its own
firmware default, which in practice (on the user's real hardware) is 0
flushes. **Confirmed identical in classic RTS2** (`src/camd/fli.cpp`,
byte-for-byte) - full write-up in `UPSTREAM_BUGS.md`.

User's guidance on what the right numbers actually are: 1 flush is
right for a camera in regular use; after a longer idle gap, 2 or
(exceptionally) 3 would be better for just the *first* exposure back -
but the better fix for that case is a periodic idle-time CCD dump, not
raising `nflush` (which would slow down every subsequent normal
exposure's charge-up, not just the first one after a gap). They also
flagged the deeper architectural point: classic `camd` assumes the
driver/hardware autonomously handles CCD reset between exposures - not
true for FLI, and nothing in `camd` compensates.

**Fix, both parts in `camd/fli/fli.cpp`:**
- `nflush` now defaults to `1` instead of `-1` (still overridable via
  `-l`, including back to `-1` for the old behavior).
- New, FLI-specific periodic idle-flush timer: `idle_flush_period`
  (writable `ValueInteger`, seconds, default 300, `0` disables). Follows
  the exact same self-rescheduling `addTimer`/`postEvent` pattern
  already used in this file for `EVENT_TE_RAMP` (temperature ramping) -
  a new `EVENT_IDLE_FLUSH` case in `Fli::postEvent()` calls
  `Camera::isIdle()` (already used the same way in `temperatureCheck()`)
  and, if idle, the same `FLIFlushRow()` call `stopExposure()` already
  uses elsewhere in this file, then re-arms itself. Armed once from
  `initHardware()`; `setValue()` re-arms it immediately if the period is
  changed from disabled (`<=0`) back to a positive value at runtime,
  since the self-rescheduling chain would otherwise have already stopped.

This is genuinely new functionality with no equivalent in classic - not
just a straight bug-for-bug port. Flagged in `UPSTREAM_BUGS.md` as worth
generalizing to a `camd`-level mechanism if other camera families turn
out to have the same gap, rather than reimplementing it per-driver.

Builds clean (`rts2-camd-fli`, only pre-existing unrelated warnings).
**Not empirically tested against real FLI hardware** - unlike the
window-position fix, there's no dummy-camera equivalent for this (the
mechanism is FLI-SDK-specific, `FLIFlushRow`/`FLISetNFlushes`), so this
needs real-world verification at a site with actual FLI hardware
attached before being fully trusted.

## Focusc (base/focusc/ - rts2-focusc, command-line camera exposure client)

Ported `src/focusc/focusclient.h`/`.cpp` + `focusc.cpp` as new top-level
`base/focusc/` (`FocusClient`/`FocusCameraClient` -> executable
`rts2-focusc`). Kept the classic name after asking the user - despite
"focus" no longer being the primary use (per the user: it's really a
command-line FITS-grabbing client with an optional post-exposure script
hook; the focus-adjustment protocol exists but "in practice has not been
used that way").

Turned out to be almost entirely mechanical: the whole class hierarchy
this client drives - `DevClientCameraFoc`/`DevClientFocusFoc`/`ConnFocus`/
`DevClientPhotFoc` (`kernel/include/devclifoc.h`) and every `Command*`
class it queues (`CommandBox`/`CommandCenter`/`CommandChangeFocus`/
`CommandIntegrate`/`CommandExposure`/`CommandChangeValue`) - was already
ported to `kernel/` by earlier tasks, for other consumers (`element.cpp`/
scriptexec). `focusc` is the first thing to actually exercise
`devclifoc.h`'s classes end to end.

**Deliberately not ported** (out of scope, same reasoning as the earlier
`rts2x`/`rts2-viewer` planning): `foctest.cpp` (a tiny separate smoke-test
binary) and `xfocusc.cpp`/`xfitsimage.cpp` (the X11 GUI - superseded by
the planned Qt5 `rts2-viewer` in a future `rts2x/` subtree, not this one).
`focusclient.h`'s classic `EVENT_INTEGRATE_START`/`EVENT_INTEGRATE_STOP`/
`EVENT_XWIN_SOCK` event codes were dropped for the same reason - grepping
the classic tree shows they're only ever referenced by `xfitsimage.cpp`/
`xfocusc.cpp`, never by `focusclient.cpp`/`focusc.cpp` itself.
`EVENT_EXP_CHECK` (drives the live exposure/readout progress bar) is kept.

The one piece of real porting work: the classic autotools build picked
between `<curses.h>`/`<ncurses/curses.h>` via `RTS2_HAVE_CURSES_H`/
`RTS2_HAVE_NCURSES_CURSES_H` feature-detection macros that no longer exist
in this tree (base targets modern Linux only). Replaced with a direct
`<curses.h>`/`<term.h>` include and a `pkg_check_modules(NCURSES REQUIRED
ncurses)` in `focusc/CMakeLists.txt`, copying `rts2-mon`'s already-proven
ncurses CMake pattern (`base/monitor/CMakeLists.txt`) rather than
inventing a new one. `ProgressIndicator` (`kernel/include/utilsfunc.h`)
lives in the global namespace, not `rts2core` - a one-line fix once the
build caught it.

Builds clean, no warnings. **Verified empirically**: stood up a real
throwaway `centrald` + `rts2-camd-dummy` chain and ran `rts2-focusc
--server localhost --port <port> -d C0 -e 0.5` against it - logged in,
took repeated exposures, printed each written filename to stdout exactly
like classic, rendered the live `EXPOSING`/`READING` progress bar
correctly, and produced valid FITS files (`astropy.io.fits` confirmed
`NAXIS1`/`NAXIS2`/`EXPOSURE`/`CCD_NAME` all correct).

Wired into Debian packaging: added `focusc/rts2-focusc` to
`RTS2_BASE_BINARIES` in `base/debian/rules`. Also added
`sendcmd/rts2-sendcmd`, which was already fully ported and buildable but
had been missed from the packaging list until now - a pre-existing
omission noticed while touching this file, not something introduced by
this task.

### Code review follow-up: Ctrl-C didn't stop exposures, and omitting -d silently touched every camera

The user asked two review questions right after the port landed: does
Ctrl-C stop an in-progress exposure and disconnect politely, and what
happens if you run `rts2-focusc` with no `-d` at all? Both turned out
to be genuine bugs, confirmed identical in classic (full write-up in
`UPSTREAM_BUGS.md`, this is just the base-side summary):

1. **Ctrl-C did nothing but close the socket.** Neither `FocusClient`
   nor `FocusCameraClient` overrode any shutdown hook - `Connection::
   ~Connection()` just calls `close(sock)`, no "stop exposure" message
   of any kind. Verified: a 20s exposure SIGINT'd at ~40% ran to full
   completion regardless (dummy camd logged `end exposure without
   exposure connection, state 1` - tolerated gracefully, but nothing
   ever asked it to stop).

2. **Omitting `-d` wasn't a no-op** - `initFocCamera()` applied window/
   exposure-time/binning/dark-shutter settings to *every* connected
   camera unconditionally, gating only the final exposure-trigger
   command on a name match. Verified: `-X 10 -Y 10 -W 50 -H 50` with no
   `-d` produced no visible output, but a *separate*, later `-d C0 -e
   0.3` invocation (no window options at all) came back windowed to
   50x50 anyway - the earlier "no-op" had silently left that window on
   the camera.

**Fixes:**
- New `rts2core::CommandStopExposure` (`kernel/include/command.h`/
  `command.cpp`, mirrors the existing `CommandBox`/`CommandCenter`
  pattern - sends `stopexpo`). `FocusClient::run()` now calls a new
  `stopOwnedExposures()` after `Client::run()`'s loop exits for any
  reason: scans live connections for cameras this process itself armed
  (tracked in a new `armedCameras` list - never a camera it's only
  passively watching), sends `CommandStopExposure` to any still
  `CAM_WORKING`, and pumps `oneRunLoop()` for up to 2s to let it
  actually land before the sockets close.
- `initFocCamera` split into `armCamera()` (settings + exposure-trigger,
  only ever called for a camera this process has decided it owns) plus
  an explicit, gated decision for what "-d not given" means, per the
  user's own suggested policy (there's no concept of a default camera
  in RTS2 - `rts2.ini`'s `imgproc.astrometry_devices` is a
  special-purpose astrometry hint, not a general default, and would be
  a weird thing to repurpose): after a 1s settle delay
  (`CAMERA_CHOICE_DELAY`, new `EVENT_CAMERA_CHOICE`) for centrald to
  report every connected device - exactly one camera -> use it (with a
  printed note); zero -> stay passive; more than one -> print every
  connected camera's name, ask for `-d <name>`, touch nothing.

**Verified empirically**, three scenarios:
- Real hardware (this sandbox's live `rts2-camd-v4l`): a 20s exposure
  SIGINT'd after ~3s aborted immediately instead of running to
  completion; the live `rts2-executor` also watching that camera logged
  `detected exposure failure. Continuing with the script` - noticed and
  recovered gracefully.
- No `-d`, exactly one camera connected (same live instance): printed
  `no -d given and exactly one camera (C0) connected - using it` and
  exposed it correctly.
- No `-d`, two cameras connected (throwaway dummy pair): printed `no -d
  given and more than one camera connected (C0, C1) - pass -d <name> to
  pick one; nothing was touched`, exited cleanly (not killed), no FITS
  file, no settings changed on either camera.

Same Ctrl-C gap exists in `xfocusc.cpp` (not yet ported, see the
`rts2x`/`rts2-viewer` plan) - worth carrying the same fix over then.

### Follow-up: -e/-b/-X/-Y/-W/-H aren't scoped per -d camera either

Third review question from the user, about a command line like
`rts2-focusc -d C0 -e 1 -b 0 -d C1 -e 5 -b 1`: is this actually
per-camera (each `-d` "owning" the settings that follow it), or could
it silently do something else? Confirmed the latter (full write-up in
`UPSTREAM_BUGS.md`) - `-e`/`-b`/`-X`/`-Y`/`-W`/`-H` are plain scalars,
last-value-wins, applied identically to *every* named camera regardless
of position relative to `-d`. Verified: that exact command line gave
**both** C0 and C1 `EXPOSURE=5.0`/`BINNING=2x2`, not the per-camera
1s/1x1 and 5s/2x2 it visually suggests.

Asked the user how to handle it (real per-camera settings scoping would
mean reworking the option parser into per-name groups - a materially
bigger change than anything else in this file); they chose the cheapest
option: warn, don't redesign. Added per-flag occurrence counters and
`warnIfSettingsAmbiguous()` (called once from `init()`): if more than
one camera is named *and* any of those flags was given more than once,
prints a clear warning explaining the real semantics. Deliberately
narrow - `-d C0 -d C1 -e 5` (shared settings, completely unambiguous)
and repeating `-e` with only one camera named (ordinary CLI behavior)
both correctly stay silent. Verified all three cases empirically.

### Hooking rts2-focusc up to ds9 via XPA (`-F`), and a real crash found along the way

The user asked whether a finished FITS file could be shipped to `ds9`
via XPA, "even a hook-on script is a valid solution." It already was -
`-F <script>` (the `ConnFocus` mechanism, already ported to
`kernel/devclifoc.cpp`) execs a script with the completed image's path
as `argv[1]` after every exposure with the shutter open, no new code
needed. Verified for real against this sandbox's actual `ds9` (XPA
reachable, confirmed via `xpaaccess ds9`):

```sh
#!/bin/sh
# must stay silent on stdout - see the crash/parsing note below
xpaset -p ds9 fits "$(readlink -f "$1")" >/dev/null 2>&1
```
```
rts2-focusc -d C0 -e 0.5 -F /path/to/that/script.sh
```

Two gotchas surfaced, one a real bug and one just a usage note:

- **Real crash, fixed** (full write-up in `UPSTREAM_BUGS.md`):
  `ConnFocus`'s destructor posts `EVENT_CHANGE_FOCUS` whenever the
  script doesn't itself print a `change <id> <val>` line - true for any
  non-focusing hook script, including this one.
  `DevClientCameraFoc::postEvent()` handles that by looking up
  `getValueChar("focuser")`, which is `nullptr` on any camera without a
  paired focuser (the dummy camera, `v4l`, most single-camera setups),
  and passes that straight into `Block::getOpenConnection()` ->
  `Connection::isName()`, which did a raw `strcmp()` against it -
  deterministic SIGSEGV on literally the first completed exposure.
  Confirmed identical in classic. Caught live under `gdb`, fixed with a
  one-line null-guard in `isName()` (`kernel/include/connection.h`) -
  fixed at that layer rather than just the call site, since
  `getOpenConnection(nullptr)` has no sensible meaning for any caller.
  Re-verified clean after the fix: multiple exposures in a row, no
  crash, hook fires every time.
- **Hook script must stay silent on stdout.** `ConnFocus` parses the
  script's stdout as protocol data (`change <id> <val>` or six
  sextractor-style numbers) - any other output gets logged as a
  `MESSAGE_ERROR "Get line: ..."`. `xpaset` is silent on success, so
  redirecting its output away is enough.
- **Relative paths don't work** - not an RTS2 bug, a `ds9`/XPA quirk:
  the path `rts2-focusc` hands the hook is relative to the directory it
  was run from, but `ds9` resolves relative paths against its own
  process's cwd, not the caller's. `readlink -f` (as above) fixes it.

Also confirmed the alternative, classic-documented idiom (piping
filenames on stdout through a shell loop to `xpaset`, as
`scriptexec --help` itself suggests) needs a tweak for `rts2-focusc`
specifically: unlike `scriptexec` (which deliberately dropped its live
progress bar - see the scriptexec section above), `rts2-focusc` keeps
one, and its `\r`-redrawn progress segments share stdout "lines" (as
seen by a plain newline-based reader) with the actual filenames. Piping
through `tr '\r' '\n' | grep '\.fits$'` first isolates the filenames
correctly; `-F` avoids the whole problem by getting the exact filename
directly as an argument, no line-parsing involved, and is the
recommended approach for this tool.

### valgrind check (user-requested) turned up one real, kernel-wide leak

After the crash fix above, the user asked to run `rts2-focusc` under
`valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes`
to check for memory issues. Ran it across four scenarios: a plain
exposure loop with an `-F` hook, a `SIGINT` sent mid-exposure
specifically to exercise the Ctrl-C stop-exposure fix (an earlier
section above), the ambiguous-multi-camera path, and the single-camera
auto-pick path.

All four came back identical: a `still reachable` ~31KB (all `ncurses`/
`libtinfo` terminfo caching plus `Configuration`'s parsed `rts2.ini` -
both long-lived globals, not leaks) and one small `definitely lost`
block, always the same backtrace - `FocusClient::postEvent()` re-arming
the `EVENT_EXP_CHECK` progress timer. Nothing from any of this session's
own additions (`armCamera`/`stopOwnedExposures`/`CommandStopExposure`/
`warnIfSettingsAmbiguous`/`EVENT_CAMERA_CHOICE`) showed up at all.

The one real finding turned out to be kernel-wide, not focusc-specific:
`Block::~Block()` (`kernel/src/block.cpp`) never sweeps its own
`timers` map, so any timer still pending at shutdown - which for a
self-rescheduling timer is always exactly one, since the next firing is
queued before the current handler returns - never gets `delete`d. Full
write-up (including why it's safe to fix unconditionally) in
`UPSTREAM_BUGS.md`. Confirmed identical in classic. Fixed with a
four-line addition to `~Block()`; re-ran the same four valgrind
scenarios afterward and all came back `definitely lost: 0 bytes in 0
blocks`, full test suite (`ctest`, all 7 including `block_connection`)
still green.

### Teld-dummy: parked mount flooding the log with "below horizon" - fixed

Unrelated to focusc: a live `rts2-teld-dummy` (T0) had been sitting
parked since 02:21 and had been logging "info retrieved below horizon
position, stop move" continuously ever since, at a rate climbing from
~1.4k lines/hour right after park to ~5k/hour by mid-afternoon (more
pollers/monitors connecting through the day, not the mount getting
worse). Root cause, two parts:

1. `Telescope::infoUTCLST()` (`lib/rts2tel/teld.cpp`) logs that ERROR
   on literally every `info()` poll while alt/az is below the hard
   horizon - the `if (ret <= 0)` guard around it never actually guards
   anything, because the base `Telescope::abortMoveTracking()` (used
   by the dummy driver) unconditionally returns 0. No debounce, no
   once-per-episode logging. Confirmed identical in classic
   (`lib/rts2tel/teld.cpp`) - full write-up in `UPSTREAM_BUGS.md`.
2. `Dummy::startPark()` (`teld/dummy/dummy.cpp`) parked to a hardcoded
   RA=2h/Dec=2deg with no horizon awareness at all - if that arbitrary
   point sits below the site's hard horizon (it did), the dummy just
   sits there logging the error forever.

Fixed the dummy-specific half (the general debounce issue is left as a
documented but unaddressed upstream wart, since deliberately tracking/
slewing through a below-horizon path is expected to be noisy - that's
not this bug): `Dummy::startPark()` now parks to local zenith (alt=90),
computed via the existing protected `Telescope::getEquFromHrz()`
helper - always above any hard horizon regardless of site latitude or
time of year. Since zenith's RA constantly shifts with sidereal time,
`Dummy::info()` now re-locks onto the *current* zenith every poll while
in the `TEL_PARKED` state, so it can't drift back below horizon just
from sitting idle for hours. `startPark()` also sets `setTelTarget()` to
the same zenith point, so `isMoving()`'s instant-teleport fast path
(triggered whenever `move_fast` is set or the estimated slew time has
already elapsed) lands there too instead of on a stale previous target.

Verified live: isolated centrald + `rts2-teld-dummy` pair, `rts2-sendcmd
... T0 park`, confirmed the log stopped growing new "below horizon"
lines immediately, and stayed silent across a further 45s parked.

## Conventions being used

- `#pragma once`, `nullptr`, `<cstdint>`/`<cstring>`/... over C headers.
- C++17, but change kept minimal per file - not gold-plating every method
  with `override`; correctness and a green build come first given the size
  of this codebase.
- Drop dead legacy-platform branches (`sun`, `__CYGWIN__`, ancient-GCC
  feature detection) outright - base targets modern Linux/glibc only.
- When a file's only problem is an accidental/unnecessary `#include`, drop
  it and say so in a comment - don't silently carry it forward. Several
  found this way: `timestamp.cpp`/`block.h`, `iniparser.h`/`value.h`,
  `block.cpp`/`client.h`, `devclient.h`'s circular `block.h` include.
  Verify with `grep` before dropping, every time - don't assume.
- Headers copied faithfully in structure/naming from the classic tree so
  diffing against `../include/*.h` stays easy; comments note every
  deliberate deviation inline (grep base/ for "base note:" to find all of
  them).
- When a header declares a large family of concrete subclasses tied to
  specific driver/client types, port only the base class and defer the
  subclasses (see "Deliberately incomplete ports" above) - write down
  *exactly* which ones were dropped and why, don't just say "some subclasses".
