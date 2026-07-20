# db porting status

Working notes for the db effort so it can be picked up cold. Update this
file at the end of each work session; don't rely on chat history surviving.

## What db is

The database-bound half of the RTS2 backend/frontend split: `rts2db`
(target/observation/plan persistence over PostgreSQL) and, on top of it, the
`rts2-executor` observation-execution daemon. It depends on `base` (kernel +
drivers, no DB - see `../base/STATUS.md`) but is its own CMake project,
living next to it (`../db`) in the same repo. Reasoning for the split is
written up in the design note linked from `base`'s
`STATUS.md`/`rts2c_porting_initiative` memory:
https://claude.ai/code/artifact/3e367840-cfbc-405a-ba8f-3119f0eee9d9

Same working method as base: bottom of the dependency graph first, review
and lightly modernize while porting (dead legacy branches stripped, real bugs
fixed and documented, unnecessary `#include`s dropped - verified with grep
before dropping, every time), rather than a mechanical translation.

A dependency-mapping pass (before any file was ported) established the real
bottom-up order and which classic `rts2db` files are executor-only vs.
httpd/scheduler-only (deferred). See "Deliberately deferred" below.

## How to build

```
cd db
cmake -S . -B build
cmake --build build -j$(nproc)
```

Requires PostgreSQL client dev headers + `ecpg` (embedded-SQL preprocessor)
and libxml2, all found via pkg-config (`libpq`, `libecpg`, `libecpg_compat`,
`libxml-2.0` - all present as pkg-config modules on this system, unlike
classic `configure.ac` which hand-rolls `pg_config`/version detection).

db does **not** build against an installed base package - it nests the
base source tree directly via `add_subdirectory` (see `DB_BASE_DIR` in
the top-level `CMakeLists.txt`, defaults to `../base`). base has no
install/export step yet, and pinning to a source tree keeps this simple
while both projects are under active, single-tree development. This required
one small upstream fix to `base/CMakeLists.txt`: the generated
`base-config.h` include dir was only `include_directories()` (directory-
scoped), which doesn't propagate to a project nesting base from a different
directory scope - changed to also be a `PUBLIC` `target_include_directories`
on `base_kernel`, so it now propagates transitively through
`target_link_libraries` the way a real exported target would.

The `ecpg`-preprocessing step is a CMake function, `db_ecpg_sources()`
(defined in the top-level `CMakeLists.txt`), that runs `ecpg -o <out>.cpp
<in>.ec` as a custom command per file - the CMake equivalent of classic's
`.ec.cpp:` implicit automake rule.

## Conventions being used

Same as base (see that project's STATUS.md "Conventions" section for the
full list) plus:
- `.ec` files (embedded SQL) live in `src/`, alongside plain `.cpp` - ecpg
  preprocesses them at build time into `${CMAKE_CURRENT_BINARY_DIR}/src/*.cpp`.
- Headers live in `include/rts2db/*.h` (mirroring classic's
  `include/rts2db/` - the namespace is `rts2db`, distinct from base's
  driver-tier `rts2core`/`rts2image`/`rts2script`).
- Comments marking deliberate deviations from classic use `db note:`
  (base's own convention is `base note:` - kept distinct so `grep` can tell
  which project's deviations you're looking at, even though both trees will
  eventually sit in the same repo).
- Legacy ecpg version branching (`RTS2_HAVE_PGSQL_9` and similar) is dropped
  outright - the classic tree supported ecpg <9, PG18 (this system) is
  nowhere near that old, so only the modern branch survives, same reasoning
  as dropping dead legacy-platform branches in base.

## Progress

### DB bootstrap tier (sqlerror, sqlcolumn, camlist, devicedb, appdb) - DONE

First slice, proves the whole toolchain end to end (ecpg preprocessing,
PostgreSQL/libecpg/libxml2 detection, nested base dependency, `sqlca`
auto-inclusion by ecpg).

- `SqlError` (`db/include/rts2db/sqlerror.h` + `db/src/sqlerror.ec`) - thin
  `rts2core::Error` subclass wrapping `sqlca`. Dropped two unused classic
  includes (`app.h`/`event.h` - grep-verified neither symbol is used).
- `SqlColumn`/`SqlColumnObsState` (`sqlcolumn.h`/`.ec`) - column metadata for
  the CLI table-display helper (`SqlQuery`, see appdb below). **Found the
  real bottom of the graph is one level lower than the classic `#include`
  graph suggests**: classic `sqlcolumn.ec` pulls in the entire
  `rts2db/target.h` cluster just for two bitmask macros
  (`OBS_BIT_INTERUPED`/`OBS_BIT_ACQUSITION_FAI`) - those are actually defined
  in the kernel-level `rts2target.h` (already in base, task 24), so this
  port includes that instead and stays a true leaf, unblocking the DB
  bootstrap tier without needing any of the target/observation cluster yet.
- `CamList` (`camlist.h`/`.ec`) - simple `std::list<std::string>` of camera
  names loaded from the `cameras` table.
- `DeviceDb` (`devicedb.h`/`.ec`) - adds DB connectivity to
  `rts2core::Device` (connect-string/config-file options, `initDB()`,
  `connectToDb()`/`checkDbConnection()` free functions with thread-local
  reconnect parameters). Faithful port, no bugs found.
- `AppDb`/`SqlQuery` (`appdb.h`/`.ec`) - DB connectivity for CLI tools
  (`rts2core::CliApp` subclass) plus a generic SQL-query-and-display-as-table
  helper used by several classic `db/*` CLI tools (not ported yet - only the
  library piece). Dropped the classic `RTS2_HAVE_PGSQL_9` version branch in
  `display()` (unquoted `DESCRIPTOR disp_desc` predates the SQL-standard
  syntax; PG18's ecpg always wants it quoted) - same "drop dead legacy
  branch" reasoning used throughout base.
  - **New base addition needed**: `rts2core::CliApp` (App subclass adding
    `run()`/`doProcessing()`/`afterProcessing()`) didn't exist in base yet -
    it's DB-agnostic, so it was added to `base/kernel/include+src/cliapp.h/.cpp`
    (faithful port, trivial, `App::run()` was already pure virtual so this
    slots in cleanly) rather than db, since other future CLI tools outside
    db could use it too.

All five files build clean against PostgreSQL 18 / ecpg 18 (only pre-existing
kernel warnings from `value.h`/`valuearray.h`'s `setValueInteger` hiding,
already present before db existed - not new).

### Target cluster - DONE

The big one. `target.h`'s own header dependency closure turned out to need
one more thing than the earlier dependency-mapping pass expected: `Target`
has a **value member** `Labels labels;` (not a pointer), so `labels.h`/`.ec`
had to be ported too, not deferred as originally planned - `labellist.h`
(the separate "list every label in the system" CLI helper) is still
deferred, that one's genuinely unused by `Target`.

Ported, bottom-up:
- `timelog.h` (`TimeLog` abstract base, trivial, header-only).
- `labels.h`/`.ec` (`Labels`/`LabelsVector`/`Label`) - see above, needed by
  `Target` directly, not deferrable.
- `targetset.h` (header only so far - `.ec` implementation came later,
  see below) - forward-declares `Target`/`TargetGRB`/`Constraints`, no
  circular include with `target.h` despite both needing each other's types.
  Dropped classic's `#include "nan.h"` (pure legacy WIN32/sun/pre-C99 NAN
  shim; modern `<math.h>` already provides `NAN`).
- `target.h` (`Target` + 14 concrete subclass *declarations* -
  `ConstTarget`, `DarkTarget`, `FlatTarget`, `PosCalibration`,
  `CalibrationTarget`, `ModelTarget`, `OportunityTarget`, `LunarTarget`,
  `TargetSwiftFOV`, `TargetIntegralFOV`, `TargetGps`, `TargetSkySurvey`,
  `TargetTerestial`, `TargetPlan` - forward-declares `Plan`, no `plan.h`
  dependency in the header).
- `xmlerror.h` (plain `XmlError`/`XmlMissingAttribute`/etc., libxml2-based) -
  kept in db (not base) since it's the one thing pulling libxml2 in and
  only `constraints` needs it.
- `constraints.h`/`.cpp` (`Constraint`/`ConstraintInterval` + 12 concrete
  constraint types, `Constraints`, `MasterConstraints`) - pure C++, no ecpg.
  Needs `ConnNotify` (closed out task #5 in base for this). Dropped a
  legacy `ln_get_alt_from_airmass` fallback definition and an
  `RTS2_HAVE_STRTOF` conditional (both unconditionally available on any
  current libnova/glibc).
- `targetell.h`/`.cpp` (`EllTarget` - comets/asteroids on elliptical orbits,
  self-contained via `libnova_cpp`'s `LibnovaEllFromMPC` helpers).
- `targetgrb.h`/`.ec` (`TargetGRB` - GRB follow-up, faithful port).
- `rts2targetplanet.h`/`.cpp` (`TargetPlanet` - Sun/planets/Moon, a static
  table of libnova function pointers per body).
- `imagesetstat.h`/`.cpp`, `imageset.h`/`.ec` (`ImageSetStat`, `ImageSet` +
  4 concrete subclasses) - needed a new DB-aware image layer, see below.
- `rts2fits/dbfilters.h`/`.ec` (`DBFilters` - filter-name/id cache+creator)
  and `rts2fits/imagedb.h`/`.ec` (`ImageDb`/`ImageSkyDb`) - **new gap found**:
  classic's `librts2imagedb` looked like a full second recompile of the
  whole `rts2fits` source tree (duplicate `.o` files for
  image/channel/cameraimage/fitsfile), but turned out to just be
  `ImageDb`/`ImageSkyDb` subclassing the already-ported `rts2image::Image`
  - classic's build system recompiles the shared files a second time under
  a different macro for its own reasons, but no new kernel work was needed
  here, just these two new files on top.
- `rts2count.h`/`.cpp` (`Rts2Count` - ported as plain `.cpp`, not `.ec`; the
  classic `.ec` extension has zero actual `EXEC SQL` in it).
- `observation.h`/`.ec` (`Observation`, `ObservationState`).
- `timelog.h`/`observationset.h`/`.ec` (`ObservationSet` + `ObservationStatistics`/
  `DateStatistics`/`ObservationSetDate`, `lastObservationId()`).
- `targetset.ec` (implementation - `TargetSet` + `TargetSetSelectable`/
  `TargetSetByName`/`TargetSetCalibration`/`TargetSetGrb`,
  `sortByAltitude`/`sortWestEast`, `resolveAll`/`consoleResolver`). **Real bug
  found and fixed** - see UPSTREAM_BUGS.md: both sort functors had an
  inverted NULL check that set `observer = NULL` on the common no-arg call
  path instead of falling back to the configured observer.
- `taruser.h`/`.ec` (`UserEvent`/`TarUser` - pulled forward out of task #41
  since `imagedb.h` needs it transitively).
- `plan.h`/`.ec` (`Plan`) - pulled forward since `sub_targets.ec` needs it
  for `TargetPlan`. Noted (not fixed) a genuinely dead declaration:
  `Plan::getObservation()` is declared but never defined or called anywhere
  in the classic tree.
- `target.ec` (`Target`'s ~50 method bodies + `createTarget()`/
  `createTargetByName()` factories). **Two target types deliberately
  stubbed in the factory, not silently mis-handled**: `TYPE_TLE` and
  `TYPE_AUGER` now `throw rts2core::Error(...)` with a clear message instead
  of instantiating - see "Deliberately deferred" below.
- `sub_targets.ec` (all 14 concrete `Target` subclass implementations).
  **Suspected bug found, documented not fixed** - see UPSTREAM_BUGS.md:
  `TargetPlan::startSlew()` passes its own `plan_id` (int) into
  `Plan::startSlew()`'s `update_position` (bool) parameter slot; low
  confidence this legacy plan-scheduling path is even exercised given the
  Python queuer now owns target-level selection.

### Plan/queue persistence - DONE

- `planset.h`/`.ec` (`PlanSet`/`PlanSetTarget`/`PlanSetNight`).
- `queues.h`/`.ec` (`QueueEntry`, `Queue`, `queueQids()`) - DB persistence
  for the classic C++-native queue tables (`queues`/`queues_targets`),
  needed by executor's `ExecutorQueue`. Distinct from the Python queuer
  (`rts2-queue`) discussed in the design note - this is lower-level DB
  storage, not the target-selection policy layer.

### Executor daemon - DONE

`db/executor/` is a new CMake target directory (sibling of `db/db/`),
building `db_executor` (static lib) and the `rts2-executor` binary.

- `executorque.h`/`.cpp` (`QueuedTarget`, `TargetQueue`, `ExecutorQueue`,
  `Queues`) - the in-memory/DB-backed observation queue executor pulls
  targets from. **Real bug found and fixed** - see UPSTREAM_BUGS.md:
  `ExecutorQueue::addFirst()` silently dropped its own `rep_n`/
  `rep_separation` parameters and misrouted `plan_id`/`hard` into their
  slots via a positional-argument mismatch. `SimulQueueTargets`/
  `selectNextSimulation()` deliberately not implemented (declared only, for
  interface fidelity) - confirmed via grep that `simulque.h` is used only by
  the dead `rts2-selector` binary, not by executor.
- `scriptduration.h`/`.cpp` (new, not in classic layout) - re-adds
  `rts2script::getMaximalScriptDuration()` as a DB-aware additive extension.
  Classic declared this in base/script's `script.h`, but it takes
  `rts2db::CamList&` - the one DB touch deliberately dropped from
  `base/script` during task 25. Added back here instead of modifying
  base/script.
- `execclidb.h`/`.cpp` (`DevClientCameraExecDb`) - clean subclass of
  base/script's `DevClientCameraExec`, faithful port, no surprises.
- `rts2devcliphot.h`/`.ec` (`DevClientPhotExec`) - clean subclass of
  `rts2core::DevClientPhot` + base/script's `DevScript`, faithful port.
- `command.h`/`.cpp` (base/kernel) - added `CommandCorrect` (was missing,
  needed by `commandAuthorized()`'s `correction_info` handling).
- `executor.cpp` (`Executor:public rts2db::DeviceDb`, ~1290 lines after
  dropping Magick++) - the daemon itself. Faithful port with two
  deliberate deviations, both noted inline:
  - Dropped `<Magick++.h>`/`Magick::InitializeMagick` entirely (not
    deferred) - user-confirmed vestigial, see
    [[rts2_executor_jpeg_thumbnails_dead]].
  - Dropped the `#ifdef RTS2_HAVE_SYS_INOTIFY_H` guard around
    `fileModified()` - modern Linux always has inotify, matches the
    dead-legacy-branch convention used throughout this port.
  - Two unqualified names (`Configuration::instance()`,
    `(Rts2SelData *)`) that only compile in classic because
    `rts2script/element.h` has a stray file-scope `using namespace
    rts2core;` that leaks transitively into every TU that includes
    `execcli.h` - qualified explicitly here (`rts2core::Configuration`,
    `rts2core::Rts2SelData`) instead of relying on the same leak (which
    base/script's ported `element.h` still has, faithfully, so either
    form would have worked).

**Smoke-tested**: `rts2-executor --help` runs and prints the full option
list (executor-specific `--ignore-day`/`--no-dark`/`--no-auto`/
`--exe-timeout` plus inherited DB/daemon options) - confirms the binary
actually starts and its option-parsing/App/Daemon machinery works
end-to-end. Not yet tested against a live database or real devices.

Verified with a full from-scratch rebuild (`rm -rf build`, reconfigure,
build everything) - exit 0, only pre-existing warning patterns.

### Deliberately deferred

Confirmed via dependency-mapping (grep across all of classic `src/`) to be
used only by other classic tools (httpd, scheduler/selector - the latter
itself dead code superseded by the Python queuer, see
`rts2_script_language_underused`-adjacent memory), not by executor:
`account`/`accountset`, `labellist` (but not `labels` - see above, `Target`
needs that one directly), `messagedb`, `records`/`recvals`/`recordsavg`,
`user`/`userset` (except whatever `taruser` needed, which is now ported),
`mpectarget`/`simbadtargetdb`/`targetres`, `schedule`. Revisit only if/when
`rts2-httpd` (see the design note) is actually started.

**`TargetAuger` and `TLETarget` are deferred, not ported**, for the same
"defer a concrete subclass with no live consumer" reasoning used throughout
this port:
- `TargetAuger` (~2300 lines, `target_auger.h`/`.ec`) needs Auger-network-
  specific DB schema/fields; the user has no access to test against (see
  the Coihueco/Auger site-coverage note in the porting-initiative memory).
- `TLETarget` (`tletarget.h`/`.cpp`) needs the ~3200-line SGP4/SDP4 satellite
  propagator (`lib/pluto/{sgp,sdp4,sdp8,deep,basics,common,get_el,satellit}.cpp`)
  - only `pluto/observe.cpp` made it into base so far (needed by teld-d50),
  and this is the same gap flagged in the base bugfixing-raids memory.

`createTarget()`'s factory `switch` now throws a clear
`rts2core::Error` for `TYPE_AUGER`/`TYPE_TLE` instead of instantiating -
fails loudly rather than silently returning a wrong-behaving `ConstTarget`.

**JPEG thumbnail generation is dropped, not deferred** - classic
`executor.cpp` links GraphicsMagick++ (`Magick::InitializeMagick`) purely to
feed thumbnail generation in `rts2image::Image`; base's own `Image` port
(task 23) already dropped this silently, and the user confirmed (2026-07-14)
it's genuinely vestigial in the executor context - real image previews at
live installs (D50, SBT) are produced by a separate project ("asarina"), not
by executor. Do not re-add Magick++ to db.

**`SimulQueueTargets` (`simulque.h`/`.cpp`) is deferred** along with
`ExecutorQueue::selectNextSimulation()`'s implementation - confirmed via
grep this is used only by the dead `rts2-selector` binary
(`selectordev.cpp`), not by executor.

## "executor-lite" - REMOVE list implemented (2026-07-15)

The user's explicit instruction (2026-07-15): finish a clean, faithful port
first (done), *then* strip it down. Full inspection done, the user
confirmed a concrete remove/keep list, and **the removal pass is now
done** - **see [[rts2_executor_lite_plan]] memory for the itemized
decision and its "done" status**, summarized:

**Removed**: `Plan`/`PlanSet`/`TargetPlan` (deleted
`db/db/include/rts2db/plan.h`+`.ec`, `planset.h`+`.ec`, `TargetPlan`
from `target.h`/`sub_targets.ec`; `createTarget()`'s `TYPE_PLAN` case now
throws, same pattern as `TYPE_TLE`/`TYPE_AUGER`; `plan_id` kept as an
inert int everywhere it was already baked into a signature - e.g. the
kernel-level `Rts2Target::startSlew()` - rather than touching those
signatures), `ExecutorQueue`'s scheduling machinery (6 sort modes,
repeat/requeue logic, the multi-queue `Queues`/`activeQueue` apparatus,
the wire-protocol queue-editing methods `addFirst`/`moveIndex`/
`updateIndexTimes`/`queueFromConn`/`queueFromConnQids` - none of which
executor.cpp ever called, they were selector-only dead code - and,
discovered along the way, `selectNextObservation`/`EVENT_NEXT_START`/
`EVENT_NEXT_END`/`getMaximalDuration`, which turned out to be *also*
selector-only dead code, so `rts2script::getMaximalScriptDuration()` and
the `scriptduration.h`/`.cpp` extension added to support it were deleted
too), `selectorNext`/`selector_next_reported`/`DEVICE_TYPE_SELECTOR`
special-casing, and the dark/flat auto-scheduling policy (`doDarks` and
its `--no-dark` option, `flatsDone`, twilight auto-flat-switching,
`next_night`/standby dark auto-injection). `ExecutorQueue` is now a plain
`std::list<QueuedTarget>` (FIFO, no per-queue DB persistence - confirmed
dead too, since the only `ExecutorQueue` ever constructed uses
`queue_id = -1`, a "virtual" non-DB-backed queue). `current_plan_id`
stays as a `ValueInteger` pinned to `-1` - it can't be deleted outright
because `DevClientTelescopeExec::actionEvent()` (base/script) dereferences
the `EVENT_SLEW_TO_TARGET(_NOW)` event argument as a `ValueInteger*`.
Full clean rebuild verified; `rts2-executor --help` smoke-tested.

**Keep**: Auger/shower handling (`setShower`, active at the real site),
photometer support (`rts2devcliphot`, unused but cheap to keep), and the
acquisition state machine (`EXEC_ACQUIRE*`, currently dead since
`ElementAcquire` was never ported - but user wants to potentially revive
this later, so don't remove it).

Labels (`Labels`/`LabelsVector`, present in DB schema at every real site but
empty in practice) was flagged earlier as a lower-priority secondary
candidate - not part of the executor-lite discussion yet, revisit
separately if it comes up.

The three previously-open questions are now resolved (2026-07-15, see the
memory for full reasoning): GRB dual-target comparison - keep as-is, revisit
later once Kafka's GRBd duality work matures; `write_headers` - keep, it's
the core post-exposure telemetry-to-FITS-header write path (rare misbehavior
reports are a separate future bug-hunt, not in scope here); constraint-file
live-reload (`Constraints`/`MasterConstraints`/`ConnNotify`) - confirmed as
a soft/plan-level concern that duplicates the independent, DB-free hard
safety backstop already provided by `base/kernel`'s `ObjectCheck` +
teld horizon-limit code, making it a removal candidate too.

**Task #48 (investigation) is also done**: `base/teld/src/teld.cpp` wires
a `hardHorizon` `ObjectCheck` into the live move path unconditionally (info
refresh + target-set, not just present-but-unused code), and even with no
`--horizon` file configured, `ObjectCheck::getHorizonHeight()` defaults to
a 0°-altitude floor - so the hard-limit backstop is real and independent
of the executor at every site, configured or not. Removing `Constraints`/
`MasterConstraints`/`ConnNotify` from the executor is therefore confirmed
safe in principle, and the user explicitly said to go ahead. **Removed**:
`ConnNotify`/`notifyConn`, `MasterConstraints::setNotifyConnection()`, and
`Executor::fileModified()` from `executor.cpp`; the per-target soft
`Constraints` check inside `ExecutorQueue::isAboveHorizon()` (kept the hard
`Target::isAboveHorizon()` check there). `setGrb()`'s own
`checkConstraints()` call was deliberately **kept** - GRB alerts bypass
the queuer entirely (direct real-time interrupt), so there's no other
decision-maker to reject an out-of-constraints GRB target; removing it
would have been a regression, not a cleanup. `Target::addWatch()` no-ops
safely with no notify connection installed, so `db/db` needed no
changes. Full clean rebuild + `--help` smoke test passed.

Tasks #44-48 are all complete - the executor-lite pass is done. See
[[rts2_executor_lite_plan]] for the full writeup.

## Target-management CLI tools + DB bootstrap (2026-07-18)

Before this pass, db had the full `rts2db` library and `rts2-executor`
ported but no way to actually get a target into the database, and no
schema-bootstrap script. Scope (per explicit user direction): only what's
needed to create the database and create/list/edit targets so one can be
fed to the executor - not the full classic `src/db` toolset. See
`rts2c_porting_initiative` memory for the framing.

**Four binaries ported**, faithfully, from classic `src/db/*.cpp`, all under
the new `db/db/tools/` directory (own `CMakeLists.txt`, wired into
`db/CMakeLists.txt` via `add_subdirectory(db/tools)`):

- `rts2-newtarget` (`newtarget.cpp` + `rts2targetapp.cpp/.h`) - interactively
  create a target (RA/Dec, MPC ephemeris, or `-a` autogenerated ID).
- `rts2-target` (`target.cpp`) - the main management tool: enable/disable,
  priority/bonus/next-observable-time, scripts, constraints (airmass/lunar
  distance/altitude/max repeats), PI/program name, delete, observation
  start/end markers.
- `rts2-targetinfo` (`targetinfo.cpp`, needs `PrintTarget`) - target
  details/visibility/constraints/scripts, the main way to sanity-check a
  target before handing it to the executor.
- `rts2-targetlist` (`targetlist.cpp`) - simple listing (all/GRB/selectable).

Two new pieces had to be ported to support them, neither previously in
db:

- **`createTargetByString()`** (`db/db/include/rts2db/targetres.h` +
  `db/db/src/targetres.cpp`, added to the `db` library) - needed by
  `Rts2TargetApp::askForObject()`. Classic resolved plain names via SIMBAD
  (`SimbadTargetDb`, needs an XmlRpc client), an MPEC-catalog lookup
  (`MPECTarget`), and TLE satellite elements (`TLETarget`, needs the
  ~3200-line SGP4/SDP4 propagator) - none of those three dependencies exist
  in this tree yet. This port keeps the empty-target, RA/Dec-parse, and
  MPC-ephemeris (`EllTarget::orbitFromMPC`, already ported) branches
  faithfully and throws a clear `rts2core::Error` for anything else, same
  "defer a concrete case, fail loudly" pattern already used for
  `TYPE_AUGER`/`TYPE_TLE` in `Target::createTarget()`. `rts2-newtarget`'s
  `usage()` text was trimmed to match (dropped the "resolved by Simbad"/
  "TLEs separated with |" mentions, kept the MPC-ephemeris example since
  that one still works).
- **`PrintTarget`** (`db/db/tools/printtarget.h/.cpp`, ~800 lines,
  compiled straight into `rts2-targetinfo` rather than promoted to a shared
  library since it has exactly one consumer in this reduced scope) - a
  faithful, complete port of every display mode (extended info, constraints,
  GNUplot/bonus-plot script generation to stdout, DS9 `.reg` output,
  images/counts, script pretty-print). All of it runs on already-ported
  `Target`/`ObservationSet`/`ImageSetTarget`/`Script` methods, no new
  dependencies. `targetinfo.cpp`'s classic `--auger-id` option/branch
  (instantiates `rts2db::TargetAuger` directly) was dropped, not ported -
  `TargetAuger` itself is still deliberately deferred (see above).
- **`rts2script::getMaximalScriptDuration()`** had to be revived. It was
  deliberately dropped when `base/script` was ported (see this file's
  "executor-lite" section above) because its only callers at the time were
  either unported (`httpd/bbapi.cpp`) or confirmed dead code
  (`executorque.cpp`'s use of it, superseded by the executor-lite rewrite).
  `PrintTarget::printScripts()` is a real, live caller, so the function is
  back - but declared/defined locally in `db/db/tools/printtarget.h/.cpp`
  (needs `rts2db::CamList`) rather than back in `base/script`, to keep
  `base/script` itself DB-free.

One include-path lesson (only caught by the actual build, not by reading
classic source): `base/script`'s public headers are flat
(`base/script/include/script.h`, no `rts2script/` subdirectory) - unlike
`db/executor`'s own DB-aware headers, which *do* sit under an
`include/rts2script/` subdir. Classic `#include "rts2script/script.h"`-style
includes had to become `#include "script.h"` in `target.cpp` and
`printtarget.h`.

**DB bootstrap**: classic `src/sql/rts2-builddb` (schema under `create/`,
version-upgrade scripts under `update/`, `data/init.sql` for the mandatory
`types`/`medias` lookup rows, plus optional `data/{targets,landolt,planets}.sql`
demo data, `grant.sql`) and `src/sql/rts2-configdb` (insert camera/mount/
filter rows) were copied verbatim into `db/sql/` - pure shell + SQL, no
C++ changes, and no path adjustments turned out to be needed either (both
scripts default to `INITDIR=.`, so they just work run from inside
`db/sql/`).

**Verified**: full from-scratch rebuild (`rm -rf build`, reconfigure, build
everything) - exit 0. `--help` smoke test on all four new binaries -
confirmed `rts2-targetinfo`'s option list matches `PrintTarget`'s full set
with no `--auger-id`.

**Live-DB round trip verified (2026-07-19)**, on the user's own laptop, and
successfully - a real first for the whole base/db initiative (everything
before this was SDK-only smoke tests, no live backing store). Setup, matching
the real D50/SBT convention: a system user `rts2` (no login shell) plus a
matching Postgres role (peer auth, no passwords needed), a `stars` database
owned by that role, `rts2-builddb -A stars` then `rts2-configdb stars
--testdb` (adds test camera `C0`, mount `T0`, standard filters) - both run as
`rts2` via `sudo -u rts2`. One pre-existing, unrelated hurdle: `createdb`
initially failed with a Postgres collation-version mismatch (OS glibc had
been upgraded, 2.37 -> 2.43, since the cluster was initialized) - fixed with
`ALTER DATABASE template1/postgres REFRESH COLLATION VERSION` (a metadata-only
fix, not a data change) before retrying. Also discovered: this laptop already
had a real `/etc/rts2/rts2.ini` installed (a D50 config, dated 2023) with
`[database] name = stars` and real observatory coordinates already filled
in, so none of the new tools needed `--config`/`--database` flags at all.

Exercised end to end: `rts2-newtarget -f <id> <name> <radec>` (non-interactive
target creation), `rts2-targetlist` (full listing, including the ~530-entry
Landolt block from `data/landolt.sql` - see the flagged data-quality note
below), `rts2-targetinfo -e <id>` (full extended info - altitude/airmass/
rise-set/transit, galactic/solar/lunar distances, bonus, per-camera script
with expected-duration computation, constraint status), `rts2-target -c C0 -s
"E 10" <id>` (per-camera script assignment) confirmed via a follow-up
`rts2-targetinfo`, and `rts2-target -d`/`-e <id>` (disable/enable). All
worked correctly on the first real database this tree has ever touched.
Three warning-level log lines appeared and are expected, not bugs: "cannot
open horizon file" (no `[observatory] horizon` file configured - falls back
to a flat 0 degree floor, exactly the hard-limit backstop documented earlier
in this file), "cannot find value 'default_camera' in section 'observatory'"
(informational only - `TargetApp::init()` always probes this optional key;
harmless when `-c` is given explicitly), and "cannot parse constraint file"
for both the system and per-target constraint XML (neither file exists yet -
correctly reported as "empty/not used" a few lines later, not a hard error).

**Fixed (2026-07-19): `data/landolt.sql` replaced with `data/stetson.sql`.**
The user pointed out that `data/landolt.sql`'s ~530 entries were individual
named Landolt catalog stars (bare RA/Dec, no magnitude/spectral-type data
attached) mislabeled as calibration "fields" - not the same thing as
Landolt's actual standard fields or Stetson secondary-standard fields, and
not usable for photometric calibration as-is (also compounded by
`rts2-builddb`'s prompt for this list defaulting to yes on empty input,
which is presumably why it shipped in every RTS2 database for decades). Per
the user's request, replaced with the 103 Stetson photometric standard field
positions from `~/stetson-old/stetson/positions` (name + RA/Dec in decimal
degrees, columns 1-3 of that file) - `data/landolt.sql` deleted, new
`data/stetson.sql` added at `tar_id` 200-302 (same slot `data/landolt.sql`
used to occupy). Kept the existing `l` type code rather than minting a new
one (the user's call: the executor only needs to distinguish a handful of
special types - GRB, elliptical, TLE/LEO - the rest, including this one,
don't matter to it either way) - just updated `types.type_description` for
`l` from "Landolt calibrations fileds" to "Photometric standard fields
(Stetson)". `rts2-builddb`'s prompt text/log messages updated to match
("Would you like to create Stetson photometric standard field targets
[Y|n]?"). Not every Stetson field is observable from every site (some are
deep-southern, e.g. Carina/LMC/47 Tuc) - left unfiltered on purpose, exactly
as `data/landolt.sql` always was; RTS2's own visibility/selectability
logic already handles per-site unobservable targets at runtime. Verified via
a full teardown+recreate cycle (see below) - loads clean, no constraint
violations.

**New: `db/sql/rts2-testdb-create` / `rts2-testdb-destroy`** - a
repeatable clean-slate test harness for the whole bootstrap chain, added at
the user's request ("deletion is interesting for testing the creation").
`rts2-testdb-destroy [dbname] [dbuser]` (default `stars`/`rts2`) terminates
open connections, drops the database, drops the Postgres role, and removes
the system user - undoing everything `rts2-testdb-create` sets up.
`rts2-testdb-create` is idempotent (only creates the system user/Postgres
role if missing) and chains: system user -> matching Postgres role (peer
auth, LOGIN/CREATEDB/CREATEROLE) -> `createdb` -> `rts2-builddb -A` ->
`rts2-configdb --testdb`. Verified end to end on the user's laptop: destroy
then create ran with zero errors, full schema + Stetson/planet data +
test camera/mount/filters all loaded cleanly from a truly empty starting
state (no pre-existing user/role/database).

**Live `rts2-executor` run verified (2026-07-19) - first one for this whole
port.** Set up a minimal live system on the user's laptop: `rts2-centrald`
(unprivileged port 6617, since 617 needs root), `rts2-teld-dummy -d T0`, and
`rts2-camd-dummy -d C0`, all run by the agent directly (no DB dependency, so
no `sudo` needed - matches the earlier finding that camd/teld-dummy don't
call `loadFile()` for DB purposes, only centrald/some teld drivers touch
config at all). A minimal scratch `rts2.ini` (`[observatory]` real D50
coordinates, `[database] name=stars user=rts2`) was used instead of the
real `/etc/rts2/rts2.ini` to avoid that file's D50-specific `blocked_by`
device-name assumptions (`T0 F0 W0`) tripping up a bare-bones dummy setup.
`rts2-executor` itself has a hard DB dependency (peer auth), so it had to
be started by the user via `sudo -u rts2` in their own terminal - the
agent's shell has no sudo TTY. Needed `--ignore-day` (it was daytime) and
one operational lesson: `--lock-prefix` must point somewhere the DB-bound
`rts2` user can actually create files - both the agent's private scratchpad
directory (mode 700) and a stale lock file left behind by an earlier failed
run-as-`mates` attempt blocked this the first two times; fixed by using a
shared, world-writable `/tmp/rts2-exec-test/` and clearing the stale lock.

**New tool: `base/sendcmd/` (`rts2-sendcmd <device> <command...>`)** - not
a classic port, a small new `rts2core::Client` subclass (modeled on how
`rts2-scriptexec`/`rts2-mon` already use `Client`/`ConnClient`/`Command`)
that connects to centrald, waits for a named device to become ready, sends
one raw wire command (e.g. `now 5001`), prints the result, and exits. Built
because there is no Python queuer or `rts2-httpd` in this tree yet, so
nothing could script `rts2-executor`'s `now`/`queue`/`next` commands
non-interactively - `rts2-mon` can send them, but only interactively. Ended
up unused for this particular test (the user drove `rts2-executor` directly
through their own `rts2-mon` session instead, queuing target 2 - the
built-in "Flat frames" target, sensible since it was evening), but stays in
the tree as a reusable non-interactive command-sending tool for future
scripted tests.

The actual run: `rts2-teld-dummy` tracked continuously for ~14s, camera
`C0` ran a repeating exposure loop matching Flat frames' default script,
then a `stop()` cleanly parked the telescope. One momentary "exposure
failure"/"no-target image" pair in the log coincides exactly with the
`stop()`/`EVENT_KILL_ALL` sequence - an in-flight exposure being aborted,
not a bug. Confirms the full chain end to end: DB-backed target lookup,
script retrieval and parsing, device command dispatch, and telescope/camera
device-client coordination, all through code this session's work put
together (base kernel/script/teld/camd + db db/executor). Target 5001
(created earlier, then lost when the `stars` DB was torn down and rebuilt
for the Stetson-data fix) was never re-created to prove the point
separately - the user's reasoning, which holds: if a real DB-defined target
Type with its own script (Flat frames) executes correctly, an arbitrary
user-created target with an assigned script will behave identically, since
nothing in the executor path branches on "is this a demo target."

**Where the logs are**: every daemon here uses the syslog-first logging
from the earlier `base` logging redesign (see
`rts2_logging_architecture_flaw` memory) - `journalctl -t rts2` shows the
merged stream across all daemons (centrald, teld, camd, executor alike),
tagged by PID, regardless of which one the agent started directly vs. which
one the user started via `sudo -u rts2` in their own terminal.

## Debian packaging: `rts2-db` (2026-07-19)

First package for the DB-bound half, mirroring `rts2-base`'s established
conventions (`base/debian/`). New `db/debian/`. Live-tested against a
real installed `rts2-base` (the user's own laptop, running centrald +
dummy telescope/camera/filter-wheel/focuser as `rts2.service`) - went
through three real bugs before it worked, all fixed the same session, see
below.

**Contents**: `rts2-executor` + the four target-management CLI tools
(`rts2-newtarget`/`rts2-target`/`rts2-targetinfo`/`rts2-targetlist`) +
`db/sql/{create,update,data,grant.sql,rts2-builddb,rts2-configdb}`
installed under `/usr/share/rts2-db/sql/`. **Deliberately not shipped**:
`db/sql/rts2-testdb-create`/`rts2-testdb-destroy` - dev-only test
tooling (the latter literally drops the database), stay source-tree-only.

**Build** (`db/debian/rules`): same pattern as `rts2-base` - no CMake
`install()` rules yet in either tree (still the same deferred cleanup),
`dh_auto_install` overridden to copy build output directly. Builds the
whole `db` tree, which nests `base` source via `DB_BASE_DIR`
exactly as it does for local development - the build host needs both
source trees checked out side by side. `Depends: rts2-base,
postgresql-client, adduser` + the usual `${shlibs:Depends}`;
`Recommends: postgresql` (not a hard `Depends` - a site could point at a
remote DB and skip local provisioning).

**No systemd unit of its own.** First attempt shipped a standalone
`rts2-executor.service` (`Type=simple`, `User=rts2`,
`ExecStart=/usr/bin/rts2-executor --database stars`) - this was wrong on
a live system and got replaced entirely, not just patched. What actually
runs `rts2-executor` is `rts2-base`'s existing `rts2-start`/
`rts2@.service` mechanism, exactly like every device: `postinst` appends
one line to `/etc/rts2/services` (a conffile *owned by* `rts2-base` -
user's explicit call: append idempotently from this package's postinst
rather than restructure `rts2-base`'s packaging into a drop-in-directory
scheme, which would have needed rebuilding/reinstalling the already-
installed `rts2-base` too):

```
executor	EXEC	--run-as rts2
```

`--run-as user[.group]` (`Daemon::switchUser()`, `base/kernel/src/
daemon.cpp`) is the mechanism actually being reached for here, not
systemd's `User=`: it runs inside `doDaemonize()`, **after** the lock
file is already created - so launched as root (via `rts2-start`, same as
every other device), it creates its lock file with no permission issue,
*then* drops to `rts2` for the database connection. Start/stop it per
device with `systemctl enable --now rts2@EXEC.service` /
`rts2@EXEC.service stop` - never auto-started by this package, same
"don't auto-start something that can command real hardware" reasoning as
`rts2-base`'s own units.

**Three real bugs found via live installs, not just building** (each
its own version bump: 0.1.0-1 -> -2 -> -3 -> -4):
1. **`rts2-executor.service` crash-looped immediately**
   (`cannot create lock file /var/run/rts2_EXEC: Permission denied`) -
   `User=rts2` can't write the default root-only lock path. First fix
   attempt added `RuntimeDirectory=`/`--lock-prefix` (worked, but see #3).
2. **`Depends: rts2-base (= ${source:Version})` could never be
   satisfied** - that substitution variable is *this source package's
   own* version, not `rts2-base`'s; they're independent source packages
   with independently evolving version numbers. `dpkg` correctly refused
   to configure once the two drifted apart (`rts2-base` at `0.1.0-1`,
   `rts2-db` at `0.1.0-2`). Fixed by dropping the version pin entirely.
3. **The whole standalone-unit approach was itself wrong**, discovered
   only by testing against `rts2-base`'s real running service: the
   daemon started but never appeared in `rts2-mon`. `-d`/`--server`
   already have working fallbacks in the code (`Device::Device()`
   defaults `device_name` to the constructor's fixed name; `centraldHosts`
   defaults to `localhost:617`), so those weren't it - the real problem
   was `Type=simple` with a bare `ExecStart` bypassing `rts2-start`'s
   shared-flag injection (any `-`-prefixed line in `/etc/rts2/centrald`
   gets appended to every device/service's invocation) and not matching
   `rts2-base`'s own units (`Type=forking`, tracked via `PIDFile=`).
   Replaced wholesale with the `services`-file + `--run-as` approach
   above rather than debugged further - the D50 install the user already
   runs proved this pattern works.

**`postinst`** (the DB-provisioning part, unaffected by the above) -
deliberately more conservative than `rts2-testdb-create` it's modeled on:
- System user/group `rts2` via `adduser --system` if missing.
- Postgres role `rts2` via `createuser --login` if missing - **LOGIN
  only**, no `CREATEDB`/`CREATEROLE` (unlike the dev test script, which
  grants both - fine for a disposable role, not appropriate as a
  permanent production privilege).
- Checks whether database `stars` already exists first. **If so, does
  nothing further to it** - covers sites with an existing D50/SBT-style
  manual setup, or a package reinstall. Only a genuinely fresh install
  triggers schema creation. **Verified live**: on the user's laptop,
  where `stars` already existed from this session's manual testing,
  postinst correctly printed "database 'stars' already exists - leaving
  it untouched" and just ran the idempotent grant.
- On a fresh DB: runs `rts2-builddb -A` **as the `postgres` superuser**
  (not as `rts2`), so the `observers` group + every table end up owned by
  `postgres`; then `GRANT observers TO rts2` - the app role gets full
  access purely through the existing grant-to-group mechanism already in
  `grant.sql`, without ever holding schema-creation privileges itself.
- **Deliberately never runs** `rts2-configdb` (camera/mount/filter setup
  is real-hardware-specific, left to the site admin - exactly what
  classic `rts2-builddb`'s own trailing message already says) - postinst
  ends with a short pointer to `rts2-configdb`/`rts2-newtarget` instead.
- No local Postgres cluster detected (`pg_lsclusters`) -> skip
  provisioning entirely with a message, rather than failing; a site
  pointing at a remote DB is expected to configure that themselves.

**`postrm`** - explicitly does **not** drop the database, role, or system
user, even on `purge`. Unlike `rts2-testdb-destroy` (test tooling, meant
to destroy), a real site's `stars` database is irreplaceable observation
data - `purge` only prints the exact manual commands to remove things, for
an admin who actually wants to. Doesn't remove its appended
`/etc/rts2/services` line either, for the same don't-clobber-what-you-
don't-own reasoning.

**Fully verified live (2026-07-19), end to end, on the user's real
`rts2-base`-managed system**: installed `rts2-db_0.1.0-4`, `postinst`
correctly detected the pre-existing `stars` database and skipped
provisioning (just the idempotent grant), appended the `executor` line to
`/etc/rts2/services`, then `systemctl enable --now rts2@EXEC.service` (via
a full `rts2.service` restart) brought it up as
`/usr/bin/rts2-executor -d EXEC --run-as rts2 --server localhost` -
`rts2-start`'s own construction from the services-file line plus
`/etc/rts2/centrald`'s shared `--server` option, exactly as designed.
Confirmed via `ps -o pid,user,cmd` that the process actually runs as
`rts2` (not root) after the fork-then-drop-privileges sequence, and it
showed up connected in `rts2-mon` alongside the real telescope/camera/
filter-wheel/focuser devices. This closes out the packaging work for this
pass - `rts2-db` genuinely creates the database and installs a working
executor, the user's original ask at the start of this thread.

Not in scope for this pass: CMake `install()` rules for a proper
shared-library-level dependency (still the same deferred cleanup from
`base/STATUS.md`); a configurable database name (hardcoded to `stars`,
matching every classic default); dbconfig-common (explicitly declined -
custom postinst chosen instead); converting `/etc/rts2/services`/
`/etc/rts2/devices` into a proper drop-in directory in `rts2-base` itself
(the "cleaner" alternative to the idempotent-append postinst, explicitly
declined this round - see `rts2c_porting_initiative` memory for both
decisions).

## Post-packaging live-fire bugs (2026-07-19/20): images never being saved

Discovered while actually observing a real target (283, a Stetson field)
on the user's live system - "I wonder where the images are going" turned
into a real multi-layer bug hunt, all fixed same session.

**Layer 1 - the `images` DB table didn't exist at all.** `\dt images` on
the live `stars` database came back empty, despite `grant.sql` explicitly
granting on it and `create/tables.sql` clearly defining it. Root cause:
`CREATE FUNCTION ... LANGUAGE 'c'` (used by `create/typ_wcs2.sql` to
define the `wcs2` astrometry column type, backed by the classic tree's
`src/pgsql/pg_wcs2.c` - a small, already-working, already-installed
Postgres C extension, `/usr/lib/postgresql/15/lib/pg_wcs2.so`, left over
from a 2023-era install on this laptop) **requires database superuser**.
The `stars` database was originally built via the dev-only
`rts2-testdb-create` script, which ran `rts2-builddb` as the `rts2` role
(only ever `LOGIN`/`CREATEDB`/`CREATEROLE`, never superuser) - so
`typ_wcs2.sql` silently failed, leaving `wcs2` an incomplete shell type,
which silently broke `CREATE TABLE images` (references `astrometry
wcs2`), which cascaded to every later `ALTER TABLE images ...` in every
`update/rel_*.sql` also silently failing for the rest of that original
bootstrap run. All silent because `rts2-builddb`'s `run_psql` helper
unconditionally echoes " OK" regardless of whether the underlying `psql`
call actually succeeded - a real, pre-existing (inherited, not
introduced) bug worth knowing about if a build ever looks suspiciously
too-clean. **Does not affect the real `rts2-db` package** - its
`postinst` already runs `rts2-builddb` as the `postgres` superuser
specifically to avoid this whole class of problem; this was purely a
defect in the one already-existing dev-created database.
- **Fix applied live**: replayed `create/tables.sql`, `create/views.sql`,
  every `update/rel_*.sql` in order, then `grant.sql`, all as `postgres`
  superuser, directly against the live `stars` database. Safe because
  none of those files wrap themselves in a transaction (verified: no
  `BEGIN`/`ON_ERROR_STOP` in any of them), so each top-level statement
  autocommits independently - anything already correctly in place just
  errors "already exists" and moves on. One deliberate exception: skipped
  `update/rel_0_9_5.sql`'s `UPDATE filters SET filter_id =
  nextval('filter_id');` line (a one-time historical migration that would
  have needlessly reassigned every already-existing filter's ID, since
  `filters` was empty on the original run but has real rows now).
  Verified via `\d images` afterward - correct final column set, indexes,
  and FK constraints, matching every `update/rel_*.sql` patch applied in
  order (including the `img_filter` -> `filter_id` migration's own
  view-drop-cascade-and-recreate, which worked exactly as designed).

**Layer 2 - even with the table fixed, no FITS files were ever written,
silently.** Found via `find / -iname "*.fits" -newermt "-30 min"` turning
up nothing. Root cause: `[observatory] base_path`
(`Configuration::getSpecialValues()`, code default `/images/` when unset)
is root-owned and unwritable by the unprivileged `rts2` user
`rts2-executor` now correctly runs as - and `FitsFile::createImage()`
(`base/kernel/src/fitsfile.cpp`) had a **missing error log** on exactly
this one failure path (`mkpath()` failing) while every other failure
branch in the same function does log - so the failure was completely
invisible until the log-fix below made it visible.
- **Fixed**: `fitsfile.cpp` now logs `MESSAGE_ERROR` with `strerror
  (errno)` on `mkpath()` failure, matching the surrounding pattern
  (`rts2-base` 0.1.0-2).
- **Fixed**: `base/packaging/rts2.ini` now sets `base_path =
  /var/lib/rts2/images/` explicitly; `rts2-db`'s `postinst` creates that
  directory and `chown`s it to the `rts2` user (`rts2-db` 0.1.0-5) - the
  actual place that needs to exist, regardless of which config value is
  active. **Only helps fresh installs** - conffile semantics mean an
  already-customized `rts2.ini` (this laptop's real D50-style config)
  doesn't pick up the new packaged default on upgrade; confirmed the user
  had to add the line by hand.
- **A third layer surfaced past that**: the D50-style config doesn't
  actually use `%b`/`base_path` at all for `que_path`/`acq_path`/
  `archive_path`/`trash_path`/`flat_path`/`dark_path` - it hardcodes each
  one to a literal `/images/%N/...` path (the commented-out alternates
  right above each active line *do* use `%b`, the active ones don't).
  Setting `base_path` alone was therefore a no-op for six of the seven
  image-path keys; only `Image::toMasterFlat()` (hardcoded `%b/flat/%c/
  master/%f` in `image.cpp`, no config override at all) actually reads
  it. Real fix for this specific config: replace the literal `/images/`
  prefix with `/var/lib/rts2/images/` in all six explicit path lines.

**Verified fully working end to end**: real FITS files (with the dummy
camera's synthetic star field + noise) now land under
`/var/lib/rts2/images/`, no permission errors in the log, `images` table
populates correctly. This closes the loop the user opened by asking
"where are the images going" - a good reminder that "it seems to be
observing fine" doesn't mean the whole pipeline is actually intact until
something (in this case, literally checking for the output files)
confirms it.
