# gui: status

`gui` is a Qt5 Widgets GUI camera-viewer client, sibling to `base`
(kernel + drivers, no DB) and `db` (rts2db + rts2-executor). It
replaces the old X11-based `rts2-xfocusc` tool
(`src/focusc/{focusclient,xfocusc,xfitsimage}.cpp`), combining:

- the RTS2 wire-protocol/image-receiving **transport** logic that
  `rts2-xfocusc` proved out (login, device discovery, receiving images) -
  reused not by porting `FocusClient`/`XFocusClientCamera` directly, but by
  building on the already-ported, focus-independent
  `rts2image::DevClientCameraImage` (`base/kernel/include/devcliimg.h`),
  which turned out to already provide exactly the needed
  `cameraImageReady(Image*)` hook with none of the focus-stepping or X11
  baggage the classic tool carried.
- the **UI/UX conventions** of `~/fiber_pointing/bin/fiber_pointing_client.py`
  (the Perek-2m fiber pointing camera client) - a `QGraphicsView`/`Scene`
  image canvas with a draggable crosshair overlay, dock widgets for
  expose controls and a log console - rebuilt in Qt5/C++, not that
  script's own instrument-specific business logic (G1/G2 fiber-pointing
  constants, field-rotation math, astrometry.net WCS solving,
  autoguiding, dithering - none of that was reusable, it's all specific
  to that one two-camera fiber spectrograph).

A third option, building this in `~/rtspy` (a from-scratch Python
reimplementation of RTS2), was considered and ruled out: it has no
image-transfer layer at all yet, which would have been a much bigger
project than reusing the already-working C++ transport in `base`.

## v1 milestone: done, verified live

`rts2-viewer --device <name> --server <host>` (the `--server`/`--port`
options come for free from `rts2core::Client`'s own constructor, same as
every other RTS2 client):

- Connects to centrald, discovers the named camera device via the same
  generic `Client::createOtherType()` dispatch every RTS2 client uses
  (`gui/viewer/src/viewerclient.cpp`) - no focus-specific code needed at
  all, confirmed by grepping the classic transport for what a plain
  viewer doesn't need (`center()`, `EVENT_INTEGRATE_START/STOP`,
  `addPollSocks`/`pollSuccess`/`EVENT_XWIN_SOCK`).
- Displays received images in a `QGraphicsView`/`Scene` canvas
  (`imagecanvas.cpp`) with a draggable crosshair overlay
  (`sightitem.cpp`, a C++ port of `fiber_pointing_client.py`'s
  `SightGraphicsItem`, stripped of its spin-box binding and
  star-centroid autodetection - just the box, crosshair, and
  drag-clamped-to-scene behaviour).
- Fires exposures from a dock-widget "Expose" panel (exposure time +
  button), and shows a log console dock, matching fiber_pointing's own
  dock-widget layout convention.
- **Verified live** against the dummy-device systemd setup already
  running on this machine (`rts2-camd-dummy -d C0` etc., set up during
  the `rts2-db`/executor testing session): connected, requested a 1s
  exposure, received and displayed a real (dummy, noise+stars) image,
  confirmed visually via a screenshot. Window-close quits cleanly and
  instantly (MainWindow's destructor calls `requestQuit()`, consumed on
  the worker thread's next `idle()` tick, ending `Client::run()`'s loop -
  `QThread::wait()` returns immediately, no hang).

## Stretch/scaling: reused existing code, no new math needed

`rts2image::Image::getChannelGrayscaleImage()` (`base/kernel/src/image.cpp`)
already does exactly what classic `XFitsImage` used for its own display:
quantile-based auto-scaling to an 8-bit buffer plus a Y flip
(`invert_y=true`). `ViewerCamera::cameraImageReady()`
(`viewer/src/viewercamera.cpp`) calls this directly and wraps the result
in a `QImage(..., QImage::Format_Grayscale8)` - v1 needed no separate
zscale/asinh/etc. stretch implementation of its own (the plan originally
assumed hand-rolling that math; this existing method made it
unnecessary). One consequence: since the buffer already arrives
pre-flipped top-down, `ImageCanvas` does **not** need
`fiber_pointing_client.py`'s `graphicsView.scale(1, -1)` view-level flip.

## Threading model

`rts2core::Client::run()` is its own blocking `poll()`-based loop; Qt owns
its own (`QApplication::exec()`). Rather than integrating the two (the
way classic `XFocusClient` added the X11 display fd into RTS2's own poll
loop via `addPollSocks`/`pollSuccess` - there's no equivalent hook for
handing Qt's loop to a foreign event source), `ViewerClient` runs
unmodified on its own `QThread` (`ClientThread` in `viewerclient.h`).

- **Worker -> GUI** (images, camera-created, exposure state): plain Qt
  signals with automatic queued dispatch - this direction works fine
  because the *receiver* (`MainWindow`, GUI thread) runs a real Qt event
  loop, regardless of what the sending thread is doing.
- **GUI -> worker** (trigger an exposure, request quit): NOT done via Qt
  signals, because the worker thread runs RTS2's own loop, not
  `QThread::exec()` - a queued signal delivered there would never be
  pumped. Instead, `std::atomic<bool>`/`std::atomic<double>` flags set
  directly from the GUI thread's button handler, consumed on
  `ViewerClient::idle()`'s next tick (same polling idiom `sendcmd.cpp`
  already uses for its own timeout/done checks).

## Known gaps / follow-ups (not blocking, not yet requested)

- **External SIGTERM doesn't quit the process** (found while testing):
  killing `rts2-viewer` from a shell doesn't terminate it, only `SIGKILL`
  does. Normal usage - closing the window - quits instantly and cleanly
  (verified), so this doesn't affect a real user, but the discrepancy is
  unexplained (likely `rts2core::App`/`Daemon`-family signal handling
  interacting oddly with living inside a Qt app) and worth a proper look
  before packaging this.
  - Related cosmetic quirk, same root cause family: at process start (and
    at abrupt exits like `--help`), Qt prints
    `QSocketNotifier: Can only be used with threads started with QThread`.
    Harmless so far (has not affected the verified working paths above),
    not yet root-caused.
- `getChannelGrayscaleImage`'s quantile parameter is hardcoded (0.005,
  the classic default) - not yet exposed as a UI control. Easy follow-up:
  a spinbox wired the same way as the expose panel.
- No Debian packaging yet (`gui/debian/`) - deliberately out of scope
  for this pass, same reasoning as `rts2-base`/`rts2-db`: prove the app
  works first.
- Everything listed as explicitly out of scope when this was planned
  remains out of scope: WCS solving, autoguiding, dithering, DS9
  spawning, sound effects, multi-camera tabbed UI, field-rotation math.
  Add only if a specific instrument needs this viewer to do more than
  view + mark + expose.

## Round 2: exptime bug fix, multi-camera support, camera control panel

User caught a real bug from live use: the exposure request never actually
sent the requested exposure time, so the camera just used whatever
"exposure" value was already set on the device from previous use (an
executor test the night before had left it at 20s). Root cause:
`CommandExposure` (`kernel/include/command.h`) takes no exposure-time
argument at all - it just triggers a `CCD_WORKING` exposure using
whatever the device's own `"exposure"` Value currently holds. Classic RTS2
cameras always work this way (`exposure` is a plain writable Value, set
via `CommandChangeValue`, same as `binning`/`filter`/`CCD_SET`/`COOLING` -
see `base/camd/src/camd.cpp`'s `createValue()` calls). Fixed in
`ViewerClient::idle()` (`viewerclient.cpp`): now queues
`CommandChangeValue(this, "exposure", '=', requestedExptime)` immediately
before `CommandExposure` on the same connection - RTS2 processes commands
queued on one connection strictly in order, so this is guaranteed to land
before the exposure it's meant for (same idiom classic
`FocusCameraClient` always used).

Also implemented, following the same session's request to mirror
`fiber_pointing_client.py`'s multi-camera setup (it drives two cameras,
G1/G2, from one app) and its "right column" of per-camera controls:

- **Multi-camera enumeration**: `ViewerClient::createOtherType` no longer
  filters by a single `--device` name - it accepts every
  `DEVICE_TYPE_CCD` device that appears, keeping a
  `std::map<std::string, ViewerCamera*>` and emitting
  `cameraCreated(name, camera)` for each one. `--device` still exists but
  is now just an optional hint for which camera starts out selected;
  every discovered camera is watched in the background regardless, and
  can be picked from a new camera-selector combo box.
- **Right-hand control dock** (`mainwindow.cpp`): camera selector, the
  existing exptime/Expose panel, plus new filter and binning combo boxes
  and a cooling group (`CCD_TEMP` read-only monitor label, `CCD_SET`
  target-temperature spinbox, `COOLING` on/off checkbox) - per the user's
  explicit framing, cooling is monitor-and-adjust-the-target, not force
  an absolute value; `rts2-camd`'s own automation handles the rest.
- **Generic value-change plumbing**: `ViewerCamera::valueChanged()`
  (override of the `DevClient` hook every RTS2 client already gets
  post-connect and on every live update) emits a single
  `valueUpdated(name, numericValue, choices)` signal; `choices` is
  populated for `ValueSelection`-typed values (`binning`, `filter` -
  `rts2core::ValueSelection::selSize()`/`getSelName()`) and empty for
  plain numeric ones (`CCD_TEMP`, `CCD_SET`, `COOLING`). `ViewerClient`
  gained a matching generic `requestValueChange(name, op,
  variant<int,double,bool>)`, queued the same way exposure requests are
  (GUI thread sets, `idle()` drains and dispatches
  `CommandChangeValue`) - one mechanism now covers exposure, filter,
  binning, and cooling instead of a one-off per feature.
- **Per-camera state cache** (`MainWindow::cameraStates`): every camera
  keeps receiving/updating in the background even while a different one
  is selected in the combo box, so switching back to a previously-seen
  camera replays its last-known filter/binning/temperature immediately
  from the cache rather than waiting for the device to resend it (it
  only resends on an actual change, not on request).

**Not fully re-verified visually this round**: screen-capture tooling
(`import`/`xwd`) that worked earlier in this session started failing
consistently (`BadMatch`/`X_GetImage` from `xwd`, an oddly-worded
"missing an image filename" from `import` even with a valid path) -
apparently an XWayland quirk on this host, not something this session
resolved. The build is clean and the app runs/connects exactly as before
(confirmed via `journalctl -t rts2`, which shows the dummy camera's real
exposure-state transitions), but the new exptime-fix/filter/binning/
cooling behavior needs a live look by the user rather than an
agent-side screenshot this time.

## Round 3: progress bar, run/stop, repeat count

User confirmed round 2 (exptime fix + multi-camera + control panel) works
live, then asked for three more fiber_pointing_client.py-style additions:
an exposure/readout progress bar, a "run/stop" continuous-exposure
button, and a repeat-count box.

- **Progress bar**: explicitly asked to copy "the RTS2 way", i.e.
  `rts2-mon`'s own mechanism, not reinvent one. Traced it to
  `rts2core::Connection::getProgress(now)`
  (`kernel/src/connection.cpp:2515`), backed by two fields
  (`statusStart`/`statusExpectedEnd`) populated whenever the server sends
  a `PROTO_PROGRESS` wire message - which happens automatically on every
  relevant state change (exposure start, then readout start), no
  subscription needed. `Block` exposes this as an overridable hook,
  `virtual int progress (Connection *conn, double start, double end)`
  (`kernel/include/block.h:597`) - overrode it in `ViewerClient`
  (`viewerclient.cpp`), mapping the raw `Connection*` back to a camera
  name (new `cameraNames` map alongside the existing `cameras` one) and
  emitting `progressUpdated(cameraName, start, end)`. Deliberately did
  **not** poll this from the worker thread's `idle()` - `start`/`end` are
  plain wall-clock timestamps, so `MainWindow` caches them per camera and
  a local `QTimer` (200ms, GUI thread only) interpolates
  percent/elapsed/remaining against `QDateTime::currentMSecsSinceEpoch()`
  - no cross-thread polling needed for a smooth-looking bar.
- **Run/Stop + repeat count**: a new `repeatSpin` (0 = "run until
  stopped", matching the user's "run-till-abort" framing;
  `QSpinBox::setSpecialValueText`) alongside the existing single-shot
  "Expose" button and a new "Run"/"Stop" toggle. Run fires exposures back
  to back via the same `requestExposure()` path, chained off
  `onExposureStateChanged(false)` (i.e. each time the active camera goes
  idle again) until `runIndex` reaches `runTotal` (or forever if 0).
  Stop sends `"stopexpo"` - a plain-text device command
  (`base/camd/src/camd.cpp`'s handler, takes no arguments), same
  raw-`Command` idiom `rts2-sendcmd` already uses elsewhere in this tree;
  there is no dedicated `CommandStopExposure` C++ class in this port.
  Threaded through with the same atomic-flag-drained-on-idle() mechanism
  as exposure/value-change requests (`ViewerClient::requestStop()`).

**Verification this round**: confirmed clean build, confirmed the app
still connects and runs stably (including with the new 200ms
`QTimer` ticking) via `journalctl -t rts2` and process inspection over
several seconds with no crash. Screen-capture tooling
(`import`/`xwd`) remained broken on this host throughout this round (see
round 2's note - not resolved, still assumed to be an XWayland quirk
unrelated to this project) so, as in round 2, the actual look and feel of
the progress bar / Run-Stop button / repeat box has **not** been visually
confirmed by the agent - needs a live look from the user.

## Round 4: dark/light (SHUTTER), left-corner live status panel

User confirmed round 3 (progress bar, Run/Stop, repeat count) works
live, then asked for two more things: a dark/light control (there's a
`SHUTTER` variable on the camera) so darks can actually be taken, and a
fiber_pointing_client.py-style compact status corner for the active
camera (it shows state/chip temp/voltage/power utilization/filter).

- **SHUTTER (dark/light)**: turned out to be exactly the same shape as
  filter/binning - a plain `rts2core::ValueSelection` (`base/camd/include/
  camd.h`'s `expType`, options `"LIGHT"`/`"DARK"`, added via
  `createValue(expType, "SHUTTER", ...)`). Added a `shutterCombo` using
  the *exact* existing generic mechanism (`applyValueToWidgets`/
  `onValueUpdated`/`requestValueChange`) - no new plumbing needed, just
  one more `else if (valueName == "SHUTTER")` branch and a combo box.
  This is what actually lets a dark frame be taken (the exposure request
  path itself was already generic).
- **Left-corner "Status" panel** (`Qt::LeftDockWidgetArea`, opposite the
  existing right-hand Controls dock): read-only summary for the active
  camera only, mirroring `fiber_pointing_client.py`'s own
  `refresh_state()` corner -
  - **State**: `rts2core::Connection::getStateString()` - the exact same
    text `rts2-mon` shows per device
    (`monitor/src/ndevicewindow.cpp:55`), surfaced via a new
    `ViewerCamera::stateChanged()` override (`stateTextChanged` signal).
    Colour-coded red/green via `Connection::getErrorState()`
    (`DEVICE_ERROR_MASK`), not a guessed threshold.
  - **Chip temp vs set-point**: reuses the already-tracked `CCD_TEMP`/
    `CCD_SET` values, shown together as `-10.1 °C / -10.0 °C`; green if
    within 0.5°C, red otherwise - the same threshold
    `fiber_pointing_client.py` used for its own chip-temp box.
  - **Voltage / cooling power**: `VOLTAGE`/`TEMPPWR`, gxccd-driver-specific
    Values (`base/camd/gxccd/gxccd.cpp`) - not present on the dummy
    camera used for testing here, so these stay "n/a" in this session;
    same green/red bands `fiber_pointing_client.py` used (11.5-15.5 V,
    <90% cooling power) carried over verbatim, **not verified against
    real gxccd hardware** since none is available in this environment -
    worth a sanity check whenever this runs against a real gxccd camera.
  - **Filter**: reads the already-tracked `filter` selection/choice pair,
    shown as plain text (separate read-only label from the interactive
    `filterCombo` already in Controls - matches
    `fiber_pointing_client.py`'s own split between its status corner and
    its expose panel).

**Verification this round**: clean build; confirmed the process starts,
connects (`cannot find section 'C0'` - the same expected, harmless
per-device-config-section notice as every round), and stays alive and
stable for 18+ seconds via direct `ps`/PID inspection (deliberately
avoided `wait` this time - blocks forever on a GUI process that doesn't
exit on its own, which is what made one earlier attempt in round 3 look
like a hang). Screen-capture tooling is still broken in this environment
(see rounds 2-3) - the actual on-screen look of the shutter combo and
status corner again needs a live check from the user.

## Round 5: fixed top/bottom layout, clean device-state text

User tried round 4 live: worked, but flagged two things - the window's
dock-widget arrangement should be a firmer top/bottom split (top: image +
controls, with room reserved on the left for a future focus-helper/
guiding panel; bottom: status + log), and the Status corner's "State"
field was showing RTS2's raw, variable-length status dump
(`" | IMAGE_READY | SHUTTER_CLEARED"` at rest, `"0 EXPOSING |
SHUTTER_CLEARED | BLOCK TELESCOPE MOVEMENT"` while exposing) rather than
a plain word - and asked for it to be fixed-length so the window doesn't
keep reflowing.

- **Layout**: replaced all three `QDockWidget`s (`Controls`/`Status`/
  `Log`) with a `QSplitter(Qt::Vertical)` central widget containing two
  `QSplitter(Qt::Horizontal)` rows - top: `[Focus & Guiding placeholder |
  ImageCanvas | Controls]`, bottom: `[Status | Log]`. Docks were
  floatable/rearrangeable by the user, which cuts against the actual ask
  (a stable, predictable shape); splitters give resizable-but-structurally-
  fixed regions instead, plus an actual reserved slot (currently just an
  empty `QGroupBox` labelled "(planned)") for the focusing/guiding panel
  mentioned as likely future work.
- **Device state text**: traced the raw string to
  `Connection::getStateString()`
  (`kernel/src/connection.cpp:226`, the CCD branch around line 302) -
  it "|"-joins every currently-set status/shutter/bop bit, by design (it's
  built for `rts2-mon`'s terminal display, where variable length doesn't
  matter). Replaced `ViewerCamera::stateChanged()`'s use of that string
  entirely: it now reads the raw bits directly
  (`Connection::getRealState()`, `getErrorState()`, and the `CAM_*`/
  `DEVICE_ERROR_MASK` constants from `status.h`) and classifies them into
  exactly one fixed phase word - `Idle`/`Exposing`/`Reading`/`Shifting`/
  `Frame transfer`/`HW error`, checked in that priority order since more
  than one bit can be set at once (e.g. reading while shifting). The
  Status corner's state label's minimum width is computed once at
  startup from the widest of those six words (`QFontMetrics`), so the
  layout can't reflow no matter which one is showing. Also removed the
  now-redundant plain "State:" row from the Expose panel (it only ever
  said "exposing"/"ready" - the Status corner's classifier already covers
  that, no need for two state indicators).

**Verification this round**: clean build (one trivial `-Wrange-loop-
construct` warning fixed in passing), confirmed the app starts, connects,
and stays alive for several seconds via direct `ps`/journalctl inspection
- same tooling limitation as rounds 2-4 (screen capture still broken in
this environment), so the actual on-screen shape of the new top/bottom
split and the fixed-width state label need a live look from the user
again.

## Round 6: save-to-disk toggle button

User asked for a button near Expose/Run to switch FITS saving on/off,
green when saving, red when not.

- Turned out to need no wire protocol at all: `DevClientCameraImage`
  (the base class `ViewerCamera` is already built on) has a public
  `setSaveImage(int)` and a `saveImage` member that gates the actual
  `Image::saveImage()` disk write in `processCameraImage()`
  (`kernel/src/devcliimg.cpp:546`) - purely local, client-side state,
  not a device Value. `cameraImageReady()` (what actually feeds the
  on-screen display) runs regardless of this flag, so toggling it never
  affects live viewing, only whether a FITS file gets written.
- **Default is off**, forced via `cam->setSaveImage (0)` right after
  each `ViewerCamera` is constructed (`ViewerClient::createOtherType()`)
  - the base class itself defaults to *on* (`saveImage = 1` in its own
    constructor), so without this override, this viewer would have been
    silently writing a FITS file for every frame since round 1. Explicit
    opt-in felt right for a tool meant for casual framing/focusing as
    much as real acquisition.
- Threaded the same way as the exposure/value-change requests
  (`ViewerClient::requestSaveToggle`, an atomic flag drained on the next
  `idle()` tick) even though no `queCommand` is involved, so the actual
  `setSaveImage()` call - a plain, non-atomic member write - only ever
  happens on the worker thread that also reads it, keeping the same
  no-lock safety invariant as everything else in `ViewerCamera`.
- Per-camera, like everything else in the Controls panel: `CameraState`
  gained a `saveEnabled` field, restored (with signals blocked) whenever
  the camera selector switches, so each camera remembers its own saving
  state.
- Button: `QPushButton`, checkable, text "Saving: ON"/"Saving: OFF",
  green (`#66CC66`) / red (`#CC6666`) background - matches the user's
  spec exactly.

**Verification this round**: clean build, confirmed the app starts,
connects, and stays alive (~19s via direct `ps` inspection - the
`run_in_background` tool's own "failed"/exit-code-1 status reports
turned out to be unrelated to the app itself across several rounds now,
confirmed again here: `ps`/`pgrep` show it running fine even when that
status says otherwise). **Could not verify the actual disk write
end-to-end**: `/var/lib/rts2/images/` is owned by `rts2:rts2`, mode
`755` - the `mates` user this session runs as has no write access there,
so toggling saving on and firing an exposure as this user would hit a
permission error, not demonstrate success. This isn't a bug in the
button/toggle logic itself (already true of every RTS2 client on this
host - the executor and rts2-db's postinst both deliberately run image-
writing code as the `rts2` user for exactly this reason), but it does
mean the happy path - green button, click Expose, a new file actually
appearing under `/var/lib/rts2/images/...` - needs the user's own
live check, ideally run as a user with write access to that path (or
pointed at a different, writable location) to see the save actually
land.

## Round 7: windowing (chip subframe) + focus centroid/FWHM fit

Biggest feature pass yet. User's ask: a blue "windowing" cursor (off by
default, region-selector, no crosshair) independent of the existing red
"measure" cursor, to pick a chip subframe before an exposure (focusing on
a large chip needs a small readout, not the whole thing); and, under the
red cursor, an automatic centroid/FWHM fit with a zoomed preview - same
idea as `fiber_pointing_client.py`'s star-profile view, but with a
deliberately simple fit algorithm instead of a full 2D Gaussian
(background-plane-subtracted barycenter + second moments, exactly as the
user specced it) rather than reaching for a Python/scipy dependency.

- **Windowing**: `WINDOW` (`base/camd/include/camd.h`'s `chipUsedReadout`)
  turned out to be a `rts2core::ValueRectangle` - x/y/width/height - with
  `CommandChangeValue`'s existing rectangle overload
  (`Block*, string, char op, int x, int y, int w, int h`) already built
  for exactly this. `SightItem` gained a `showCrosshair` flag (false for
  the new blue box - it's a region selector, not a point marker) and
  `width()`/`height()` accessors; `ImageCanvas` now owns two of them
  (`measureRect()`/`windowRect()` in the same display-pixel coordinates
  as the shown image). The "Windowing" checkbox in Controls just toggles
  the blue box's visibility - the actual `WINDOW` change is sent by
  `MainWindow::sendWindowForNextExposure()`, called right before every
  `requestExposure()` (both the single "Expose" button and each step of
  a Run sequence): the blue box's rect if windowing is checked, or the
  full chip rect (from the cached `"SIZE"` ValueRectangle - see below) if
  unchecked - so turning windowing off actually restores full-frame reads
  on the next exposure, rather than leaving the last window in place.
  `ViewerClient` gained a dedicated `requestWindowChange()`/`idle()` path
  (a rectangle doesn't fit the existing `int/double/bool` variant used
  for plain value changes) queued ahead of the exposure block in the same
  function, so the two commands land on the connection in the right
  order.
- **New ValueRectangle plumbing**: `ViewerCamera::valueChanged()` now
  checks for `rts2core::ValueRectangle` first (a new `rectangleUpdated`
  signal) before falling through to the existing `ValueSelection`/plain
  branches - covers both `WINDOW` and `SIZE` (the full chip geometry,
  used to build the "windowing off" full-frame rect and never sent
  itself).
- **Focus fit**: `ViewerCamera::runFit()`, called from
  `cameraImageReady()` on every image, right on the worker thread against
  the *raw* pixel data (`Image::getChannelData()`/`getDataType()`) rather
  than the 8-bit display-stretched copy - avoids the display quantile
  stretch corrupting the photometry. Algorithm, per the user's own
  suggestion rather than a full nonlinear Gaussian fit: fit a background
  *plane* `a + b*x + c*y` (3x3 normal equations, solved directly via
  Cramer's rule - only from the outer 2px border of the measure box, not
  the whole thing), subtract it from every pixel, clamp negative
  residuals to 0, then take the intensity-weighted barycenter and second
  moments (dispersion) of what's left - FWHM via the standard
  `2*sqrt(2*ln2)` sigma-to-FWHM factor, same constant
  `fiber_pointing_client.py` got from `astropy.stats.
  gaussian_sigma_to_fwhm`. Where the red cursor *is* has no live-drag
  signal to react to (`QGraphicsItem` isn't a `QObject`, so `SightItem`
  can't emit one) - `MainWindow::onProgressTick()` (already running every
  200ms for the progress bar) also pushes the cursor's current rect to
  `ViewerCamera::setMeasureRegion()` every tick, a plain atomics-only
  method safe to call directly from the GUI thread (unlike everything
  routed through `ViewerClient`'s queues, nothing here touches RTS2/wire-
  protocol state). Coordinate care: `setMeasureRegion()`/the fit result
  are in the same top-down display-pixel space as the red cursor and the
  shown `QImage`, but the raw pixel buffer is bottom-up (FITS convention,
  same reason `getChannelGrayscaleImage` flips for display) - each row
  read is translated (`rawY = height-1-dispY`) while every accumulated
  position stays in display coordinates throughout, so results land where
  the cursor visually is.
- **Focus panel** (the "room reserved" placeholder from round 5, now
  populated): measure-box size spinbox (default 32, per the user's
  recollection of this project's earlier size), FWHM X/Y and peak labels,
  and a 200x200 zoomed crop of the measure box (`QImage::copy()` +
  `scaled()`) with the fitted centroid drawn as a small red crosshair - a
  C++/Qt rebuild of `fiber_pointing_client.py`'s
  `StarMatplotlibCanvas.plot_star()`.

**Verification this round**: clean build (zero new warnings, confirmed
with a full rebuild of just the viewer sources), confirmed the app starts
and stays connected/stable for ~60s via direct `ps` inspection. **Could
not exercise the actual fit/windowing flow**: it needs a live image, which
needs either clicking "Expose" (screen-capture/interaction tooling still
broken this session, same as rounds 2-6) or passively catching one of the
executor's own background exposures (none happened to fire during this
session's test window, per `journalctl`). Traced through the arithmetic
by hand instead (index bounds, the bottom-up/top-down row flip, the
Cramer's-rule solve, the signal/slot parameter ordering into
`onFitResult`) and it type-checked and built cleanly, but the actual
on-screen behaviour - does the blue box show/hide correctly, does a
windowed exposure actually come back smaller, does the zoomed crop/
centroid marker look right - needs the user's own live test, same as
every round so far.

**Round 7 fix**: user tried it live - both windowing and the focus fit
worked individually, but not together: with windowing on, the red cursor
couldn't be dragged, and the windowed image looked zoomed in. Root cause:
`ImageCanvas::setImage()` called `fitInView(m_pixmapItem,
Qt::KeepAspectRatio)` every time the image's size changed - which a
windowed image always does (smaller than the last full-frame one),
rescaling the view's transform to stretch it to fill the same viewport
each time, i.e. an actual zoom. Removed the `fitInView()` call entirely -
the canvas now always shows images at native 1:1 scale, full-frame or
windowed alike, which is what made the red cursor hard to grab where the
user expected it. User was already ahead of the diagnosis ("it is not
strictly necessary... zooming is an indexing nightmare") and explicitly
asked to defer real zoom in/out support (useful for large chips vs small
windowed sections, per their own reasoning) rather than get the
view/scene/item coordinate math involved in a rescaled transform right
under time pressure - noted as a real future candidate, not implemented.

The zoom fix turned out not to be the whole story: the real remaining bug
was z-order. `m_crosshair` (red) and `m_windowItem` (blue) are both added
to the same `QGraphicsScene` with no explicit `zValue()`, so they default
to the same stacking level - ties go to whichever was added to the scene
later, which is the (usually much larger) blue window box. Once a
windowed exposure came back small enough that the blue box covered the
whole visible image, it sat on top of the red cursor everywhere and
absorbed every mouse press meant for it - not a coordinate/zoom problem
at all, a hit-testing one. Fixed with `m_crosshair->setZValue (1)` /
`m_windowItem->setZValue (0)` in `ImageCanvas`'s constructor, so the red
measure cursor is always on top and draggable regardless of how large the
window box is or how much of the image it covers.

**Round 7 second follow-up**: both windowing and the focus fit confirmed
working together after the z-order fix, but the user flagged two more
real gaps from live use:

1. **Refit on cursor move, no new exposure**: dragging the red cursor
   onto a different star already in the frame didn't do anything -
   `runFit()` only ever ran from `cameraImageReady()`, i.e. only when a
   new image actually arrived. Fixed by caching the raw pixel buffer
   (`ViewerCamera::lastImageData`, a byte-for-byte copy via
   `Image::getChannelData()`/`getPixelByteSize()`) every time an image
   comes in, and factoring the fit itself out of `cameraImageReady()`
   into `runFitOnData(data, dataType, width, height)` so it no longer
   needs a live `Image*`. A new `refit()` method re-runs it against
   that cache; `ViewerClient` gained a matching `requestRefit()` on the
   same idle()-drained-queue idiom as `requestSaveToggle()` (no wire
   command either, but still only safe to actually call on
   `ViewerCamera`'s own worker thread - see below). `MainWindow`'s
   existing 200ms cursor-position poll (`onProgressTick()`) now compares
   against the last-pushed rect and only calls `setMeasureRegion()` +
   `requestRefit()` when it actually changed, instead of unconditionally
   every tick.
2. **Windowing ignores binning entirely**: user's own diagnosis, dead
   right - `WINDOW` (`base/camd/include/camd.h`'s `chipUsedReadout`) is
   always in absolute *unbinned* chip pixels, but the blue cursor is
   drawn in *display* pixels of the (possibly binned) shown image, and
   the code was sending the blue box's raw display coordinates straight
   through with no conversion at all. Two distinct sub-bugs, both
   present: (a) the box's on-screen *size* never shrank as binning went
   up, so a "256 px" box meant 256 unbinned pixels at 1x1 but the same
   256 *display* pixels (1024 unbinned) at 4x4 - not what the spinbox
   value was supposed to mean; (b) the box's *position* was sent as raw
   display coordinates with no offset/scale correction at all, wrong the
   moment either binning != 1x1 or a non-full-chip WINDOW was already
   active. Fixed by tracking `BINX`/`BINY` (plain numeric Values, already
   covered for free by the existing generic `valueUpdated` mechanism -
   no new plumbing needed there) and the device's own last-reported
   `WINDOW` origin (`CameraState::lastWindow`, `onRectangleUpdated()`)
   per camera:
   - `MainWindow::updateWindowBoxSize()` divides `windowSizeSpin`'s value
     by `BINX`/`BINY` before calling `ImageCanvas::setWindowSize()` (now
     takes width/height separately, since the two axes can legitimately
     differ) - so the spinbox keeps meaning "this many real, unbinned chip
     pixels" regardless of current binning, and the on-screen box visibly
     shrinks as binning increases, exactly as the user described.
     Recomputed on binning changes, camera switches, and the spinbox
     itself.
   - `sendWindowForNextExposure()` now converts the blue box's on-screen
     rect to absolute chip coordinates as
     `chipX = lastWindowOriginX + dispX * BINX` (and the same for Y/
     width/height) before sending - not just `dispX * BINX` alone, since
     the currently-displayed image's pixel (0,0) is wherever the last
     applied `WINDOW`'s origin was, not necessarily the chip origin.
     Handles the user's own worked example (survey at 4x4 binning, pick a
     region, then switch to 1x1 for a detailed look at the same real
     spot) as a special case of the general conversion, not a hack for
     that one workflow.

**Verification**: clean build (confirmed via a full rebuild of the viewer
sources, zero new warnings), confirmed the process starts, connects, and
stays stable (~20s via direct `ps` inspection). As with the fit/windowing
work itself, exercising the actual live-refit-while-dragging and
binning-aware box-resizing behaviour needs the user's own hands - no
screen-capture tooling available this session to click/drag anything
directly.

**Round 7 third follow-up**: user tried it live, confirmed it's stable,
but caught one more real bug in the binning fix above: the blue box's
on-screen size was tracking the *live* `BINX`/`BINY` value - i.e.
whatever binning is currently configured for the *next* exposure - not
the binning the *currently displayed* image (the one the box is actually
drawn over) was taken with. Those two differ for exactly as long as it
takes between the user changing the binning combo and the next exposure
actually landing - during that window the box would resize to represent
a binning that image on screen never used.

Fixed by snapshotting `BINX`/`BINY`/`WINDOW` at the moment each image
actually arrives (`CameraState::imageBinX/imageBinY/imageWindow`, set in
`onImageReady()`) instead of reading the live values on demand.
`updateWindowBoxSize()` (the box's on-screen size) and
`sendWindowForNextExposure()`'s chip-coordinate conversion (the box's
on-screen *position*) both switched to these snapshotted fields - the
live `BINX`/`BINY`/`WINDOW` values in the generic `values` map / `lastWindow`
are still tracked (still needed as *display* values in the Status/Cooling
panels and to know what's coming next), just no longer used to interpret
where the box currently is or how big it currently looks, since that box
is drawn over a specific already-received image and has to be interpreted
against what *that* image actually used. The live-BINX/BINY-triggered
resize in `onValueUpdated()` was removed entirely - box geometry now only
ever updates from `onImageReady()` (new image landed) or the size
spinbox/camera switch (both already correct, since they read the same
snapshotted fields).

This was a bug in the sending logic too, not just the on-screen sizing -
the user only pointed out the visible symptom (box size), but the same
live-vs-actual mismatch would have sent the wrong chip coordinates if an
exposure was fired after changing binning without an intervening image,
so both were fixed together for consistency rather than patching only
the visible half.

Confirmed unable to test positioning accuracy from either side this
round: the dummy camera driver generates a fresh random star field on
every exposure, so there's no fixed on-sky truth to verify a computed
chip position against - noted by the user themselves, not a gap in this
testing pass specifically.

## Round 8: "logfit" display stretch

User asked for the display stretch to match what's used in the *current*
classic `rts2-xfocusc` and in `pyrt-f2cj`/fiber_pointing_client.py (which
calls it `_logfit_to_screen`) - not the fixed 0.5%-quantile clip
(`Image::getChannelGrayscaleImage()`) this project had been using since
round 1.

Found it: `~/rts2/src/focusc/xfitsimage.cpp`'s `drawImage()` (the classic,
*already-modernized* autotools tree, not this project's own earlier port)
has exactly this algorithm already, its own comment explicitly says
"similar to f2cj.py". Ported it faithfully rather than reimplementing
from the Python version:

- Sort the *entire* image's pixel values, read off the 10th/50th/90th/
  99.95th percentiles.
- Fix `C` just below the noise floor: `qLow - (qMidHigh - qLow) / 1000`.
- Fit `log10(y) = A + B*log10(x - C)` through those four points, each
  mapped to a target grey level (1, 255/8, 255/4, 255) - a closed-form
  ordinary-least-squares line fit (`xfitsimage.cpp`'s own formula, not a
  general nonlinear solver; mathematically the same line `curve_fit`
  converges to for this 2-parameter linear-in-log-space model, so numerically
  equivalent to the Python versions here despite the different fitting
  mechanics).
- Apply `10^(A + B*log10(max(pixel - C, 1e-10)))` to every pixel, clamped
  to [0, 255].

Implemented as a new `logFitGrayscale()` free function in
`viewercamera.cpp`, replacing the `getChannelGrayscaleImage()` call in
`cameraImageReady()` entirely - reuses the same generic per-dataType pixel
reader (`readPixelAt()`/`readPixel()`) the focus fit already had, so it
isn't limited to the two types (`BYTE`/`USHORT`) the classic C++ version
special-cased, and needed no new `#include`s. Same top-down display-row
convention as everywhere else in this file (`rawY = height-1-y`).
Constructing the output `QImage` directly (versus the old raw-`buf`-plus-
manual-`QImage`-wrapper-plus-`.copy()` dance `getChannelGrayscaleImage`
needed) also let the copy-before-signal step go away - the new `QImage`
already owns its own buffer.

Not ported: the classic version's diagnostic `std::cout` prints
(median/sigma via `classical_median()`, the old quantile cuts) - traced
through the code and confirmed they're display-only, never feeding into
the actual transform, so there was nothing to preserve there.

**Performance note, not addressed**: this sorts every pixel of the full
image on every single frame, same as the reference implementation - fine
for the dummy camera's small test frames, but worth revisiting (e.g.
percentile estimation from a random subsample) if this ever runs against
a real large-format chip and framerate matters.

**Verification**: clean build (confirmed via a full rebuild of the viewer
sources, zero new warnings), confirmed the process starts, connects, and
stays stable (~20s via direct `ps` inspection). Numerically checked the
OLS formula and the C++/Python quantile-index correspondence by hand
(`q_mid_high`/`quantiles[2]` line up) but - as with every visual change
this session - the actual look of the resulting stretch on a real image
needs the user's own live look, no screen-capture tooling available here.

## Round 9: focus-progress graph

User's likely-final request for this app: an X-Y graph under the focus
fit/zoomed-view panel plotting FWHM history - X/Y in independent red/blue
lines, X-axis either image index or focuser position, with an enable
checkbox, a reset button, and the axis choice. No focuser *control* -
purely a read-only progress plot.

- **No Qt5Charts** (its dev headers aren't installed on this host, `pkg-
  config --exists Qt5Charts` fails) - a two-series line plot is simple
  enough to draw directly. New `FocusGraphWidget` (`focusgraphwidget.h/
  .cpp`), a plain `QWidget` subclass with a hand-rolled `paintEvent()`:
  auto-scaled axes, red/blue polylines + point markers for FWHM X/Y. Same
  "don't reach for a dependency when a simple custom widget suffices"
  call already made for the zoomed star crop.
- **Camera-linked focuser values** ("focpos"/"foc_moving"/"focuser" from
  the user's own message) turned out to need zero new plumbing:
  `base/camd/src/camd.cpp` declares `camFocVal` ("focpos") as a plain
  `ValueInteger` and `focuserMoving` ("foc_moving") as a plain
  `ValueBool` - both already flow through the existing generic
  `valueUpdated`/`CameraState::values` mechanism from round 2 with no
  changes needed. "Focuser position" only becomes a selectable X-axis
  option once `"focpos"` has actually been seen from the active camera
  (`updateFocusGraphAxisAvailability()`, disables/enables the combo
  item via its `QStandardItemModel` directly) - per the user's own spec,
  falls back to "Image index" if it becomes unavailable (e.g. switching
  to a camera with no linked focuser) while selected.
- **New image vs. manual refit**: the graph must only grow from fits on
  genuinely new images, never from a refit-on-drag (round 7's
  cursor-move fix) - those would flood it with points that don't
  represent real focusing progress. `onImageReady()` sets a new
  per-camera `pendingNewImageFit` flag; `onFitResult()` checks and
  clears it, only appending to `CameraState::focusHistory` when it was
  set. Relies on Qt's guaranteed in-order queued delivery to the same
  receiver: for a real new image, `imageReady` and `fitResult` always
  fire in that order from the same `cameraImageReady()` call on the
  worker thread, with no interleaving from other images (one camera
  processes one image at a time) - a manual `refit()` call only ever
  emits `fitResult` on its own, so the flag reliably distinguishes them.
- **Per-camera, opt-in**: `focusHistory`/`focusGraphEnabled` live in
  `CameraState` like everything else here - switching the active camera
  restores that camera's own tracking checkbox state, axis-combo
  availability, and plotted history. Off by default, same reasoning as
  image saving (round 6): a casual framing session shouldn't silently
  start accumulating a history.
- **Known, deliberately unfixed**: user flagged that `focusd-dummy`
  (this test rig's dummy focuser) always reports position -1, suspected
  to be a real bug in that driver - explicitly out of scope for this
  round. The graph itself doesn't care what `focpos` actually contains;
  a real focuser would plot correctly against it.

**Verification**: clean build (full rebuild of the viewer sources, zero
new warnings), confirmed the process starts, connects, and stays stable
(~20s via direct `ps` inspection). Same recurring caveat as every round
this session: no screen-capture tooling available here to confirm the
graph's actual on-screen appearance, axis-availability toggling, or the
new-image-vs-refit distinction live - needs the user's own test.

## Round 10: "Save to disk" OFF never actually stopped saving - fixed, default flipped to ON

User reported the Round 6 toggle button didn't work: images kept getting
saved to disk regardless of the button's state.

Root cause is in shared kernel code, not this project's own
`viewercamera.cpp`/`mainwindow.cpp` - `DevClientCameraImage::
processCameraImage()`'s `if (saveImage) { ci->image->saveImage(); }`
guard (`base/kernel/src/devcliimg.cpp`) never prevented anything. By the
time that runs, the FITS file has already been created on disk with real
pixel data in it (`createImage()` calls `fits_create_file()` immediately,
`writeData()` streams pixels in as they arrive, both independent of this
flag) - and `Image::~Image()` unconditionally calls `saveImage()` again on
destruction regardless of the flag. So `saveImage=0` only ever skipped one
redundant explicit close, never the actual persistence. Confirmed
identical in classic `rts2fits/devcliimg.cpp` too - a genuine long-standing
upstream bug, not a porting mistake; full writeup in `base/UPSTREAM_BUGS.md`.

- **Fix** (`base/kernel/src/devcliimg.cpp`): added the missing `else`
  branch, `ci->image->deleteImage()` - closes and unlinks the file,
  mirroring what `~CameraImage()` already does for images that never
  received any data at all.
- **Default flipped from off to on**, per explicit user request this
  round ("ON is good and it should be the default, but if they are not to
  be saved, it should work"): `ViewerClient::createOtherType()`'s
  `setSaveImage(1)` (was `0`), `CameraState::saveEnabled` now defaults to
  `true`, and the "Saving" button now starts checked/green. This reverses
  Round 6's original off-by-default policy choice - reasonable at the
  time, but it happened to closely resemble the buggy always-saves
  behavior closely enough that the underlying bug went unnoticed until
  the user actually tried turning it off.

**Verified without needing the GUI or the permission-restricted
`/var/lib/rts2/images/` path that blocked Round 6's own end-to-end check**:
a standalone test program, linked directly against the real
`libbase_kernel.a` (not a mock), using `Image`'s actual constructor and
destructor - confirmed a file created and then merely left to go out of
scope with no explicit `saveImage()`/`deleteImage()` call (the exact old
code path when `saveImage` was 0) is still on disk afterward, and that
adding the explicit `deleteImage()` call (the fix) removes it and keeps it
removed. Full `gui`/`base`/`db` rebuild clean. Still needs the user's own
live check that flipping the button to OFF in a running `rts2-viewer`
now actually stops new files from appearing.

## Round 11: configurable archive path for FLORES production deployment

Prototype behaviour saved images into the process's own working
directory - fine so far, but the FLORES PC deployment needs saved images
to land in the shared `/images`-style archive (site array, so this is
"free" backup/archiving), using the same base-path + expand-path
machinery classic RTS2 already has for this - just without any target
involved, since this viewer has no concept of one.

- **`Configuration::observatoryBasePath()`** (`kernel/include/
  configuration.h`, reads `[observatory] base_path` from rts2.ini,
  defaulting to `/images/`) plus `image.h`'s existing `%Y`/`%N`/`%c`/...
  expand-path syntax (`Expander`/`Image::expandVariable()`,
  `kernel/src/expander.cpp`/`image.cpp`) already do exactly this - no new
  path-building code needed, only wiring it into `ViewerCamera`.
- **`ViewerCamera::createImage()`** (new override, `viewercamera.cpp`):
  when `saveImage` is on *and* an archive expand-path expression has been
  set (`setArchivePath()`), constructs the `Image` via the expanding
  7-argument constructor (`image.h`, expression + expNum + connection),
  same one `DevClientCameraExec::createImage()` (`script/src/execcli.cpp`)
  already uses for the target-based executor path - just with no target.
  Otherwise falls straight through to the base class's own default
  (relative-to-cwd, unchanged from before this round).
- **Deliberately *not* used for `saveImage=0` frames** - the whole point
  of Round 10's create-then-delete fix was that it's harmless in a
  scratch location, but the user was explicit this round that the shared
  archive must never see a file appear then vanish (other tools rely on
  it holding only definitive, kept images - backup/sync jobs, etc.).
  `createImage()` only reaches for the archive path when `saveImage` is
  actually true, so a saveImage=0 frame still gets its usual harmless
  create-then-delete cycle, just in the base class's cwd-relative
  default location, never inside the archive.
- **Two ways to set the archive expression** (`ViewerClient`,
  `viewerclient.cpp`), exactly as asked ("either from command line or
  even from rts2.ini"): a new `--images <expand-expression>` option
  (mirrors `scriptexec`'s own `-e`/`--images`-shaped option,
  `scriptexec.cpp`), falling back to rts2.ini's `[viewer] expand_path` if
  not given (same `[section] expand_path` convention `scriptexec.cpp`
  itself uses for its own `[scriptexec] expand_path`), falling back in
  turn to a built-in default: `%b%Y/%N/%c_%H%M%S-%s.fits` - e.g.
  `/images/2026/20260802/C0_174528-970.fits`, matching the user's own
  example layout exactly (`%Y`/`%N` are the *night*-based year/date
  variants, not calendar-day `%y`/current date, so a frame taken just
  after local midnight still files under the night it belongs to).
- Every newly-discovered camera gets the same resolved path
  (`ViewerClient::createOtherType()` calls `cam->setArchivePath
  (imageExpandPath)` right next to the existing `setSaveImage(1)`) -
  one archive location per viewer process, not per-camera, since nothing
  about this deployment needs per-camera archive roots.
- Deliberately did **not** add any explicit writable-directory check
  before first use - `FitsFile::createFile()` already calls `mkpath()`
  and logs a clear `MESSAGE_ERROR` if that (or the actual file creation)
  fails, same as every other RTS2 client hitting an unwritable path; the
  user's own note that `/images` needs to be writable by whichever user
  runs the viewer is a deployment/permissions requirement for FLORES, not
  something this code needs to pre-validate.

**Verification, round 1**: clean rebuild of the viewer sources. Confirmed
the expansion logic itself with a standalone test linked against the real
`libbase_kernel.a` (not a mock), calling `Image::expandPath()` directly
on the same `%Y/%N/%c_...` template used here - resolved `%Y` to `2026`,
`%N` to `20260802`, exactly the requested nesting. Could not go further
than that in this environment at the time (no writable archive-style
path, no display).

**Verification, round 2 - live, end to end**: FLORES reported archiving
"doesn't work" - `FitsFile::createImage cannot create directory for
/images/2026/20260802/FLI_205235-967.fits: Permission denied` (`~/images`
there is `root:root`, mode `755`). To rule the code in or out, set up a
matching `/images` (world-writable-by-`mates` this time) on this
machine, pointed `[observatory] base_path`/`[viewer] expand_path` at it
the same way, and drove the *real* compiled `rts2-viewer` binary against
the actual live `rts2-camd-v4l` (device C0) already running here -
headless (`QT_QPA_PLATFORM=offscreen` for a plain watch-only run;
`QCoreApplication` + `ClientThread` + `QTimer::singleShot` driving
`requestExposure()` directly, bypassing `MainWindow`/widgets entirely, to
also exercise the real exposure-request path) - not a reimplementation,
the actual shipped `ViewerClient`/`ViewerCamera` classes. Result:
`/images/2026/20260802/C0_205449-214.fits` and two more, created
correctly by the real binary on a real exposure, confirming the feature
works exactly as designed. **FLORES's problem is a filesystem
permission, not a bug** - `/images` (or wherever it's really pointing)
needs to be owned by, or group-writable to, whichever user runs
`rts2-viewer`.

One real bug surfaced *while building this test harness*, unrelated to
archiving itself: connecting `ClientThread::cameraCreated` to a bare
lambda with no receiver-context object makes Qt deliver it via a direct
(non-queued) connection on the *worker* thread - which never runs a real
Qt event loop (it just calls blocking `rts2core::Client::run()`), so any
`QTimer::singleShot()` posted from inside that handler silently never
fires. Passing `&app` (living on the main thread, which does run
`QCoreApplication::exec()`) as the context object, plus an explicit
`Qt::QueuedConnection`, fixed it. This is a property of *any* code
connecting to `ClientThread`'s signals with a plain lambda, not specific
to this test - worth remembering if `MainWindow` itself is ever
refactored to use lambdas here instead of named slots.

## ORM deployment bug: `rts2-viewer` never loaded `rts2.ini` - fixed

Found during real hardware testing at ORM (`cta-n`): running bare
`rts2-viewer` against the live WF0 (`rts2-camd-gxccd`) camera logged
`cannot find section 'WF0'` (and, in one run, `'C0'` too - a second,
not-currently-connected camera name from `/etc/rts2/devices`, likely a
lingering/stale reference) on every startup. Traced to a real gap:
`ViewerClient` never overrode `init()` to call
`Configuration::instance()->loadFile()` - every other RTS2 client
(`scriptexec.cpp`, classic's `focusclient.cpp`) does this itself in its
own `init()`, since `rts2core::Client::init()` does not do it generically.
Without it, `Configuration::instance()` stays a permanently-empty
singleton (it's a lazy `new Configuration()` on first access, nothing
more - see `configuration.cpp`), so `DevClientCameraImage`'s constructor
(`kernel/src/devcliimg.cpp`), which looks up each connected camera's
`instrume`/`telescop`/`origin`/`template` config keys by device name,
"cannot find section" for every camera, every time, regardless of
whether that section actually exists in `rts2.ini`.

**Fixed** in `viewerclient.h`/`.cpp`: added the standard `--config` option
(`OPT_CONFIG`, already defined in base's `option.h`) and an `init()`
override that loads it, following the exact `scriptexec.cpp`/classic
`focusclient.cpp` pattern (fail loudly if the file can't load, rather
than silently continuing empty). Rebuilt clean, `ctest` still 7/7.

Two other messages from the same ORM session, investigated and NOT
porting bugs:
- `Undefined base type of RTS2_VALUE_MMAX` (`Connection::metaInfo`,
  `kernel/src/connection.cpp`) - calls `exit(10)`, faithfully matching
  classic's own behavior at this exact spot (verified byte-for-byte
  against `/home/mates/rts2/lib/rts2/connection.cpp`). Initially
  suspected to be the same `rts2-gcnkafka`/`rtspy` bug below, but it
  reproduced even after that daemon was killed and confirmed fully
  disconnected from centrald (`ss -tnp` showed no live connection) - so
  some other connected device (WF0/gxccd is the only camera; DOME/zelio,
  WEATHER/UPS sensors, or one of imgproc/executor/selector are the other
  candidates) is declaring a value with an `RTS2_VALUE_MMAX` ext-type and
  an unrecognized base-type nibble. Reviewed every `ValueDoubleMinMax`/
  `ValueIntegerMinMax` construction site in `base` (`camd.h`'s
  `createTempSet()`, `gxccd.cpp`'s `desiredNightWindowHeating`,
  `zelio.cpp`'s `domeTimeout`, `focusd.h`'s min/max values) against
  classic byte-for-byte - all identical, no construction bug found.
  Couldn't pin down the exact offending value without a live protocol
  capture (direct raw-socket queries against the device ports didn't
  work - device ports don't answer unauthenticated `info` queries the
  way centrald's does). **Improved diagnostics instead**: both this call
  site and `Daemon::duplicateValue`'s matching branch (`daemon.cpp`) now
  log the value's name (and encoded type/flags int) alongside the error,
  so the next occurrence is actually identifiable. Next step when this
  recurs: capture the new log line's value name directly.
- `cannot find section 'C0'` - once the config-load fix above lands, this
  either resolves (if `[C0]` legitimately exists in `rts2.ini`, which it
  does) or simply stops appearing (if `C0` isn't really a live connection
  at all). Not independently chased further.

## Build note: AUTOMOC quirk on this host

This host's CMake (4.4.0, snap package) only picks up a `Q_OBJECT` header
for `moc` if it's listed **explicitly as a target source**, not just
reachable via `#include` through `target_include_directories`. Reproduced
in an isolated minimal case outside this project (a header under a
subdirectory `#include`d by a `.cpp`, with `target_include_directories`
supplying the path - silently produces "no files found that require moc",
no error, just missing vtables/staticMetaObject at link time). Fixed by
listing every `Q_OBJECT` header directly in `viewer/CMakeLists.txt`'s
`add_executable()` call alongside its `.cpp`. Worth re-checking whether
this is still needed if the build host's `cmake` ever moves off the snap
package.
