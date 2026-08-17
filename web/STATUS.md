# web design & porting status

Working notes for the `web` effort so it can be picked up cold. Update this
file at the end of each work session; don't rely on chat history surviving.

**Status as of 2026-08-17: tasks 1-6 done (the entire non-DB track), task
7's first slice done and DB queries moved to the worker pool after being
proven to block the whole daemon against real production data, and task
8's first pass done (`web/static/` dashboard, live-verified in an actual
headless browser).** Also: first live testing against a real production
site (`lascaux.asu.cas.cz`), not just this laptop - found and fixed a
real libmicrohttpd version-portability gap and a real DB-query
concurrency bug that the local test database was too small to expose.
See the phased plan below for the real progress entries. Remaining:
task 7's night reports/image search/DB-availability state machine, and
task 9 (Big Brother, deferred pending site confirmation).

## What web is

The from-scratch CMake port of classic `src/httpd` (`rts2-httpd`): the
daemon that exposes RTS2's device bus and (optionally) its database to the
outside world over HTTP - JSON/REST API, live value/state push, image
previews, plots, target/observation history. Sibling to `base`, `db`, and
`gui`: same "reviewed and lightly modernized port, not a mechanical
translation" approach as the rest of `rts2ng`, same bottom-of-dependency-
graph-first working method.

Unlike `base`/`db`/`gui`, this is **not primarily a mechanical port** - the
user explicitly asked for a redesign, not a straight carry-over, because
the classic implementation has real architectural problems (see "Why not
just port it" below). What *is* carried over: the JSON/REST endpoint
surface, the `AsyncValueAPI` push-on-change idea, and the general shape of
what the daemon needs to do - not the networking/HTTP implementation
underneath it.

## Why not just port it

Classic `rts2-httpd` was analyzed file-by-file (`lib/xmlrpc++/*`,
`src/httpd/*`, `lib/rts2json/*`) before this design was agreed. Findings:

- **Everything runs on one thread**, including the RTS2 bus connection
  itself. `XmlRpcDispatch::work()` (`lib/xmlrpc++/XmlRpcDispatch.cpp:71`)
  is a single `poll()` loop shared by device-bus sockets *and* every HTTP
  client socket. Image/plot generation (`Magick++`, synchronous,
  `lib/rts2json/imgpreview.cpp:257,352`) and DB queries (synchronous
  `ecpg`/`libpq`) block that one thread - a slow preview request or query
  stalls telescope-control traffic to every other client for its
  duration.
- **`XmlRpcDispatch::work()` has a hardcoded `struct pollfd fds[MAX_POLLS]`,
  `MAX_POLLS 200`** (`XmlRpcDispatch.cpp:68,88`), written into with no
  bounds check for every monitored source (devices + HTTP clients +
  long-poll connections). Past 200 concurrent sources this is a stack
  buffer overflow, not graceful degradation. `rts2ng/base/kernel`'s own
  `Block` already fixed the equivalent problem with a dynamically sized
  `struct pollfd *fds` - `web` should reuse that reactor, not reintroduce
  a second, buggier one.
- **No compression, no streaming, no static-file serving.** Every response
  is fully buffered in memory (`new char[]` + `memcpy`, e.g.
  `httpreq.h:169`, `imgpreview.cpp:264,367,420,577`) before anything is
  sent. Static JS/CSS are C++ string literals compiled into the binary
  (`lib/rts2json/libjavascript.cpp` 1376 lines, `libcss.cpp` 1824 lines) -
  editing a stylesheet means recompiling and restarting the daemon, which
  also drops the live bus connection.
- **True XML-RPC (`src/httpd/xmlapi.cpp`, the `R2X_*` method-call surface)
  duplicates the JSON REST API (`api.cpp`) feature-for-feature, with no
  real consumer left.** Checked: `python/rts2/rtsapi.py` (the actual
  client library) talks plain JSON over `http.client`; `gui/viewer` has
  no XML-RPC references; the only consumer is `rts2-xmlrpcclient`, a CLI
  tool shipped alongside the server whose functionality is a strict
  subset of the JSON API. Dropping true XML-RPC removes a whole redundant
  protocol layer.
- **Image previews have zero server-side cache and do full-resolution work
  regardless of the requested thumbnail size** - see "Image archive/
  preview performance" below. This is the user-identified top pain point
  ("looking up images from the archive... very slow in the existing
  version") and the concrete evidence behind it.
- What *is* worth keeping conceptually: `AsyncValueAPI`
  (`include/rts2json/asyncapi.h`) - a long-poll/chunked push mechanism for
  live value/state changes. Right idea (push diffs, not poll-the-world),
  wrong transport by today's standards (WebSocket is simpler and
  universally supported now).

## Image archive/preview performance (priority driver, confirmed 2026-08-17)

The user flagged archive image lookup as the single most painful part of
the existing daemon in practice. Traced the concrete cause in
`lib/rts2fits/image.cpp`/`lib/rts2json/imgpreview.cpp`:

- **No server-side thumbnail cache exists at all.** `cacheMaxAge()` calls
  in `imgpreview.cpp:259,363,416` only set an HTTP `Cache-Control` header
  telling the *browser* to cache the response - there is no on-disk or
  in-memory cache of a previously generated preview on the server side.
  Every single request for the same image at the same size, from any
  client, at any time, regenerates it from scratch.
- **Regeneration is full-resolution regardless of requested thumbnail
  size.** `Image::getMagickImage()` (`lib/rts2fits/image.cpp:1617`) loads
  full channel data and runs the quantile/grayscale-stretch pass
  (`getChannelGrayscaleImage`/`getChannelPseudocolourImage`) over every
  pixel of the original image *before* ImageMagick ever scales it down to
  the requested preview size (`imgpreview.cpp:355,397`). A 128px thumbnail
  of a multi-megapixel CCD frame pays the full decode + full-image
  histogram-stretch cost every time.
- **All of that runs serially on the single request-serving thread**, so a
  search-results page showing N thumbnails (`Previewer`'s paging, e.g.
  `imgpreview.cpp:522` on) issues N independent `<img>` requests that get
  processed one at a time, each paying the full cost above, on the same
  thread that's also relaying live device state.

None of these three problems require each other to fix, but stacked they
compound: cold cache + full-res processing + serialization is close to
worst case for a page of thumbnails. Priority order if tackled
incrementally: **on-disk thumbnail cache first** (turns "regenerate every
view" into "regenerate once per image+size+quantile combination, ever" -
almost certainly the single biggest win, independent of any other
redesign work), then downsampling before the quantile pass instead of
after (cuts the per-miss cost), then the worker-pool offload from the
concurrency design below (stops a cache miss from blocking bus traffic
while it computes). Cache invalidation key: image file path + mtime +
requested size/quantile/channel/colour-variant params - images in the
archive are immutable once written, so mtime is a safe, simple
invalidation signal (no need for content hashing).

## Design decisions (agreed 2026-08-16)

### Deployment model: proxy is optional, opportunistic - never required

**Clarified 2026-08-17**: the daemon must be fully self-sufficient on its
own - plain HTTP, its own static-file serving, its own auth - and work
correctly with zero proxy in front, since dev/test environments (this
machine included) have no Apache/nginx and none is planned. A reverse
proxy is real infrastructure that *happens to already exist* at real
deployment sites (confirmed with the user: Apache there already terminates
the public entrance, `server.cz/api -> localhost:8000`, and handles
authorization for external clients; anything hitting `localhost:8000`
directly - trusted local/intranet clients - needs no proxy-level auth
today). `web` is designed to *take advantage of* that when present, not to
*require* it - nothing about the design is proxy-specific or browser-
specific either way: a browser talking straight to the daemon's own port
sees the same plain HTTP API as one going through Apache, just without the
perimeter auth/TLS/compression Apache would otherwise add. Concretely:

- **Static assets (JS/CSS/HTML) are real files on disk**
  (`web/static/`, installed via CMake), not compiled-in string constants -
  independent of whether a proxy exists. The daemon serves them itself via
  `libmicrohttpd`'s built-in file responses by default (needed for this
  dev machine and any proxy-less deployment); a site running Apache/nginx
  *may* `Alias` them directly and skip the daemon for that path as a pure
  optimization, but nothing in the design assumes that happens.
- **TLS and gzip are opportunistic, not assumed.** Where a proxy exists it
  can take over TLS termination and compression for the public path (that
  retires the hand-rolled `XmlRpcSocketSSL.cpp`), but the daemon on its own
  port speaks plain HTTP without either - fine for local/intranet/dev use,
  and consistent with how it's tested here.
- **The daemon's own auth (`UserLogins`/session/password) is always
  needed and always fully enforced by the daemon itself** - it can never
  assume a proxy already checked anything, since it must work standalone.
  Where a proxy *is* present, it adds a second, independent layer at the
  public perimeter; it doesn't let the daemon skip its own checks.

### Networking: `libmicrohttpd` on top of `Block`, not a second reactor

Classic's mistake was running two event loops (`Block`'s `poll()` for the
bus, `XmlRpcDispatch`'s `poll()` for HTTP) that never talk to each other,
in the same thread, taking turns. `web`'s `HttpD` daemon is a `Block`
subclass like every other `rts2ng` daemon (`centrald`, `teld`, `camd`,
...); HTTP serving is layered on top via `libmicrohttpd`
(`MHD_USE_EPOLL`/external-select mode - MHD supports being driven by an
*external* poll/epoll loop instead of owning its own thread, which is
exactly the integration point needed to share `Block`'s reactor instead of
adding a second one). Chosen over heavier alternatives (Boost.Beast,
Drogon) because it's the same weight class RTS2 already depends on
(libnova, cfitsio - small, mature C libraries), not a framework.

**Verified 2026-08-17** (`libmicrohttpd-dev` 1.0.1, this Debian 13
machine): built a standalone smoke test against the installed
`/usr/include/microhttpd.h`. `MHD_start_daemon(MHD_USE_EPOLL, ...)`
(no `_INTERNAL_THREAD` flag, i.e. external-polling mode) starts fine, and
`MHD_get_daemon_info(d, MHD_DAEMON_INFO_EPOLL_FD)` hands back **one**
epoll fd representing MHD's entire internal connection set - that single
fd is what gets registered as one more source in `Block`'s own poll set;
when it's readable, call `MHD_run()` and MHD handles all its connections
internally. Much cleaner than the `MHD_get_fdset`/`fd_set`-based
integration (also confirmed working, `MHD_get_fdset`/`MHD_get_timeout`
both returned sane values in the same test) which would need translating
an `fd_set` into `Block`'s `pollfd` array on every iteration - the single
epoll-fd approach avoids that translation entirely. This is the
integration to use.

`MHD_UPGRADE`/`MHD_upgrade_action`/`MHD_create_response_for_upgrade` are
present in this header (the generic HTTP Upgrade mechanism WebSocket rides
on), but there's **no `microhttpd_ws.h`** shipped by `libmicrohttpd-dev`
1.0.1 on this system - no bundled WebSocket frame-parsing helper. RFC 6455
framing is small enough (a few hundred lines: opcode/mask/payload-length
parsing, ping/pong, close) to write directly against the raw `MHD_UPGRADE`
byte stream rather than pull in a separate WebSocket library - confirmed
as the plan, not just a fallback.

### Concurrency: worker thread pool for anything that isn't a cheap
in-memory lookup

`Block`'s poll loop (main thread) handles bus I/O and answers cheap
requests inline - JSON serialization of device/value state that's already
sitting in memory costs microseconds, no need to offload it. Anything
genuinely expensive - image preview generation, plot rendering, DB
queries - goes on a small worker thread pool via a job queue; results are
marshaled back to the main thread (eventfd/self-pipe registered in
`Block`'s poll set) to write the HTTP response. One process, one bus
connection, no new IPC protocol between "gateway" and "worker" - just a
queue and a wakeup fd.

This was chosen over splitting into separate processes (a slim bus-facing
gateway + a separate DB/image worker service) because the process-per-
device model RTS2 already has *is* the isolation layer that matters (a
camera driver crashing can't take down centrald); the HTTP-serving daemon
isn't in that category - it's a single aggregation point either way, and
a two-process split would just relocate that aggregation problem to an
HTTP proxy layer instead of removing it, at the cost of a second session/
auth surface to keep in sync.

### DB access: reuse `db`'s existing synchronous `rts2db` (ecpg) on the
worker pool, not a rewrite onto async libpq - for now

`rts2db` (in `../db`) is ecpg-based - embedded SQL preprocessed into
blocking C calls - and already ported, tested, and used by
`rts2-executor`/`rts2-imgproc`. The pragmatic default for `web`'s DB-bound
endpoints (target/observation history, night reports) is to call that
existing, already-correct query code from the worker thread pool, exactly
like image/plot generation - not to rewrite query logic against libpq's
native async API (`PQsendQuery`/`PQconsumeInput`, multiplexable on
`Block`'s own poll set with no threads needed) just to avoid a thread
hop. Async libpq is a real, available option and worth keeping in mind
if the worker-pool queue depth ever becomes the actual bottleneck under
real load - but it means duplicating query logic outside `rts2db`'s ecpg
layer, which is new-code cost `web` shouldn't pay in a first pass.

### Runtime DB availability is a separate concern from compile-time DB support

**Clarified 2026-08-17** (user correction): `WEB_WITH_DB` only controls
whether DB-bound *code exists in the binary* - it's a packaging/dependency
question (this system will most likely end up with two Debian packages,
one depending on `libpq`/Postgres and one that deliberately doesn't, for
sites that don't want the dependency at all), analogous to how `db`
already produces `rts2-executor` as a hard-DB-required binary. It does
**not** answer the practical runtime question, which is different and
specific to `web`: **what happens when the DB-capable binary is running
but Postgres isn't actually reachable right now.**

For `rts2-executor`, "no DB" is a legitimate reason to not run at all - the
daemon's entire purpose is DB-bound. `web` is not like that: its core job
(live device monitoring/control, image previews, static assets) has
nothing to do with the database, and must keep working even when the
DB-enabled binary's Postgres connection is down - during a DB restart, a
network blip, planned maintenance, whatever. A DB outage should degrade
`web` to "target/observation history and DB-backed search return a clear
503/unavailable," not take down live telescope monitoring, and not block
the main thread retrying a connection.

Concretely, this means (task 7, not yet started - flagging the shape now
so it isn't designed as an afterthought later): a small DB-connection
state machine (`CONNECTED`/`UNAVAILABLE`, checked before routing a
request to a DB-bound endpoint; a background retry timer - not the
request path - attempts reconnection), and every DB-bound endpoint
handler has to treat "database unavailable" as an expected, ordinary
response state, not an exception that propagates somewhere undefined.
This is orthogonal to the ecpg-on-the-worker-pool decision above - it's
about what happens *before* a query is even attempted, not about how the
query itself runs.

**Revised 2026-08-17, after task 6** (user framing): task 6 settled the
piece of this that actually mattered most - auth turned out to need no
database at all, for *either* build variant (see "Local, DB-free auth"
below). That removes DB availability as a foundational, "can this daemon
even be used at all" question: live device monitoring/control, previews,
static assets, *and* authentication are now all provably independent of
whether a DB is configured or reachable. What's left for task 7 is
narrower than originally scoped here:

- **DB presence is a startup-time property of a given deployment, not a
  highly dynamic runtime one.** You either launch the DB-enabled binary
  with real connection info (and a failure to connect *at startup* is a
  clear, immediately visible failure state to handle explicitly - not
  silently limp along) or you don't run DB-bound endpoints at all. The
  elaborate `CONNECTED`/`UNAVAILABLE` reconnect state machine sketched
  above is still worth having eventually (a live site's Postgres
  genuinely can restart mid-session for maintenance), but it's a
  resilience nicety for task 7, not a prerequisite the rest of the
  daemon's usability was blocked on - it never was, in hindsight.
- **The resilience requirement that actually matters is schema/version
  tolerance, not connection flapping**: `web`'s `rts2db` schema
  expectations and a given site's actual DB schema can drift (different
  `rts2ng`/classic versions, mid-upgrade states). DB-bound endpoints must
  not crash or throw an unhandled exception when a query hits an
  unexpected shape (missing/renamed column, different table structure) -
  they should fail that one request cleanly, the same "don't crash on a
  surprise" principle `checkPreviewCache()`/`generatePreview()` and
  `UserLogins::load()` already follow for their own unexpected-input
  cases.

### Dropped outright

- **True XML-RPC** (`xmlapi.cpp`, `r2x.h`, the whole `XmlRpcServerMethod`
  call surface) - see "Why not just port it" above. The JSON/REST surface
  in `api.cpp` is the real API and is what gets ported.
- **`Magick++`/ImageMagick** for JPEG previews - heavyweight general-
  purpose image library for what's fundamentally "decode FITS, scale,
  encode JPEG." Default to `libjpeg-turbo` directly on the worker pool;
  confirm during the imaging task whether any preview feature (label
  overlay text, specific color-variant handling) genuinely needs more
  than that.
- **Compiled-in JS/CSS** (`libjavascript.cpp`/`libcss.cpp`) - real files
  on disk instead, per the deployment-model decision above.
- **`rts2-xmlrpcclient`** - no reason to port a CLI client for a protocol
  being dropped. `rts2-jsonclient`/the Python `rts2.rtsapi` client remain
  the real client-side story.
- **Big Brother federation** (`bbserver.cpp`/`bbapi.cpp`, pushing status
  to a multi-site aggregator) - not evaluated yet. Same tier-model
  judgment `base` already applies to vendor SDKs and `db`/`gui` apply to
  peripheral features: defer until a real site confirms it's still in
  use, don't port speculatively.

## Directory layout (proposed, not yet created except this file)

Mirrors `db`'s/`gui`'s "own directory, compiles against the kernel"
pattern:

```
web/
  CMakeLists.txt       # nests base (and, if WEB_WITH_DB, db) - see below
  STATUS.md            # this file
  httpd/                # the rts2-httpd daemon itself
    include/
    src/
  static/               # real JS/CSS/HTML, installed alongside the binary
  debian/
```

**Resolved 2026-08-17**: `db/CMakeLists.txt` and `gui/CMakeLists.txt` each
nest `base` independently via
`add_subdirectory(${..._BASE_DIR} base-build)`. `web` needs `base`
unconditionally (bus-only mode must work standalone, same as a bare
`base` install runs devices with no database) but needs `db` only when
DB-bound endpoints are wanted. `db` already nests `base` itself - nesting
*both* `base` directly and `db` (which nests `base` again) in the same
CMake configure would collide on duplicate targets (same library/
executable names defined twice). Fix, confirmed with the user as exactly
the right shape: a `WEB_WITH_DB` cache option (default `ON`) that picks
*one or the other* to nest, never both -
`add_subdirectory(${WEB_DB_DIR} db-build)` when `ON` (which pulls `base`
in transitively through `db`), or
`add_subdirectory(${WEB_BASE_DIR} base-build)` directly when `OFF`. This
is a build-time toggle, exactly analogous to classic `httpd.h`/
`httpd.cpp`'s `#ifdef RTS2_HAVE_PGSQL` branch that built two different
`HttpD` variants - just resolved by CMake configure-time subdirectory
choice instead of the C preprocessor, consistent with how `base`/`db`
were split in the first place. `HttpD` itself mirrors that: a
`Block`-only class when built without DB, a `rts2db::DeviceDb`-derived
class (adding the DB-bound endpoints) when built with it - same shape as
classic, same resolution mechanism `db` already uses.

**This is purely a compile-time/packaging question** - a "does the code
and the `libpq` dependency exist at all" switch, expected to eventually
produce two Debian packages (one Postgres-dependent, one not), same
category as `base`'s opt-in vendor-SDK drivers. It says nothing about
whether Postgres is actually *reachable* while the DB-enabled binary is
running - see "Runtime DB availability is a separate concern from
compile-time DB support" above for that half of the problem.

## Proposed phased plan (bottom of the dependency graph first, per the
`base` working method)

Numbering is provisional - expect it to shift as real constraints show up,
same as happened repeatedly in `base`/`db`.

Image/thumbnail work is pulled forward relative to the original draft of
this plan: it's file-system-only (no DB needed - the raw "decode this
FITS path, produce a preview" operation doesn't touch `rts2db`, only
target/night/observation *search* does), it's the user-identified top
pain point, and the cache-hit path is cheap enough to answer inline on
the main thread even before the worker pool exists - only cache-miss
regeneration needs offloading, and that can arrive one step later.

1. **DONE (2026-08-17)** - Skeleton: `HttpD : rts2core::Device` (bus-only,
   no DB, `DEVICE_TYPE_HTTPD` - already defined in `base/kernel/include/
   status.h:424`), `libmicrohttpd` wired into `Block`'s own poll loop via
   the single-epoll-fd integration confirmed above, real `/api/devices`
   endpoint backed by `getConnections()` (live bus state, not a stub).
   New `web/` subtree: `web/CMakeLists.txt` (nests `base` via
   `WEB_BASE_DIR`, same pattern as `db`/`gui`), `web/httpd/CMakeLists.txt`
   + `web/httpd/src/httpd.cpp` (~165 lines, single file - no reason to
   split `.h`/`.cpp` yet at this size, matching how small single-class
   drivers like `sensord/external/external.cpp` are laid out).
   Configures and builds clean against `libmicrohttpd-dev` 1.0.1 first
   try after the exploratory smoke tests above confirmed the API shape.

   Implementation notes/deviations from the plan as written:
   - `Block::willConnect()` defaults to "don't connect" - a device only
     opens direct peer connections it actually needs. Classic
     `src/httpd/httpd.cpp`'s non-PGSQL `HttpD::willConnect()` overrides
     this to opt into every device (lower-type, or same-type-lower-name,
     initiates - keeps two devices from both dialing each other
     simultaneously); ported the same override here. Without it,
     `/api/devices` stayed permanently empty even with a live camera
     connected to the same centrald - HttpD knew the device existed via
     centrald's roster but never actually connected to it. Not called out
     in the original design write-up above; adding this note so it isn't
     rediscovered from scratch later.
   - Unknown paths now correctly return HTTP 404 (`MHD_HTTP_NOT_FOUND`),
     not 200 - caught by testing the daemon's own behavior, not planned
     in advance.

   **Smoke-tested for real**: built `rts2-centrald` + `rts2-camd-dummy`
   (both already-ported `base` binaries) plus this new `rts2-httpd`, all
   three pointed at a scratch centrald on a non-standard port
   (`--lock-prefix` pointed at the scratchpad dir to avoid needing root
   for `/var/run`). Confirmed with `ss -tnp` that `rts2-httpd` holds two
   independent listening sockets (the RTS2 bus port and the HTTP port)
   plus an established connection to centrald, exactly as designed.
   `curl http://localhost:18889/api/devices` returned `[]` before the
   camera joined, `["C0"]` after (proving live bus state, not a
   hardcoded string), and unknown paths return a 404 JSON error.
   Repeated requests didn't destabilize the daemon or the bus connection.
   This is the first real evidence the `Block`+`libmicrohttpd` reactor-
   sharing integration (the whole point of this design vs. classic's two-
   event-loop mistake) actually works, not just compiles.

   Not yet done, left for task 2+: any real endpoint beyond the one
   hardcoded proof-of-plumbing path, auth, WebSocket push, and the
   RFC 6455 framing/`MHD_UPGRADE` integration (only the plain-HTTP half
   of `libmicrohttpd` has been exercised so far).
2. **DONE (2026-08-17)** - Real JSON/REST endpoint set: `/api/devices`
   (extended from task 1 to JSON-escape names), `/api/getall` (every
   value on every connected device plus centrald), `/api/get?d=&n=`
   (whole device or a single value), `/api/set` / `/api/inc` / `/api/dec`
   (`?d=&n=&v=`), `/api/messages` (bounded recent-message ring buffer).
   All cheap and answered inline on the main thread, per plan - no worker
   pool needed yet since nothing here is CPU-heavy.

   New `web/httpd/include/jsonvalue.h` + `src/jsonvalue.cpp`: `Value` ->
   JSON serialization split out of `httpd.cpp` since task 4 (WebSocket
   push) will need the same encoding - freshly written (not a mechanical
   port of classic `lib/rts2json/jsonvalue.cpp`), simplified to what's
   used so far (no "extended" `[flags,value,error,warning,description]`
   mode, no array/stat/rectangle value rendering yet). **One deliberate
   fix over classic**: string values and names are now actually
   JSON-escaped (`jsonEscape`/`jsonString`) - classic's `sendValue`/
   `jsonValue` wrote raw strings straight into the response with no
   escaping at all, so a value or device name containing a literal `"`
   would have corrupted the output.

   Deliberately not ported in this pass (classic `api.cpp` has more):
   `devbytype`, `selval`, `deviceinfo`, `sunalt`/`taltitudes` (libnova-
   dependent, not linked into `web` yet), everything DB-bound. Add on
   demand rather than porting the full surface speculatively.

   Two real things found and fixed while testing against a live daemon,
   not anticipated in the design write-up:
   - **`set`/`inc`/`dec` are fire-and-forget here**, not classic's
     default (an `AsyncAPI` holds the HTTP response open until the
     device acknowledges the command; classic only fire-and-forgets with
     an explicit `async=1` param). Responding immediately with the
     pre-ack value state is the only option available before the async-
     response plumbing (WebSocket push / worker+wakeup, tasks 4-5)
     exists - captured as a known simplification in the code comment,
     not silently different behavior.
   - **`message()` was never being called at all** until `init()` also
     called `setMessageMask (MESSAGE_MASK_ALL)` (ported from classic
     `HttpD::init()`) - centrald only forwards its log broadcast to
     connections that explicitly opt in. Without it `/api/messages`
     silently stayed empty forever, with no error - the kind of gap that
     only testing against a real running daemon surfaces, not a code
     read.

   **Smoke-tested for real** against the same live `centrald` +
   `rts2-camd-dummy` (`C0`) pair from task 1: `/api/getall` returned the
   full real value set for both `centrald` and `C0` (dozens of live
   values, not stubs); `/api/get?d=C0&n=CCD_TEMP` returned the camera's
   actual current temperature; `/api/set?d=C0&n=exposure&v=2.5` then
   `/api/get?d=C0&n=exposure` confirmed the value genuinely changed
   (`1` -> `2.5`) via a real `CommandChangeValue` round-trip over the
   bus, not just an accepted-and-ignored request; `/api/inc?...&v=1`
   correctly took it to `3.5`; a deliberate bad write
   (`/api/set?d=C0&n=CCD_TEMP&v=notanumber`, `CCD_TEMP` being read-only)
   correctly failed at the device and its real error message
   (`"command end with error -5 description: cannot set read-only
   value"`) showed up in `/api/messages`. Error cases (missing device,
   missing variable, missing params, unknown path) all return the
   correct 400/404 with a JSON error body instead of crashing or
   returning 200.
3. **DONE (2026-08-17)** - `GET /preview/<path>?ps=<size>&q=<quantile>`,
   backed by an on-disk JPEG cache keyed on relative path + `ps`/`q` (cache
   filename = `<cacheDir>/<relPath>.ps<N>.q<NNNN>.jpg`), freshness checked
   via mtime (cache mtime >= source mtime = hit) exactly as designed - no
   content hashing needed since archived images are immutable. New CLI
   options `--images-dir`/`--cache-dir` (`--cache-dir` defaults to
   `<images-dir>/.cache`); `/preview/` 404s with a clear error if
   `--images-dir` isn't set, rather than guessing a path.

   New `web/httpd/include/preview.h` + `src/preview.cpp`. FITS decoding
   reuses `base/kernel`'s already-ported `rts2image::Image` (confirmed
   its Magick++-dependent methods were already dropped when it was
   ported - see `image.h`'s "base note" - `getChannelGrayscaleImage()`
   already gives an 8-bit quantile-stretched grayscale buffer with no
   Magick++ involved). JPEG encoding is hand-written against plain
   `libjpeg` (`pkg-config libjpeg`, this system's is `libjpeg-turbo`
   underneath, ABI-compatible) - genuinely replacing `Magick++`, not just
   planned to. A simple box-average downsampler fits the full-resolution
   grayscale buffer to the requested preview size before encoding - per
   the plan, this still quantile-stretches at full resolution first
   (`Image::getChannelGrayscaleImage()` has no downsample-aware fast
   path); that specific refinement is still deferred, the cache is what
   actually removes the repeat cost.

   Path safety: a request path is rejected (before any filesystem call)
   if it contains a literal `..` segment, then independently re-checked
   after the fact by `std::filesystem::weakly_canonical()`-resolving the
   full path and confirming it's still prefixed by the canonicalized
   `imagesDir` - defense in depth, not just the string check.

   **Smoke-tested against real D50 data**: the user pulled a real one-
   night sample (163 FITS files, 598MB, 1024x1024 16-bit, real flats/
   darks/science frames with real WCS-less headers) to `/home/mates/
   images` specifically so this could be tested against real telescope
   data instead of synthetic files. Generated and visually confirmed
   (rendered the JPEG output directly) two previews: a flat-field frame
   showing real vignetting and dust-donut artifacts, and a science frame
   showing a real star field with detected point sources - both
   correctly quantile-stretched and legible, not noise or garbage.
   Verified: cache miss then hit for the same request returned
   byte-identical JPEGs (`cmp` clean) with the hit measurably faster
   (~59ms -> ~13ms even at this small 1024x1024 test scale, where the
   miss cost is already cheap - the gap only grows with real CCD
   resolutions and concurrent thumbnail-page load); touching the source
   FITS file's mtime and re-requesting produced a genuinely regenerated
   cache file (new mtime), confirming invalidation isn't a no-op.
   Path-traversal defense verified two ways: `curl --path-as-is` with an
   absolute-looking traversal correctly got `{"error":"invalid path"}`,
   and - since `curl` itself normalizes `..` segments client-side before
   sending, which would have made that test meaningless - a raw
   unnormalized HTTP request line sent directly over a `/dev/tcp` socket
   (bypassing curl entirely) confirmed the server's own check is what
   rejects it, not an artifact of the test tool.

   One side effect worth flagging: the mtime-invalidation test used
   `touch` on a real file under `/home/mates/images` (the user's actual
   pulled sample) - metadata-only (mtime), no content change, but a
   modification to their real data nonetheless.

   Not yet done, left for later tasks: downsample-before-quantile-pass
   (mentioned above), channel/colour-variant selection (`/preview/`
   always uses channel 0 grayscale - matches the real D50 data tested,
   which is single-channel), and moving cache-miss generation onto a
   worker thread (task 5) so it can't block bus traffic under load.
4. **DONE (2026-08-17)** - `GET /ws` upgrades to a WebSocket; every
   `valueChanged`/`stateChanged` on a connected device is pushed to every
   open WS client as `{"event":"value","device":"...","v":{"name":val}}`
   / `{"event":"state","device":"...","value":N,"statestring":"..."}` -
   the same push-on-change idea as classic's `AsyncValueAPI`, over a real
   WebSocket instead of chunked long-poll HTTP.

   New `web/httpd/include/websocket.h` + `src/websocket.cpp`: RFC 6455
   protocol mechanics only (handshake key via OpenSSL's `EVP_Digest`/
   `EVP_EncodeBlock`, frame encode/decode) - no socket handling, that
   stays in `httpd.cpp` next to the rest of the connection management.
   Client->server frame parsing is deliberately minimal: this is a
   push-only channel (the client isn't expected to send application
   data), so fragmented data-frame reassembly isn't implemented - only
   close/ping frames are acted on (ping -> pong), everything else is
   parsed just far enough to correctly track frame boundaries and
   discarded.

   `HttpD::createOtherType()` now returns one generic `HttpDevClient`
   (new class, `httpd.cpp`) for every connected device, overriding the
   base `rts2core::DevClient`'s `valueChanged`/`stateChanged` hooks to
   call back into `HttpD::broadcastValue`/`broadcastState`. This is
   simpler than classic's approach (`XmlDevClient`/
   `XmlDevTelescopeClient`/`XmlDevFocusClient`, one subclass per device
   type) because `web` has no camera/telescope-specific behavior to add
   here at all (no image writing, no script execution - that's
   `execcli`/`scriptexec`'s job) - one class genuinely covers every
   device type. **Known gap, not yet closed**: this only covers *other
   devices'* value/state changes via their `DevClient`, not centrald's
   own state (day/night, weather) - centrald's connection isn't a
   `DevClient`-managed peer connection, so it needs a different hook;
   deferred until a real consumer needs it.

   Socket lifecycle: after `MHD_UPGRADE`, MHD hands the raw socket
   entirely to this daemon - `WsClient` (new struct) owns it from there,
   registered into `Block`'s poll set the same way as every other source
   (`addPollFD`/`isForRead` in `addPollSocks`/`pollSuccess`, following
   the exact pattern already established for `mhdEpollFd` in task 1).
   `wsClients` is walked in full every poll cycle rather than
   incrementally maintained, same reason as `mhdEpollFd`: `Block::
   addPollSocks()` resets the whole pollfd array from scratch every
   cycle.

   **One real bug found and fixed by testing against a live handshake,
   not caught by review or by compiling clean**: the very first working
   attempt sent a byte-perfect `101 Switching Protocols` response
   *never actually reached the client* - the raw socket accepted bytes
   and returned nothing at all (not even an error), leaving `curl`/
   Python's `socket.recv()` to just hang until timeout. Root cause:
   `MHD_start_daemon` was only passed `MHD_USE_EPOLL` - `libmicrohttpd`
   silently can't process any 101/upgrade response at all unless the
   daemon is also started with `MHD_ALLOW_UPGRADE`. Nothing in the
   library complains when this flag is missing; it just never delivers
   the response. Fixed by OR-ing `MHD_ALLOW_UPGRADE` into the
   `MHD_start_daemon` flags. Also found and fixed while testing: the
   response had a duplicated `Connection: Upgrade, Upgrade` header - MHD
   already adds its own `Connection: Upgrade` for a 101 response, adding
   one manually just duplicated the value (harmless per spec, but
   untidy) - removed the manual one.

   **Smoke-tested for real**, not just compiled:
   - A raw Python `socket` handshake using RFC 6455's own published test
     vector (`Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==`) got back
     `Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=` - character-for-
     character the value the spec's own worked example gives, confirming
     the SHA-1+base64 handshake computation is correct, not just
     plausible-looking.
   - A real WebSocket client (Python's `websockets` library, not a
     hand-rolled test harness) connected to `/ws` and, while listening,
     received real live push messages triggered by real REST writes
     against the same live `centrald` + `rts2-camd-dummy` (`C0`) rig from
     earlier tasks: `/api/set?d=C0&n=exposure&v=7.25` produced
     `{"event":"value","device":"C0","v":{"exposure":7.25}}` over the
     WebSocket, and a following `/api/inc?...&v=1` produced `{"event":
     "value","device":"C0","v":{"exposure":8.25}}` - both arriving
     pushed, with no polling on the client's part.
   - Connect/disconnect cleanup verified with an fd-count check: 5 full
     WebSocket connect-then-close cycles left the daemon's open fd count
     exactly where it started (57 before, 57 after via `/proc/<pid>/fd`),
     and the daemon stayed fully responsive to plain REST calls
     throughout - no fd leak, no crash, no wedged state from a client
     disconnecting mid-session.
   - `stateChanged`/`broadcastState` share the identical tested
     transport (`WsClient::sendFrame`/`wsEncodeTextFrame`) as the
     verified `valueChanged` path, but weren't independently triggered
     live in this session (the dummy camera didn't undergo a state
     transition during testing) - noting this honestly rather than
     claiming a test that didn't happen; low risk given the shared code
     path, but worth a real check (e.g. triggering an actual exposure)
     next time this area is touched.
5. **DONE (2026-08-17)** - Generic `WorkerPool` (new `web/httpd/include/
   workerpool.h` + `src/workerpool.cpp`: fixed-size `std::thread` pool,
   `std::function<void()>` job queue, a caller-supplied wakeup callback -
   no opinion on what a job does or how results get back, that's the
   caller's problem) plus the actual wiring for cache-miss preview
   generation: an `eventfd` (`wakeupFd`) registered in `Block`'s poll set
   exactly like `mhdEpollFd`/WS client sockets from earlier tasks; a
   worker thread writes to it (thread-safe - eventfd writes are atomic
   at the kernel level) after pushing a `PreviewResult` onto a
   mutex-guarded queue, waking the main thread to drain it.

   `preview.h`/`.cpp` split in two, matching the actual cost boundary:
   `checkPreviewCache()` (cheap - path validation + stat + maybe a file
   read, stays inline on the main thread, exactly like before) and
   `generatePreview()` (the actual FITS decode + downsample + JPEG
   encode + cache write, now only ever called from a worker thread).
   `HttpD::handlePreview()` calls `checkPreviewCache()` synchronously as
   before; on `PreviewStatus::Miss` it calls `MHD_suspend_connection()`
   and submits a job to the pool instead of generating inline.

   **Deferred HTTP response mechanics** (`MHD_suspend_connection`/
   `MHD_resume_connection`), the trickiest new piece: a job closure
   captures the `MHD_Connection*` and its own copies of every parameter
   it needs, runs `generatePreview()` on a worker thread with no MHD
   calls at all (MHD itself is touched only from the main thread,
   respecting the fact that this daemon drives `MHD_run()` from exactly
   one thread), then pushes a `PreviewResult`. The main thread's new
   `drainPreviewResults()` (called from `pollSuccess()` when `wakeupFd`
   is readable) queues the real response via `MHD_queue_response()` and
   calls `MHD_resume_connection()` for each completed job, then calls
   `MHD_run(mhd)` **unconditionally once** afterward - per
   `MHD_resume_connection()`'s own header documentation, in external-
   polling mode the resume doesn't actually take effect (the queued
   response doesn't go out) until `MHD_run()` runs again, and nothing
   else guarantees that happens just because a resume occurred.

   **Revised same day, 2026-08-17**: the first version of this task
   serialized all cfitsio access behind a blanket `std::mutex`
   ("thread-safety isn't confirmed" - overcautious, asserted without
   actually checking). Challenged by the user, who correctly pointed out
   that read-only decode of independent files has no obvious reason to
   need shared state - so it was actually checked instead of left as an
   assumption:
   - `nm -D` on this system's `libcfitsio.so.10` (4.6.3) shows it linked
     against `pthread_mutex_init`/`_lock`/`_unlock` - it's built with its
     internal locking enabled, protecting the one genuinely global thing
     cfitsio has (its open-file table).
   - Reading `base/kernel/src/fitsfile.cpp` directly: the *other* global
     cfitsio has - a process-wide error-message stack, drained by
     `getFitsErrors()`'s call to `fits_read_errmsg()` - is only ever
     touched on a cfitsio *error* path (every call site is gated on
     `fits_status != 0`, and `fits_status` itself is a plain per-`Image`-
     instance `int`, not shared). Even a race there could only swap which
     of two simultaneously-*failing* threads' log message text comes out
     garbled - it can't affect whether `openFile()` throws or what pixels
     get decoded.
   - Removed the mutex entirely and verified empirically rather than
     just trusting the source-reading above: generated 30 previews of 30
     distinct real D50 files sequentially as a byte-for-byte reference,
     then regenerated the identical set from a cold cache with 8 worker
     threads, all 30 requests fired concurrently - genuine parallel
     `rts2image::Image`/cfitsio decode across threads, not just
     non-blocking. **0 mismatches across 3 separate concurrent runs (90
     total concurrent decodes)**, all byte-identical to the sequential
     references, daemon healthy throughout. (First attempt at this test
     produced 30/30 "mismatches" - a false alarm from comparing against
     a reference set built with a separate, non-order-guaranteed `find`
     invocation, not a real corruption bug; fixed by pinning both runs
     to one `sort`ed, persisted file list before concluding anything.)

   Net effect: FITS decoding now genuinely parallelizes across the
   worker pool (not just "doesn't block the main thread" - actual
   concurrent throughput), with no serialization anywhere in
   `generatePreview()`.

   New CLI option `--preview-workers N` (default 2).

   **Smoke-tested for real**, all against the same live `centrald` +
   `rts2-camd-dummy` (`C0`) rig plus the real D50 archive from earlier
   tasks:
   - A fresh (never-before-requested) preview parameter combination
     correctly round-tripped through suspend -> worker -> resume: `200`,
     a valid 333x333 JPEG, and a follow-up request with the same
     parameters came back cache-hit and byte-identical (`cmp` clean).
   - **The actual point of this task, tested directly**: fired 20
     concurrent fresh-cache-miss preview requests (real files from
     `/home/mates/images`, distinct `ps`/`q` per request so all 20 were
     genuine misses, cache cleared first) in the background, then made
     12 separate `curl` calls to `/api/devices` *while all 20 were still
     in flight*, timed with `%{time_total}`. Every single one came back
     in **0.18-0.37 milliseconds** - indistinguishable from an idle
     daemon, even though task 3's own single-image timing showed one
     cache-miss decode+encode alone costs ~59ms on this same test data
     (so 20 of them serialized on the old synchronous path would have
     blocked the bus/API thread for well over a second). All 20 preview
     requests completed successfully with valid JPEG output (`file`
     confirmed all 20 as real JPEG image data), confirming the worker
     pool didn't just avoid blocking - it actually got the real work
     done correctly under concurrent load.
   - Clean shutdown verified: `SIGTERM` to a running daemon (with the
     worker pool live) exited within 1 second, confirming
     `WorkerPool`'s destructor actually joins its threads rather than
     hanging or leaking them - important since `HttpD`'s destructor
     deliberately tears the pool down *first*, before `wakeupFd`/
     `wsClients`/`mhd`, specifically so no worker thread can still be
     running (and possibly calling `wakeup()`) while those are being
     destroyed.
6. **DONE (2026-08-17)** - HTTP Basic Auth (RFC 7617) gating `/api/set`,
   `/api/inc`, `/api/dec` against a local, DB-free credentials file - see
   "Local, DB-free auth" below for the full design (this settled a real
   architectural question the user raised: classic's login/password
   pairs live in the database, so a naive port would have made even the
   no-DB build depend on Postgres just to authenticate - wrong, and
   classic's own non-PGSQL build already proved it's unnecessary).

### Local, DB-free auth (task 6, 2026-08-17)

Raised by the user: classic's DB-backed `UserLogins` (login/password
pairs stored in `rts2db`) would have made *even the no-DB build*
depend on Postgres just to authenticate, undermining the whole point of
a bus-only `web` variant. Investigated classic's own source before
designing anything new, and found it had already solved this: classic's
non-PGSQL `httpd` build doesn't use the DB-backed `UserLogins` at all -
it falls back to `rts2core::UserLogins` (`lib/rts2/userlogins.cpp`,
explicitly titled "Logins for non-DB users" in its own header), a
`crypt(3)`-hashed, flat-file credential store that was sitting there
unused by this port until now. Ported it as `rts2web::UserLogins`/
`UserPermissions` (new `web/httpd/include/userauth.h` + `src/
userauth.cpp`) essentially unchanged in design, modernized in
implementation:

- File format unchanged: `username:cryptedpassword:permissions` lines,
  permissions a space-separated allowed-device list (`*` or `prefix*`
  wildcards), exactly as classic had it.
- Password hashing unchanged in spirit (`crypt(3)`, glibc/libxcrypt's
  `$6$` SHA-512 scheme - compatible with hashes from `openssl passwd -6`
  or `mkpasswd -m sha-512`), but classic's `#ifdef RTS2_HAVE_CRYPT`
  plaintext-password fallback (for crypt()-less systems) is dropped
  outright - consistent with `base`'s "modern Linux only" convention,
  and switched to the reentrant `crypt_r()` instead of plain `crypt()`
  since this daemon is now genuinely multi-threaded (task 5's worker
  pool) even though auth checks themselves currently only run on the
  main thread.
- **Deliberately independent of `WEB_WITH_DB`**: this is how *every*
  build variant authenticates, not a no-DB fallback - settles the
  "Runtime DB availability" design tension from earlier tasks (see the
  revised note above). New CLI option `--auth-file <path>`; empty
  (unset) means writes are completely unrestricted - a deliberate,
  explicitly-logged default (`MESSAGE_WARNING` at startup), matching how
  every earlier task in this session was actually tested, not a silent
  security gap.
- **Read endpoints stay open, only writes are gated** - `/api/set`,
  `/api/inc`, `/api/dec` require valid HTTP Basic Auth credentials *and*
  `UserPermissions::canWriteDevice()` for the specific device being
  written; `/api/devices`, `/api/getall`, `/api/get`, `/api/messages`,
  `/preview/`, and `/ws` all remain unauthenticated. This mirrors
  classic's own actual security model (many classic deployments
  intentionally leave read/monitor access open on a trusted network,
  gating only control actions) rather than inventing a new, broader
  permission matrix classic never had either.
- Startup behavior for a malformed (as opposed to missing) `--auth-file`
  is a deliberate asymmetry, worth calling out: a *missing* file loads
  as "no users configured" silently (not every site needs auth, and a
  typo'd path shouldn't be indistinguishable from "auth intentionally
  disabled" - though in practice both currently log the same way, this
  is flagged as worth revisiting), but a file that *exists and parses
  incorrectly* makes `init()` return `-1` and the daemon refuses to
  start at all. Reasoning: this is a security-relevant config file -
  silently running with a truncated or empty credential set because of
  a typo is worse than refusing to start with a clear error.

  **Smoke-tested for real** against the live rig: created a real test
  credentials file (`openssl passwd -6` hash) with two users - `admin`
  (permissions `*`, i.e. every device) and `readonly` (permissions
  `NOPE`, a device name that never matches, standing in for "valid
  login, wrong authorization"). Verified the full matrix: unauthenticated
  reads still `200`; an unauthenticated write `401` `"authentication
  required"` with a `WWW-Authenticate: Basic realm="rts2-httpd"` header
  (so a browser would prompt natively); a wrong password `401`
  `"invalid credentials"`; correct password but wrong device permission
  `401` `"not authorized to write to this device"`; correct admin
  credentials `200` with a genuine value change on the real device
  (`exposure` `1` -> `1.5` via `/api/set`, then -> `2.5` via `/api/inc`,
  both confirmed via a follow-up `/api/get`). Also verified the
  malformed-file startup refusal directly (`-i` mode, real exit code 10
  and a clear log line), and confirmed the daemon stays fully healthy
  (reads, preview, WebSocket) throughout all of the above.
7. **DONE, first slice (2026-08-17)** - `WEB_WITH_DB` wired up for real
   and a first vertical slice of DB-bound endpoints (target listing/
   detail, observation history), built and smoke-tested against a real
   local `stars` database - not `--help`-only, actual live queries
   against real data. Night reports and image search deferred (see
   below) - this slice proves the wiring works end to end and is more
   valuable than a broader, untested surface.

   **`WEB_WITH_DB` CMake option** (`web/CMakeLists.txt`, default `ON`):
   nests `../db` (which pulls in `base` transitively) instead of `../base`
   directly, exactly the mutually-exclusive nesting resolved earlier -
   confirmed no duplicate-target collision by building both variants
   clean from scratch (`WEB_WITH_DB=ON` default and `-DWEB_WITH_DB=OFF`
   into a separate build dir, both `cmake --build` clean, `OFF`'s
   `--help` confirmed to have zero DB-related options, `ON`'s shows
   `--database`/`--config`/`--debugdb` from `DeviceDb`).

   **`HttpD` now conditionally derives from `rts2db::DeviceDb` instead of
   `rts2core::Device`** via a `HttpDBase` typedef gated on a new
   `WEB_HAVE_DB` preprocessor define (set by CMake iff `WEB_WITH_DB` is
   `ON`) - mirrors classic `src/httpd/httpd.h`'s own `#ifdef
   RTS2_HAVE_PGSQL` split exactly, just resolved at CMake configure time
   instead of the preprocessor picking between two hand-maintained
   branches. `DeviceDb::init()` already calls `Device::init()` internally
   and then connects to the database (`--database`, defaulting to
   `"stars"` - already matches this session's local test DB name) - no
   separate connection step needed in `HttpD::init()`, just calling
   `HttpDBase::init()` instead of `Device::init()` was enough.

   **New `web/httpd/include/dbendpoints.h` + `src/dbendpoints.cpp`**
   (compiled only when `WEB_HAVE_DB`), reusing `db`'s already-ported
   `rts2db` classes rather than writing raw SQL:
   - `GET /api/db/targets` - every target (id, name, type, current
     ra/dec), via `rts2db::TargetSet` - exactly mirrors `db/db/tools/
     targetlist.cpp`'s own plain-listing call shape.
   - `GET /api/db/target?id=N` - one target's full detail (adds
     comment), via the free function `createTarget()` (found it's
     declared at *global* scope in `target.h`, not inside `namespace
     rts2db` despite returning `rts2db::Target*` - first build attempt
     got this wrong, fixed once the compiler pointed it out).
   - `GET /api/db/observations?id=N` - a target's observation history,
     via `rts2db::ObservationSet::loadTarget()`. Explicitly confirms the
     target itself exists first (a second `createTarget()` call) so "no
     observations yet" and "target doesn't exist" give different,
     correct responses instead of both silently returning `[]`.
   - `rts2db::SqlError`/`rts2core::Error` (thrown by `createTarget()` for
     a bad id) is caught in `httpd.cpp`'s `handleRequest()` alongside the
     existing `ApiError`, same 400-with-JSON-body treatment - "don't
     crash on unexpected/bad input" now covers the DB layer too, not just
     `checkPreviewCache()`/`UserLogins::load()`.

   **A real crash found and fixed by live testing, not by review**:
   the very first request to `/api/db/targets` against the real `stars`
   database segfaulted the daemon. Reproduced under `gdb` (`-batch -ex
   run -ex "thread apply all bt full"`) rather than guessing from source
   reading - the backtrace pinpointed it exactly: `rts2web::jsonEscape
   (s=0x0, ...)` dereferencing a null pointer, called from `dbListTargets`
   -> `jsonString(tar->getTargetName(), os)` for target ID 6, the
   database's "Master calibration target" singleton. Root-caused from
   there: `CalibrationTarget::load()` (`db/db/src/sub_targets.ec`) is a
   deliberately different code path from every other target type - for
   `TARGET_CALIBRATION`'s own ID it searches the DB for *other*
   calibration-capable targets to use as sources, and never calls
   `ConstTarget::load()`/`setTargetName()` on itself, so
   `getTargetName()` legitimately returns `nullptr` for this one specific
   target - not a data-quality problem in the test DB, confirmed real
   RTS2 behavior. The actual bug was entirely on this side: `jsonEscape`/
   `jsonString` (`jsonvalue.cpp`, used everywhere in `web`, not just this
   new endpoint) assumed every `const char*` they're handed is
   non-null, which is only true for `std::string`-backed accessors -
   `rts2db::Target`'s name is backed by a raw `char*` that starts and can
   stay `nullptr`. Fixed at that one shared layer (`jsonEscape` now
   treats `s == nullptr` as `""`) rather than patching the one call site,
   so every current and future caller across the codebase is protected,
   not just `dbListTargets`.

   **Second, smaller correctness fix found the same way**: `Dark
   frames`' (target 1, a target type with no fixed position) `ra`/`dec`
   came back as the literal text `nan` in the JSON body - not a crash,
   but not valid JSON either (`nan`/`inf` aren't legal JSON tokens;
   `JSON.parse` and most strict parsers reject them outright). Factored
   the NaN-aware handling `jsonValue()` already had inline for
   `RTS2_VALUE_DOUBLE` out into a new shared `jsonNumber()` helper
   (`jsonvalue.h`/`.cpp`) and used it for `ra`/`dec` in both DB
   endpoints - `nan` now correctly becomes JSON `null`.

   **Smoke-tested for real** against the live local `stars` database
   (real schema from the July bootstrap session, connected as the
   `mates` OS user via Postgres peer auth - no `sudo` needed, per this
   session's `db/STATUS.md` access-path note) and the same live
   `centrald`/`camd-dummy` rig from earlier tasks, all at once:
   - `/api/db/targets`: 120 of the database's 121 target rows returned as
     valid JSON (checked by actually parsing the response with Python's
     `json.load`, not just eyeballing it) - target 6 (calibration
     singleton) present with `"name":""` as expected post-fix, real solar-
     system ephemeris (Sun/Mercury/.../Pluto, live-computed RA/Dec) and
     all 103 Stetson standard fields present. The one row *not* returned
     (target 7, "Master plan", type `p`) is correctly absent, not lost
     data - `TYPE_PLAN` targets are a known, deliberate gap in `db`'s own
     port (the classic Plan-scheduling feature was never carried over,
     `createTarget()` throws for it and `TargetSet::load()` already
     catches that per-target and skips it, not something `web` needed to
     handle specially).
   - `/api/db/target?id=6` and `?id=2` (a real flat-field target) both
     `200` with correct detail; `?id=999999` (nonexistent) correctly
     `400` with the underlying `SqlError` text, not a crash.
   - `/api/db/observations?id=2` returned real data, not just the empty
     case - `[{"id":1,"start":...,"end":...}]`, an actual observation
     record left over from the live `rts2-executor` run verified in
     `db/STATUS.md`'s 2026-07-19 session (target 2 is "Flat frames",
     exactly what that run executed). `?id=999999` correctly `400`s
     instead of returning a misleading empty array.
   - Missing `id` parameter correctly `400`s (`ApiError`, not a crash).
   - The daemon stayed fully responsive to `/api/devices` (bus-side)
     throughout every one of the above, including right after the crash-
     and-fix cycle.

   Not yet done, left for later: night reports and image *search* (as
   opposed to `/preview/`'s raw path-based lookup from task 3) - both
   need data this sparse test database doesn't have (no real observation
   nights, no archived images with DB rows) to test against meaningfully;
   and the DB-connection availability state machine from "Runtime DB
   availability" above (this slice assumes the DB is reachable for the
   whole process lifetime, matching how it was actually tested).

### DB queries moved to the worker pool - measured live against real production data (2026-08-17)

The "worth measuring before deciding, not assuming" flag on DB-query
threading (task 7's first write-up, above) got its measurement: tested
against `lascaux.asu.cas.cz`, a live production telescope-control host
(see "Live production testing (lascaux.asu.cas.cz)" below for the full
access/safety story), whose real `stars` database has 15,753 targets -
not the 121-row local test DB task 7 was originally built and verified
against.

`/api/db/targets` against that real data took **~14.7 seconds** for a
single request. Confirmed this wasn't just "one slow endpoint" but
actually blocked the whole daemon: fired `/api/devices` (an otherwise
sub-millisecond, purely in-memory bus-side endpoint, extensively proven
fast under load back in task 5) from a second connection while the slow
query was still running - it *also* took ~14 seconds. `TargetSet::
load()` running synchronously on `Block`'s single poll-loop thread meant
one DB query froze live device/bus traffic for its entire duration - a
real operational problem for a telescope-control daemon, not a
theoretical one.

**Fixed**: moved `/api/db/targets`, `/api/db/target`, `/api/db/
observations` onto the same `workerPool` already built for cache-miss
preview generation (task 5) - identical `MHD_suspend_connection()`/
`MHD_resume_connection()` pattern, identical eventfd wakeup, sharing the
pool rather than standing up a second one (both are occasional, bursty
background work; not worth a dedicated pool each until real contention
between the two shows up as an actual problem). `rts2core::Error`
(`SqlError` for a bad target id, or any other DB-layer failure) is now
caught *inside* the worker job and turned into the JSON error body
there, since it can no longer propagate to the main thread's
`try`/`catch` the way the synchronous version did.

**Re-verified against the same real production database after the
fix**: the query itself still takes ~14.7s (that's real, inherent
Postgres query cost against 15,753 rows via `rts2db`'s existing
per-target-row `createTarget()` loop - not something this fix was meant
to address, and not addressed here), but `/api/devices` fired
concurrently while it's in flight now returns in **~0.28 milliseconds**
- completely unblocked, matching task 5's preview-generation result
exactly. Confirmed production stayed untouched throughout both rounds of
testing: same process count before/after, the real classic `HTTPD`
(port 8889) still responding, and `SELECT count(*) FROM targets` still
`15753` both times (zero writes, as designed - these endpoints are
read-only and this session never touched `/api/set,inc,dec` against
lascaux at all).

Actually fixing the *query* being slow (e.g., avoiding `TargetSet::
load()`'s one-`createTarget()`-call-per-row pattern for a bulk listing)
is a separate, not-yet-investigated question - today's fix is "a slow
query can no longer block the daemon," not "make the query fast."
Worth a real look before `/api/db/targets` becomes something an actual
site UI calls routinely against a large database.

### Live production testing (lascaux.asu.cas.cz, 2026-08-17)

First time anything in this port has been tested against a real,
currently-in-use production telescope-control host rather than this
laptop's local rig or a disposable test database - handled deliberately
cautiously throughout, per explicit user direction ("the tests have to
be very careful, it is a live production machine"), not just building
and running things there the way earlier tasks did locally.

**Read-only recon before touching anything**: confirmed via SSH what's
actually running - a fully live D50-class setup (real `centrald`, three
real cameras `C1`/`C2`/`C3`, mount, dome, focusers, weather/rain
sensors, `rts2-executor`, `rts2-imgproc`, classic `rts2-httpd` already
on port 8889, plus a large number of `rtspy-queue-sel` processes - the
Python queuer effort). `~/rts2ng` was already checked out there (a real
`git@github.com:mates14/rts2ng.git` remote, tracked history - this
session's local checkout shares the same remote), behind by several
commits including everything from today. The `mates` OS user already
had a Postgres role with full read access to the real `stars` database
(15,753 targets - genuinely the live dataset, confirmed, not a copy).

**Getting the code there**: asked the user how (rather than assuming) -
chose commit + push + pull over a direct copy, to match the project's
normal workflow and leave a real history entry. Staged carefully: `git
add -n` first caught that `web/build-nodb/` (this session's local
regression-test build directory) wasn't covered by `.gitignore`'s
`build/` pattern and would have been committed wholesale, plus two
stray `.STATUS.md.swp`/`.swo` vim swap files - deleted both before
staging, not silently included.

**A real, unpredicted portability gap, found immediately**:
lascaux's system `libmicrohttpd` is 0.9.75 - the dev machine this was
all built against has 1.0.1. `MHD_basic_auth_get_username_password3()`
(task 6's auth code) didn't exist before 0.9.77; `MHD_OPTION_SOCK_ADDR_LEN`
needed for clean interface binding didn't exist before 0.9.77.06. Both
replaced with their older, still-present-but-deprecated equivalents
(`MHD_basic_auth_get_username_password()`, `MHD_OPTION_SOCK_ADDR`) -
verified locally first that nothing regressed, then confirmed the build
succeeded on lascaux itself. A concrete reminder that "compiles and
works on the dev machine" and "compiles and works on whatever
libmicrohttpd version a real site actually has installed" are different
claims - this project has only verified the first one so far, at any
real site, for any of its dependencies.

**New `--bind-address` option**, added specifically because of this
testing, not a preexisting feature: since `--auth-file` is off by
default, running this daemon on a shared production host would
otherwise mean an unauthenticated, write-capable HTTP port bound to
*every* interface for the test's duration, on a machine with no
per-daemon firewalling to fall back on (confirmed - no passwordless
`sudo` for even a read-only `iptables -L` check). Implemented, verified
locally (`ss -ltnp` shows the socket bound to `127.0.0.1:<port>`, not
`0.0.0.0:<port>`, and still fully functional via that address) before
ever using it against lascaux. This is also just a genuinely useful
feature independent of the testing need - it lets the "fronted by a
proxy, never reachable directly" deployment model (an early design
decision) actually be enforced at the socket level.

**The test methodology itself**: built in `~/rts2ng/web` (nesting the
already-checked-out `db`/`base` there, same as locally), ran with a
distinct device name (`-d HTTPDNG`, avoiding a collision with the real
`HTTPD` already registered on the bus), distinct HTTP and bus ports
(`28889`/`28701`, both confirmed free first), `--bind-address 127.0.0.1`,
and `--database stars` pointed at the real production database -
deliberately never touching `/api/set`, `/api/inc`, `/api/dec` against
it, since task 7 is read-only by design and there was no reason to risk
commanding real hardware to prove a database-query endpoint works.
Verified `/api/devices` showed the full real device roster (`C1`/`C2`/
`C3`/`T0`/`DOME`/`EXEC`/the real `HTTPD`/...) coexisting cleanly with
the test instance, confirming the bus layer (tasks 1-2) works correctly
against a real, complex, multi-device production deployment, not just
this session's minimal local rig. After every round of testing:
explicitly killed the test process, confirmed via a fresh `pgrep` (not
just trusting the kill command's exit status) that nothing was left
running, and confirmed the real production `HTTPD`, the real process
count, and the real database row count were all unchanged.

This is also where the DB-worker-pool finding above actually came from -
see that section for what the real dataset exposed that the local test
database was too small to reveal.
   `style.css`/`app.js`, real files on disk, no CDN dependency, no build
   step) plus daemon-side serving. Verified in an actual headless
   browser via Chrome's DevTools Protocol, not just curl/code review -
   see below.

   **Design reference, not a literal port**: the user pointed at an
   existing hand-built D50 control page (`~/rts2/webmon/monitor.html`,
   822 lines) as prior art worth learning from. It polls 13 devices
   sequentially every 10s (`await`ed one at a time in a loop against
   classic's `/api/get?d=`), embeds plaintext Basic Auth credentials
   directly in the page's JS source, hardcodes D50's exact device
   roster and a dozen Lascaux-specific external image/webcam/graph URLs,
   and has three duplicate copies of the same init/interval code from
   accumulated edits. None of that is right for `web/static/`, which has
   to work for any deployment, not just D50 - confirmed with the user
   (generic, device-agnostic dashboard, not a reskin) before writing
   anything. What's actually genuinely reusable from it: the idea of a
   single status+control dashboard at all, and a concrete list of what a
   real observatory operator wants to see at a glance.

   **What changes architecturally, not just cosmetically**, versus that
   reference page:
   - **Push replaces polling entirely** for values/state: one `/ws`
     connection instead of 13 sequential per-device HTTP requests every
     10 seconds. This was the actual point of building task 4 - this is
     the first thing in the whole project that exercises it as a real
     consumer rather than a synthetic test client.
   - **No embedded credentials.** Since reads are unauthenticated
     (task 6's design) and only the command panel's `/api/set,inc,dec`
     calls can ever hit a 401, there's nothing to embed - the browser's
     own native `WWW-Authenticate: Basic` prompt handles it exactly
     once per session, and it's not in this code at all.
   - **Device-agnostic rendering**: devices and their values come
     entirely from `/api/devices`/`/api/getall` at load time and `/ws`
     deltas after that - no hardcoded device names or value lists
     anywhere in `app.js`. Whatever's actually connected to whatever
     `rts2ng` deployment this is pointed at is what renders.
   - **A generic "Send command" panel** (device dropdown + variable +
     value + Set/Inc/Dec) replaces the reference page's one hardcoded
     light-toggle button - demonstrates the same write path
     (`/api/set` etc.) without assuming any specific device/command
     exists.
   - **No image panels, no webcams, no external site links** - the
     "latest camera image" panel in the reference turned out to come
     from a *separate* external cron-style process that isn't part of
     `rts2-httpd` at all (see the write-up above); nothing in `web` can
     answer "what's the latest image for device X" yet (that's the
     deferred image-search half of task 7), so it isn't faked with a
     placeholder - left out entirely until it's real.

   **Live message push added along the way, not originally planned**:
   `HttpD::message()` (task 2) only appended to the local ring buffer
   before this; while wiring the dashboard's message panel it became
   clear that leaving messages polling-only while everything else
   pushes would be an inconsistent, worse version of the same page -
   added `HttpD::broadcastMessage()` (mirrors `broadcastValue`/
   `broadcastState` exactly) and a `"event":"message"` case in `app.js`,
   so the message panel updates live too. `/api/messages` is still used
   once at load for backlog.

   **Daemon-side serving** (`HttpD::handleStatic()`, new `--static-dir`
   CLI option, empty/unset = disabled - same convention as `--images-
   dir`/`--auth-file`): `MHD_create_response_from_fd()` for zero-copy
   file serving, `/` mapped to `<static-dir>/index.html`, the same
   two-layer path-safety check as `preview.cpp`'s (`..`-segment
   rejection, then `std::filesystem::weakly_canonical` confirming the
   resolved path is still inside `staticDir`) - reused the identical
   pattern rather than inventing a second one. A minimal extension-to-
   Content-Type table (html/css/js/json/svg/png/ico) - not a general
   MIME database, just what this tree currently ships.

   **Smoke-tested for real in an actual browser**, not just `curl`:
   this session's sandbox has no `claude-in-chrome` extension available,
   so verification used `google-chrome`'s own headless mode directly -
   first a one-shot `--screenshot` (caught one real sandbox quirk along
   the way: `localhost` resolves to `::1` first here and hung
   indefinitely against an IPv4-only listener, `127.0.0.1` worked -
   noted as a sandbox-specific finding, not a server bug, since a normal
   host's IPv6 loopback works), then a persistent session driven over
   Chrome's DevTools Protocol (raw CDP JSON-RPC over the `websockets`
   library already used elsewhere this session, no Puppeteer/Playwright
   needed) to prove the actual live-push behavior, not just that the
   page loads:
   - Confirmed `document.readyState === "complete"`, the connection
     badge reads "live" (not stuck on "connecting…"), real initial data
     rendered (e.g. `CCD_TEMP: 30.8155`, real D50 `latitude`/`longitude`
     from `centrald`'s own values).
   - **The actual proof of the push architecture**: with the page
     already loaded and its own `/ws` connection open, fired
     `curl .../api/set?d=C0&n=exposure&v=9.75` from a completely
     separate process (simulating another client or the device itself
     changing) - the already-rendered `exposure` cell in the live DOM
     updated to `9.75` with no page reload, no polling, confirmed both
     by reading `textContent` back over CDP and by an actual screenshot
     - and correctly got the `just-changed` CSS flash class applied.
   - Console checked for JS errors (`--enable-logging=stderr`) - none;
     only unrelated Chrome-internal GCM/sync noise from the profile.
   - Message panel populated (6 real messages from this session's
     testing).

   Not yet done, left for later: an example Apache/nginx front-end
   config (genuinely optional per the deployment-model decision, so not
   urgent); a device/value detail or history view; the image panel this
   session deliberately left out until task 7's image search exists;
   and a `WEB_HAVE_DB`-gated view for the new `/api/db/*` endpoints
   (target list/detail) - task 8 as built only renders bus-side
   device/value data, matching where the reference page's own focus
   was.
9. **DEFERRED, not scheduled** - Big Brother federation client, pending
   confirmation a real site still needs it.

## Conventions to follow (inherited from `base`/`db`/`gui`)

- C++17, `#pragma once`, `nullptr`, `<cstdint>`/`<cstring>` over C headers.
- Drop dead legacy-platform branches outright - modern Linux/glibc only.
- Headers/structure kept diffable against classic where a real port is
  happening (the JSON endpoint logic, `AsyncValueAPI` semantics); the
  networking/HTTP substrate underneath is new, not diffable against
  classic, and shouldn't pretend to be.
- Every deliberate drop/deviation gets written down here with the reason,
  same as `base`'s "base note:" comment convention - don't just say "some
  things were dropped," say exactly what and why.
