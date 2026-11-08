# python: status

The `rts2` Python module (`rts2/`), the site-independent scripts
(`scripts/`) and the scripts of single observatories (`sites/<site>/`).
Packaged as `rts2-python` (`debian/`, pybuild on the same `setup.py`).
Changes to this subtree are logged at the end of this file.

## Must stay true

- Only the module and `scripts/` are packaged. `sites/` is never packaged:
  a site's scripts are its own, in `/etc/rts2/scripts`, which the package
  does not own (a directory, or for our telescopes a symlink to
  `python/sites/<site>` in a checkout; other sites bring their own,
  possibly modified copies). A script useful everywhere moves to
  `scripts/` and into `debian/rts2-python.install`. [MJ 2026-10-26]
- Packaged scripts go to `/usr/share/rts2/scripts`, never under `/etc`
  (dh_installdeb would make them conffiles). rts2-base finds both
  directories by bare name: `[scriptexec] script_path`, default
  `/etc/rts2/scripts:/usr/share/rts2/scripts`, site first. [MJ 2026-10-26]

## Log

### Site scripts out of the package (2026-10-26) [MJ]

`rts2-python-sbt` and the packaged `/etc/rts2/scripts` symlink removed;
see base/HISTORY.md, "script_path is a search list". At SBT, after the
upgrade (before it, dpkg would remove the new link as the old package's
obsolete file):
`sudo rm /etc/rts2/scripts && sudo ln -s <checkout>/python/sites/sbt /etc/rts2/scripts`,
then `sudo apt remove rts2-python-sbt`.

### Morning flats: the dome close no longer ends the darks (2026-11-07) [JS]

After the morning flats `flats.py` closed the dome with `close_for 3600`.
That sets the dome's `next_open` an hour ahead, and until then the dome
reports bad weather ("waiting for next_open"): centrald went to standby,
EXEC ended the target and all three SBT flat scripts died with
`Rts2NotActive` ("flat script interruped") one second after C3 logged
"taking calibration darks" (2026-11-05, -06, -07, 05:44-05:46 UTC). C3 had
no morning darks on any of those mornings; C1 and C2 (`domeDevice=None`,
they finish earlier) took theirs under the open roof and were cut off at
the same moment. Now a plain `close`: the system stays ON dawn, the darks
run under a closed roof until the sun reaches -0.5 deg or EXEC changes
target (`takeDarks`). The dome is closed again by the driver when the
morning state starts. Lost: the hour's lock-out - between the close and
morning (~25 min) a hand-made switch to ON would reopen the dome. A sensor
alarm still closes it, and at SBT (`weather_open = 0`) good weather does not
switch the system back on. Needs the rts2-python package from this date.
D50: target 2 ran `/etc/rts2/flatC0_nobin.py` with the 2023 copy of
flats.py and `domeDevice=None` (darks under the open roof, DF filter). Now
`python/sites/d50/flatC0.py`, same settings, packaged flats.py, closing the
dome; the D50 dome parks T0 before it closes (06:04 that morning: park 12 s,
close 27 s).

### A script whose pipes RTS2 closed ends quietly (2026-11-07) [JS]

When RTS2 drops a running script but the process lives on, the script's
next write raised `BrokenPipeError` (and an empty read returned `''` to the
caller): a traceback plus "Exception ignored in: <stdout>" in the log, 20-80
a night at SBT (guideccd.py, 2026-11-07 04:45:20 and 04:47:19), harmless but
burying real script errors. scriptcomm now turns both into `Rts2NotActive`,
which every site script already treats as "RTS2 ended me", points stdout at
/dev/null and makes `log ()` a no-op, so the handlers' last log line and
Python's flush at exit raise nothing. Bench (a harness that answers a few
queries, then closes stdin/stdout and keeps reading stderr): old - exit 120,
BrokenPipeError traceback, in a write and in a read; new - exit 0, stderr
empty, both. Through rts2-scriptexec ending (SIGTERM) the script gets
SIGTERM from ConnFork's `kill (-childPid)` and ends either way, so that is
not the path seen at SBT. Needs the rts2-python package from this date.

### SBT targets run the scripts from /etc/rts2/scripts (2026-11-08) [JS]

The morning-flats fix above did not reach SBT: target 2 still ran
`exe /etc/rts2/flatC1.py`..`flatC3.py`, which import the 2023 copy
`/etc/rts2/flats.py` (`close_for 3600`), not the packaged module. C3 had no
morning darks on 2026-11-08 either (cut off at 05:46:58). The DB now points
every SBT script at `/etc/rts2/scripts/` (= `python/sites/sbt`): target 2,
60 (make_survey), 54000 (ojitos), the ojitos_adaptive targets and
`grb_script` (ojitos_adaptive, and guideccd on C3, which until now ran the
copy from before the 2026-11-07 noblock change). Kept: target 56000,
`exe /etc/rts2/do_grb_C1.py`, not in the repo. The scripts left in
`/etc/rts2` are dead copies. Before the switch the old and repo copies
differed only in imports, except ojitos_adaptive: the site had set the GRB
filter to clear (2026-09-25), the repo had r - now clear in the repo too.
A site script fixed in the repo only reaches a target whose DB script names
the `/etc/rts2/scripts` path (or a bare name, found through `script_path`).
