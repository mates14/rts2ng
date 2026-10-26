# RTS2 Python client scripting support

`rts2.scriptcomm` is the library RTS2's "exe" observation scripts use to talk to the
running system - reading/writing device values, waiting on state, taking exposures,
filing images - over the stdin/stdout protocol the executor speaks to a forked script
process.

The site-independent scripts built on it are in `scripts/`: autofocus (`focus.py`) and an
airmass/extinction-curve scan (`vaodscan.py`). Scripts of one observatory are in
`sites/<site>/` (SBT's guiding, flats, filter sequences and survey, D50's guiding,
makak, ovis); they are kept here for reference and for those sites, not packaged.

`pip install .` installs the `rts2` package only - the scripts aren't meant to become
`$PATH` commands, they're meant to be invoked by RTS2's executor via `exe <name>`. A bare
name is looked up along `[scriptexec] script_path` in `rts2.ini`, a `:`-separated list,
default `/etc/rts2/scripts:/usr/share/rts2/scripts`, first match wins. The Debian package
(`debian/`) installs the library via the same `setup.py` (through `dh-python`'s
`pybuild`) and the site-independent scripts to `/usr/share/rts2/scripts`.

`/etc/rts2/scripts` belongs to the site, and a script there overrides a packaged one of
the same name. Make it a directory of your own scripts, or a symlink to a checkout, e.g.
for SBT:

    sudo rm /etc/rts2/scripts     # left by the upgrade from the old package, if there
    sudo ln -s /home/<user>/rts2ng/python/sites/sbt /etc/rts2/scripts
