# RTS2 Python client scripting support

`rts2.scriptcomm` is the library RTS2's "exe" observation scripts use to talk to the
running system - reading/writing device values, waiting on state, taking exposures,
filing images - over the stdin/stdout protocol the executor speaks to a forked script
process.

This directory also carries a set of ready-to-use system scripts built on it: guiding
(`guideccd.py`), sky flats (`flats.py` + `flatC1/2/3.py`), filter-synchronized
multi-camera sequences (`filtros.py`, `ojitos.py`), GRB follow-up (`ojitos_adaptive.py`),
an idle-time sky survey (`make_survey.py`), an airmass/extinction-curve scan
(`vaodscan.py`), and a worked single-target example (`cygnus.py`).

`pip install .` installs the `rts2` package only - the standalone scripts aren't meant
to become `$PATH` commands, they're meant to be invoked by RTS2's executor via `exe
<name>`, resolved against `[scriptexec] script_path` in `rts2.ini` (default
`/etc/rts2/scripts`) when referenced by a bare filename rather than a full path. The
Debian package (`debian/`) installs both: the library via the same `setup.py`
(through `dh-python`'s `pybuild`), and the scripts to `/etc/rts2/scripts`.
