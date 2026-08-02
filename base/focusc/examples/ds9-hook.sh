#!/bin/sh
#
# Example rts2-focusc -F hook: ship each completed exposure into a running
# ds9 via XPA. This is a starting point, not a shipped/installed tool -
# not built by CMakeLists.txt, not packaged, not covered by any test.
# Point rts2-focusc at it directly:
#
#   rts2-focusc -d C0 -e 5 -F /path/to/ds9-hook.sh
#
# Two non-obvious constraints this depends on (both confirmed live against
# a real ds9, see base/STATUS.md/UPSTREAM_BUGS.md "hooking rts2-focusc up
# to ds9" for the write-up):
#
#  - Must stay silent on stdout. ConnFocus (kernel/devclifoc.cpp) reads
#    this script's stdout back as protocol data - either a "change <id>
#    <val>" focus-adjustment line, or a six-number sextractor-style star
#    data line - and logs anything else as a parse error. xpaset is
#    silent on success, so redirecting it away is enough; any script
#    grown from this one needs to keep doing the same, or switch to
#    actually emitting "change"/star-data lines on purpose (see below).
#
#  - $1 is relative to the directory rts2-focusc was started from, not
#    to this script or to ds9. ds9/XPA resolves relative paths against
#    ds9's own process cwd, not the caller's - hence readlink -f.
#
# Ideas for growing this into something more useful, discussed with the
# user (2026-08-02) - none implemented yet:
#
#  - Push ds9 region files (`xpaset -p ds9 regions` fed a region-file on
#    stdin) to overlay a crosshair on a detected star, the commanded
#    target position, or similar - turns this into a lightweight
#    focusing/acquisition display.
#  - If a companion script plate-solves the image (astrometry.net or
#    similar) and writes/pushes a real WCS, ds9's native RA/Dec grid
#    overlay (`xpaset -p ds9 grid yes`) becomes meaningful, and opens the
#    door to a live polar-alignment display: mark the mount's known
#    rotation-center pixel (calibrated once, e.g. from star-trail curvature
#    while slewing in HA) as one region, mark where the WCS says the
#    celestial pole actually falls as another, and report the offset
#    between them - the same measurement a much older terminal-only tool
#    used to print as a raw X/Y distance, just drawn instead of printed.
#  - This script deliberately does *not* print a "change <id> <val>" line,
#    so ConnFocus always logs "we don't get focus change, let's try next
#    image". A real focusing script could compute a focus step from the
#    image and print that line instead (or as well) - see FocusCameraClient/
#    DevClientFocusFoc for what happens on the receiving end.
#
xpaset -p ds9 fits "$(readlink -f "$1")" >/dev/null 2>&1
