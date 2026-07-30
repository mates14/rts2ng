# RTS2 (rts2ng)

**RTS2 (Remote Telescope System, 2nd version)** is a distributed control
system for fully autonomous, robotic astronomical observatories. It runs the
whole observation loop unattended: watching the weather, opening and closing
the dome, pointing and tracking the mount, driving cameras/focusers/filter
wheels, selecting and scheduling targets, and shutting everything down safely
when conditions turn bad — then doing it again the next night, indefinitely,
without a human in the loop.

RTS2 is not a planetarium or an interactive telescope-control GUI. It's the
software that *runs* the observatory; other tools (including RTS2's own
clients, and third-party software such as the
[Stellarium](https://stellarium.org) RTS2 plugin) talk to it over its wire
protocol or JSON API to observe or supervise what it's doing.

This repository, **rts2ng**, is a CMake-based port of RTS2: the classic
codebase carried over file by file, reviewed and lightly modernized (real
bugs fixed, dead legacy branches dropped) rather than rewritten from
scratch, built next to (not in place of) the classic autotools-based
[RTS2 tree](https://github.com/RTS2/rts2). It exists because the classic tree
has no way to configure out drivers you don't need, and no clean separation
between the device-facing backend (network protocol + drivers, no database)
and the scheduling/database-bound frontend. See [Relationship to classic
RTS2](#relationship-to-classic-rts2) below for what that means in practice,
and each subtree's `STATUS.md` for the full porting log.

## How it works, briefly

RTS2 is organized as a bus of independent daemons that talk to each other
over a small text-based wire protocol, brokered by a central daemon:

- **`centrald`** — the hub. Tracks every connected device, aggregates
  observatory-wide state (day/night, weather-safe/unsafe, on/standby/off),
  and brokers logins and device discovery for everything else.
- **Device daemons** — one process per physical device: cameras (`camd`),
  mounts (`teld`), domes (`dome`), focusers (`focusd`), filter wheels
  (`filterd`), and assorted environment sensors (`sensord`) — weather
  stations, cloud sensors, UPS monitoring, and similar. Each exposes its
  state and controls as typed, introspectable values over the wire protocol.
- **Clients** — connect to `centrald` and the device daemons to observe or
  drive the system: `rts2-mon` (a full ncurses TUI showing live state of
  every device), scripting/CLI tools, and (in `gui/`) a Qt camera viewer.
- **Database/scheduling layer** (`db/`) — target and observation persistence
  over PostgreSQL, plus `rts2-executor`, the daemon that selects and carries
  out observations of database-defined targets. This is the optional
  frontend on top of the device-facing backend; a bare `base` install runs
  devices and lets you drive them manually without ever touching a database.

## Repository layout

This repo currently holds three independent CMake projects, each buildable
and packageable on its own:

| Directory | What it builds | Depends on |
|---|---|---|
| [`base/`](base/) | Backend: wire protocol/kernel, `centrald`, and every device driver (camera, mount, dome, focuser, filter wheel, sensors), plus `rts2-mon` | — |
| [`db/`](db/) | `rts2db` (PostgreSQL-backed target/observation schema) and `rts2-executor` | `base` (nested via `add_subdirectory`) |
| [`gui/`](gui/) | `rts2-viewer`, a standalone Qt5 camera-viewer client | `base` (nested via `add_subdirectory`) |

Each has its own `STATUS.md` with a detailed, chronological porting log —
what was ported, what was deliberately deferred and why, what was
smoke-tested and how. Start there for implementation-level detail; this
README stays at the "what is this and how do I build it" level.

## Requirements

Common to all three subtrees:

- CMake >= 3.16
- A C++17 compiler (g++)
- [libnova](http://libnova.sourceforge.net/) (celestial-mechanics library)
- libcfitsio
- libncurses
- libusb-1.0

`base/` additionally, only for specific opt-in drivers (each gated behind
its own CMake cache variable, skipped by default — see `base/STATUS.md` and
`base/CMakeLists.txt`):

- Finger Lakes Instrumentation (FLI) camera/focuser/filter-wheel SDK
  (`-DBASE_FLI_SDK_DIR=...`)
- Andor camera SDK (`-DBASE_ANDOR_SDK_DIR=...`)
- Moravian Instruments (gxccd) camera SDK (`-DBASE_GXCCD_SDK_DIR=...`)
- Martin Jelínek's `paracl`/`libmks3` (Software Bisque Paramount ME/MYT
  mount protocol) (`-DBASE_PARACL_DIR=...`)

None of these vendor SDKs are redistributed in this repository; the drivers
that need them are simply skipped at configure time if the corresponding
path isn't provided.

`db/` additionally requires:

- PostgreSQL client development headers and `ecpg` (embedded-SQL
  preprocessor) — `libpq-dev`, `libecpg-dev`
- libxml2

`gui/` additionally requires:

- Qt5 (Widgets)

On Debian/Ubuntu, the build/packaging dependencies are declared in each
subtree's `debian/control`; roughly:

```sh
sudo apt install build-essential debhelper devscripts dpkg-dev cmake \
    libcfitsio-dev libncurses-dev libnova-dev libusb-1.0-0-dev \
    libpq-dev libecpg-dev libxml2-dev qtbase5-dev
```

## Building

Each subtree builds independently with plain CMake:

```sh
cd base
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

`db/` and `gui/` nest the `base` source tree via `add_subdirectory` (see
`DB_BASE_DIR`/`GUI_BASE_DIR` in their respective `CMakeLists.txt`, both
defaulting to `../base`) rather than depending on an installed `base`
package, since `base` doesn't have an install/export step yet:

```sh
cd db    # or gui
cmake -S . -B build
cmake --build build -j$(nproc)
```

### Debian packages

Each subtree is also a Debian source package (`base/debian`, `db/debian`,
`gui/debian`), producing `rts2-base` (+ `rts2-drivers-fli` /
`rts2-drivers-gxccd` if the vendor SDKs are available), `rts2-db`, and
`rts2-viewer` respectively. `buildme.sh` at the repository root drives all
three with `dpkg-buildpackage` and collects the resulting `.deb`s into
`dist/`.

## Relationship to classic RTS2

RTS2 has a long history: it began as a Python tool ("RTS1") for the BART
telescope, was rewritten in C, and then rewritten again in C++ as the
autotools-based codebase at [github.com/RTS2/rts2](https://github.com/RTS2/rts2)
that has run observatories worldwide for two decades.

This repository is a parallel CMake port of that backend (and, in `db`/`gui`,
the database/scheduling and GUI layers), built by carrying the classic tree
over file by file, bottom of the dependency graph first. It is **not** a
mechanical translation, nor a rewrite from scratch: each file is imported,
reviewed, and lightly modernized as it's ported (dead legacy-platform
branches stripped,
`NULL` → `nullptr`, raw owning pointers → `unique_ptr` where safe, real bugs
found and fixed — see each subtree's `UPSTREAM_BUGS.md` where present), and
genuinely optional/vendor-tier subsystems (e.g. TLE/SGP4 satellite
propagation, SIMBAD name resolution, SEP star-finding) are deliberately
deferred until a real consumer needs them, rather than ported speculatively.

The motivating design goals, spelled out in `base/STATUS.md`:

- **Configurability** — the classic tree builds every driver unconditionally
  from a single autotools invocation; this tree builds only what you ask
  for, with vendor-SDK-gated drivers opt-in via CMake cache variables.
- **Backend/frontend separation** — device control (no database) and
  scheduling/persistence (database-bound) are two separate projects
  (`base` vs. `db`) that can be built, packaged, and deployed independently.

If you're looking for the long-established, production-proven RTS2 codebase,
that's the [classic tree](https://github.com/RTS2/rts2). This repository is
an active rewrite-in-progress — see each subtree's `STATUS.md` for exactly
how much of the classic system it currently covers.

## Authors

RTS2 was created by **Petr Kubánek** (`petr@rts2.org`), who remains the
project's main author, with contributions over two decades from a long list
of institutions and individuals credited in the classic tree's `AUTHORS`
file — among them Markus Wildi, Lee Hicks, Matt Thompson, and bug reports,
documentation, and smaller contributions from observatory teams at Ondřejov,
IAA, UCD, and elsewhere.

This CMake rewrite (`rts2ng`) is being developed by **Martin Jelínek**
(`mates14`), building directly on that body of work — every ported file
retains its original copyright header.

## License

RTS2 has historically been split into an LGPL-licensed core library and
GPL-licensed drivers/daemons; individual source files in this repository
carry their original license header (mostly GNU GPL v2 or later) carried
over from the classic tree during porting. See `COPYING` / `COPYING.LESSER`
in the [classic RTS2 repository](https://github.com/RTS2/rts2) for the full
license texts.

## Getting help / further resources

- Classic RTS2 project site: [rts2.org](http://www.rts2.org)
- Classic RTS2 source and issue tracker: [github.com/RTS2/rts2](https://github.com/RTS2/rts2)
- Each subtree's `STATUS.md` (`base/STATUS.md`, `db/STATUS.md`,
  `gui/STATUS.md`) for a detailed porting log and design rationale.
