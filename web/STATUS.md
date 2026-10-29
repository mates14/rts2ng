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
  being dropped. `rts2-jsonclient` (written for this tree, see below) and
  the Python `rts2.rtsapi` client are the real client-side story.
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
  jsonclient/           # rts2-jsonclient, the CLI client for its API
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

**Second slice (2026-08-17)** - night reports and image search, the two
pieces deferred from the first slice because the local test DB had
nothing to exercise them against meaningfully. Both now exist:

- `GET /api/db/nights?year=&month=&day=` - hierarchical drill-down
  (year -> month -> day -> hour) via `rts2db::ObservationSetDate`,
  exactly the same aggregation classic's `Night::callAPI()` uses (same
  GROUP BY, same map-of-int-to-DateStatistics shape) - reused, not
  reimplemented. Unlike classic's tabular `{"h":[...],"d":[...]}` grid
  format (a UI-widget convention, not a clean API), this returns
  `{"level":"month","entries":[{"key":7,"observations":4,"images":49,
  "goodImages":0,"timeOnSky":980}]}` - self-describing, matching the
  plain-named-field style the first task-7 slice already established.
- `GET /api/db/night?year=Y&month=M&day=D` (all three required) - every
  observation during one specific night, boundaries computed by a
  ported `getNightDuration()` (file-local to `dbendpoints.cpp`, ~30
  lines, straight port of classic's `lib/rts2json/nightdur.cpp` -
  astronomical-night start via `Configuration::getNight()`, not
  calendar midnight).
- `GET /api/db/images?target=N` / `GET /api/db/images?year=&month=&day=`
  - every archived image of a target, or from one night, via
  `rts2db::ImageSetTarget`/`ImageSetDate` (already-ported, reused as-is).
  Each entry includes a `previewPath` computed by stripping `--images-
  dir`'s prefix off the DB's stored absolute path when it actually falls
  under it (empty string, not a guess, when it doesn't - e.g. images-dir
  not configured, or this site's archive is laid out differently) so a
  client can feed it straight to `/preview/<previewPath>` without the
  daemon needing to know or care how the frontend wants to construct
  that URL. Used `getFileName()` (raw stored path), not
  `getAbsoluteFileName()` - see task 3's write-up for why the latter is
  unreliable for a DB-sourced path in a daemon (resolves relative to
  `getcwd()` when the stored path isn't already absolute).

  All five endpoints (this slice's three plus the first slice's
  `targets`/`target`/`observations`) now go through `workerPool` from
  the moment they were written, not added synchronously and fixed
  later - directly applying the lesson the first slice's production
  testing surfaced.

  **Found and fixed a real memory-safety bug along the way, not
  hypothetical**: testing `/api/db/images` against this session's local
  test DB (real observations/images, but `img_path` rows pointing at
  files that don't exist at that path on this host - an entirely
  plausible situation for any real deployment too, e.g. an archive
  that's been reorganized or is mounted differently) produced responses
  with `targetName` fields full of garbage binary bytes instead of an
  empty string. Root cause: `Image::getTargetHeaders()`
  (`base/kernel/src/image.cpp`, a pre-existing method inherited
  unchanged from classic - confirmed identical in
  `~/rts2/lib/rts2json`'s equivalent) allocates `targetName = new
  char[FLEN_VALUE]` and only ever writes to it on a *successful* FITS
  header read; when the underlying file can't be opened at all (not
  just "keyword missing" - the file genuinely isn't there), the
  triggering `getValue()` call swallows its own exception and returns
  without touching the buffer, leaving `getTargetName()`'s
  `std::string(targetName)` to scan uninitialized heap memory for a
  stray null terminator. Fixed by zero-initializing the buffer
  immediately after allocation, before the fallible read - a one-line
  fix at the allocation site (chosen over fixing only in
  `dbendpoints.cpp`'s call site, same reasoning as the earlier
  `jsonEscape` null-safety fix: other raw-pointer-backed `Image`
  accessors could hit the same failure mode from any caller, not just
  this one). Verified both directions with a small standalone probe
  linked against `libbase_kernel.a`: a real openable FITS file still
  correctly returns its `OBJECT` header (`"flat target"`); a path that
  can't be opened now yields `""` through the real `getTargetHeaders()`
  catch path (confirmed via the live endpoint - 49 images from
  intentionally-unreachable paths, zero garbage, zero crashes) rather
  than the crash-under-a-debugger a *naive* reproduction gets (a
  standalone probe calling `openFile()` directly without the try/catch
  `getTargetHeaders()` wraps it in throws unhandled - that's a gap in
  the throwaway test harness, not evidence of a second bug in the real
  call path).

  Verified end-to-end against the local `stars` test DB (real night of
  2026-07-19/20, targets 268/283/2, 4 observations, 49 images):
  hierarchical `nights` drill-down at all three levels, full `night`
  detail, both `images` search modes, all the "missing required
  parameter"/"nonexistent target"/"nonexistent night" 400 paths, and a
  concurrent-request check (`/api/devices` alongside an in-flight
  `/api/db/images` call) confirming the worker-pool offload still holds
  for these three new endpoints exactly as it does for the first
  slice's. Both `WEB_WITH_DB=ON` and the `OFF` regression build compile
  clean. Not yet re-run against lascaux's production database - the
  first slice's lascaux round already validated the deployment
  methodology and the worker-pool architecture on real production data;
  these three endpoints reuse that exact same architecture and the
  already-ported `rts2db` classes, so a repeat full lascaux round
  wasn't judged necessary to write this up as done, but is still
  reasonable to do before calling task 7 fully closed out.

  **Two bugs found live during the lascaux trial (2026-08-18)**, both
  fixed and redeployed same session:
  - `/api/getall` produced invalid JSON against the real `EXEC` device:
    array-typed values (`rts2core::BoolArray`/`IntegerArray`/...) share
    their scalar counterpart's base type - the array bit lives in a
    separate ext-type mask `jsonValue()` never checked - so an
    `ExecutorQueue` `BoolArray` value (`*_hard`) fell into the scalar
    bool branch and its `getValue()` returned `""` for an array,
    producing a bare `"next_hard":,` that broke the whole response.
    `jsonvalue.cpp` now checks `getValueExtType() & RTS2_VALUE_ARRAY`
    first and renders a real JSON array. This gap was already flagged
    in `jsonvalue.h`'s own doc comment ("no array/stat/rectangle value
    rendering yet") - the local test rig never had a device using one,
    so it took real production traffic to trigger.
  - `/api/db/images` took 6+ seconds against a real 6585-image night
    and monopolized the shared worker pool, causing an unrelated
    concurrent request to 502 through Apache's proxy timeout. Root
    cause: `Image::getTargetName()` unconditionally opens the actual
    FITS file on disk when the image came from a DB row (`targetName`
    is left null by `setTargetHeaders()` - there's no name column on
    `images`, only `target_id`), so the per-image loop in
    `writeImageSetJson()` was one real file open per image.
    `dbSearchImagesByTarget`/`dbSearchImagesByNight` now resolve names
    once per unique `target_id` via a request-local cache instead - a
    night reuses a handful of targets across thousands of images, and
    the ID is already free from the DB row.

  **Also added `/api/db/current-night`** (2026-08-18) after the same
  trial showed the dashboard's default landing view - the unbounded
  `dbNightsSummary()` drill-down with no year/month/day - paying for a
  full-table aggregate over every observation/image ever recorded just
  to render the *first* screen. Returns `{"year":Y,"month":M,"day":D}`
  for tonight via `Configuration::getNight()` with no DB access at all
  (answered inline, not through the worker pool), so `app.js` can jump
  straight to that night's detail on load; the "all nights" breadcrumb
  still reaches the full historical drill-down as an explicit action.

  **A third, more serious bug found immediately after (2026-08-18)**:
  live production logs showed real libpq protocol corruption -
  `message contents do not agree with length in message type "T"`,
  `server sent data ("D" message) without prior row description`,
  `insufficient data in "T" message` - the daemon recovered via its own
  reconnect logic, but requests in flight during the corruption failed.
  Root cause: `rts2db`'s ECPG-generated queries run over a single
  implicit connection with no locking anywhere in the DB layer, but
  every `/api/db/*` endpoint runs on `workerPool`'s multiple threads -
  and the dashboard's own night-detail view fires `/api/db/night` and
  `/api/db/images` *concurrently* via `Promise.all`. Two DB-touching
  worker jobs racing on the one connection at once corrupts the wire
  protocol - not a hypothetical, it happened on the very first real
  concurrent page load once `current-night` made that page the default.
  Fixed with a single `std::mutex` (`dbAccessMutex`, file-local to
  `dbendpoints.cpp`) taken by every public `dbXxx()` function for its
  whole body - DB endpoints now queue up behind each other instead of
  running truly in parallel, but they were already serialized by the
  one physical connection regardless; the worker pool's job is keeping
  the main bus/WebSocket thread free, not DB-query parallelism. Preview
  generation (task 5) is unaffected - it never touches `rts2db` at all,
  and stays genuinely concurrent. Verified with 30 concurrent `/api/db/
  night`+`/api/db/images`+`/api/db/targets` requests locally (all 200,
  no protocol errors in the log) before redeploying to lascaux.

8. **DONE (2026-08-17)** - Generic device-agnostic web dashboard
   (`web/static/index.html`/`style.css`/`app.js`, real files on disk, no
   CDN dependency, no build
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

   Not yet done, left for later: a device/value detail or history view;
   a `WEB_HAVE_DB`-gated view for `/api/db/targets`/`target`
   (target list/detail) - the dashboard doesn't render those yet, only
   the nights/images panel below.

   **Images/nights panel added (2026-08-18)**, once task 7's night-
   reports/image-search endpoints existed to back it: a
   year -> month -> day drill-down (`/api/db/nights`) landing on a
   night's observation table (`/api/db/night`) and a thumbnail grid
   (`/api/db/images` + `/preview/<previewPath>`). `WEB_HAVE_DB=OFF`
   builds get a 404 on the first `/api/db/nights` call and the panel
   just hides itself rather than showing a permanently-broken UI.

   **Made proxy-subpath-safe while doing this**, prompted by planning a
   real test on lascaux: Apache's existing vhost there already has
   `ProxyPass /images http://localhost:8889` pointed at classic httpd
   (the *only* public route to it - confirmed via `ssh l`, see below),
   and `app.js` had been using absolute root paths (`fetch('/api/...')`,
   a `/ws`-rooted `WebSocket()` URL) that only work when the page is
   mounted at the server's actual root. Under a reverse-proxied
   subpath, those absolute paths get resolved by the *browser* against
   the site's real root, bypassing the proxy prefix entirely - Apache
   has no rule for bare `/api/...`, so every fetch would 404. Fixed by
   switching `fetch()` calls to plain relative paths (`api/getall`,
   not `/api/getall` - resolves correctly against whatever directory
   the page was actually loaded from) and computing the WebSocket URL
   from `location.pathname` instead of assuming `/ws` at the root
   (`new WebSocket()` needs a full URL, so it's the one case `fetch()`'s
   automatic relative resolution can't cover). Verified locally with a
   throwaway Python script reproducing Apache's exact prefix-stripping
   `ProxyPass` semantics (no `ProxyHTMLURLMap`, since lascaux's vhost
   doesn't have one either) in front of a real running `rts2-httpd` -
   `index.html`, `app.js`, `style.css`, `api/getall`, `api/db/nights`,
   and `preview/*` all resolved correctly through the simulated
   `/images` mount before this was ever tried against the real Apache
   config. One caveat this only mitigates, doesn't eliminate: a client
   that opens the mount path *without* a trailing slash
   (`https://host/images`, not `/images/`) still gets relative-path
   resolution wrong, same as any web server's directory URLs - not
   fixed here (would need the daemon to know it's mounted at a
   sub-path, which it currently has no way to know), so this only works
   reliably when entered with the trailing slash.

   **Recon on lascaux for a real (reversible) production trial**: the
   real `rts2-httpd` runs under `rts2.service` (`rts2-start`/`rts2-stop
   all`, PID-file-based per-service control - `rts2-stop httpd` alone
   stops just it), as user `rts2`, bound to `localhost:8889` only,
   invoked as `-d HTTPD --event-file /etc/rts2/events --run-as rts2
   --server localhost` (built from `/etc/rts2/services`). Apache 2.4.68
   (`proxy`/`proxy_http` only, no separate `wstunnel` module - fine,
   `mod_proxy_http` has forwarded `Upgrade` requests transparently since
   2.4.47, well below this version, so `/images/ws` should Just Work
   through the existing rule with no Apache config change at all).
   `/etc/rts2/rts2.ini`'s `archive_path = "/images/%Y/%N/%c/%t/%f"` and
   the real `/images` (a symlink to `/bart/images`) confirm `--images-
   dir /images` is the right value - and that the DB's stored
   `img_path` convention this session's local test DB already used
   (`/images/<night>/...`) matches real production, not a made-up
   layout. Database name is `stars` in both places too. Confirmed (via
   a harmless `kill -0`, not a real attempt) that the `mates` recon
   account cannot signal the real httpd process (owned by user `rts2`)
   even though it's a group member - stopping/restarting it needs root
   or the `rts2` account, which past sessions established the user
   handles themselves on this host rather than delegating.

   Also flagged, not yet resolved: `HttpD::checkWriteAuth()` returns
   `true` unconditionally when `--auth-file` isn't set (`web/httpd/src/
   httpd.cpp:800`) - fine for the sandbox/local-test-DB rounds so far,
   but running *this* build reachable from the public internet via
   Apache without an auth file would mean `/api/set,inc,dec` against
   real hardware with zero authentication for as long as the trial
   runs. Needs a real `--auth-file` (chosen/typed by the user, not
   generated by the assistant) before any lascaux trial that leaves the
   daemon reachable through the public proxy, even a short one.
9. **DEFERRED, not scheduled** - Big Brother federation client, pending
   confirmation a real site still needs it.
10. **DONE, phase 1 of 2 (2026-08-26)** - Target/scheduling *write* path -
    everything task 7 built was read-only until now. User request: a web
    page to edit any property of an existing target (equatorial or MPEC/
    elliptical - Alt/Az deferred, see below) plus the site's separate
    `scheduling` table, eventually across both D50's and SBT's independent
    databases. Scoped to single-DB first (user's explicit choice - see
    the phase-2 note below) so the write path itself gets proven correct
    before the dual-ECPG-connection plumbing is added on top of it.

    **Investigated before writing anything** (this took real digging, not
    assumption): read `db/db`'s `Target`/`ConstTarget`/`EllTarget` save
    paths, the real production schema on lascaux (`\d scheduling`,
    `\d targets` via `ssh l`), and the user's own Python scheduler
    (`sch/database.py`, `request.py`, `scheduler_core.py`,
    `kernel/reservation.py`) to find out what already exists vs. what's
    still just an idea:
    - `tar_telescope_mode` (schema since `rel_0_9_5.sql`) is loaded/saved
      but genuinely unused by the scheduler - "any/specific telescope" is
      entirely implied by per-DB `tar_enabled`, not a stored mode.
    - The real "simultaneous" mechanism is `scheduling.sinfo`'s `type=`
      keyword feeding `kernel/reservation.py`'s `CompoundReservation`
      (`single`/`oneof`/`and`/`sim`). Only `type=and` is actually wired
      end to end (`request.py`'s `has_and_type()` ->
      `scheduler_core.py`'s `cr_type='and'`) - `sim` ("starting together",
      what the user meant by "simultaneous") is solver-supported but
      nothing in `sch/` translates a sinfo keyword into it yet. This
      editor lets `sinfo`'s `type=` be set to anything including `sim`,
      but making `sim` actually take effect needs a `sch/` change that's
      out of this pass's scope.
    - `TYPE_TERESTIAL` (the closest existing thing to "Alt/Az terrestrial")
      is legacy HAM/FRAM-specific and is just a fixed-RA/Dec `ConstTarget`
      - it does not track a fixed Alt/Az position over time. Real Alt/Az
      support needs a new `getPosition()` override doing the hrz->equ
      conversion each call - deferred (user's explicit choice); this pass
      only ships equatorial + MPEC/elliptical editing.
    - `scheduling(tar_id, sinfo)` already exists on real production DBs,
      added directly to production SQL, never through a versioned rts2ng
      migration - and has no primary/unique key on `tar_id` there. Given
      a proper migration now (`db/sql/update/rel_1_0_2.sql`, also added to
      `db/sql/create/tables.sql` for fresh installs) with a primary key
      this time; the write path (`rts2db::Scheduling::setSinfo()`,
      `db/db/src/scheduling.ec`) is deliberately delete-then-insert in one
      transaction rather than UPDATE-or-INSERT so it's correct either way
      - collapsing to exactly one row - against production's unconstrained
      copy too, not just a fresh schema.

    **Real bugs found in `db/db`'s existing (pre-`web`) code while wiring
    this up, not hypothetical**:
    - `interruptible` (schema since `rel_0_8_1.sql`) was never read or
      written by `rts2db` at all - `Target::loadTarget()`'s SELECT simply
      omitted it, `saveWithID()` too. Added a real
      `getInterruptible()`/`setInterruptible()` pair and wired both the
      load and save paths (`target.h`/`target.ec`).
    - `tar_comment` is even worse: `saveWithID()` writes it, but
      `loadTarget()`'s SELECT omits it - so a comment set via any existing
      classic tool round-tripped fine at the DB level but every C++ reader
      of a freshly-loaded `Target` always saw `nullptr`. Found live: a
      comment set through this session's own new endpoint round-tripped
      into Postgres correctly but came back empty from the very next GET.
      Fixed at `loadTarget()` (now selects and populates `target_comment`,
      `nullptr`-preserving for a real SQL NULL, matching every existing
      `comment ? comment : ""` call site's expectation).
    - `setTargetComment()` freed `target_comment` with plain `delete`
      against a `new char[]` allocation - a `new[]`/`delete` mismatch
      (undefined behaviour), inconsistent with the destructor's own
      `delete[]` for the same pointer. Fixed to `delete[]`.
    - `Target::saveWithID()`'s name/comment handling did an unbounded
      `strcpy()` into a fixed-size `VARCHAR[150]`/`VARCHAR[2000]` ecpg host
      buffer with no length check - harmless while every caller was
      internal/trusted, but a real stack-buffer-overflow risk now that
      `/api/db/target-save` lets an HTTP client set these directly.
      Clamped both (matching the truncation guard `tar_info` already had),
      and added an explicit length check at the `dbUpdateTarget()` API
      layer so an over-length value gets a clean 400 instead of silent
      truncation.
    - `rts2db::SqlError`'s default constructor issues its own
      `EXEC SQL ROLLBACK` (after capturing `sqlca`'s message/code into the
      exception) - this session's first draft of `scheduling.ec` also
      rolled back explicitly *before* `throw SqlError()`, which wiped
      `sqlca` with the rollback's own (successful, empty) result before
      `SqlError()` ever read it, turning every real failure into a
      content-free `"error:  (#0)"`. Found by testing the literal failure
      path live (a first `scheduling-save` attempt), not by inspection -
      fixed by not rolling back before constructing `SqlError()`. Also
      found live: ecpg reports a 0-row `DELETE` the same way it reports a
      singleton `SELECT INTO` matching no row (`ECPG_NOT_FOUND`/sqlcode
      100) - the expected case on a target's first scheduling save, not a
      real error; `setSinfo()`'s delete-then-insert now treats that
      specifically as non-fatal.

    **New endpoints** (`web/httpd/include/dbendpoints.h`+`.cpp`, routed in
    `httpd.cpp`'s `handleDb()` on the same worker-pool/suspend-resume
    pattern every other DB endpoint already uses): `POST /api/db/
    target-save?id=N&...` (partial update - a field's absent from the
    query string means "leave it alone", not "clear it"; see
    `TargetUpdate`'s doc comment for exactly which fields apply to which
    target type and why type itself isn't editable yet), `GET`/`POST
    /api/db/scheduling` / `/api/db/scheduling-save?id=N&sinfo=...`. Gated
    by the same `checkWriteAuth()`/`--auth-file` mechanism as `/api/set`
    (task 6) under a new `"db-targets"` permission token. `dbGetTarget()`'s
    JSON grew `info`/`priority`/`bonus`/`enabled`/`interruptible`/
    `isElliptical`/`hasPosition`/`pmRa`/`pmDec` alongside what task 7
    already sent - `hasPosition`/`isElliptical` exist specifically so the
    frontend doesn't need its own copy of "which type_id chars mean
    what".

    **A second real bug found by this session's own browser testing, in
    already-shipped task-8 code**: `handleStatic()` used
    `MHD_create_response_from_fd()` (zero-copy) for every static file;
    turns out *any* static asset at or above 4096 bytes hung indefinitely
    past its first 4096-byte chunk under this daemon's external-polling
    `Block`+`libmicrohttpd` integration (confirmed with `curl --max-time
    30` genuinely never completing, not just being slow). Reproduced with
    the pre-existing `app.js` (14570 bytes) too, not just this session's
    new `target.js` - meaning this has apparently always been broken for
    any client that actually blocks on the full body; not chased into
    *why* libmicrohttpd's internal epoll write-continuation doesn't
    reliably re-signal `mhdEpollFd` in this integration, since every other
    response in this file already uses `MHD_create_response_from_buffer`
    successfully - switched `handleStatic()` to match (read the whole
    file, buffer it) rather than root-causing the zero-copy path. Verified
    fixed: `app.js`/`target.js` now serve byte-identical to their on-disk
    copies via `diff`.

    **Smoke-tested end to end** against the local `stars` test DB (121
    targets) - a real, not synthetic, exercise of every new code path:
    - `target-save` on a real Landolt-standard (`ConstTarget`) row: name/
      comment/priority/interruptible/ra/pm_ra all round-tripped correctly
      through a partial update (only `ra`/`pm_ra` sent - `dec`/`pm_dec`
      correctly left at their prior stored values, via the new
      `ConstTarget::getRawPosition()` - deliberately *not*
      `getPosition()`, which would have applied proper-motion correction
      and drifted the stored value on every save).
    - A scratch `TYPE_ELLIPTICAL` target (inserted directly, a
      hand-verified valid MPC one-line element for "Ceres") correctly
      loaded (`isElliptical:true`, `hasPosition:false`, real computed RA/
      Dec via `EllTarget::getPosition()`'s orbit propagation) and a
      `mpec=`-only update to a different valid line (for "Pallas")
      correctly re-derived the name (`placeholder` -> `Pallas`) and orbit
      in one call, alongside an unrelated `priority` change in the same
      request.
    - Cross-type rejection verified both directions: `ra=` against the
      elliptical target and `mpec=` against the `ConstTarget` both
      correctly 400 with a clear message, neither touched the DB.
    - `scheduling-save`/`scheduling` get/set/upsert-collapse (two saves
      in a row leave exactly one row), unauthenticated-write 401,
      nonexistent-target 400 on both endpoints, oversized-name 400.
    - The new `web/static/target.html`/`target.js` editor page itself,
      real-browser-tested (headless `google-chrome` + raw CDP, this
      sandbox has no `claude-in-chrome` extension available - same
      fallback task 8 used): loads a target via `?id=` or the form,
      renders the right fields for its type, submits a real save via
      `form.requestSubmit()`, confirmed in Postgres afterward. One
      false alarm chased down and ruled out during this: RA/Dec displayed
      with a comma (`20,9208`) on this Czech-locale (`cs_CZ.UTF-8`) test
      machine - confirmed via CDP (`element.value` is `"20.9208"`, period-
      based, and the actual saved DB value was unchanged/correct) that
      this is `<input type="number">`'s locale-aware *display* only, not
      a real data bug.
    - All test data cleaned up afterward (scratch elliptical target
      deleted, `scheduling` table emptied, target 202's edited fields
      restored to their original values) - confirmed via a final DB
      query, not just assumed.

    **Not yet done**: creating new targets via `web` (this pass is
    edit-only - `rts2-addtarget`/`rtspy.cli.addtarget` remain the
    creation path), and Alt/Az terrestrial targets (needs new
    `getPosition()` logic in `db/db`, not just a `web` change - see
    above).

    **Phase 2, DONE (2026-08-26) - D50/SBT peer sync, entirely
    client-side, not the dual-ECPG-connection design sketched above
    when phase 1 landed.** User's own call, and the right one: since
    "a desync makes no true harm" here (no atomicity/safety requirement),
    routing sync through a *second daemon's own DB connection* - i.e.
    the peer's `rts2-httpd` doing its own local write via the exact
    `target-save`/`scheduling-save` endpoints phase 1 already built and
    tested - is strictly simpler than teaching one process to hold two
    live ECPG connections (`EXEC SQL SET CONNECTION` swapping bracketed
    by `dbAccessMutex`, teaching every call site which connection it's
    on). **Neither daemon knows the other exists** - zero changes to
    `db/db` or `dbendpoints.cpp` for this phase; it's entirely new
    browser-side logic in `web/static/target.js`, gated behind an
    off-by-default, per-browser (`localStorage`) "peer proxy path"
    setting that most deployments will never touch.

    **CORS vs. same-origin proxy, decided in favour of the latter**: a
    page loaded from one telescope's origin fetching the other's API is
    cross-origin. Considered adding CORS headers (`Access-Control-Allow-
    Origin`/`-Credentials` + `OPTIONS`-preflight handling in `httpd.cpp`,
    needed because `Authorization` is a non-"simple" header) but rejected
    it: real, permanent surface area in the daemon for a feature only
    this site uses, versus reusing infrastructure that already exists
    and is already proven - `app.js`'s relative-fetch design was already
    built and tested (task 7/8) specifically to work correctly when
    mounted under an Apache `ProxyPass` subpath. User confirmed Apache
    is already configured this way on both real vhosts (`ProxyPass /d50
    http://d50.asu.cas.cz:8889` on lascaux's, `/sbt` -> lascaux
    symmetrically on d50's, matching `rts2-scheduler.cfg`'s own
    `d50`/`sbt` resource names) - `target.js` just needed to know the
    local peer-proxy path exists, nothing about the daemon changes.

    **Sync field list is a deliberate, user-specified split, not
    everything**: synced (both directions) - name, comment, priority,
    interruptible, position (ra/dec/pm) or the MPC line, and *only*
    `scheduling.sinfo`'s `type=` token. Never synced: `enabled` (that's
    what encodes which telescope(s) run the target at all), `bonus`/
    `bonus_time` (grounded in `create/tables.sql`'s own original
    comment on those columns: *"here start site dependent part - that
    depends highly on local object visibility"* - i.e. always meant to
    be per-site, not a guess made for this feature), and the rest of
    `sinfo` (`duration=`/`mag=`/`snr=`/`filters=`/`count=`/`pscale=` -
    user's call: a 50cm and a 20cm scope legitimately want different
    exposure parameters for the same target). `sinfo` sync merges just
    the `type=` key into whatever the peer's sinfo already has (parse,
    overwrite/delete one key, reserialize) - verified this leaves the
    peer's own `duration=`/etc. genuinely untouched, not overwritten.

    New "Peer telescope" panel in `target.html`/`target.js`: a
    same-origin-relative "peer proxy path" setting (`localStorage`,
    empty = feature fully inert - the panel still shows so it's
    discoverable, but does nothing until set), a comparison table
    (local vs. peer, per synced field, numeric fields compared with a
    1e-6 tolerance rather than exact float equality) with mismatched
    rows highlighted, and two explicit actions: **Push** (writes the
    locally-loaded/saved target's synced fields to the peer via its own
    `target-save`/`scheduling-save` - the peer's own auth gates it
    independently) and **Pull** (fills the local *form* from the peer's
    values - does not save; the operator still has to click the normal
    Save buttons to commit, same single write-path mental model as
    everywhere else on this page).

    **A real, unrelated CSS bug found while testing this** (not
    specific to the peer feature, but found because the peer buttons
    are the first `hidden`+flex-classed element exercised in the
    "should be hidden" state): `[hidden]` and an author class that sets
    its own `display` (`.cmd-buttons`/`.field-grid`/`.block-label`, all
    `display: flex/grid/block`) have *equal* CSS specificity - at a
    specificity tie an author-origin rule always wins over the
    browser's UA-stylesheet `[hidden]{display:none}`, regardless of
    source order, so any element combining `hidden` with one of those
    classes silently stayed visible. Confirmed this silently affected
    `#position-fields`/`#mpec-label` too (not just the new `#peer-
    buttons`) - i.e. a real, pre-existing bug in this session's own
    task-10-phase-1 work, just never noticed because every target
    tested so far happened to need those fields shown anyway. Fixed
    with the standard `[hidden] { display: none !important; }` override
    (same fix Bootstrap etc. ship for exactly this gotcha) rather than
    stripping `display` from the three classes, since more classes will
    likely combine with `hidden` later.

    **Smoke-tested end to end against two genuinely separate local
    databases** (not two connections to one DB - `stars` plus a real
    `pg_dump`/`pg_restore` copy, `stars_peer`, with deliberately
    diverged data), two scratch `rts2-httpd` instances, and headless
    Chrome driven over raw CDP (`--disable-web-security` standing in
    for what Apache's same-origin proxy provides in real deployment -
    the only thing that flag is asked to relax here). One real
    CDP-testing wrinkle worth recording: a cross-origin authenticated
    `fetch()` to the write endpoint hung forever in headless mode even
    with an explicit `Authorization` header - not a product bug, traced
    to headless Chrome having no UI to resolve the native HTTP Basic
    challenge; fixed the *test* by using CDP's `Fetch.enable
    ({handleAuthRequests:true})`/`Fetch.continueWithAuth` (the documented
    mechanism for exactly this), which unblocked it immediately and
    confirmed the real code path works. Verified: the compare table
    correctly flagged all real seeded differences (comment/priority/
    sinfo-type=, plus an incidental PM-null-vs-zero case) and nothing
    else; Push wrote the synced fields to the peer DB and merged only
    `type=` into its sinfo, leaving the peer's own `duration=`/`filters=`
    genuinely intact (checked via `psql` on both databases, not just
    the UI's own read-back); Pull filled the form correctly and
    provably did not touch the local DB until a separate explicit Save.
    All test artifacts (scratch `stars_peer` database, scratch
    daemons/centralds, seeded drift on `stars`) cleaned up and verified
    removed afterward.

    **Phase 3, DONE (2026-08-26) - per-camera script overrides, and the
    peer-sync UX redesigned again from "manual push/pull" to "always
    syncs".** Both driven by the user's own explicit design, not open
    questions this pass had to resolve itself.

    **Scripts** (`db/db/include/rts2db/targetscripts.h`+`.ec`, new
    `Target::deleteScript()` in target.h/target.ec, three new endpoints
    `GET /api/db/scripts` / `POST .../script-save` / `.../script-delete`,
    new "Scripts" panel in `target.html`/`target.js`): a target's
    per-camera observing-script override, editable and clearable. `Target::
    getScript()`/`setScript()` already existed (classic-derived, unchanged)
    but nothing could *list* which cameras have an override without
    already knowing their names, and nothing could *delete* one at all -
    `TargetScripts::listForTarget()` and `Target::deleteScript()` are both
    genuinely new. Deleting an override (the "[*] default" button) reverts
    the camera to its own device's configured default per `getScript()`'s
    existing `rts2.ini` fallback - the button doesn't invent that
    mechanism, it just clears the row that was overriding it. Camera list
    per site is a hardcoded `SITES` constant in `target.js` (D50: C0, C1;
    SBT: C1, C2, C3, per the user directly) - scripts are never synced
    between telescopes (they don't share cameras), so this is the one new
    panel with no cross-DB logic at all.

    **Real pre-existing bug found while wiring the DB layer for this**:
    `scripts(tar_id, camera_name)` has no unique constraint in
    `create/tables.sql` (same gap `scheduling` had) even though both real
    production DBs already carry one (`scripts_uniq_cam_tar`, most likely
    from `rts2-configdb`'s original bootstrap) - without it,
    `Target::setScript()`'s insert-then-update-on-failure upsert pattern
    silently degrades into "keeps inserting duplicate rows" on repeated
    edits of the same (target, camera). Added the constraint to
    `create/tables.sql` and a guarded `db/sql/update/rel_1_0_3.sql` (a
    no-op where it already exists, e.g. real production) rather than
    leaving fresh installs exposed to it.

    **Peer sync redesigned**: phase 2's manual Push/Pull buttons are gone.
    User's framing - "a smart tool that always syncs the given fields,
    and actually handles the state of synchronisation" - meant three
    concrete behaviour changes, all in `target.js` only (still zero
    daemon changes, still entirely browser-side):
    - **Site identity and peer path are now preconfigured defaults, not
      something the operator types in.** `SITES.sbt`/`SITES.d50` give
      each a sensible peer proxy path (`/d50`/`/sbt`); `detectSite()`
      guesses from `location.hostname` (containing "d50" -> D50, else
      SBT - matches the real `d50.asu.cas.cz`/`lascaux.asu.cas.cz`
      hostnames) and both remain overridable via a de-emphasized
      `<details>` settings widget for the rare case the guess is wrong.
    - **Auto-fill runs on every load, unconditionally, for the synced
      field set** (name/comment/priority/interruptible/position-or-mpec,
      plus sinfo's `type=` token): whichever side is missing a value
      gets it written immediately from the side that has it - no button,
      no confirmation, per "we can trust people... a desync makes no
      true harm". Verified this is a real write, not just a display
      change, by checking Postgres directly on both sides after a load.
    - **A field present-and-different on both sides is never
      auto-written** (the one case explicitly called out as not safe to
      resolve blindly) - **the SBT value is authoritative for display**:
      on the non-SBT instance, the local *form* (not the DB) is
      overridden to show SBT's value, flagged in a new sync log; on the
      SBT instance itself, nothing changes (it's already the source of
      truth) and the log just notes D50 wasn't touched. A subsequent
      Save (on either side, for any reason - a fresh edit or adopting a
      shown conflict value) now always also pushes the same synced-field
      set to the peer - verified with a real edit (renaming the target)
      that correctly landed on both databases after one Save, alongside
      a conflict-shown priority value that also committed to both sides
      only once Save was clicked, never before.
    - Comparison table's columns are now explicitly labeled "(SBT)"/
      "(D50)" rather than generic "Local"/"Peer", using `currentSite` to
      know which is which.

    **A real, if minor, validation gap fixed alongside this**:
    `dbUpdateTarget()` rejected `ra=`/`mpec=` sent to the wrong target
    type but had no matching rejection for a plain `info=` sent to an
    *elliptical* target - it would have called `setTargetInfo()` directly,
    silently bypassing `orbitFromMPC()` and decoupling the stored text
    from the orbit/name/type it's supposed to drive. Found while making
    sure the new auto-fill/push logic always routes an elliptical
    target's info through `mpec`, never `info` - closed the gap at the
    API layer itself (`dbendpoints.cpp`) so it's not just this page's
    JS that happens to avoid it.

    **Smoke-tested end to end**, same two-genuinely-separate-local-DBs-
    plus-headless-CDP methodology as phase 2, now modeling the real
    camera layout too (`stars` seeded as D50 with cameras C0/C1,
    `stars_peer` seeded as SBT with C1/C2/C3): scripts panel showed the
    right camera list per site and round-tripped a save/clear through
    real button clicks (not just the API) with a real DB write confirmed
    via `psql`; seeded a genuine three-way drift (comment/pm_ra missing
    on D50 only, priority conflicting on both) and confirmed after one
    page load that D50's comment/pm_ra were *actually written* to
    Postgres while its conflicting priority was *not* (form showed 9,
    database still had 5) and SBT's database was completely untouched
    throughout; confirmed the SBT-side load of the same target correctly
    left its own form alone and explicitly logged "D50 not touched"
    rather than adopting D50's differing value; a subsequent Save on the
    D50 page (with both the conflict-shown priority and a fresh,
    deliberate name edit) correctly landed both changes on *both*
    databases; `sinfo`'s `type=`-only merge re-verified in this new
    model, including that it correctly registered as unequal, non-`nan`
    RA/Dec drift once a proper-motion value from an earlier auto-fill
    step made the two sides' live-computed positions genuinely diverge
    by a small but real amount over the seconds between the two fetches
    - a real instance of exactly the "coordinates marginally differ"
    case the user flagged as acceptable to just treat as an ordinary
    conflict, not a hypothetical. All test data (target 202's edited
    fields, seeded scheduling/scripts/camera rows, the scratch
    `stars_peer` database) restored/removed and verified clean
    afterward.

    **Real-deployment finding (2026-08-26), both `/d50` (lascaux) and
    `/sbt` (d50) proxy paths**: went live returning a generic Apache
    "Internal Server Error" on both sides symmetrically. Root cause
    (confirmed by the user, not just guessed): `ProxyPass`/
    `ProxyPassReverse` to an `https://` upstream needs `SSLProxyEngine
    on` inside the *same* `<VirtualHost>` block - a separate switch from
    the server-side `mod_ssl` that terminates the incoming connection,
    easy to miss since neither vhost had it and the resulting 500 gives
    no hint what's actually wrong. Also worth recording since it cost
    real debugging time: `d50.asu.cas.cz`/`lascaux.asu.cas.cz` are each
    just aliases that redirect to a *different* canonical hostname
    (`lascaux50.asu.cas.cz` for D50), so a `ProxyPass` naively pointed at
    the alias would send the browser off-origin via that redirect,
    defeating the whole same-origin design - both real vhosts were
    already correctly written against the canonical names from the
    start (`ProxyPass /d50 https://lascaux50.asu.cas.cz/images`, and
    symmetrically `/sbt` -> `https://lascaux.asu.cas.cz/images`), this
    was a real gotcha checked for and confirmed already handled
    correctly, not one that needed fixing. Verified working end to end
    after the `SSLProxyEngine on` fix: `curl https://lascaux.asu.cas.cz/
    d50/api/devices` returns D50's real live device list through the
    full real proxy chain.

    **Phase 4, DONE (2026-08-26) - reframed the whole page around the
    user's own correction: "This is not SBT - this is SBT+D50 unified
    target database... except of course, that it is two databases that
    want to be maintained in sync."** Phases 2-3 modeled this as "I am
    one telescope, optionally synced with a peer" (a `currentTarget`/
    `peerTarget` split, "Local"/"Peer" table columns, a de-emphasized
    opt-in settings panel). That was the wrong mental model for a page
    whose *only* real users are people who think of D50+SBT as one
    facility - reworked around a `siteData = {d50, sbt}` structure with
    no privileged "local" side, and three purpose-named boxes instead of
    one generic target form:
    - **What** (`target-form`) - fields genuinely shared between both
      telescopes: name, comment, position-or-MPC-line, info. Exactly the
      phase 2/3 reconciliation algorithm (missing-on-one-side auto-fills
      immediately and for real - verified via `psql`, not just the UI;
      present-and-different-on-both is never auto-written, SBT's value
      is shown for review instead), just operating on `siteData.d50`/
      `.sbt` and feeding *one* form rather than two. Save now always
      writes to both telescopes that have the target at all - there's no
      more "save locally, then push" asymmetry, because there's no more
      "locally".
    - **How** - unchanged script-editing mechanics (STATUS.md phase 3),
      now showing both telescopes' camera groups (D50: C0/C1, SBT:
      C1/C2/C3 - 5 rows total) in the same box instead of only the
      locally-served telescope's cameras, fetching each group through
      `apiUrl(site, ...)`.
    - **When** - genuinely restructured, not just moved: priority and a
      new `duration=` (scheduling.sinfo's actual production-used key -
      confirmed by the user the other sinfo keys this session had
      speculatively supported, `mag=`/`snr=`/`filters=`/`count=`/
      `pscale=`, are unused in production and not worth keeping in the
      UI at all) are **per-telescope and deliberately never reconciled**
      - a checkbox + priority + duration row per telescope, matching
      exactly how `sch/scheduler_core.py` already treats them (its own
      priority×duration aperture-equalization logic assumes they
      legitimately differ per telescope). One shared "request type"
      dropdown (`oneof`/`and` labelled "All"/`sim`) writes `sinfo`'s
      `type=` key identically to both sides on save, merged in
      preserving each side's own `duration=` - verified via `psql` that
      a save changing D50's priority/duration and the shared request
      type left SBT's own priority/duration genuinely untouched while
      both sides picked up the same `type=`.
    - **Dropped entirely per explicit direction, not deferred**: `bonus`
      (internal, user doesn't need to see it), `interruptible` (never
      used in practice - everything is interruptible), every sinfo key
      except `duration=`/`type=`. None of these are hidden-but-still-
      wired - the frontend simply no longer reads or writes them, so an
      existing value in the database is left exactly as it was, never
      touched by this page again.
    - Layout: `.target-boxes` (new `style.css` rule) is a CSS grid,
      `repeat(auto-fit, minmax(340px, 1fr))` - three side by side on a
      wide screen, wrapping to 2+1 then 1-per-row on narrower ones, per
      the user's own description; built to accept a fourth
      scheduler-output box later without a layout rewrite.
    - The site-identity/peer-path settings survive (still needed - the
      page still only gets served by one of the two daemons and reaches
      the other through the same-origin proxy) but are now purely a
      fallback for when hostname auto-detection guesses wrong, tucked
      into one `<details>` at the very bottom of the page instead of
      being a named, prominent "peer sync" section - there is no more
      user-facing concept of "peer" at all, only "D50" and "SBT" shown
      as equal rows.

    **Smoke-tested end to end**, same two-genuinely-separate-local-DBs
    methodology, headless-Chrome/CDP with `Fetch.enable
    ({handleAuthRequests:true})` for the write paths: seeded a missing
    comment on D50 only, different priority/duration on each side, and
    `type=and` on SBT only, then confirmed after one load that the
    comment was genuinely auto-filled into D50's database (not just
    displayed) while priority/duration stayed independently different
    on each side (no cross-contamination) and the request-type dropdown
    correctly showed SBT's `and`; then, via real button clicks, renamed
    the target (What box Save) and changed D50's priority/duration plus
    the shared request type to `sim` (When box Save), and confirmed via
    `psql` on both real databases that the name landed on both, D50's
    priority/duration changed while SBT's stayed exactly as seeded, and
    both sides ended up with `type=sim` while keeping their own
    `duration=`. Screenshotted the resulting three-box layout (and,
    incidentally, the "one side unreachable" degradation path, hit for
    free by a fresh browser profile defaulting to the wrong site
    identity - D50's box correctly grayed out with a clear "not present"
    note in all three boxes rather than erroring or showing stale data).
    All seeded test data restored/removed and verified clean afterward.

    **Phase 5, DONE (2026-08-27) - target creation (both "replicate an
    existing target to the telescope missing it" and genuinely brand-new
    targets), sexagesimal RA/Dec, and a few layout/sizing cosmetics.**
    Real new capability, not previously in scope (phase 1 deliberately
    left target creation to `rts2-addtarget`) - the user found the real
    gap by hand ("target 8040 does not exist in SBT database, if I save
    the target, will it be inserted there too? Tried, it does not, I
    think it should be possible").

    **New `db/db` capability**: `rts2db::newTargetId()` (`target.h`/
    `target.ec`) factors the `nextval('tar_id')` draw `Target::save()`
    already did internally into its own function, so a caller can
    reserve an ID *before* deciding where to create the row - needed
    because `Target::saveWithID(true, id)` (already existed, already
    used since phase 1 as the upsert both `dbUpdateTarget()` and now
    target replication rely on) needs a specific ID up front, and
    genuinely-new-target creation needs one that doesn't collide.
    **Real cross-database subtlety worth recording**: D50's and SBT's
    `tar_id` sequences are two independent Postgres sequences in two
    independent databases - a fresh ID from one is *not* guaranteed free
    in the other (their histories have diverged for years). The
    frontend's "New target" flow draws from the current site's sequence,
    checks the *other* site for a collision via a plain GET, and retries
    (up to 8 times) if taken - not a hypothetical, this is exactly the
    kind of assumption that would have silently clobbered an unrelated
    real target on the other side had it gone unhandled.

    **New endpoints**: `GET /api/db/new-target-id` (mints via
    `newTargetId()`, does not create anything) and `POST /api/db/
    target-create?id=N&type=equatorial|elliptical&...` (new `dbCreateTarget()`
    in `dbendpoints.cpp` - instantiates a bare `rts2db::ConstTarget()`/
    `EllTarget()` via their no-arg constructors, exactly the "create new
    target, save() will persist it" usage those constructors' own doc
    comments already described but nothing had exercised until now;
    "equatorial" defaults to `TYPE_OPORTUNITY` ('O'), confirmed against
    the user's own `sch/database.py` query as the real "ad-hoc science
    target" type; reuses `TargetUpdate` for the field payload and the
    same `mpec`-must-be-used-not-`info`-for-elliptical validation
    `dbUpdateTarget()` already enforces). Alt/Az terrestrial remains out
    of scope, same as every earlier phase.

    **Frontend**: the When box's per-telescope checkbox is no longer
    `disabled` when a site doesn't have the target - it's clickable, and
    checking it (then clicking **Save target**) creates the target there
    using the current What-box field values plus that site's own
    Priority field, unifying "replicate to the missing side" and "create
    a brand-new target" into one mechanism (the latter starts with
    *both* sides missing). A new **New target…** button next to Load
    reserves a collision-checked ID, shows a blank form with an
    equatorial-vs-elliptical type picker (only shown when neither side
    has the target yet - once one side exists, type is inferred from it,
    same as before), and both telescope checkboxes default unchecked so
    creation only happens where explicitly requested.

    **Sexagesimal RA/Dec, purely cosmetic per explicit request** (a
    decimal-degree `<input type=number>` rendering with a locale comma
    decimal separator - confirmed back in phase 1's own testing on this
    session's Czech-locale machine - "hurts to be seen"): `f-ra`/`f-dec`
    are now plain text inputs, always *displayed* sexagesimal
    (`decToSexagesimal()`, RA in `HH:MM:SS.SSS`, Dec in
    `±DD:MM:SS.S`, with round-to-60 carry handling) and *parsed*
    (`parseCoordinate()`) accepting either sexagesimal (colon or
    space-separated) or plain decimal (`.` or `,`) input - "just
    recognise what is there," per the user. Storage and the API contract
    are untouched (still plain decimal degrees) - this is a display/
    input-parsing layer only, verified by round-tripping a hand-typed
    mixed-separator value (`"12:34:56.7"` RA, `"-05 06 07.8"` Dec)
    through creation on both real (test) databases and confirming via
    `psql` the stored decimal degrees matched the hand-computed
    conversion exactly (188.73625° / -5.10216666666667°).

    **Two smaller cosmetics, also per explicit request**: Comment moved
    to sit directly above Info (both textareas, together at the bottom
    of the What box, instead of Comment being separated from Info by the
    position fields); the How box's script input is now a `<textarea
    rows="2">` spanning the full row width (CSS `flex-basis: 100%`)
    instead of a single-line `<input>`, matching Comment's sizing -
    "some scripts are simply longer."

    **Smoke-tested end to end** against the same kind of two genuinely
    separate local databases as every earlier phase, headless-Chrome/CDP
    with `Fetch.enable({handleAuthRequests:true})`: deleted target 202
    from the SBT-role database, loaded it (confirmed the checkbox was
    enabled, not disabled, with the new "check to create" wording),
    checked SBT's box, set a distinct priority, clicked Save target, and
    confirmed via `psql` on the real SBT-role database that the row now
    existed with D50's shared fields (name, RA/Dec correctly round-
    tripped through sexagesimal display) and its own independently-set
    priority (77, distinct from D50's). Separately exercised **New
    target** end to end: reserved an ID, filled the form with
    intentionally mixed-format sexagesimal RA/Dec and different
    per-telescope priorities, checked both boxes, saved, and confirmed
    via `psql` on both databases that the target was created with
    identical name/position (parsed correctly from the typed sexagesimal
    text) and independently-correct per-telescope priority. One real
    test-harness gotcha hit and resolved during this, not a product bug:
    reusing a long-lived headless-Chrome tab across separate test script
    invocations serves whatever `target.js` was loaded at the *original*
    navigation, not the current on-disk file - same class of issue as
    phase 2's `const`-redeclaration finding. All test targets/data
    cleaned up and verified removed afterward.

## `rts2-jsonclient` - the CLI client (2026-08-31)

`web/jsonclient/`, one binary (`rts2-jsonclient`), shipped in the same
`rts2-web` package as the daemon. It is the scriptable half of the API
surface and the replacement for classic's dropped `rts2-xmlrpcclient`:
`devices`, `getall`, `get`, `set`/`inc`/`dec`, `selval`, `switchstate`,
`messages`, `horizon`, plus `db <endpoint>` / `api <path>` escape hatches
that take `key=value` arguments and pretty-print whatever JSON comes back
(so an endpoint added to the daemon later is reachable without touching
this client).

Decisions worth recording:

- **HTTP, not the bus.** It duplicates nothing from `rts2-sendcmd`: that
  one speaks the RTS2 protocol and has to run where the bus is reachable,
  this one needs only the HTTP port, so it works from a laptop, a cron
  job on another host, or through the optional reverse proxy.
- **libcurl, client-side only.** The daemon still depends on
  `libmicrohttpd` alone and makes no outgoing requests; the `pkg_check_
  modules(CURL ...)` lives in `jsonclient/CMakeLists.txt`, not the web
  top level, so that stays visible. libcurl (rather than a hand-rolled
  socket client) because the write endpoints are gated by HTTP basic auth
  and real deployments sit behind proxies that redirect and speak TLS.
- **Own JSON reader** (`json.h`/`json.cpp`), the reading counterpart of
  `httpd`'s write-only `jsonvalue.cpp`. Same reasoning as the serializer:
  no JSON library is otherwise anywhere in this tree, and ~200 lines of
  recursive-descent parser is a better trade than a new Build-Depends for
  turning `{"name":value}` back into printable text. It is a real parser
  all the same (`\u` escapes with surrogate pairs, nesting depth capped,
  trailing garbage rejected) - pointed at the wrong port it must say so,
  not misparse. Object members keep document order, since a device's
  value list is meaningful as the device ordered it.
- **`base_kernel` only, never `db`** - even when `WEB_WITH_DB` is ON. The
  client has no database connection of its own; what it uses from the
  kernel is the `CliApp` option/help/`askForPassword` scaffolding every
  other RTS2 command line tool uses, plus `Timestamp` and `message.h`'s
  severity bits.
- **Writes print nothing on success.** `/api/set` is fire-and-forget on
  the daemon side (it queues the change and answers before the device
  acknowledges), so the only value it could echo is the pre-change one.
  Silence plus the exit code is both the honest and the script-friendly
  answer; `--debug` shows the URL actually fetched.
- **`-u user` without a password prompts for it** via
  `App::askForPassword()` rather than requiring `-u user:pass` on the
  command line, where it would land in shell history and in every `ps`
  listing on the machine.

Tested against the live daemon on this machine (`devices`, `getall`,
`get` both forms, `selval` against a real filter/binning selection,
`messages`, `horizon`, `db current-night`, `api`, `--json`, and the
error paths: unknown device, unknown command, connection refused, bad
`key=value`). The write path (`set`/`inc`/`dec`/`switchstate`, URL
encoding of a script string full of spaces and braces, preemptive basic
auth, 401 handling, non-JSON response) was exercised against a stub HTTP
server rather than the production observatory.

Two daemon-side defects surfaced while testing it. Both are now fixed
in `httpd`:

- **Times were serialized at default `ostream` precision** (6 significant
  digits): `/api/messages` sent `"time":1.78818e+09`, and every
  `RTS2_VALUE_TIME` in `/api/get`/`/api/getall`, every observation
  `start`/`end`/`slew` and every image `exposureStart` in the `/api/db/`
  endpoints was rounded to the nearest ~1000 s - a whole message listing
  came out stamped at the same instant. `jsonNumber()` now writes the
  shortest representation that round-trips exactly, via `std::to_chars`
  (C++17): nothing is lost, and `0.1` still prints as `0.1` rather than
  `setprecision(17)`'s `0.10000000000000001`. Non-finite doubles became
  `null` as well - `inf` was being written raw, and is no more valid JSON
  than `nan` is.

  A `jsonTime()` call now marks the time-valued fields explicitly (same
  output as `jsonNumber()`, one place to change if the wire format ever
  moves). It deliberately ignores base's new `--jd`/`--ctime` display
  mode: a protocol must mean the same thing regardless of how the daemon
  was started, so the wire is always ctime seconds, which is what
  `app.js`'s `new Date (t * 1000)` and `rts2-jsonclient` read.

  The general half of this - times are not just large doubles, they have
  a display mode, and it belongs in the C++ formatting layer - is fixed
  in `base`: see "Time display mode: `timeDisplay_t`" in `base/HISTORY.md`.
  Every RTS2 tool gained `--jd`/`--ctime` from it.
- **`/api/devices` reported an unnamed connection as `""`**, while
  `/api/getall` skipped those (`getName()[0] == '\0'`). The endpoint now
  agrees with `/api/getall` and skips them. `rts2-jsonclient` keeps its
  own filter for the same case: it is the client that talks to whatever
  daemon a site is actually running, including one older than this fix.

## Telemetry graphs: `/api/db/records` + `records.html` (2026-08-31)

The read half of the feature D50 lost when classic `rts2-xmlrpcd` was
replaced - the cloudmeter sky-transparency plot. The write half (who
fills the tables) is `rts2-recordd`, a separate daemon in `db`, on the
user's call that recording is not the web server's job; see
`db/STATUS.md` for it.

- `GET /api/db/recvals` - the catalogue of recorded device/value pairs,
  each with the extent of its stored samples (`from`/`to`/`samples`). The
  graph page's picker is built from it, and the extent is included
  because a value whose last sample is two years old should look that
  way rather than as an empty plot the user has to hunt a date range for.
- `GET /api/db/records?device=&value=&from=&to=&points=` - one series.
  Defaults to the last 24 hours and 1000 points, `points` capped at
  20000 so a hand-written URL cannot ask for a million-row response. A
  device/value pair that is not recorded is a 400 - this endpoint never
  creates a `recvals` row the way the recorder does.
- Points are arrays, not objects: `[[t, avg, min, max, n], ...]`. A
  thousand `{"t":...,"avg":...}` objects is three times the bytes for the
  same numbers, and this is the one endpoint here whose response size is
  bounded only by the caller's own `points`. `min`/`max` are the spread
  within a bucket when the range was downsampled (`n > 1`).
- Both run on the worker pool like every other `/api/db/` endpoint (see
  the 2026-08-17 finding), not inline on the poll loop.

`static/records.html` + `records.js` draw it in a plain 2d canvas -
vanilla JS, no charting library, consistent with the rest of this
frontend and with the "no server-side image rendering" decision that
dropped Magick++ (classic rendered these graphs as PNGs). Being
client-side makes the plot interactive for nothing: hover reads out a
sample and, on a downsampled range, the bucket's sample count and
min/max. The min/max band is drawn only where a bucket actually holds
more than one sample, gaps in the data stay gaps rather than being
bridged by a line nobody measured, and the chart palette lives in
`style.css` as `--rec-*` variables that `records.js` reads off the
computed style rather than keeping a second copy of the colours.

A **state series** (`recvals.value_type` 0 - the `rts2_status_t` bitmask
`rts2-recordd`'s `@state` entries write, see `db/STATUS.md`) is drawn as
steps, with the last level running to the right edge: a state holds until
something changes it, so sloping between two samples would draw a
transition that never happened and imply intermediate bitmask values that
mean something else entirely. Nothing else about the endpoint changes -
the same `/api/db/records` call serves it, with `records_state`'s `state`
column standing in for `value`.

Verified against the live daemon and real recorded `CLOUD` telemetry:
raw, bucketed and explicit-range responses, the boolean series, both
error paths (unknown value, missing parameters), and the page itself
rendered in headless Firefox - which also caught the last x-axis label
being clipped mid-digit at the canvas edge (now aligned inwards).

## Target visibility plot: `/api/db/target-altitude` + the editor's canvas (2026-08-31)

The staralt-style night plot classic produced with `rts2-targetinfo -g`,
which printed gnuplot code to pipe into gnuplot. Same content, drawn in
the target editor instead of a separate command: the night from sunset to
sunrise, RTS2's own night boundaries marked, the target's trace, the
Moon's, and the horizon along the bottom.

- **The horizon is the horizon at the target's azimuth at that moment**,
  not one flat limit line. That is the whole reason the plot is worth
  drawing per target: where the trace meets that curve is where *this*
  telescope loses the object behind a hill, which is a different time
  from "when it gets low". `ObjectCheck::getHorizonHeight()` answers it
  per sample, from the same horizon file the dome-safety checks use.
- **Night boundaries come from centrald's own state machine**, not a
  second opinion: `next_event()` (riseset.h) walked forward from local
  noon, with the same `[observatory]` `night_horizon`/`day_horizon`/
  `evening_time`/`morning_time` keys and defaults centrald's
  `initValues()` reads. DUSK begins at sunset, NIGHT at the
  night_horizon crossing, DAWN at the end of night, MORNING at sunrise -
  so all four times fall out of one loop.
- **Position is per sample**, `rts2db::Target::getAltAz()`, so an
  elliptical/GRB/planet target traces its real path across the night
  rather than a frozen RA/Dec. That is why the endpoint lives under
  `/api/db/` and takes a target id; `ra=&dec=` computes a fixed position
  with no database involved, for previewing a target that has not been
  saved yet.
- A night belongs to the day it starts on, so everything is anchored to
  **local noon** - asking at 23:00 and at 03:00 describes the same night,
  and `date=YYYY-MM-DD` needs no special case.
- Points are arrays again - `[t, alt, az, horizonAlt, moonAlt, moonDist,
  sunAlt]` - for the same size reason `/api/db/records` gives.

The editor's panel draws it in a canvas (vanilla JS, no charting
library, palette from `style.css`'s new `--sky-*` variables): twilight
shaded either side of the RTS2 night, dashed night boundaries, the
horizon filled from the bottom, the Moon dashed because it is context
rather than a second subject, and a hover readout of altitude, compass
azimuth and Moon distance. Azimuth is converted to compass degrees for
display (libnova counts from south) - the same `+ 180` the site monitors'
sky charts already do. The heading summarises what a plan actually asks:
peak altitude and time, hours observable, and the Moon's illumination and
closest approach - all measured **inside the RTS2 night**, since a peak
reached during twilight is a number nobody can observe at.

Verified against the live daemon with a real target (TT Boo, id 1152):
sunset 17:34, sunrise 04:25, RTS2 night 18:47-03:12 with the sun at -10
at that boundary and at day_horizon (+1 at this site) at sunset, the
horizon curve reproducing the loaded horizon file's ~3 deg at azimuth 82
rising to ~30 deg where the target sets into the hills. Also checked the
fixed-position path, a target that never rises (all altitudes negative),
a malformed date, missing parameters and a nonexistent id. Rendered in
headless Firefox, which is what caught this page's `fetchJson()`
returning `{ ok, body }` rather than the body - the plot had been
silently reading `undefined.points`.

## Three graph follow-ups (2026-09-01)

Three requests against the two graphs above - two small, one a new
endpoint - after this checkout was found nine commits behind (`git pull`
brought in both graphs above; this session had been working from a stale
tree that had neither).

**Day/twilight/night shading on the cloud sensor graph.** The user's own
observation: cloudmeter readings mean something different taken in
daylight than at night. `/api/horizon` (`httpd.cpp`) now also reports
`nightHorizon`/`dayHorizon` - the same `[observatory]` keys
`dbTargetAltitude()` already reads - alongside the existing lat/lon/
horizon, so a client classifying day/twilight/night off the Sun's
altitude it computes itself agrees with what RTS2 itself calls night,
not a fixed astronomical-twilight constant. `d50-monitor.html` and
`bart-monitor.html` each gained `sunAltDeg()` (their existing
`julianDate`/`gmstDeg`/`sunEquatorial`/`equToHrz` - already there for
`renderSkyChart()`'s Sun marker - evaluated at an arbitrary time instead
of "now") and `dayNightBands()` (3-minute sampling into contiguous `day`/
`twilight`/`night` runs), and `drawCloudGraph()` fills `day`/`twilight`
bands (new `--graph-day-bg`/`--graph-twilight-bg` per file, light/dark
tuned like the other graph colours there) before drawing anything else -
`night` gets no fill at all, unchanged from before this existed, per the
user's own "let night stay as it was" framing. Deliberately not added to
the generic `records.js` explorer (confirmed with the user): day/night
framing is specific to what the cloudmeter means, not something every
recorded value should assume.

**`#sky-panel` narrowed to one grid column.** It was a full-width
`.panel` sitting outside `#target-boxes`; now it is a `.panel.target-box`
picking up the same `repeat(auto-fit, minmax(340px, 1fr))` sizing the
What/How/When boxes already use - exactly the "accept a fourth box"
extensibility that grid was built for (see the target-editor reframe
phase above). No JS changes; the panel's contents are looked up by id
regardless of DOM position.

Tested live at D50 with five boxes (What/How/When plus the two
visibility plots) all in that one grid: on a narrower/laptop screen
`auto-fit` was cramming all five into one row rather than wrapping after
three, per the user ("the number of columns raised to 5 may be too much
for my screen"). Fixed by giving the two plots their own sibling grid,
`#target-graphs` (same `.target-boxes` class, so identical column
sizing/gap), placed after `#target-boxes` closes rather than as more
items inside it - a separate grid container always starts its own row,
where `auto-fit` within a single grid does not, so What/How/When wrap
independently of however many plot panels follow.

**Yearly visibility ("butterfly") plot**: `/api/db/target-visibility-year
?id=N[&year=YYYY]` (or `ra=&dec=` for an unsaved target, same contract as
`target-altitude`) plus a fifth `.target-box`, `#year-panel`, right after
`#sky-panel`.

- One point per calendar day of the year (noon-anchored, walked via
  `struct tm` + `mktime()` rather than adding 86400 to a `time_t`, so the
  two DST-transition days don't leave every later noon off by an hour
  until the next transition corrects it back). For each day: that
  night's sunset/nightStart/nightEnd/sunrise (`findNightBoundaries()`,
  factored out of `dbTargetAltitude()`'s own inline loop so both
  endpoints agree on where night starts and ends instead of keeping two
  copies of it - `dbTargetAltitude()` itself is otherwise unchanged), then
  the target's `riseTime`/`setTime` (first/last sample above the real,
  per-azimuth horizon that night - `ObjectCheck::getHorizonHeight()`,
  same as `dbTargetAltitude()`) and `peakAlt`/`peakTime`, sampled at ~10
  minute resolution (coarser than `dbTargetAltitude()`'s up-to-2000-point
  single night, because this is one call doing that 365-366 times, not
  once per pixel of one interactive plot).
- A day with no sunset/sunrise pair at all (polar day/night) is skipped
  outright rather than aborting the whole year the way a single missing
  night is an error in `dbTargetAltitude()` - one bad day out of 365
  shouldn't blank the other 364. No Moon in this response: at one sample
  per night its ~29.5-day cycle doesn't carry a stable per-night meaning
  the way it does in the single-night plot.
- The editor's panel (`target.js`) draws it in a canvas, same
  no-charting-library convention as everywhere else: X axis is the
  day's actual date (so a skipped polar day leaves a real gap, not a
  seam), Y axis is seconds-after-that-day's-own-local-noon (DST-safe by
  construction - real elapsed time, not wall-clock arithmetic),
  auto-scaled to the year's own sunset/sunrise extremes rather than a
  fixed window that would need re-tuning per latitude. Twilight is
  filled between sunset/sunrise and the night boundary curves (reusing
  `--sky-twilight`, the same variable `drawVisibility()` uses, so the two
  panels agree visually) - a day with no true RTS2 night (sun never
  reaches `night_horizon`, a summer "white night" at high latitude) is
  twilight the *entire* sunset-to-sunrise span rather than unshaded: a
  real bug caught before shipping, where the naive "twilight ends at
  nightStart, or nothing if there's no nightStart" fallback silently
  turned every white night into a blank (day-looking) gap instead of the
  solid twilight band it actually is.
- Year navigation (prev/this year/next) mirrors `#sky-panel`'s
  prev/today/next, same `apiUrl(currentSite, ...)` per-telescope pattern.

**Tested live at D50** - real day/night shading and the one-column width
both confirmed working. The butterfly plot itself had a real bug, caught
this way: a screenshot showed the visibility band staying solid blue
across large stretches of the night "particularly at times when there
should be none." Cause: the endpoint tracked only the first sample above
the horizon and the last one (`riseTime`/`setTime`, one span per night) -
correct for a simple rise-culminate-set target, wrong for anything
circumpolar or near it, which sweeps through every azimuth over the
course of a night and can duck behind a real horizon obstruction and
re-emerge. The single span painted straight through that dip as if still
visible. It also explains the plot's stray rectangular "notches": as the
target's visibility structure flips between one continuous window and
two separate ones through the year, which extreme sample "first-up" or
"last-up" happens to land in jumps discontinuously day to day.

Fixed by having `dbTargetVisibilityYear()` track the actual union of
above-horizon intervals per night (`windowStart`/`windowEnd` vectors,
one open/close pair per up/down transition) instead of two scalars. The
day array is now `[noon, sunset, nightStart, nightEnd, sunrise, peakAlt,
peakTime, windows]` - `windows` a (possibly empty) list of `[start, end]`
pairs, `peakTime` no longer nulled (it is well-defined - the best moment
that night - whether or not the target ever clears the horizon).
`target.js` draws the visibility band as one filled rectangle per
interval per day (column edges at the midpoint to each neighbouring
day) rather than a single ribbon connected across days - a smooth
connected area assumes one span per day, which is exactly the assumption
that was wrong.

Still to verify live after this fix: a genuinely circumpolar or
near-circumpolar target (high declination for this latitude) across a
full year, checking the visibility band now shows real gaps where the
target ducks behind the horizon rather than solid blue through them.

## Site auto-detection bug: D50's own page misidentified itself as SBT

Found by the user testing at D50 directly: the target editor "does not
load properly" there - and, worse than a simple failure, `#when-panel`'s
SBT-labelled data was actually silently showing D50's own real data
under the wrong label.

`detectSite()`'s heuristic was `location.hostname.includes('d50')`. The
two real production hostnames are `lascaux.asu.cas.cz` (SBT/BART) and
`lascaux50.asu.cas.cz` (D50) - and `lascaux50` does **not** contain the
literal substring `d50` (it's `...aux50`), so that check was false on
D50's own page, which fell through to the `sbt` default. From there,
`apiUrl('sbt', path)` treated `site === currentSite` and returned a bare
(same-origin) path - so "SBT" data was actually D50's own daemon
answering about itself, mislabelled - while `apiUrl('d50', path)` was
now the "peer" and got prefixed with `/d50`, a proxy path that exists on
*other* vhosts to reach D50, not on D50's own vhost to reach itself, so
it 404s/fails there. Net effect: the box labelled D50 fails to load, and
the box labelled SBT is quietly showing real D50 data. `lascaux` (SBT)
"worked fine" only because the `else` branch (`'sbt'`) happens to be
correct there regardless of what the hostname actually contains.

Fixed by also matching `lascaux50` explicitly in `detectSite()`
(`web/static/target.js`) - the manual override in the page's own
"Advanced: telescope identity" `<details>` (added for exactly this
"auto-detection guesses wrong" case, see the target-editor reframe phase
above) was always available as a per-browser workaround, but the actual
production hostname should be detected correctly without it.

## Yearly plot: show the other telescope's horizon too

The user's own framing: "the visibility graph shows only horizon from
'our' telescope... I want to show also the D50 horizon" (asked from
SBT/lascaux). Fair - this page already presents D50+SBT as one facility
everywhere else (the What/How/When boxes), so "can this be observed"
should answer for both telescopes, not just whichever one happens to be
serving the page.

`loadYearVisibility()` now fetches `/api/db/target-visibility-year` from
*both* `currentSite` and `otherSite()` in parallel (guarded by
`siteData[otherSite()].exists` - a peer without this target, or one that
fails to answer, just means no overlay, not a failed panel - matches how
the What/How/When boxes already treat a missing peer). D50 and SBT/BART
share the same physical site (same weather station - see the cloud
sensor's own "shared by both telescopes" note above), so sunset/
twilight/night are identical between them to any precision this plot
cares about; only the two telescopes' own horizon files differ. The
twilight/night hourglass background is therefore drawn once (from the
local data, as before), and the peer's visibility windows are overlaid
as a second colour (`--sky-target-fill-peer`, violet vs. the local
`--sky-target-fill` blue) via the same `drawVisibilityWindows()` (a new
name for what was the local-only rectangle-drawing code, now a function
taking a day list and a colour). Where both telescopes can see the
target at once, the two translucent fills blend into a third colour for
free - a deliberately cheap "visible from both" cue rather than
computing an explicit interval intersection. Legend and hover tooltip
both label each band by the real telescope name (`SITES[..].label`).

## Nightly plot: the peer telescope's horizon too

Same request extended to `#sky-panel`, the single-night plot, once the
user had it on the yearly one. `loadVisibility()` now also fetches
`/api/db/target-altitude` from `otherSite()` in parallel with the local
call (same `peerHasTarget` guard as the yearly plot), for the *same*
date - no need to sequence one call after the other to learn a resolved
"tonight" first, since D50 and SBT share one physical site and so
resolve to the same night either way.

Unlike the yearly plot, only the peer response's horizon column
(`points[i][3]`) is used - its target/Moon/Sun altitude columns would
just be a near-identical second copy of numbers already on screen, since
target position, Moon and Sun don't care which of the two telescopes is
asking. Drawn as a dotted line only (no fill - filling both telescopes'
horizon would make whichever is drawn second obscure the first), read
against the same `sx`/`sy` as the local trace with points outside
`[sunset, sunrise]` dropped instead of assuming both responses reported
exactly the same time domain.

**Real color bug caught while wiring this one up, fixed before either
plot had shipped its first live use**: the yearly plot's original
`--sky-target-fill-peer` was `rgba(180, 83, 9, ...)` - the exact same hue
as `--sky-moon`, `#b45309`. Harmless as an isolated translucent fill on
the yearly plot, but the nightly plot draws the peer horizon as a dashed
line sitting right next to the Moon's own dashed line - two
indistinguishable dashed lines in the same colour would have been a real
usability bug. Both `--sky-target-fill-peer` and the new
`--sky-horizon-peer` now use violet (`#7c3aed`), a hue not already
spoken for by target/Moon/horizon/night on either plot - one "this is
the peer telescope" colour identity shared across both plots rather than
two independently-chosen ones.

### "New target id doesn't work" - real bug, found via a real user report (2026-09-04)

Filip reported the **New target…** button failing with "could not find
an id free on both telescopes after several tries - try again" where it
should have found something around 8040. Root cause, confirmed by
reading the actual current code rather than assuming: a genuine
regression interaction between two changes that landed separately and
never got reconciled.

`rts2db::newTargetId()` was rewritten (commit a322bf1, a fix for a real
problem of its own - `nextval('tar_id')` permanently burned an id even
when the web form was abandoned without saving, which is exactly how
IDs 8044..30007 got silently consumed during this session's own
testing) from a monotonic sequence draw into a **stateless live scan**
for the smallest free `tar_id`. That's correct and desirable on its
own, but nobody updated the caller that mattered: the web target-
editor's "New target" retry loop (`web/static/target.js`,
`new-target-btn`'s click handler) was written against the *old*
semantics - "call `/api/db/new-target-id` again, get a fresh number" -
and calling the *new* stateless scan again with nothing changed in
between simply returns the **identical** candidate every time. On a
genuine cross-telescope collision (the candidate free on this site but
already a real, unrelated target on the sibling site), the loop
re-asked the exact same question up to 8 times, got the exact same
(still-colliding) answer every time, and failed - not occasionally,
*every single time* there was a real collision to resolve, which is
exactly what Filip hit.

**Fix**: `rts2db::newTargetId()` now takes an `after` parameter
(default: the whole `[8000,49999]` range) so a caller can ask "the
smallest free id *above* this one" - `GET /api/db/new-target-id?after=N`.
The frontend's retry loop now tracks the last rejected candidate and
passes it back in as `after` on the next attempt, so each retry
genuinely advances instead of re-asking the same question.

**Verified against the literal failure mode, not just the API in
isolation**: seeded two real target rows (`8000`, `8001`) on a test
"SBT" database specifically to block D50's first two scan results,
confirmed via raw `curl` that `new-target-id` without `after` returns
`8000` twice in a row (the bug, reproduced deliberately) while
`?after=8000` correctly returns `8001` and `?after=8001` returns
`8002`, then drove the real **New target** button via headless-Chrome/
CDP against that exact seeded collision and confirmed it now correctly
lands on `8002` - free on both sides - instead of failing. Test rows
and databases cleaned up afterward.

**Follow-up (2026-09-30)**: still failed "basically every time". The
`after` fix made each retry advance, but only by one hole of *this*
site's free list, 8 tries max - and this site's lowest holes are ids of
purged targets, most of which are still real targets on the peer, so
8 steps rarely got past them. The loop now alternates: this site's
smallest free id, then the peer's smallest free id `>=` that
(`new-target-id?after=candidate-1` on the peer), then this site's again
from there, until both return the same id. Each step skips a whole run
of taken ids, so it settles in a few rounds. If the peer can't allocate
(unreachable/not logged in), it falls back to the old existence check.
No backend change.

### Guiding performance display (C1) - 2026-09-08

D50's monitor page had a C1 box showing `C1_small.jpg`/`C1_center.jpg`
from the external dark+flat pipeline, and what it actually displayed was
a **very old image**: C1 is the guide camera, so the last frame that
pipeline ever had for it was whatever C1 last took as something other
than a guider. The user asked for the last guiding frame plus a plot of
the same content as their `guiding.png` gnuplot script (which greps
`guide: val` lines out of `/var/log/rts2-debug` and plots RA/DEC error
against time plus an RA-vs-DEC scatter).

Built in three layers, all hanging off **one** change of source: the
guide script now publishes its own numbers on the bus.

- **`python/guide.py`** (D50's `/etc/rts2/guide.py`, copied into the tree
  this session) creates `guide_dx`, `guide_dy`, `guide_pulse_ra`,
  `guide_pulse_dec`, `guide_stars`, `guide_fwhm` (+ a static
  `guide_scale`, arcsec/pixel, and a string `guide_last_image`) **on C1**
  and updates them every guiding cycle, alongside the `guide: val` log
  line it already wrote. Values have to name the camera explicitly:
  `value_create` is handled by `ConnExe` in the *executor*
  (`base/script/src/connexe.cpp:523`), unlike a plain `value X = Y` which
  `elementexe.cpp:395` routes to the camera running the script, so an
  unqualified create would land on `EXEC`. NaN when not guiding, which is
  what both consumers want: `rts2-recordd` skips it
  (`db/recordd/src/recordd.cpp:384`) rather than recording a flat line,
  and `jsonNumber()` sends it as `null`.
- **The frames**: `toArchive()` became `delete()`. The `images` row is
  inserted well before a script ever sees a frame
  (`processCameraImage()` saves the image, which for an object frame is
  an INSERT via `ImageSkyDb::updateDB`), so archiving guide frames had
  been filling the database with thousands of junk rows a night;
  `delete` is the one image action that also removes that row
  (`ImageSkyDb::deleteImage` -> `deleteFromDB`). What is worth keeping is
  kept outside the database entirely: a 48x48 int16 cutout around the
  brightest detected star, written by the script into
  `<images-dir>/guide/<night>/C1_YYYYmmdd-HHMMSS-mmm.fits` - RTS2's own
  naming, RTS2's own night boundary (`Configuration::getNight()`), ~9 kB
  a frame, ~250 MB for a full night at 1 Hz, and the first frame of a new
  night deletes the previous ones. rts2-httpd's existing `/preview`
  endpoint renders them, cache and all, with no new server code.
- **`web/sites/d50-monitor.html`**: the C1 camera box is gone, replaced
  by a **Guiding** panel - the kept cutout, a 5-minute strip
  chart of dx/dy, an RA/DEC scatter with the newest points brightest, and
  underneath, the night's history from `/api/db/records` with 2 h/8 h/
  24 h buttons (the same shape as the cloud panel, which is the working
  precedent for a records-backed canvas plot on this page). The live half
  polls `/api/get?d=C1` at guiding rate and needs no database at all; it
  self-reschedules to 5 s when nothing is guiding rather than asking
  every second all day. The panel is **not** full width: it and
  `.cam-panel` now share a `.right-panel` flex column, so it is exactly
  as wide as the C0 box and sits beside the title/status column rather
  than under it - that left column is going to grow, and a full-width
  panel under a short one left the page lopsided (user call, 2026-09-08).
  Note `align-self: start` cannot be used on `.cam-panel` any more: in a
  flex column that is the horizontal axis, and it would shrink the panel
  to its content width. `updateCameraStatus()`'s FWHM for C1 now falls
  back to `guide_fwhm` - IMGP has no `fwhm_C1` and never will.

**FWHM is measured here, not taken from `detect4g`**: that wrapper's
column meaning is undocumented in this tree and the two scripts using it
disagree (`guide.py` treats column 2 as SNR, `guideccd.py` calls column 3
snr), so the cutout gets its own area-above-half-maximum estimate - no
fit to fail to converge, one bad frame costs a NaN instead of an
exception. Same reasoning for the y convention: nothing here documents
whether `detect4g`'s y counts numpy rows or FITS rows (guiding never
cared - it only ever compares one frame against another), so both are
tried on the first star of a run and the one that lands on the star is
kept, logged once.

**Tested offline** (`scratchpad/test_guide.py`, not committed): synthetic
frames with a known FWHM through the real `keep_frame()` path - cutout
centred and 16-bit, FWHM recovered to ~0.1 px, both y conventions
detected correctly, night purge removing only 8-digit directories,
`value_create`/`V` wire format verbatim, and an unwritable images
directory logging once and letting guiding continue. That last one found
a real bug: **astropy refuses NaN in a FITS header**, so writing a
reference frame (no shift to record) threw and would have silently
dropped every such frame - cards that would carry NaN are now simply
left out. **Tested live** in headless Chrome against a mocked daemon:
guiding, not-guiding, "camera has no guiding values" (an older guide.py)
and "recordd is not recording this" all render sensibly, no page errors.

**Not done here, deliberately**: the `rts2-recordd` config line
(`C1  5  guide_dx guide_dy guide_pulse_ra guide_pulse_dec guide_stars
guide_fwhm`) is site configuration, not repo content, and that daemon
has to actually be running at D50 for the history plot to have anything
in it - same dependency the cloud panel already carries.

**Also fixed, on the user's call** (found while reading `guide.py`, not
part of the display work): `self.setValue('T0', 10, 'speed_guide_ra')`
had its arguments in the wrong order - the signature is `setValue(name,
value, device)` - so it addressed a device named `speed_guide_ra`, which
does not exist. The command went nowhere and the 10% sidereal guiding
rate the 947 pulse-calibration constant is derived from was never
actually being set; the mount evidently already sat at it, since guiding
worked. Both lines now say `setValue('speed_guide_ra', 10, 'T0')`.

**No daemon change at all**: this whole feature is a script, a static
page, and one line of `rts2-recordd` configuration. `/preview/` and
`/api/db/records` are endpoints the deployed daemon already serves (the
image archive and the cloud panel use them), so nothing here needs
rts2-httpd rebuilt or restarted - `d50-monitor.html` is served by the
site's own Apache, and `guide.py` is picked up by the executor the next
time it launches the script.

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

### Prefilled "new target" link (2026-09-30)

Filip's request (`rts2_target_prefill_request.md`): his D50 transient list
wants a "Schedule" link that opens the editor with a new target filled in.
`target.html?new=1&name=&ra=&dec=&comment=&info=&site=d50,sbt&priority=&duration=`
(`id` wins if both are given). Frontend only (`static/target.js`):

- **Duplicate check first**: `api/db/targets` from both telescopes, match
  on name ignoring case/whitespace or within 5" of ra/dec (name match
  wins, else nearest). A hit loads that target as `?id=N` would, rewrites
  the address bar to `?id=N` and says why in the status line. A site
  whose list can't be read is named in the status line, not fatal.
- Otherwise the New target flow runs (now the named `startNewTarget()`),
  then the What box and the When-box ticks/priority/duration are filled.
  RA/Dec are shown sexagesimal like a loaded target. Nothing is written
  until Save target; the query string is dropped via
  `history.replaceState` so a reload doesn't redo it.
- **Visibility before saving**: `visibilityQuery()` uses `ra=&dec=` for a
  target that exists on neither telescope, so both plots (and the peer
  horizon overlay) show for any unsaved new target, and redraw on RA/Dec
  change. New target also hides a previously loaded target's plots now.
- **Duration on first save**: Save target writes the scheduling row
  (duration, request type) right after `target-create` succeeds on a site.

Tested in headless Chrome (CDP) against a mock of both APIs, page served
as D50 with SBT holding 8000-8120: new target landed on 8121 in four
`new-target-id` calls with no writes before Save, Save issued
`target-create` + `scheduling-save duration=600`; name and 2.8"-position
duplicates loaded the existing target; `?id=&new=1` loaded the id. Not
tried against the real daemons.

Not done: write endpoints still accept GET (Filip's CSRF note in the
request) - separate change.

### Target list (2026-09-30)

Level zero of the program work (proposal in `programs.md`): one table of
every target on both telescopes, so relative priorities and durations
can be compared. Read-only; a row opens `target.html?id=N`.

- **Backend**: new `GET /api/db/target-list` (`dbTargetList()`), backed by
  `rts2db::TargetSummarySet` (`db/db/src/targetlist.ec`) - one joined
  query over targets, scheduling (`DISTINCT ON`, since production's table
  has no key on tar_id) and observations (count, max obs_start). Not
  TargetSet: that would be a Target object plus extra queries per row
  for ~16k targets. RA/Dec are the stored columns (null for elliptical),
  comment is cut to 200 characters to keep the response small.
  `/api/db/targets` is unchanged (target.js uses it).
- **Frontend**: `static/targets.html` + `targets.js`, linked from the
  device monitor and the target editor. Loads both sites, merges by
  tar_id. Filters: words in name/comment, id or id range, cone around
  RA/Dec (seconds optional), enabled on which telescope, type, and
  "scheduled only". Sortable columns, per telescope: on, priority,
  duration, observation count, last observation; plus the request type
  (highlighted when D50 and SBT differ). Filters and sort live in the
  query string, so a view can be linked. The summary line gives, per
  telescope, the number of scheduled targets among those shown and
  their summed duration.
- **"Scheduled"** mirrors `sch/database.py`'s two queries: enabled type
  `O` with tar_id 1000-49999, or enabled type `G` never observed.
- Renders 500 rows, with a "Show all" button (15.6k rows take ~0.5 s).
  A telescope that can't be reached is shown as unavailable, not as
  "not on" it.

Tested: the endpoint against the local dev database (121 targets); the
page in headless Chrome against a mock serving ~13.6k targets per site
(load ~0.7 s, filters, sort, URL state, cone search, one site down). Not
yet tried against production data.

### Target editor: light curve panel (2026-10-08)

A "Light curve" box under the two visibility plots in `target.html`,
showing everything D50's photometry pipeline measured around the target's
position. The data and the plot both come from lcgen
(`https://hog.asu.cas.cz/lcgen/`, a separate Flask service on hog next to
the photometry database), not from rts2-httpd.

- **How**: target.js loads lcgen's own widget (`<lcgen>/static/lcplot.js`,
  vanilla canvas like the rest of this frontend) and points it at the RA/Dec
  in the What box; the widget fetches `<lcgen>/api/lc` cross-origin itself.
  Nothing is proxied through this daemon, so the two never have to run on
  the same machine.
- **Login**: lcgen hides data younger than a year and embargoed
  (unpublished GRB) positions from anonymous users. Its login is lcgen's
  own (same htpasswd as hog's Apache), traded for a bearer token kept in
  this page's localStorage - no cookies, so third-party-cookie blocking
  does not matter.
- **Setting**: "Light curve service" in the Advanced box (localStorage key
  `rts2-target-lcgen`); empty turns the panel off.
- Shown for fixed positions only (not elliptical targets), refreshed when a
  target loads, a new one is prefilled, or a new target's RA/Dec change.
  Spans the whole row: a light curve in a third of the width is unreadable.

Tested: in headless Chrome against a local rts2-httpd (`stars` dev
database, spare ports) and the live lcgen - a prefilled new target at TT Boo
(4772 public points), an existing local target with no D50 data ("No public
measurements here"), and the panel switching off with an empty setting.

### D50 monitor: full centrald state on the Status line (2026-10-12) [JS]

`d50-monitor.html`'s "Status:" line showed only on/standby/off. It now
renders centrald's whole `state` field the way rts2-mon's bottom line does
(`CentralState::getString`, `base/kernel/src/centralstate.cpp`): `ON night`,
`bad weather | rain | ON night`, `stop | ON dusk, block exposure`, with the
weather/stop/off parts in red. The raw state is in the tooltip as hex (rts2-mon
prints it after the text). When the weather is bad, centrald's own
`bad_weather_device: bad_weather_reason` is shown on a red line below it.
Deviations from rts2-mon: STANDBY in capitals to match the buttons, and
`unknown`, not `unknow`, for a day period outside 0-5. Note the weather bit
is the sign bit of a JS bitwise result, hence the `>>> 0`.

Tested in headless Firefox with the function lifted out of the page and fed
the states above, plus an unreachable centrald (`N/A`). Not tried against the
live daemon.

`bart-monitor.html` got the same function and reason line, on the user's
call: its old comment said weather was left out deliberately because "Weather
allows opening" under Dome covers it, but that row says only whether the roof
may open, not why, nor anything about stop or blocks. Same headless-Firefox
test against BART's copy.

### D50 monitor: T0 values and OTA fan from the old PHP page (2026-10-12) [JS]

First batch carried over from the old status page (`/var/www/observ/
fr_status.php` on lascaux50, dead since the new httpd has no XML-RPC). The
user will rearrange placement and looks later; for now they sit where the
old page had them.

- **Telescope block**: Coordinates (OBJ), Offsets (OFFS), Corrections
  (CORR_), T0 RA/Dec (TEL), T0 Alt/Az (TEL_, "alt az" in that order), then
  Airmass, Tracking + block move, and T0 info time in UT. All sexagesimal as
  the PHP page printed them (`fmtRa`/`fmtDeg`, rounded on the total so
  59.999" carries). Coordinates were decimal degrees until now.
- **block_move**: `Yes` amber / `No` green. The old page had On green and Off
  red, but set means teld refuses every software move (`teld.cpp`), so Yes is
  the state to notice.
- **OTA fan power** (SHUTTER1 `OTAfanPower`, 0-9) under the temperature
  table, coloured as the PHP did: 0 green "(off)", 9 red "(max)", in between
  `#<16n+111>2222`.
- **Fixed**: "Rain? (D50)" read `RAIN_BART` instead of `RAIN_D50`, so it
  always repeated the SBT sensor. Also removed the stale comment claiming
  SHUTTER1 is deliberately not shown.

Tested live: the page in headless Firefox on lascaux50, served through a
local proxy to the running rts2-httpd (:8889), during an observation of
Mrk 501. All values filled in and CORR_ matched the raw value. Not in
`bart-monitor.html`.

### D50 monitor: what "Weather allows opening" means (2026-10-12) [JS]

DOME's `weather_open`, shown as "Weather allows opening", says whether the
system switches itself from standby to on by the weather, with no human at
the controls; it is not "the weather is good now". D50 does not use it yet:
the operator switches to on by hand each evening once it is safe, so "No"
next to an open roof and "ON night" is normal (user's explanation; they
may turn automatic opening on later). Comment added at the code; display
unchanged.

### SBT monitor: the same T0 block as D50 (2026-10-12) [JS]

`bart-monitor.html` got the telescope block from the D50 page (`fmtSexa`/
`fmtDeg`/`fmtRa` and `updateTelescopeStatus()` copied over): sexagesimal
Coordinates, Offsets, Corrections, T0 RA/Dec, T0 Alt/Az, Airmass, Tracking,
T0 info time. Left out on the user's call: block move (not used at SBT) and
OTA fan power (SBT has no such fan). The value names are generic teld ones
(`teld.cpp`), so BART's T0 has them all. Tested by rendering the page in
headless Firefox against D50's live httpd, since BART's httpd is not
reachable from lascaux50; not yet seen against BART's own daemon.

### SBT monitor: both rain sensors (2026-10-12) [JS]

`bart-monitor.html`'s "Rain?" now shows `RAIN_BART` and `RAIN_D50`, labelled
SBT and D50 as on D50's page. RAIN_D50 is read from D50's own rts2-httpd
through the `/d50/api` Apache proxy the cloud history already uses
(`REMOTE_DEVICES`; `fetchDeviceData()` takes the base URL), because it was
not possible to check from lascaux50 whether BART's bus carries RAIN_D50. A
sensor that has never answered shows `N/A`, not `No`. Tested in headless
Firefox against D50's live httpd, with the proxy present (both `No`) and
missing (`N/A (D50)`).

### D50 monitor: rain sensor N/A (2026-10-12) [JS]

`d50-monitor.html`'s "Rain?" uses the same `rainStr()` as `bart-monitor.html`:
a sensor that has never answered shows `N/A (SBT)` / `N/A (D50)` instead of
`No`. Tested in headless Firefox against D50's live httpd, normally (both
`No`) and with RAIN_D50 blocked in the test proxy (`N/A (D50)`).

### Sky chart: horizon drawn as RTS2 interpolates it (2026-10-12) [JS]

Both monitor pages drew the horizon from `/api/horizon` as straight lines
between the nodes. In the alt-az polar projection that is wrong:
`ObjectCheck::getHorizonHeight` interpolates altitude linearly in azimuth
(nodes sorted by azimuth on load, wrapping from the last to the first), and
such a segment is a curve on the chart. A long segment like D50's 0-60 deg
(29 -> 17 deg alt) was a visible chord cutting into the sky.

`horizonOutline()` now walks each segment in steps of at most 1 deg of
azimuth, in the browser; `/api/horizon` and the daemon are unchanged
(deciding between that and asking the server for a denser table: the
interpolation is a few lines, and the server answer would be the same
numbers). Same code in `d50-monitor.html` and `bart-monitor.html`. Tested in
headless Firefox against D50's live httpd, before/after screenshots
compared; BART's page was not rendered with BART's horizon.

### Sky chart: grid, labels, bigger small chart (2026-10-12) [JS]

Worked out with the user over several iterations, then copied unchanged to
`bart-monitor.html` (the drawing code of the two pages is now identical
apart from comments). In both `renderSkyChart()`s:

- **Grid**: altitude circles every 15 deg, azimuth lines every 30 deg.
  The lines stop at the 75 deg circle so they don't crowd the zenith, except
  N-S and E-W, which run through it. All in the same light style (a stronger
  N-S/E-W was tried and rejected).
- **Labels, enlarged (lightbox) view only** (`opts.enlarged`): altitude
  15-75 deg just right of the north line, and azimuth in the astronomical
  convention (0 at S, 90 at W, as libnova/RTS2 count it), each pulled in
  from the rim by its own half-width/half-height so none touches it. Small
  and faint on purpose; the small chart has none (user call).
- **N/E/S/W letters** now sit just outside the rim. They used to ask
  `project()` for alt -8, but it clamps to the rim, so they were drawn on it
  and collided with the azimuth labels. In the lightbox they are white,
  because its margin shows the dark overlay.
- **Small chart**: no "Sky" title, 4 px panel padding (`.sky-panel`), 11 px
  letter margin (14 in the lightbox). Rendered at its shown width instead of
  a fixed 300x300 scaled by CSS, and redrawn on window resize.
- **Stars**: the small chart draws to 4.0 mag (551 stars) at 0.7x size; the
  lightbox keeps 5.5 mag (2877). The dot size is now measured from 5.5 mag
  (`Math.max(starsMag, 5.5)`), not from the limit, so lowering the limit
  only drops the faint stars instead of shrinking all of them. `?starsmag=`
  and `?starssize=` still override both views.

Tested in headless Firefox against D50's live httpd: both pages, small and
enlarged, light and dark (dark only on D50's). Nothing seen against BART's
own daemon or horizon.

### Webcams: AllSky (D65), and D50's Axis50/Sentinel50 live (2026-10-12) [JS]

- **AllSky (D65)**, the all-sky camera by the 0.65 m telescope that the old
  PHP page showed (`fr_allsky-d65.php`), is a fifth webcam on both pages
  (grid now 5 columns). `AllSkyCurrentImage.JPG`, 640x480, no equalized
  version, no stream; the title links to the camera's own page (archive).
  D50's page loads it through lascaux50's `/observ/allsky-d65/` proxy (as
  the PHP page did). BART's page loads `https://allsky-d65.asu.cas.cz/`
  directly: valid certificate, no login, and lascaux50's `/observ` login
  would not reach a browser on lascaux. `bart-monitor.html`'s webcams can now
  name their own `host` (default `/webcam/`), as D50's already could.
- **Axis50/Sentinel50 live** on D50's page, the PHP page's URLs:
  `/observ/axis50/video.cgi?resolution=320x240` and
  `/observ/sentinel50/video.mjpg`. Both send 320x240 (~24 and ~67 kB/s);
  axis50 can do 640x480 (~83 kB/s).
- **Corrected**: the page said AllSky (D65) and both live streams were left
  out because they "need HTTP auth (401)". That 401 is lascaux50's login on
  all of `/observ`, which a visitor of the page has already passed. Apache
  then swaps in each camera's own credentials (`default-ssl.conf`,
  `RequestHeader set Authorization`), so the page embeds none.
- **Not changed**: BART's own Sentinel stays without live. Its documented
  reason is different (a malformed `SID : 0` header that lascaux's Apache
  refuses, AH02429) and could not be rechecked from lascaux50, as the camera
  needs a login. D50's Axis50/Sentinel50 are not on BART's page (user call).

Tested in headless Firefox: D50's page through a local stand-in for Apache
(proxying `/observ/allsky-d65/` and the two streams with the forwarded
credentials), live on and off; BART's page with the image straight from the
camera. Not tried through Apache itself (no `/observ` login here).

### Sentinel50: live stream fix (Apache) and LED switch (2026-10-13) [JS]

- **Correction to the entry above**: Sentinel50's live stream does *not*
  work through lascaux50's Apache. It is the same camera model as BART's
  Sentinel (an old Vivotek, probably FD7131) and sends the same malformed
  headers (`SID : 0`, `Audio Mode : None`, `Privilege : 127`); Apache
  2.4.66 answers AH02429 and a 500. Yesterday's test used a Python
  stand-in that relayed them, so it missed this; Apache's error log
  shows the page's retries failing every 15 s.
- **Fix, Apache only, needs root**. Tested with a private Apache 2.4.66 on
  a high port against the real camera: 500 without, a 200
  `multipart/x-mixed-replace` stream of JPEGs with. mod_headers' output
  filter removes the headers before Apache checks them, and only for
  this path; `HttpProtocolOptions Unsafe` would relax the whole vhost.
  In `000-default-ssl.conf`, inside `<Location /observ/sentinel50>`:

      Header unset "SID "
      Header unset "Audio Mode "
      Header unset "Privilege "

  The same three lines would make BART's Sentinel work through lascaux's
  Apache, behind a login (the camera's firmware has a known security
  hole, so it is reachable only through these proxies). Not set up there,
  so `bart-monitor.html` still has no Sentinel `live`.
- **LED light** (Sentinel50's digital output 1, "Led přísvit" on the old
  PHP page): `d50-monitor.html` has an "LED light" toggle in the
  Webcams header next to "equalize" and "live", not under Sentinel50: the
  light is for all the cameras inside (user call). Amber when on, "LED light ?"
  when the state could not be read (a click then switches it on). It reads
  the state on load and every 60 s, and calls the Vivotek CGIs `cgi-bin/dido/getdo.cgi?do1` and `setdo.cgi?do1=N`
  through the same-origin proxy, so the visitor's `/observ` login guards
  them. The old button put the camera login in the URL (`user:pass@host`),
  which browsers no longer follow. The viewer account the stream proxy
  sends gets 401 on these CGIs, so this needs a second `<Location>` with
  the camera account allowed to set outputs (the old pages used `juzr`).
  It has to come after the `/observ/sentinel50` one so it overrides it.
  `WWW-Authenticate` is dropped so a refusal shows `?` instead of a login
  prompt for the camera:

      <Location /observ/sentinel50/cgi-bin/dido>
          RequestHeader set Authorization "Basic <base64 of juzr:password>"
          Header unset WWW-Authenticate
      </Location>

  Tested in headless Firefox against a fake camera endpoint (state on,
  and 401); the button's clicks and the real camera's reply format
  (`do1=0`, from the Vivotek API) are untested, since I had no access to
  the operator account.
- **Applied by the user** on lascaux50 the same day: the stream works, and
  after the placeholder got the real account, `getdo.cgi` answers 200.
- **BART's Sentinel**, same camera model, same LED: lascaux's Apache now has
  the same two `<Location>`s at `/observ/sentinel` (same camera accounts,
  behind lascaux's `/observ` login, 401 without it). `bart-monitor.html`
  has Sentinel `live: '/observ/sentinel/video.mjpg'` and the same LED
  toggle on `/observ/sentinel/cgi-bin/dido/`. Tested only against the
  fake endpoint (state off); I can't log in to lascaux's `/observ` from here.
- **Confirmed by the user** on both pages: Sentinel's live stream on BART's
  page and the LED toggle on both work against the real cameras.

### BART's page: AllSky (D65) through lascaux's proxy (2026-10-13) [JS]

- **Bug**: BART's page loaded `https://allsky-d65.asu.cas.cz/` directly.
  That server needs no login from inside the institute (where it was
  tested), but asks for one from outside, so the image stayed blank
  until the visitor clicked through and logged in there (user report).
  D50's page was not affected: lascaux50's `/observ/allsky-d65/` proxy
  served it to the user with no refusals.
- **Fix**: BART's page now uses `/observ/allsky-d65/` (image and title
  link), behind lascaux's `/observ` login. This needs, in lascaux's SSL
  vhost, the same lines as lascaux50's:

      ProxyPass /observ/allsky-d65/ http://allsky-d65.asu.cas.cz:80/
      ProxyPassReverse /observ/allsky-d65/ http://allsky-d65.asu.cas.cz:80/
- **Applied by the user** on lascaux; the image loads on BART's page.

### Targets without coordinates in the API and the editor (2026-10-19) [JS]

- **Bug**: the editor saved an empty or unreadable RA/Dec as `0`
  (`params.set ('ra', ra ?? '0')`), and `parseTargetUpdate ()` read the
  parameters with `atof ()`, so `ra=` or `ra=abc` became 0 too. Opening a
  target without coordinates (18 GLORIA) and saving a name change would
  store RA 0, Dec 0, a real position.
- **Fix**: `ra`/`dec`/`pm_ra`/`pm_dec` are parsed strictly
  (`parseCoordParam ()`). Empty means NaN, which is "no coordinates",
  stored as NULL by `ConstTarget::saveWithID ()` (db, same date). Anything
  else must be a whole finite number, or the request gets a 400 naming
  the parameter. Save and create refuse one coordinate without the
  other, and create accepts both empty. The editor sends empty fields as
  empty, and stops with a message on unreadable text or only one of the
  two.
- Tested on lascaux: the new `rts2-httpd` on a private port against the
  scratch DB `rts2test`, covering every case above through `curl`. The
  editor's `readWhatFormParams ()` was run under `gjs` with a mocked form
  for empty, sexagesimal, decimal, one-sided and garbled input. The
  editor was not clicked through in a browser.

### SBT monitor: rts2-httpd down is visible; C3 guiding panel (2026-10-24) [JS]

**Daemon down.** `fetchDeviceData ()` returned null on any failure and
`updateAllDeviceData ()` kept the previous values, so with rts2-httpd
(or Apache's proxy to it) down the page went on showing the last state
as if current. Now each status poll counts the answers from this host's
devices (`REMOTE_DEVICES` excluded): none at all means the daemon is down
- a red banner "No response from rts2-httpd since ... - the values below
are from ..., not current" and the status values, camera status lines,
sky chart and guiding panel dimmed (`body.api-stale`); some missing while
others answer means those devices are gone, and their old values are
dropped (N/A) instead of kept. Every request has an 8 s timeout
(`AbortController`), so a hanging daemon cannot stack polls up. Camera
images and webcams come from Apache and stay live either way.

**Guiding - C3.** The same panel as D50's (frame, live dx/dy strip,
scatter, readout, and the 2/8/24 h history from `/api/db/records`),
under the cameras in a `.right-panel` column as there. The history needs
`rts2-recordd` at SBT, which ran there before only as an installed binary
(`rts2-db`): site config `/etc/rts2/record` with `C3 5 guide_dx guide_dy
guide_pulse_ra guide_pulse_dec guide_snr guide_fwhm` and a `recordd REC ...` line
in `/etc/rts2/services`. Until it records, the graph says "rts2-recordd
is not recording C3.guide_dx (/etc/rts2/record)" (the daemon's answer is
"no recorded value C3.guide_dx"). `/api/db/records` leaves empty buckets
out, so a pause in guiding arrives as a jump in time; the line breaks
where two points are more than three buckets apart instead of being
drawn across (D50's page still draws across). The frame is
D50's mechanism: the script writes `/var/www/info/guide_last.jpg` beside
the camera previews (atomically, `os.replace`) and names the FITS in
`guide_last_image`, which the page takes as its reload trigger. C3's
guiding frame is the whole 64x64 window, so no cutout, and each frame is
scaled by its own 1-99.7 % percentiles with a square-root stretch (no
`pyrt-f2cj` fit as at D50), flipped so row 0 is at the bottom as in a
FITS viewer; written before the script deletes the frame. A failure
there is logged once and never stops guiding. Not guiding, the last
frame stays, dimmed. It reads values the guide
script publishes on C3 - `python/sites/sbt/guideccd.py` now creates
`guide_dx`, `guide_dy`, `guide_pulse_ra`, `guide_pulse_dec`, `guide_snr`,
`guide_scale` (3.790"/px from C3's `--wcs`), `guide_state` (acquiring,
guiding, star lost, idle) and `guide_time`. C3 also observes, and RTS2
can end the script before it resets its values, so the page counts C3 as
guiding only while `guide_time` is under 20 s old; otherwise it says what
C3 is doing from its state word - observing (exposure, object), reading
out, idle, hardware error - and when it last guided. Polls at 1 s while
guiding, 5 s otherwise.

Tested in headless Chromium against a mock daemon (`~/snaptest-bart`,
not committed): guiding (values, RMS, arcsec, plots, history with a
gap), history not recorded (the message), observing with
10-minute-old guiding values ("not guiding - C3 is observing - 120 s
exposure of ASASSN-24fs; last guided ..."), and the daemon returning 502
after the first poll (banner, dimmed values). Deployed to SBT's live page
(`/var/www/info/index.html` -> `~torman/rts2ng/web/sites/bart-monitor.html`
on lascaux, previous version kept as `.bak-20261024`). The new
`guideccd.py` ran at SBT the same evening (JS confirmed the panel live);
the frame rendering was checked on lascaux with a synthetic 64x64 star
(numpy 1.24, astropy 5.2, PIL 9.4 there) and in the mock. The guiding
values appear once the new `guideccd.py` replaces `/home/mates/guideccd.py`
(what `/etc/rts2/guideccd.py` links to); until then the panel says "not
guiding ... (no guiding since C3 started)".

**FWHM and layout (same evening).** `guideccd.py` also measures the guide
star's FWHM on the guiding frame, D50's way (area above half maximum
within 15 px of the peak; on synthetic stars 1.5/2.5/4.0 px read as
1.95/2.26/4.22 - coarse for the small C3 stars, a seeing and focus
indicator, not photometry), published as `guide_fwhm`; the readout shows
its 5-minute median in px and arcsec, and C3's camera box takes it while
C3 guides (guide frames never reach the pipeline), refreshed from the
guiding poll. The panel's grid now has the image and readout on the left
spanning two rows, the live strip and scatter beside them and the long
history right under those two - no empty band under the plot row.

**Camera FWHM was never shown at SBT.** IMGP's `fwhm_C<n>` are created
by the image pipeline: `/etc/rts2/img_process` runs `asarina-imgproc`
(MJ's, `~mates/asarina`), whose `_report_fwhm ()` calls
`rts2.Rts2Comm().doubleValue ('fwhm_C<n>', ...)`; `rts2-imgproc` runs it
as a `ConnExe`, which creates the value on IMGP. At SBT every call failed,
"module 'rts2' has no attribute 'Rts2Comm'" (`~mates/imgproc-<date>.log`):
the rts2ng `rts2` package had classic's top-level exports commented out.
At D50 it worked only because a classic copy of the module in
`/usr/local/lib/python3.11/dist-packages/rts2` shadows the package
(`fwhm_C0` 3.2-3.4 px that night). `python/rts2/__init__.py` exports
`Rts2Comm`, `Rts2Exception`, `Rts2NotActive` from `scriptcomm` again;
the other classic exports stay off, their modules are not in the
package. Takes effect at SBT with the next `rts2-python` release.

### D50 monitor: the same improvements (2026-10-24) [JS]

`d50-monitor.html` got what SBT's page got the same evening: the
rts2-httpd-down banner and dimmed left-over values (all of D50's devices
come from its own daemon, so "down" is no device answering), N/A for a
single silent device, the 8 s request timeout; the guiding panel's grid
with the history beside the frame and readout; and the history line
broken at gaps instead of drawn across a pause in guiding.

`python/sites/d50/guide.py` (what `/etc/rts2/guide.py` ->
`guidecmosdec.py` runs, the only difference being the classic
`import rts2comm`) also publishes `guide_time` and `guide_state`
(acquiring for the reference frame, star lost for a frame without
stars, guiding, idle), and resets its values when RTS2 deactivates it
(`Rts2NotActive`, which it did not catch, so its last values stayed up).
The page counts C1 as guiding while `guide_time` is under 20 s old and
otherwise says what C1 is doing (exposing, reading out, idle, hardware
error) and when it last guided; the readout starts with the guiding
state as SBT's does (green guiding, red star lost) and lists the pulses; with an older `guide.py`, which has no
`guide_time`, it keeps the old rule (values present = guiding). The repo
version runs on D50's Python as it is: its `rts2.scriptcomm` (the classic
copy in `/usr/local`) has `Rts2NotActive` and `valueCreate`.

Tested in headless Chromium against the mock with C1 as guide camera:
guiding, observing with 10-minute-old guiding values, an older guide.py
(no guide_time), and the daemon returning 502. Deployed for testing to
`/var/www/info/index-t.html` (-> `~torman/rts2ng/web/sites/d50-monitor.html`
on lascaux50); the live `index.html` follows MJ's checkout.

## Monitors: each camera's script, with the running element (2026-10-26) [JS]

bart-monitor.html and d50-monitor.html show each camera's `SCRIPT` in its
status lines, right under the target ("(idle)" when none), in their font
and colour (it is the camera's state, not part of the image caption) (D50's guide camera C1: in the Guiding panel, under its
values), with the element now running marked. The mark is
`scriptPosition`/`scriptLen`, the offsets into SCRIPT that the executor
sets on the camera as each element starts and rts2-mon highlights; the
trailing space `scriptLen` counts is left unmarked. The line reads
"Script: ...", in D50's Guiding panel "C1 script: ...", and "Script: -"
when SCRIPT is empty or `scriptLen` is 0: SCRIPT stays on the camera after
its script, and `scriptLen` says whether one runs. It is 0 after a
script's end, and since base's `DevScript::clearScriptPosition ()` also
after a script dropped midway (the executor stopping at the end of the
night left D50 showing the night's last script all day). A first version
asked EXEC for a current target instead; that hid scripts run by
rts2-scriptexec and rts2-viewer, which set SCRIPT too. Until the script
runners (the executor) run that build, a camera can still claim the last
script of a night. The camera line's "(idle)" is still the empty
`OBJECT`. Scripts with quotes (`exe "..."`, 189 of SBT's) reach the
camera's SCRIPT only from rts2ng b5b7310; until the cameras run it they
show the previous script or none.

Tested in headless Chromium against the mock (`~/snaptest-bart/mock.py`,
`mock50.py`, modes `script` and `stale`, both with no EXEC target): a
loop with its inner element marked, a quoted exe marked whole, D50's C1
guide script; "-" with SCRIPT left on the cameras and `scriptLen` 0.
Deployed for testing to the live index.html of both sites, which link to
JS's copies (`~torman/rts2ng/web/sites/` on lascaux and lascaux50; D50's
followed MJ's checkout until 2026-10-26, its `index-t.html` is gone).

## Monitors: camera state and progress; httpd sends progress times (2026-10-26) [JS]

Each camera's block on bart-monitor.html and d50-monitor.html starts with
its state, in bold, as rts2-mon's header gives it: "Exposing 23 / 60 s",
"Reading out", "Waiting for mount", "Waiting (exposure blocked)", "Idle",
plus " - hardware error" in red for DEVICE_ERROR_HW (not for
DEVICE_ERROR_KILL, which every aborted exposure sets). A thin bar under it
while exposing or reading out; D50's guide camera C1, whose values are one
line in the Guiding panel, shows a percentage instead, on a row of its own
above them (inline, its changing length moved the values after it). Redrawn every
second from the last poll, so the bar moves between the 10 s polls.

- The state is the device's bitmask /api/get always sent (`state`):
  chip 0 exposing (0x011) or reading (0x006).
- "Waiting" is the camera's own `wait_for_notbop`: an exposure it was
  asked for waits until block_exposure (BOP_EXPOSURE), a moving filter or
  a moving focuser clears (`Camera::camStartExposure`). "for mount" when T0
  reports TEL_MOVING. The camera itself stays idle meanwhile, so the state
  bits alone cannot say it.
- Exposure progress: `exposure`/`exposure_end`. Readout progress:
  `progress_start`/`progress_end`, which rts2-httpd now adds to every
  device's JSON next to `state`: the start and expected end the device
  sent with its last state change (`Connection::getProgressStart ()`/
  `getProgressEnd ()`; a camera's readout: now + pixels / pixels per
  second), null when none - the pair rts2-mon draws its bar from. Until
  the sites run that rts2-httpd, "Reading out" has no bar.

Tested: the pages in headless Chromium against the mock (mode `states`:
exposing, reading out, waiting while T0 slews, a hardware error); the new
rts2-httpd on lascaux against a private centrald and camd-dummy, 5 s
exposure: progress_start 2.1 s before and progress_end 2.9 s after the
query, as `exposure_end`; null after the image. camd-dummy sends no
readout end (no pixels per second yet), so the readout bar was seen only
against the mock.

## Monitors: live webcam streams end after 10 min (2026-10-27) [JS]

The "live" button on bart-monitor.html and d50-monitor.html is no longer
a lasting preference: the streams switch back to the 15 s snapshots after
10 minutes (`LIVE_MS`), so a page left open does not pull the MJPEG
streams all night. A live stream open in the lightbox closes then. The
cookie is now `liveWebcamsUntil`, the end time in ms: a reload keeps
only the time that is left. The old `liveWebcams` cookie is ignored. The
button's tooltip says when live mode ends. Clicking it again ends live
mode early, as before.

Tested: copies of both pages with `LIVE_MS` at 3 s in headless Chromium
(virtual time), no daemon behind them: live on, lightbox opened on a
stream, then back to snapshots with the lightbox closed and the cookie
at 0. Switching off by hand works, and a reload keeps the time that is left.

## Monitors: the camera's progress bar is always there (2026-10-27) [JS]

The thin progress bar under each camera's state (bart-monitor.html,
d50-monitor.html) was added only while exposing or reading out, so the
lines under it jumped up and down as exposures started and ended. It is
now always drawn, an empty track when the camera is idle or waiting.
D50's guide camera C1 keeps its percentage in the state line, no bar.

Tested: bart-monitor.html in headless Chromium with C1's data set by hand
(idle, exposing, reading out): the line under the bar stays at the same
height. d50-monitor.html has the same code.

## Monitors: camera bars on time and smooth (2026-10-27) [JS]

The camera bars on bart-monitor.html and d50-monitor.html showed up late
(D50's C0 at "8 s" already) and moved unevenly on some PCs. Three causes:

- The page learnt a camera's state only at the 10 s poll, so an exposure
  appeared up to 10 s after it started, and a short one could be missed.
  The cameras (BART's C1-C3, D50's C0; D50's guide camera C1 has its own
  1 s loop) are now polled every 2 s on their own (`cameraLoop`), each
  round after the last has answered, so a slow link does not pile up
  requests. A hidden tab skips these rounds. One camera's /api/get is
  about 2 KB.
- The bar compared the camera's times (`exposure_end`,
  `progress_start`/`progress_end`, the observatory's clock) with the
  visitor's clock, so a PC whose clock is off drew it ahead or behind.
  The page now estimates the offset from the `Date` header of the
  /api/get answers (`noteServerDate`, `serverNow`). The header has 1 s
  resolution, but each answer bounds the offset to [Date - received,
  Date + 1 s - sent], and the bounds of a few answers narrow to about the
  round trip. On BART only its own API_BASE answers count (RAIN_D50 comes
  from D50's clock).
- The bar moved in 1 s steps (the whole status block is redrawn once a
  second). It now carries its start and end (`data-from`/`data-to`) and
  `animateBars` moves it every frame.

The next step, if 2 s is still too slow: rts2-httpd already pushes every
value and state change over `/ws` (verified on lascaux: 101 and events,
directly on localhost:8889). Through Apache the sites' plain
`ProxyPass /images http://localhost:8889` likely needs `upgrade=websocket`,
not tested (the Basic auth). The state event carries no
progress_start/progress_end, so a page would fetch /api/get on a camera's
state change.

Tested: both pages in headless Chromium against a mock httpd whose clock
(Date header and camera times) runs 30 s ahead. A 10 s exposure and a
2 s readout show within one 2 s poll, the offset comes out at 30.0 +/-
0.15 s, and the bar follows the camera's own times. Smoothness can only be
seen in a real browser.

## Monitors: the sky chart follows the telescope (2026-10-27) [JS]

The sky charts on bart-monitor.html and d50-monitor.html moved the
telescope's cross only with the 10 s poll. Now `telLoop` polls only T0's
`TEL_` (`/api/get?d=T0&n=TEL_`, about 60 B; the whole T0 is 6 KB): every
2 s, and every 0.5 s once the position has moved by more than 0.02 deg on
the sky (tracking moves it far less in 2 s), until 5 s after it stops.
The fast rounds also fetch `TAR`, so a new target's circle shows during
the slew. A move redraws only the markers, and the telescope panel's
coordinates follow.

`renderSkyChart` draws everything else (sky, horizon, grid, stars, Sun,
Moon) into an offscreen copy kept with the canvas (`canvas.skyBg`) and
leaves `canvas.skyMarkers()`, which copies it to the canvas and draws
the cross and the target circle on top. The full chart is still redrawn
with the 10 s poll, on a resize and on the dark/light switch. A marker
redraw is one image copy and two strokes. The enlarged chart in the
lightbox keeps its own copy and gets the same marker updates.

Tested: both pages in headless Chromium against a mock T0 that stands
for 4 s, slews 30/0 -> 60/90 deg (alt/az) in 6 s, then stands. Polls ran
2 s / 0.5 s / 2 s as intended, the cross moved with each fast round, the
full chart was drawn only at load and at the 10 s poll, and the cross
ended at the right place. The lightbox was not tested.

## Monitors: zoom and pan in the enlarged image (2026-10-27) [JS]

The enlarged image on bart-monitor.html and d50-monitor.html (camera
images, guide frames, webcams) zooms with the mouse wheel, or with two
fingers or a pinch on a touchpad, from the fitted size up to 8 screen
pixels per image pixel. A small image, which fitting enlarges, zooms out
down to its own size (100 %). The image follows the mouse with no button held,
so a click still closes it. The mouse at the view's left edge shows the
image's left edge, the outer 10 % already counting as the edge. As a
result the point under the mouse stays put while zooming. The info line
gives the zoom in the camera's own pixels (100 % = one image pixel per
screen pixel); from 200 % up the pixels are drawn sharp
(`image-rendering: pixelated`). A click anywhere, the image included,
or Esc closes it (before, only a click outside the image did, besides
the x). Each opening starts unzoomed, and the 15 s refresh keeps the zoom.
The enlarged sky chart is not zoomed: it is drawn at the size shown.

Done with a CSS transform on #lightbox-img (`applyLightboxZoom`). The
wheel's default is always prevented over the lightbox, so the page under
it does not scroll and a touchpad pinch (a wheel event with ctrlKey) does
not zoom the browser.

Tested: d50-monitor.html in headless Chromium with a 1600x1200 test
image, and a 200x150 one zoomed out to 100 % and no further: wheel zoom about the mouse, panning to both corners, the 8x
limit, zooming back out to the fitted size, closing by a click on the
image and by Esc, and a screenshot of the zoomed view. bart-monitor.html
has the same code. Not tried with a real mouse or touchpad.

## Dashboard split: index.html is the image archive, status.html the rest (2026-10-27) [JS]

The page rts2-httpd serves at its root (`/images` on the sites, the
"image archive" link of the monitors) mixed the device values, Send
command, Recent messages and the night/image browser. Now:

- `index.html` + `app.js`: only the images (the night browser, landing
  on tonight). Titled "RTS2 Image Archive". Schematics and more functions
  will go here later.
- `status.html` + `status.js`: Send command, the device values and
  Recent messages, with the live `/ws` push, as before.

The page headers (index, status, targets, target, records) are the same
on every page: the title left, the links to all five pages centred, in
the same order (image archive, target list, target editor, telemetry,
status), and the page's badge (status: live, telemetry: values) right.
The current page's link stays, marked (`.back-link.current`,
`aria-current="page"`). The header is a three-column grid so that the
links are centred whatever is beside them; under 700 px they go to a
row of their own. "device monitor" became "image archive".

Deployment: on both sites rts2-httpd runs with `--static-dir
/home/mates/rts2ng/web/static`, MJ's checkout, not the packaged
`/usr/share/rts2-web/static`. It reads the files on each request, so a
pull there brings this live without a restart or a release.

Tested: both pages in headless Chromium against a mock of /api/getall,
/api/messages and /api/db/{current-night,night,images}: each shows only
its own parts, with no errors. Header screenshots of all five pages (1200 px
and 400 px wide) against SBT's real rts2-httpd.
Not run against a real rts2-httpd.

## status.html: send a command, as in rts2-mon; /api/cmd (2026-10-27) [JS]

The Send command panel of status.html has a new first row: device
(centrald too), command line, Send. It sends a raw command ("stop",
"killall", "on", ...), as rts2-mon's command line does, and shows
"OK", or "failed (status): <the device's error text>". The set-value
row is below it, as before. Both device lists now keep their selection
when a device turns up (renderAll rebuilt them).

rts2-httpd: `/api/cmd?d=DEVICE&c=COMMAND`, ported from classic api.cpp's
`cmd`. The same write check as set (`--auth-file`, per device;
`centrald` for centrald). A command with a newline is refused (it would
be a second command). The request is held until the device answers, as
classic does by default, and returns
`{"device","command","ok","status","error"}`; `error` is the rest of
the device's `-N <text>` line. Not ported: `async=1` and the commands
to HTTPD itself (`autosave`).

- `CommandHttp` holds the suspended request. Its answer goes through
  `commandAnswered()` and the wakeup eventfd to `pollSuccess()`, never
  out of the kernel's callback.
- A device that drops keeps the command: the kernel sends it again after
  the reconnect (`Connection::reconnectSocket`), and only the link to
  centrald fails it (`failSentCommand`). So `HttpD::idle ()` answers a
  request after 30 s (`CMD_TIMEOUT`) with status -7, "no answer from the
  device within 30 s". The command itself stays queued: if the device
  comes back, it still gets it, as with any other client.
- A command deleted unanswered answers from its destructor. `~HttpD`
  detaches the pending ones first, because the kernel deletes them with
  their connections after the HTTP server is gone.

Also fixed: value and state pushes over `/ws` carried an empty device
name for centrald (its connection has none), so status.html showed
centrald's changing values as an extra card with no name. They now say
"centrald", as /api/getall does (`pushName`).

Tested here, against a private centrald and camd-dummy C0, with
rts2-httpd built without DB: `info` OK; `nonsense` failed (-1) "unknow
command nonsense"; centrald `standby` and `on` OK; missing parameter,
unknown device and a newline all give 400. A camera stopped with SIGSTOP
and then killed: answered after 33 s with -7, and httpd carried on. The
page in headless Chromium: the four commands through the form with the
right results, and the device cards named C0 and centrald (no nameless
card). Built clean with GCC 15 here and GCC 12 on lascaux.

## status.html: rts2-mon's status list; the old status page is devices.html (2026-10-27) [JS]

The page with the device values, Send command and Recent messages is
now `devices.html` + `devices.js` ("devices" in the headers). The new
`status.html` + `status.js` is rts2-mon's "status" item: centrald and
every device, one row each, with its state text, its blocking and the
state bits in hex, coloured as rts2-mon colours them. It is refreshed
every second (every 10 s in a hidden tab) from the new `/api/status`.
That is plain HTTP, because the sites' Apache passes no WebSocket. If
rts2-httpd stops answering, the rows dim and the badge says since when.
Header links on all six pages: image archive, target list, target
editor, telemetry, devices, status.

rts2-httpd: `/api/status` lists centrald, then every device, with
`name`, `type`, `state`, `bop` (the full BOP state), `statestring`
(`Connection::getStateString (true)`, the very text rts2-mon prints),
`progress` (%, null) and `level`. `level` is rts2-mon's colour
(`NCentraldWindow::printState`, ported as `statusLevel ()`): failure,
warning (bad weather), priority (moving, exposing, ...), ok.

The page takes the blocking out of the state text and shows it as
badges: solid when the device blocks it itself (state bits, rts2-mon's
`| BLOCK EXPOSURE`), outlined when another device does (full BOP state,
rts2-mon's `# BLOCK EXPOSING`). It also shows the dome's
BLOCK_OPEN/BLOCK_CLOSE and STOP.

A camera that had read out its image (chip state `CAM_HAS_IMAGE` alone)
got no chip text from `Connection::getCameraChipState`, so its line
started " | 5 s LIGHT"; fixed in base the same day, it is "0 idle" now
(base/HISTORY.md, "A camera holding an image is idle").

Tested here, against the private centrald + camd-dummy C0: a 5 s
exposure shows C0 blue (priority), "0 EXPOSING ... 51%" and its own
BLOCK TELESCOPE MOVEMENT, and centrald the same block from another
device; after the readout the rows go back to green with no badges.
Screenshots at 1200 and 420 px. Not tested: the stale display. SBT's
rts2-httpd has no /api/status until the next release.

## devices.html: Recent messages under Send command, newest first; pushed message times (2026-10-27) [JS]

- Recent messages is now the second panel, right under Send command.
- The list was in two orders at once: the initial load (/api/messages,
  oldest first) was appended, oldest at the top, while pushed messages
  went on top; trimming to 50 cut the bottom, which were then the newest
  loaded ones. Now every message goes on top, so the list is newest first
  throughout.
- rts2-httpd's `/ws` message push wrote its time with a plain `<<`
  (`1.79313e+09`), minutes off, so pushed messages got wrong times and
  stood out of order. Now `jsonTime ()`, as base/STATUS.md requires for
  every time in rts2-httpd's JSON.
- The made-up "state -> ..." lines (from `/ws` state events, task 8's
  first pass) are gone: they were not messages, vanished at every reload,
  and status.html shows the states.

Tested here against the private centrald + camd-dummy: an exposure
started after the page loaded; its pushed messages have the right times
and the list is in order, newest first, with the panels in the new order.

## monitor.html: rts2-mon as a web page; /api/monitor (2026-10-27) [JS]

A first version, to see whether it is worth having. Laid out as
rts2-mon: the device list on the left (centrald, the devices, "status"),
coloured as /api/status's level; the selected device on the right, with
its state line and a progress bar, its values, and the description of
the value under the mouse (rts2-mon's "D:" line); the messages under
them, oldest at the top and the newest at the bottom, as rts2-mon has
them; a command line for the selected device (/api/cmd); a status bar
with centrald's state, LST (longitude from /api/horizon) and UTC.
"status" shows status.html's table. The page fills the window.

- Values: rts2-mon's texts with its W (writable), FITS (name in
  colour), warning and error colours, SCRIPT with the running part
  marked, and a flash when a value changes. Filters as rts2-mon's: all /
  FITS / writable, debug values hidden by default (rts2-mon without
  --show-debug), and a name search.
- Editing: click a W value. A selection gets its options (/api/selval),
  a bool true/false, anything else a text field with the raw value
  (doubles shortened to 15 digits, a min/max value only its value). Set
  goes through /api/set; Esc or Cancel closes the editor.
- Up/Down walk the device list (not while typing). The selected device
  is in the URL (`monitor.html#C0`).
- Polled over plain HTTP, every second (the selected device and
  /api/status) and every 2 s (/api/messages), 10 s in a hidden tab.
- The blocking flags and their helpers moved from status.js into
  `rts2status.js`, shared by status.html and monitor.html.

rts2-httpd: `/api/monitor?d=DEVICE[&desc=1]` gives the device's state
text, progress and level, and per value `n`, `v` (rts2-mon's text:
`monitorValueText ()`, a port of `NDeviceWindow::printValue`: RA/Dec and
Alt/Az through libnova by display type, azimuth as the config shows it,
a selection as index and name, else `getDisplayValue ()`), `raw`, `t`
(type), `w`, `f`, `dbg`, `lvl`, for SCRIPT `sp`/`sl`, and with desc=1
`desc`. C0's answer is 6.7 KB, 9.1 KB with the descriptions, which the
page asks for once per device.

Tested here, against the private centrald, camd-dummy C0 and
teld-dummy T0 (headless Chromium): screenshots of C0, T0 (RA/Dec,
offsets, PM in arcsec, as rts2-mon shows them) and the status item;
exposure edited 2 -> 3 through the editor; IMAGETYP's options; `expose`
from the command line, OK, with the state line exposing and the messages
in order; the search. Not tried: a real site's devices (SBT's rts2-httpd
has no /api/monitor until the next release), the keyboard.

Fixed the same day: for a device with many values, the description
line was squeezed half under the panel's border. The values area had
flex-basis auto (its full height), so everything shrank with it; now
only it shrinks (`flex: 1 1 0`), and the head, filters and description
keep their height.

## Image archive: pages, filters, enlarged images, downloads (2026-10-27) [JS]

index.html (app.js), one night's view:
- The images come a page at a time: 24/48/96/192 per page (kept in the
  browser), a pager above and below the thumbnails.
- Filters: a click on a target in the observations shows only its images
  (the row is marked; a second click, or the x of the target chip, shows
  all again); checkboxes for the cameras and the photometric filters,
  each with its count, and "all" and "none" for each (none: then tick
  the ones wanted; the counts stay, no images are shown). The counts are those the other filters let
  through, so a count is what ticking it shows.
- A thumbnail (200 px) shows the file name, UT time, exposure, filter,
  camera and target. A click opens it big (preview at 2048 px) in the
  zoomable viewer of the site pages, now `lightbox.js`, shared: wheel or
  touchpad zoom from the image's own size or the fitted size up to 8
  screen pixels per image pixel, pan with the mouse, click or Esc closes.
- Downloads: the arrow on a thumbnail downloads that FITS file; the
  checkboxes select images (both at the right of the caption, one above
  the other, so that the image itself only opens the viewer) (kept across pages and filters), with "select
  this page", "select all matching" and "download selected (.tar)". The
  tar is posted as a form, so the browser saves it as it comes, however
  large.

rts2-httpd:
- `/api/db/images?...&limit=N[&offset=][&tar=ID][&cameras=a,b]
  [&filters=a,b]` gives one page and the facets (`dbImagesPage ()`),
  from the DB rows only. Without `limit` the old whole-night list, as
  before.
- `/fits/<archive path>`: the FITS file as it is, sent from the fd.
- `POST /api/download[?name=]`: the archive paths in the body, one per
  line or as a form's `paths` field, answered with one tar (ustar,
  streamed: `TarStream`), at most 5000 files. The paths go through
  `resolveImagePath ()` (preview.cpp's check, now public): no "..", no
  absolute path, nothing outside --images-dir, files only. Its prefix
  compare also let `/images2/...` through for an images-dir of
  `/images`: fixed.
- MHD drove only responses that fit one socket write: in external epoll
  mode a connection still sending gets no new epoll event while its
  socket stays writable, and MHD asks to run again through
  `MHD_get_timeout ()` instead, which nobody asked. A FITS download
  stopped after 4 kB. Now `HttpD::idle ()` runs MHD when that is due and
  keeps the loop's wait within MHD's timeout (Block::getTimeout () added
  to base to get the loop's own back). 3 MB now take 14 ms on lascaux;
  rts2-httpd idles at 0 % CPU.
- Preview cache: thumbnails are now 200 px and the enlarged images 2048
  px; both are cached on first view (the sites' cache is
  `<images-dir>/.cache`), so the first view of a night is slow and the
  cache grows (measured: 0.7-0.8 MB a 2048 px JPEG, 3 kB a thumbnail).
- The archive holds the raw frames (no DARK/FLAT/BIAS keywords; `PROC`
  is RTS2's processing status). The site pages show corrected images:
  asarina-imgproc (MJ's pipeline, /etc/rts2/img_process) corrects each
  new frame into a temporary `-df.fits`, makes the web JPEGs from it
  (cxtoweb.sh, rts2-image) and lets RTS2 archive the raw frame. The
  archive's previews are now corrected the same way, see "Image archive:
  dark/flat corrected previews" below.

Tested on lascaux, a test rts2-httpd (own centrald, no devices, the real
`stars` DB and /images, read-only, cache in ~torman) through a tunnel,
SBT's night of 2026-10-27 (340 images, 3 cameras, 7 filters): paging,
the target filter (18 images, 12 without C3), cameras, 24 per page,
selection over pages, select all matching (303), the lightbox (open,
zoom 264 %, Esc). Downloads: one FITS (3 MB, identical to the file), a
tar of 3 (72 MB, files identical) and of all 36 C3 frames (114 MB, 0.14
s on lascaux), as lines and as a form; refused: "..", a directory, an
empty list.

## Image archive: dark/flat corrected previews (2026-10-28) [JS]

The archive's previews can be dark/flat corrected with the masters
asarina (MJ's pipeline) corrects the site pages' images with, chosen by
asarina's own rules (asarina/pipeline/image.py, ImageProcessor), ported
to `web/httpd/src/calib.cpp`:

- The chip of the frame and of every master: pyrt's camera registry,
  asked through `pyrt-chip-id` (not copied: MJ's registry stays the only
  one), its answer kept per combination of the seven header fields it
  reads (CCD_NAME, CCD_SER, CCD_TYPE, DESCRIPTION, INSTRUME, PRODUCT_ID,
  SERIAL). A master directory is one call; after that nothing.
- The masters of a year: `<calib-root>/<chip>/<year>/`, then
  `<calib-dir-template>/<CCD_NAME>/` (`{year}` in the template), the first
  loaded of a key kept. A dark: same chip, CCD_TEMP rounded to 5 C groups,
  same BINX, the closest EXPTIME within 3 s; a flat: same chip, FILTER and
  BINX; else the other years, -1, +1, ... +-5, loaded on top.
- A windowed frame takes the masters' window (CCDSEC, else LTV1/LTV2); a
  full frame is cut to the chip's active area (asarina's CAMERA_CROPS,
  copied: four chips).
- Not as asarina, which corrects nothing without both: what is found is
  applied and the rest named ("dark only, no flat for halpha (chip ...)");
  with neither, the raw preview ("raw: no dark (chip mi6166, -20 C, bin 1,
  120 s), no flat for r ...").

rts2-httpd: `--calib-root`, `--calib-dir-template`, `--chip-id-cmd`
(default pyrt-chip-id); with neither directory, previews are raw. At SBT:
`--calib-root /home/mates/calib --calib-dir-template
"/home/mates/flat{year}/"` (asarina's paths; rts2-httpd's user must read
them and run pyrt-chip-id). `/preview/...&cal=1` asks for the corrected
variant (without cal, raw as before); the answer says what applied
(`X-RTS2-Calibration`) and which masters (`X-RTS2-Masters`). The cache
keeps the variants apart: `<frame>.ps<size>.q<q>.raw.jpg` and
`.cal.jpg`, the latter with a `.cal.jpg.txt` (the calibration text and
the masters). The masters are read in strips, the
stretch is the kernel's (Image::getChannelGrayscaleByteBuffer: low
quantile white, rows flipped). A cached corrected preview does not notice
new masters.

index.html: "Previews: dark/flat corrected / raw" (corrected by default,
kept in the browser); the enlarged image says what was applied
(`openLightboxFetch ()` in lightbox.js reads the header).

Found on the way: SBT's C3 is now chip mi2065 (1536x1024), masters in
/home/mates/calib/mi2065/2026; /home/mates/flat2026/C3 holds the previous
chip's (mi0008, 2252x1509). C1's Halpha and SII flats are only in
calib/mi6166/2026.

Tested on lascaux (the test rts2-httpd, SBT's night of 2026-10-27), each
camera/filter/exposure combination of the night (25) against asarina's
own find_best_dark/find_flat (read-only, same configuration): the same
masters for all 25. The year fallback with a staged tree (C3's 2026
masters as 2024, no 2026): the same 25 again, C3 from 2024, C1/C2 raw on
both sides. Pictures: C1 cut to its active area and flat, C3 flat
(vignetting and a dust ring gone). First corrected preview of a camera
1.5-2 s (directories indexed, the registry asked), then like the raw one.
The page: the switch, cal=1/0 on thumbnails and the enlarged image, the
calibration text in it.

## Image archive: binning in the captions, a purge of the preview cache (2026-10-28) [JS]

- Captions: UT time, exposure, filter, binning ("1x1"), camera. The DB
  has no binning, so `dbImagesPage ()` reads BINX/BINY from the FITS
  headers of the page's images only (cfitsio, the header alone): 0.05 s
  for 48, 1.5 s for 192 with headers not yet in the disk cache.
- "purge preview cache" (the selection bar, with a confirmation): GET
  `/api/purge-cache` deletes the whole preview cache, raw and corrected,
  every night, on a worker (`purgePreviewCache ()`), and says how many
  files and MB went. Only what the cache writes goes, `*.jpg` and
  `*.jpg.txt`, then the directories left empty; no symlink is followed,
  and the cache directory itself stays - so a --cache-dir pointed at the
  archive by mistake loses no FITS file. A write, behind the same login
  as /api/set: with --auth-file the login needs `preview-cache` (or `*`)
  among its devices. `DbResult`, its queue and drain are no longer
  DB-only: the purge answers through them.
- The raw previews are now cached as `*.raw.jpg` (they had no marker);
  older raw previews are no longer found - a purge clears them.

Tested on lascaux (the test rts2-httpd): binning 1x1 for all three
cameras of the night, in the API and the captions; a purge with a decoy
`keepme/decoy.fits` and a symlink into the archive in the cache: 9
previews deleted, the decoy, the symlink and the archive left; the
button (96 files, 0.2 MB).

## Previews stretched from the good data section, DATASEC (2026-10-28) [JS]

A preview's stretch (the quantiles) must come from the image's real data
only, not from an overscan; the MI cameras' frames have one (C1/C2: 29
columns and 10 rows of overscan and dark edge at the start, 17 columns and
19 rows at the end). The area is the FITS keyword `DATASEC`, which camd
writes into every frame when started with `--datasec X:Y:W:H` (the good
area in unbinned detector pixels, 0-based), adjusted for a window and the
binning (devcliimg.cpp). Tested with camd-dummy (--datasec 10:5:180:140,
200x150): full frame `[11:190,6:145]`, window 50,40 100x80
`[1:100,1:80]`, binning 2x2 `[6:95,3:72]`.

- Raw previews already used it: the kernel's histogram, which the
  stretch comes from, takes only DATASEC (Image::getChannelHistogram).
  With a C1 frame given `DATASEC = '[30:4127,11:4108]'` the raw preview
  is stretched from the sky (full contrast, the vignetting shows); the
  same frame without it is a dull grey, the overscan setting the black
  level.
- The corrected previews (preview.cpp) now take their quantiles from
  DATASEC as well, within the chip's active-area crop.
- No frame has DATASEC so far: SBT's cameras are not started with
  --datasec. Measured on 2026-10-27's frames (column and row medians),
  C1 and C2 need `--datasec 29:10:4098:4098` (the same area as asarina's
  CAMERA_CROPS for mi6166/mi6167); C3 (mi2065, G2-1600, 1536x1024) has no
  overscan and needs none. That is /etc/rts2/devices at SBT (root), and a
  restart of C1 and C2. Frames taken before keep the whole-frame stretch.

Seen on the way (base, not fixed): a script value the operand parser
cannot read (`WINDOW=50:40:100:80`, also quoted with spaces) throws
`UnknowOperantMultiplier`, which nothing catches - rts2-scriptexec aborts
(SIGABRT); classic has the same code (base/STATUS.md, open items).

## Image archive: no frames around the images, FITS opened rather than saved (2026-10-28) [JS]

- Thumbnails have no black background any more (transparent, the
  caption box's colour shows); the enlarged image (lightbox.js) has no
  white border.
- The arrow on a thumbnail ("open this FITS file") opens the FITS file instead of saving it: no
  `download` attribute, and `/fits/` answers `Content-Disposition: inline`
  with `Content-Type: image/fits` (RFC 4047; the type desktops associate
  FITS viewers with - shared-mime-info has it). A page cannot start an
  application itself: the browser opens it in its associated application
  or asks, as set (Firefox asks or opens; Chrome saves it and opens it on
  a click, with "always open files of this type"). `/fits/...?download=1`
  still sends it as an attachment.

The site monitor pages' own lightbox (web/sites/*-monitor.html) keeps its
white border; only index.html's lightbox.js was changed.

## Image archive: page jumps and newest first (2026-10-28) [JS]

- The pager (above and below the thumbnails): first, previous, the page
  number in a field (Enter or leaving it goes there; past the last page
  means the last), next, last; the buttons that cannot go anywhere are
  disabled.
- "oldest first / newest first" beside it, kept in the browser. The order
  is rts2-httpd's (`/api/db/images ...&order=desc`, `ImageQuery::
  newestFirst`): the night's images are reversed before the page is cut,
  so page 1 holds the latest frames.

Tested on lascaux, 2026-10-27's night (553 images, 12 pages of 48): the
API in both orders (16:46:35 first, 00:34:18 first), last and first page
with their buttons disabled, page 4 by number (images 145-192), page 99
(the last), newest first kept and both pagers following.

## Previews: JPEG quality 98 for the enlarged images (2026-10-28) [JS]

The enlarged images showed JPEG artefacts: every preview was quality 85.
Measured on a C3 frame at 2048 px against a quality-100 encoding: around
a star, pixels off by up to 19 grey levels at 85 (the dark core speckled,
the halo blocky), 7 at 95, 4 at 98; in the plain sky the blocks drown in
the noise. Size: C3 (1536x1024) 0.8 / 1.2 / 1.5 MB, C1 (2048x2048) 1.4 /
2.3 / 3.0 MB at 85 / 95 / 98.

Now a big preview (ps over 400, the enlarged images at 2048) is quality
98, a thumbnail stays 85 (never zoomed, most of the cache by count).
`/preview/...&jq=N` sets it (10-100). The quality is part of the cache
name (`.jq98.`), so old 85 previews are not found any more - a purge
clears them; the cache grows by about 2x per enlarged image viewed.

## nightreport.html: rts2-nightreport -i as a page (2026-10-28) [JS]

"nightreport" in the headers, after monitor. What `rts2-nightreport -i`
prints for a night (the current one, or one picked): every observation's
line, then its images' lines or "--- no images ---", in monospace,
refreshed every 30 s (2 min in a hidden tab); new rows flash. Shown as
what fits the window without scrolling (the default; the list's height is
measured to the window's bottom, less what the page still overflows by,
and rows dropped until it does not scroll), the last N rows (editable), or
all - kept in the browser. A cut inside an observation puts that
observation's line on top. In an image row the acquisition time
(classic's Timestamp, found in the line by its format) is a link: the
mouse over it shows the preview (400 px) beside it - "preview on
mouse-over", on by default, kept in the browser - and a click opens it
big (lightbox.js), corrected or raw as chosen in the image archive; the
file name is its tooltip. The rest of the row does nothing.

rts2-httpd: `/api/db/nightreport?year=&month=&day=` gives the rows with the
very lines classic prints - Observation::printObsHeader () and
Image::print (DISPLAY_ALL), both in rts2ng already - plus each image's
preview path, file name and time (`dbNightReport ()`). The images: one
query for the night, grouped by observation, sorted by the exposure start
with its microseconds (the set is sorted by img_date, whole seconds, so
frames the cameras start together came in any order; classic's listing
too).

Tested on lascaux (the test rts2-httpd, 2026-10-27): the same 37
observations and the same images per observation as classic's
rts2-nightreport -i -n 2026-10-27 (590 rows); 134 kB, 0.05 s. The page:
fit 31 rows at 713 px and 47 at 1013 px, neither list nor page scrolling;
the last 20 and 5 rows, each starting with its observation's line; all
591 rows; the preview over the time only (all 29 image rows of the view
linked, nothing over the row's text), off with the checkbox; the click on
the time into the lightbox, on the row's text nothing.

## Corrected previews at D50 (2026-10-28) [JS]

asarina runs at D50 too (the site page's C0 image is a `-df.fits`), with
its default paths - the same as SBT's: `--calib-root /home/mates/calib
--calib-dir-template "/home/mates/flat{year}/"` for rts2-httpd there.
C0 is chip andor46 (1024x1024, no overscan, needs no --datasec); its
masters are in /home/mates/calib/andor46/<year>/ (2016-2026). D50's
/home/mates/flat2026/ is the old single-camera layout without camera
directories, which asarina (and calib.cpp) do not read for a frame with
CCD_NAME.

Tested on lascaux50 with a test rts2-httpd (built in ~torman, own
centrald, the real DB read-only), every camera/filter/exposure
combination of 2026-10-27 (32, all C0) against asarina's own choice: the
same masters for all 32. Exposures with no dark within 3 s (28, 30, 50 s)
get "flat only"; asarina finds no dark for them either. A corrected C0
frame loses its dust rings and the dark patch in the middle.

## nightreport hover without gaps; devices.html polls without the push (2026-10-29) [JS]

- nightreport.html: the time link (`.nr-name`) was an inline span, only
  as tall as the font, so between two rows the mouse was over no time and
  the preview blinked out. Now it is as tall as the row (inline-block,
  the list's line height in whole pixels, 17 px): moving the mouse down
  the times changes the preview without a gap.
- devices.html said "reconnecting" for good at the sites: its live
  values come over `/ws`, which rts2-httpd answers (101 on
  localhost:8889/ws on lascaux), but the sites' Apache proxies `/images`
  with a plain `ProxyPass /images http://localhost:8889`, and Apache 2.4
  passes no WebSocket without `upgrade=websocket`. Now, while /ws is not
  open, the page polls api/getall and api/messages every 2 s (10 s in a
  hidden tab): changed values are updated and flash as pushed ones do,
  messages not shown yet are added, each once (`shownMessages`, as a
  message may come both ways). The push is still tried with backoff up to
  30 s and takes over when it opens. Badge: "live", "polled", "no
  answer". `upgrade=websocket` on the sites' ProxyPass would give the
  push back; not needed for the page to work.

Tested in headless Chromium against a mock (values changing with each
request, a message added every 2 s, /ws refused): devices.html "polled",
the changing value updated with each poll, the messages newest first and
each once; nightreport.html 12 times of 17 px with 0 px between them, a
walk down the time column passing all 12 with no pixel on none. Both are
page files only: a pull in MJ's checkout on each site puts them live, no
release.

## status.html, monitor.html: devices in rts2-mon's order (2026-10-29) [JS]

The devices came in rts2-httpd's connection order (at SBT: C3 among the
sensors, C1 and C2 at the end). Now both pages sort them as rts2-mon does
by default (ORDER_TYPE, `sortConnType` in base/monitor/include/
nmonitor.h): centrald first, then by device type, then by name -
`sortLikeMon ()` in rts2status.js, from the `type` /api/status already
sends, so no rts2-httpd change. Checked with SBT's live device list:
centrald T0 C1 C2 C3 DOME KAFKA F1 F2 F3 BARTUPS CLOUD DISK_BART
RAIN_BART RAIN_D50 SCH WEATHER_BART EXEC IMGP QUEUE, on both pages. Page
files only: a pull in MJ's checkouts deploys it.

## nightreport.html: a filter by camera (2026-10-29) [JS]

A checkbox for each camera with images in the report ("cameras C1 C2
C3", shown from two cameras up), all ticked by default; the unticked ones
are kept in the browser, so a camera new in a report starts ticked. The
camera is the first word of classic's image line, so no rts2-httpd change.
The images of unticked cameras go; every observation stays, one left
without images says "--- no images ---" as classic does, and the fit /
last N / all modes count only the rows shown. Rows coming back with a
change of the filter do not flash as new.

Tested in headless Chromium against a mock (four observations, C1-C3):
all, without C3 (an observation of only C3 frames left with "--- no
images ---", nothing flashing, ["C3"] kept), only C2, only C2 with the
last 3 rows. Page files only: a pull in MJ's checkouts deploys it.
