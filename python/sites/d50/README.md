# D50 scripts - quick reference

`/etc/rts2/scripts` on lascaux50 is a symlink to this directory, so a target
script calls these by name: `exe "expose.py --filters rgi --snr 30"`.

## expose.py - filter cycles at a requested SNR

    exe "expose.py --filters rzgi --snr 30 -e 60"
    exe "expose.py --filters rgiz --snr 30 -e 60 --acquire 3x10 --dither 0.03"
    exe "expose.py --filters r,g --snr 50 -m r=14.2,g=14.8"
    exe "expose.py --filters ri -e 20 --dither 0.03"       # fixed 20 s, no SNR

It cycles through the filters, replanning each cycle from the current
conditions, as long as the queuer keeps the target; it ends at an exposure
boundary once EXEC has another target as next.  There is no repetition
count.  Mount moves (centering) do not end it.

| option | meaning |
|---|---|
| `-f/--filters` | filters in order, `rzgi` or `r,z,g,i` |
| `-s/--snr N`, `--magerr E` | SNR to reach in every filter (E = 1.0857/SNR) |
| `-e` | without `--snr`: fixed exposure; with it: exposure of filters without any magnitude |
| `-m/--mag` | target magnitude, `16.3` or `r=16.3,g=16.9` |
| `--mag-source` | where to look it up (`lcgen,atlas`): its own band from any source beats a substitute band; lcgen is the median of the newest 5 nights within 60 d |
| `-F/--fallback-mag` | magnitude when no source knows the target |
| `--tmin`, `--tmax` | limits of the computed times (1, 300 s; with accumulation, of the total) |
| `--acquire NxT` | N unfiltered frames of T s before the cycles, for the centering |
| `--acquire-filter` | filter of those frames (N) |
| `--dither D` | random T0.woffs of up to D deg in RA and Dec before each science frame |

**Magnitude**, first found: `--mag`; the local ATLAS RefCat2 (`atlas`
command, g r i z of the nearest star within 3"); lcgen on hog (median of the
newest night; anonymous access sees only data older than a year, a login in
`/etc/rts2/lcgen.auth` as `user:password` sees all); `--fallback-mag`; else
`-e`.  A filter without its own magnitude takes a neighbour (z <- i, ...).
A GRB afterglow is in neither catalogue: give `-f` (or `-m`).

**Conditions** come from the COND device (rtspy-sensor-cond), from the
newest real-time ECSV of each filter (~20 s after readout on D50): zeropoint,
sky, FWHM; a filter without a fresh frame uses the freshest transparency.
The sky at the target (T0.ORI) is a sky model (Sun, Moon, light pollution,
airglow) plus the departure from it of the freshest measured sky: this
filter's frame, another filter's, or the Makak sky monitor (COND MK/N); this
also follows re-pointing.  Filters without a model (N) take another filter's
sky; with nothing at all, a clear sky of 19.5 mag/"2.  The zeropoint is moved
to the target's airmass with the filter's extinction.

**Times** are the pipeline's noise model solved for the SNR, rounded up to
1 2 3 5 10 15 20 30 45 60 90 120 180 240 300 ... s.

**Saturation**: the sky at the end of an exposure must stay below half of
the saturation level (50000 ADU), the target's peak pixel + sky below 80 %.
With the Sun rising between -20 deg and the horizon the sky is extrapolated
to the end of the exposure (~1 mag per degree of solar altitude).  Where a
single frame would saturate, the Andor accumulates: `ACQMODE=2`,
`ACCNUM=n`, exposure = the sub-exposure (32-bit frame, EXPTIME = n x sub),
n the fewest that reach the SNR with n readouts; ACQMODE goes back to a
single scan for the filters that do not need it.  A kinetic series
(`ACQMODE=3`, `KINNUM=k`) gives the same kind of frame since 2026-11-03:
the sum of its k x ACCNUM scans, 32-bit, EXPTIME the scans' total, plus the
individual frames in `KINFILE` (/var/tmp/kin-*.out).  Such frames carry
`COADDS` (scans summed into the frame); an older kinetic frame (no
`COADDS`, `ACQMODE` 3) is the 16-bit average of its KINNUM frames. [MJ]

**Dithering**: an `exe` script's value change does not wait for the
device, so after setting T0.woffs the script waits until T0 reports the
offset in OFFS; the exposure then waits for centrald's exposure block
(the devices of `blocked-by` in rts2.ini) as a normal script's `E` does.

### What the log says

    Expose/C0: atlas: Sloan_g=16.98, Sloan_i=16.82, Sloan_r=16.85, Sloan_z=16.66
    Expose/C0: C0/Sloan_r: atlas Sloan_r 16.85: 45s (needs 38s) SNR 33, zp1=22.59 s=30.00 fwhm=3.40 (own frame 40s old)
    Expose/C0: C0/Sloan_z: atlas Sloan_z 16.66: 240s (needs 218s) SNR 32, zp1=20.95 s=11.95 fwhm=3.60 (transp, sky from C0/Sloan_g)
    Expose/C0: C0/Sloan_g: given 17.50: 10x30s (needs 2428s) SNR 11 ACCUMULATED: sky would saturate at 56.4s, ...
    Expose/C0: next target queued, ending after 12 images

`needs` is the unrounded single-frame time for the SNR; `SNR` what the plan
should give; `zp1` the 1 s zeropoint, `s` the sky variance rate
[ADU^2/s/px], and in parentheses where the conditions came from.  SNR well
below the request means `--tmax` limited it.

The calculator is `rts2/snrexp.py` (in the rts2 package; the script always loads it
from this checkout, as the installed package may carry an older one).

## guide.py - C1 guiding (2026-11-07) [JS]

Takes `exposure (noblock = True)` frames: C0's corrections and offsets do
not stop guiding, and the frames do not hold T0's moves. A frame taken during
a move is dropped and guiding restarts. Needs the rts2-python package from
that date (`noblock`, `mountMoving`). Lascaux50 runs `/etc/rts2/guide.py`
(-> `guidecmosdec.py`, an older copy); `[C1] script = "exe
/etc/rts2/scripts/guide.py"` runs this one.

## flatC0.py - C0 flats and darks (2026-11-07) [JS]

Target 2's C0 script, moved here from `/etc/rts2/flatC0_nobin.py` with the
same settings. It uses the packaged `rts2.flats` (not the 2023 copy
`/etc/rts2/flats.py`) and closes the dome after the morning flats:
the dome parks T0 first, then the darks run under the closed roof until the
sun reaches -0.5 deg. Needs the rts2-python package from that date, then
target 2's C0 script `VSPEED=3 exe /etc/rts2/scripts/flatC0.py`.
