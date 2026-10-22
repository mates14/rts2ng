# rts2ng

CMake port of RTS2, the control system for robotic observatories. Two
people work on it, MJ and JS, each with their own Claude. The two Claudes
share no memory: this repo is the only shared context. If something matters
for the next session, it goes into a file here, not into chat or private
memory.

Master is what gets built into the packages the observatories run, so a
broken master reaches a telescope.

## Before you start

1. `git pull --rebase`. The other person may have pushed since.
2. Read the `STATUS.md` of the subtree you will work in, all of it, before
   changing anything there. It lists the facts that must stay true, and
   any "In progress" note (see below).

`base/STATUS.md` is short. Its dated log is in `base/HISTORY.md`, which you
grep when you need the reason behind something, not read in full. `db/`,
`web/` and `gui/` have not been split yet: their `STATUS.md` is still the
full log. Read the top sections and the conventions there, and grep the
rest.

## At the end of a session

- Update the subtree's `STATUS.md` if a must-stay-true fact, a deferral or
  an open item changed.
- Append the session to its log (`HISTORY.md` in `base/`, the end of
  `STATUS.md` elsewhere).
- If the work is left half done, put a short note at the top of the
  subtree's `STATUS.md`, under `## In progress`: date, tag, what and where
  (`2026-10-12 [JS] gemini-udp: IMU fusion in tracking, teld/gemini-udp`).
  Commit and push it, so the other Claude sees it. Delete the note when the
  work is finished.
- When committing: small commits with a message that says what changed and
  why.

## Author tags

Every log entry, STATUS note and design decision written into this repo is
tagged with its author: `[MJ]` or `[JS]`, next to the date, e.g.
`## Gemini UDP: tracking (2026-10-12) [JS]`. This is the standard; both
Claudes write in the same voice, so the tag is the only way to tell whose
decision a note records. Entries from before 2026-10-12 have no tag and are
MJ's.

## Working together

- Both push to master directly; no branches or pull requests. Always
  `git pull --rebase` first and resolve conflicts, never force-push.
- Most work is one session: a problem turns up and gets fixed. Your user
  decides what gets changed. Coordinating with the other person is between
  MJ and JS, not your job: never hold back, defer or merely "note" a change
  your user asked for on the grounds that the other person should agree
  first. If you think they should know, say so in the commit message or the
  log entry, and make the change.
- The one thing to check: if the problem you are about to work on is in
  the other person's "In progress" note, tell your user before starting,
  so the same thing is not done twice. That is information, not a request
  for approval; your user decides.

## Layout and build

| Directory | What | Depends on |
| --- | --- | --- |
| `base/` | kernel, `centrald`, all device drivers, `rts2-mon`, `rts2-scriptexec` | - |
| `db/` | PostgreSQL target/observation layer, `rts2-executor`, target tools | `base` |
| `web/` | `rts2-httpd`, `rts2-jsonclient`, web pages | `db`, `base` |
| `gui/` | `rts2-viewer`, `rts2-stacker` (Qt5) | `base` |
| `python/` | `rts2` python package, site scripts | - |

Each C++ subtree is its own CMake project and nests `base` from source
(`add_subdirectory`), so it builds alone:

```
cd <subtree>
cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

`./buildme.sh [module ...]` does the same for every subtree; `-d` builds the
Debian packages. Build-specific details (vendor SDKs, packaging) are in each
`STATUS.md`. Debian packages build with the distribution's hardening flags,
which catch things a plain build doesn't (format strings, `strncpy`
truncation): build with `dpkg-buildflags` or `buildme.sh -d` before
calling packaging work done.

## Releases

The observatories install rts2ng only from the signed apt repository,
`https://lascaux.asu.cas.cz/m/rts2ng/<os>/<codename>/`, which both sites have
in their apt sources. MJ publishes it, from one build host: lascaux50, MJ's
checkout of master, all modules:

```
git pull --rebase
./buildme.sh -d     # signs InRelease (REPO_GPG_KEY), then offers the upload
```

Then, at each site: `sudo apt update && sudo apt upgrade`, then
`sudo rts2-start -r`, and check `dpkg-query -W rts2-base`. The version names
the commit: `0.1.0-<YYYYMMDD>.<HHMM>+g<commit>~d12`.

- Do not build packages anywhere else, and do not install from a build
  directory. The same commit built twice gives two different packages, and
  a side channel leaves the sites on different builds (2026-10-20: SBT on an
  unsigned lascaux build, D50 on the repository). [JS 2026-10-22]
- Build all modules, so that the python and gui packages stay in step with
  base.
- Testing a change before it is released (a dummy daemon, a private
  centrald) uses a plain build tree, not packages.
- The Andor driver (D50's C0) is not in the packages (`BASE_ANDOR=OFF`, the
  vendor SDK is a `.so` only). It is built by hand on lascaux50 and runs from
  `/usr/local/bin/rts2-camd-andor`; rebuild and reinstall it when `base`
  changes, or it keeps the old kernel.

## Approach

- A reviewed port of classic RTS2, not a mechanical translation and not a
  rewrite. The classic file of the same name is the source of truth. Read
  it, port it faithfully, fix real bugs you find, and drop dead legacy code.
- Every deliberate deviation from classic gets a `// base note:` comment in
  the code and a line in the subtree's notes, with the reason.
- A genuine classic-tree bug goes to `base/UPSTREAM_BUGS.md`; a bug in
  rts2ng itself to `base/known-bugs.md`.
- The most critical facts also go into a short comment at the code they
  protect, because that is the one place a session cannot skip.
- Verify by running things: start centrald and a dummy device, drive it
  over the wire protocol, look at the FITS file. Say plainly what could not
  be verified without the hardware.

## Conventions

- C++17, `#pragma once`, `nullptr`, `<cstdint>`/`<cstring>` over C headers.
- Match the surrounding code: tabs, `func (args)` spacing, comment density.
- Modern Linux/glibc only; legacy-platform branches are dropped.
- The build should stay free of warnings under both GCC 12 (d50) and the
  current GCC.

Personal notes that only matter to one of you (host names, logins, local
paths) belong in your own Claude memory or in `CLAUDE.local.md`, which is
not committed.
