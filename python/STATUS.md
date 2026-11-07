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
