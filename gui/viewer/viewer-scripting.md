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
[Spectrum + lambda]
color = green
script = FIBER.POSITION=object E $EXPTIME
  FIBER.POSITION=thar E $CALTIME
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
- for anything else (a fiber slider, a lamp) check the driver. If it
  answers before the move is done, add a `DEV.waitfor` on a value that
  shows the position, or a `sleep` long enough for the slowest move.

A selection value (one with named choices, like a filter or a slider
position) takes the name as `rts2-mon` shows it: `FIBER.POSITION=thar`.

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
does **not** catch a wrong position name (`FIBER.POSITION=thr`) or a value
the device refuses: those are logged and the frames are taken anyway. So
after writing or changing a mode, run it once with a short exposure and:

1. watch the viewer's log for errors;
2. look at the frames' FITS headers. Values of the other devices (lamp,
   slider, wheels) are written into the header of every frame, so the
   header shows where the slider really was and whether the lamp was on;
3. for calibration modes, look at the frame: arc lines, flat-field
   illumination.

## Example: FLORES

`flores.modes` has the structure, with the fiber slider still a
placeholder (`FIBER.POSITION=fiber2|thar|flat`). Until the real names are
in, the slider modes are refused and only Plain exposure and Dark run. To
finish it:

1. In `rts2-mon`, find the slider's device and value and the names of its
   positions, and the device and values of the ThAr and flat lamps.
2. Replace `FIBER.POSITION` and the position names in each mode.
3. Add the lamps: on before the calibration exposure, off afterwards, and
   off at the start of the science modes. If a lamp needs to warm up, add
   a `sleep` after switching it on.
4. If the slider's driver answers before it is in position, add a
   `waitfor` or `sleep` after each move (see above).
5. Check every mode as described above.

A finished mode could then look like this (device and value names
illustrative):

```
[Spectrum + lambda]
color = green
script = LAMP.THAR=off LAMP.FLAT=off FIBER.POSITION=object E $EXPTIME
  FIBER.POSITION=thar LAMP.THAR=on sleep 20 E $CALTIME LAMP.THAR=off

[Spectral flat]
color = yellow
script = LAMP.THAR=off FIBER.POSITION=flat LAMP.FLAT=on sleep 20
  E $EXPTIME LAMP.FLAT=off

[Lambda calib]
color = purple
script = LAMP.FLAT=off FIBER.POSITION=thar LAMP.THAR=on sleep 20
  E $CALTIME E $EXPTIME LAMP.THAR=off

[Dark]
color = red
script = LAMP.THAR=off LAMP.FLAT=off D $EXPTIME
```

Lambda calib takes the short arc with Calib time and the long one with
Exposure.
