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
