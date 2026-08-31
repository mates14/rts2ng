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
  in `base`: see "Time display mode: `timeDisplay_t`" in `base/STATUS.md`.
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
