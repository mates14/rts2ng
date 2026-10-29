# SBT scripts - quick reference

`/etc/rts2/scripts` on lascaux is a symlink to this directory, so a target
script calls these by name: `exe "filtros.py -e 120"`.

## filtros.py - synchronised filter sequences on C1/C2(/C3)

One copy runs per camera (one script line per camera in the target).  The
cameras change filters together on a common tick (longest exposure + 15 s):

| mode | C1 | C2 | C3 |
|---|---|---|---|
| wide (default) | i, r | r, g | B |
| narrow (`-u`) | halpha, sii | oiii, B | I |

It runs until EXEC has another target as next (no repetition count needed),
then ends at an exposure boundary.  A mount move (re-pointing) starts a new
synchronised run within the same script.

### Fixed exposure (as before)

    exe "filtros.py -e 120"            # every filter 120 s
    exe "filtros.py -u -e 600"         # narrow band, 600 s - keep it fixed
    exe "filtros.py -e 60 -n 20"       # stop after 20 exposures

### Exposure for a requested SNR

    exe "filtros.py --snr 30"                     # magnitude from ATLAS / lcgen
    exe "filtros.py --snr 30 -e 120"              # 120 s where no magnitude is found
    exe "filtros.py --snr 50 -m 15.2"             # same magnitude in all filters
    exe "filtros.py --snr 50 -m r=15.2,g=15.8"    # per filter (i takes r, ...)
    exe "filtros.py --magerr 0.02 -f 16"          # magerr 0.02 = SNR 54; fallback mag 16

| option | meaning |
|---|---|
| `-s/--snr N`, `--magerr E` | SNR to reach in every filter (E = 1.0857/SNR) |
| `-m/--mag` | target magnitude, `17.5` or `r=17.5,g=18.1` |
| `--mag-source` | where to look it up, in order (`atlas,lcgen`) |
| `-f/--fallback-mag` | magnitude when no source knows the target |
| `-e` | with `--snr`: exposure of filters without any magnitude (300) |
| `--tmin`, `--tmax` | limits of the computed times (5, 300 s) |
| `-n` | number of exposures (default: until the next target) |

**Magnitude**, first found: `--mag`; the local ATLAS RefCat2 (`atlas`
command, g r i z of the nearest star within 3"); lcgen on hog (median of the
newest night; anonymous access sees only data older than a year, a login in
`/etc/rts2/lcgen.auth` as `user:password` sees all); `--fallback-mag`; else
`-e`.  A filter without its own magnitude takes a neighbour (i <- r,
B <- g, halpha <- r, ...), with a warning.

**Conditions** come from the COND device (rtspy-sensor-cond), which reads
the newest real-time ECSV of each camera/filter: zeropoint, sky and FWHM.
A filter without a fresh frame (< 30 min) uses the freshest transparency
(COND.transp) and another filter's sky shifted by a typical sky colour; with
nothing at all, a clear sky of 19.5 mag/"2.  The zeropoint is moved to the
target's airmass (T0.AIRMASS) with the filter's extinction.

**Times** are the pipeline's noise model solved for the SNR, rounded up to
1 2 3 5 10 15 20 30 45 60 90 120 180 240 300 ... s and kept within
`--tmin`..`--tmax`.  They are cut (rounded down) when the sky at the end of
the exposure would exceed half of the saturation level (50000 ADU) or the
target's peak pixel + sky 80 % of it.  With the Sun rising between -20 deg
and the horizon, the sky is extrapolated to the end of the exposure
(~1 mag per degree of solar altitude).

**Synchronisation**: the first script of an observation computes the times
of all synchronised cameras and writes them to
`/tmp/snrexp-plan-<obsid>-<mode>.json`; the others read it, so all use the
same tick.  C3 follows the tick with times of its own, capped to fit.

### What the log says

    Filtros/w/C1: atlas: Sloan_g=16.98, Sloan_i=16.82, Sloan_r=16.85, Sloan_z=16.66
    Filtros/w/C1: plan 12345-wide (computed): SNR 30 X=1.30 tick 195s
    Filtros/w/C1:   C1/Sloan_i: atlas Sloan_i 16.82: 120s (needs 109s) SNR 31, zp1=17.19 s=8.38 fwhm=1.94 (transp, sky from C1/Sloan_r)
    Filtros/w/C1:   C1/Sloan_r: atlas Sloan_r 16.85: 90s (needs 84s) SNR 32, zp1=17.97 s=17.40 fwhm=1.94 (own frame 270s old)
    Filtros/w/C2: plan 12345-wide (shared): ...
    ... CUT: sky would saturate at 169.2s

`needs` is the unrounded time for the SNR; `SNR` what the chosen time should
give; `zp1` the 1 s zeropoint, `s` the sky variance rate [ADU^2/s/px], and in
parentheses where the conditions came from.  SNR well below the request
means `--tmax` (or saturation) limited it.

The calculator is `rts2/snrexp.py` (in the rts2 package; the scripts always load it
from this checkout, as the installed package may carry an older one).  SBT's 1 s
zeropoint in r is ~18.0: under a dark sky SNR 10 at r=16.5 takes ~45 s, SNR
50 about an hour.
