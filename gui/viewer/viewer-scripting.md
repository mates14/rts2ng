# rts2-viewer exposure modes

What the viewer's Expose and Run buttons do is set by the **mode
selector** next to them. Each mode is a short script in the RTS2 script
language, the same language `rts2-scriptexec` and the executor run. An
instrument's observing sequences (spectrum, flat, wavelength calibration,
dark) are therefore a configuration file, not a viewer of their own.

This file describes how to use the modes and how to write them. The
example files are `gui/viewer/modes/*.modes` in the source tree; the
package installs them, and this file, to `/usr/share/rts2/viewer-modes/`.

## Using it

- **Mode selector**: picks the script. After the viewer starts, the first
  mode in the file is selected, never the one used last time.
- **Exposure**: the time `$EXPTIME` stands for in the script.
- **Calib time** (default 1 s): the time `$CALTIME` stands for, for
  example the arc-lamp exposure. It is greyed out for modes that do not use
  it.
- **Expose** runs the selected mode once on the selected camera.
- **Run** runs it **Repeat** times (∞ = until stopped). It counts script
  runs, not frames: a mode that takes a spectrum and an arc counts as one.
- **Stop** stops the script and aborts the exposure in progress. If the
  camera does not answer, a second Stop gives the controls back.

While a script runs, the mode, camera, Expose and Run controls are locked.
Whatever the script's frames are, they arrive in the viewer and are saved
like any other frame.

**Colours.** By day the selector and the buttons take the mode's colour.
In night mode there are no colours, except that a mode which takes
**darks** shows red. A dark at night is only ever taken on purpose.

**A mode that cannot run is refused.** Before it starts, the viewer checks
that every device and value the script sets exists, and if one does not,
it takes no frame and writes the reason into its log, e.g.
`not started: device FIBER is not connected`. Without this check the
script engine would skip the missing step with a line on stderr and take
the frames anyway, so a lambda calibration without its lamp would look
like a good one.

## Enabling a modes file

In `rts2.ini`:

```
[viewer]
modes = /etc/rts2/flores.modes
```

or on the command line: `rts2-viewer --modes /etc/rts2/flores.modes`
(the command line wins). Without a modes file the viewer offers only
**Light** (`E $EXPTIME`) and **Dark** (`D $EXPTIME`).

To change a site's modes, copy the installed file to `/etc/rts2/` and edit
the copy: a package upgrade overwrites the files in `/usr/share`. The
viewer reads the file when it starts, so restart it after an edit. Its log
then says `exposure modes: 5 from /etc/rts2/flores.modes`. If the file has
an error, the log says where (file and line) and the viewer falls back to
Light and Dark.

## File format

```
# a comment, on a line of its own
[Lambda calib]
color = purple
script = TOPTEC.CAM_POS=13290 sleep 3 TOPTEC.waitfor CAM_POS 13290 3
  TOPTEC.FIBCOMP=1 sleep 3 TOPTEC.waitfor FIBCOMP 1 0.5
  E $EXPTIME E $CALTIME TOPTEC.FIBCOMP=0
```

- One `[label]` section per mode, in the order they appear in the
  selector. The label is what the selector shows.
- `color`: any Qt colour name (`cyan`, `green`, `yellow`, `purple`,
  `red`, ...) or `#rrggbb`. Optional.
- `script`: the script. A line that starts with a space or a tab continues
  the line above, so a long script can be split; the lines are joined with
  a space.
- `$EXPTIME` and `$CALTIME` are the only variables. Anything else starting
  with `$` is an error.
- **No `#` inside a script**, not even at the end of a continuation line.
  The script language ends the whole script at the first `#`, so
  everything after it would silently disappear. Put comments on lines of
  their own, outside the script.
- A mode whose script contains a dark exposure (`D`) is treated as a dark
  mode: red at night.

## The script language

Words separated by spaces, executed in order. Each step waits for the
previous one to finish.

| Script | Does |
| --- | --- |
| `E 10` | a light frame of 10 s (`SHUTTER` light) |
| `D 10` | a dark frame of 10 s (`SHUTTER` dark, `IMAGETYP` dark) |
| `NAME=value` | sets a value on the camera the mode runs on, e.g. `FILTA=grism` |
| `DEV.NAME=value` | sets a value on another device, e.g. `W1.NEON=on` |
| `NAME+=1`, `NAME-=1` | changes a numeric value by an amount |
| `sleep 30` | waits 30 s |
| `DEV.waitfor NAME 2 0.1` | waits until `DEV.NAME` is within 0.1 of 2 |
| `for 3 { ... }` | repeats the block 3 times; the braces need spaces around them |
| `exe "/path/prog args"` | runs a program that can talk to the script (rts2-scriptexec's protocol) |

Every `E` and `D` sets the shutter for that frame, so a dark cannot be
left switched on by an earlier frame or mode.

A value change waits for the device to answer it. Whether the answer
comes when the mechanism has *arrived* depends on the driver:

- a camera's filter wheels (`FILTA=...`) answer after the move;
- for anything else (a fiber slider, a lamp relay) check the driver. If
  it answers before the move is done, wait for it with `DEV.waitfor` on a
  value that shows the real position or state.

**Do not trust the value straight after writing it.** Drivers that poll
their hardware (rtspy's TOPTEC, for one) report a written value back at
once, as if it were already done, and only their next poll replaces it
with what the hardware says. A `waitfor` right after the write sees that
echo and passes immediately. Put a `sleep` longer than the driver's poll
interval between the write and the `waitfor`:

```
TOPTEC.CAM_POS=13290 sleep 3 TOPTEC.waitfor CAM_POS 13290 3
```

Do not count on the camera holding the exposure back while another
device moves either. In the FLORES test it did not, so the `waitfor` is
what keeps the frame from being taken mid-move.

`waitfor` waits for as long as it takes. If the mechanism never gets there
(a stalled stage), the script hangs at that step without taking a frame,
and Stop ends it.

A selection value (one with named choices, like a filter or a slider
position) takes the name as `rts2-mon` shows it: `FILTA=grism`.

Device and value names are what `rts2-mon` lists. Names are case-sensitive.

## Writing modes that are safe to stop

The viewer's Stop ends the script where it is: if it is stopped during
the arc exposure, the `LAMP=off` at the end never runs. So:

- **Every mode sets everything it depends on at its start**: lamps
  off for science and darks, the slider to the right place, the grism in
  or out. Then whatever an earlier stopped mode left behind is corrected by
  the next one. The OVIS file does this: each mode sets `FILTA`, `FILTB`
  and `W1.NEON`.
- Turn lamps off again at the end of the mode that switched them on, so
  they are not left burning after a normal run.

## Checking a new mode

The viewer's check only catches devices and values that do not exist. It
does **not** catch a wrong position name (`FILTA=grsim`) or a value
the device refuses: those are logged and the frames are taken anyway. So
after writing or changing a mode, run it once with a short exposure and:

1. watch the viewer's log for errors;
2. look at the frames' FITS headers. Values of the other devices (lamp,
   slider, wheels) are written into the header of every frame, so the
   header shows where the slider really was and whether the lamp was on;
3. for calibration modes, look at the frame: arc lines, flat-field
   illumination.

## Example: FLORES

`flores.modes` follows the FLORES night procedure (the howto). The
instrument box is the `TOPTEC` device:

| Value | What |
| --- | --- |
| `CAM_POS` | stage position in raw steps: 1050 object fibre 2, 13290 comp on both fibres, 14930 flat on both fibres |
| `FIBCOMP` | ThAr comp lamp (Quido relay 1), 1/0 |
| `FIBFLAT` | flat lamp (Quido relay 2), 1/0 |

`SPEC.SPCOMP` and `SPEC.SPFLAT` are the spectrograph's own lamps. They do
nothing for FLORES; do not use them.

| Mode | Does |
| --- | --- |
| Plain exposure | `E $EXPTIME`, touches nothing: for whatever is set by hand in the TOPTEC client |
| Spectrum + lambda | object fibre for Exposure, then a comp for Calib time: the howto's science frame and 1 s check comp |
| Spectral flat | flat on both fibres for Calib time (the howto says 1 s or less) |
| Lambda calib | comp for Exposure, then for Calib time: the howto's full 60 s + 1 s pair |
| Dark | `D $EXPTIME`, touches nothing |

With the usual Exposure 60 s and Calib time 1 s, every mode does what the
howto asks.

Every mode that uses the stage first switches both lamps off and waits
until they are off, then moves and waits for the stage to arrive, and only
then switches its lamp on and exposes. The comp lamp must never shine on
a single fibre: TOPTEC's comp presets 12965 and 13615 overexpose the CCD,
and the residual takes half an hour or more to clear. A mode stopped
during the comp leaves the lamp on at 13290, which is harmless; the next
mode switches it off before the stage moves.

Not in the modes, on purpose:

- `SPEC.SPECFILT`: must be 4 for FLORES and is set by hand. Every wheel
  move shifts the wavelength scale and needs a new full comp.
- Focus.

The comp and flat modes need TOPTEC's `FIBCOMP`/`FIBFLAT`. A TOPTEC
started before rtspy 1e8143f does not have them, and the viewer refuses
those modes until TOPTEC is restarted.

Tested against the real `toptec.py` with simulated TOPTEC and Quido
hardware (2026-10-13). Every frame was taken with the stage and lamps in
the right state, unchanged during the exposure; a lamp was never on away
from its position, also after Stop in the middle of a comp. Not yet run on
the instrument.
