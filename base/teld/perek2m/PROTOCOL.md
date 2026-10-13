# Perek 2m telescoped - protocol

Reverse-engineered in October 2026 from Jan Fuchs' clients:
- `observe-client.jar` (the Java "Observe" client, decompiled);
- `observe_batch` (`observe_client.py`) and its logs;
- `fiber_control_client.py`.

The other 2m daemons (spectrograph, CCD700/OES exposure, TOPTEC, Quido) are
described in rtspy's `docs/perek2m-protocol.md`. rtspy has drivers for the
spectrograph and the TOPTEC head.

Every daemon is XML-RPC over plain HTTP. `rts2-teld-perek2m` and
`rts2-dome-perek2m` call telescoped directly (alhena = 192.168.193.195, port
9999), as observe_batch does. They don't use the HTTPS proxy of the Java
client.

A command answered `"1"` was **accepted**, which doesn't mean it is done.
Completion shows up only in the next `telescope_info ()`.

## Telescope - `telescope_*` (ASCOL)

- `telescope_execute(cmd: str) -> str` sends one raw ASCOL command line; it returns `"1"` if accepted.
- `telescope_set_coordinates({"ra": "HHMMSS.ss", "dec": "+DDMMSS.ss", "position": 0|1, "object": str}) -> 1` presets the
  target (newer API, used by observe_batch; the Java client used `TSRA` instead).
  `position` 0 = east, 1 = west.
- `telescope_info() -> dict`, every value a string:

| key | content |
|---|---|
| `ut` | `"yyyy-MM-dd HH:mm:ss"` UTC |
| `glst` | 10 space-separated ints, see below |
| `trrd` | current `"HHMMSS.ss ±DDMMSS.s P"`, P = 0 east / 1 west |
| `tsra` | last preset target `"ra dec P"` (same format) |
| `trhd` | hour axis and declination axis positions, degrees `"ha da"` |
| `trgv` | guide corrections `"ra dec"` (arcsec) |
| `trus` | user speeds `"ra dec"` |
| `dopo` | dome azimuth, degrees |
| `fopo` | telescope (secondary) focus position |
| `trcs` | pointing model number |
| `object` | object name of the last preset |

### GLST

Field *i* holds a raw value *r*; the state is `TABLE[r+1]`, so -1 means UNKNOWN.

| idx | subsystem | r = 0, 1, 2, ... |
|---|---|---|
| 0 | oil | OFF, START1 (N2 pressure check), START2 (pumps), START3 (pressure stabilisation), ON, OFF_DELAY |
| 1 | telescope | INIT, OFF, OFF_WAIT, STOP, **TRACK (4)**, OFF_REQ, SS_CLU1, SS_SLEW, SS_DECC2, SS_CLU2, SS_DECC3, SS_CLU3, ST_DECC1, ST_CLU1, ST_SLEW, ST_DECC2, ST_CLU2, ST_DECC3, ST_CLU3 (SS_* = slew to HA/DA "source", ST_* = slew to RA/Dec "star") |
| 2 | HA axis | STOP, **POSITION (1)**, CA_CLU1, CA_FAST, CA_FASTBR, CA_CLU2, CA_SLOW, MO_BR, MO_CLU1, MO_FAST, MO_FASTBR, MO_CLU2, MO_SLOW, MO_SLOWEST |
| 3 | Dec axis | as HA, plus CENM_SLOWBR, CENM_CLU3, CENM_CEN, CENM_BR, CENM_CLU4, CENA_SLOWBR, CENA_CLU3, CENA_CEN, CENA_BR, CENA_CLU4 (manual/automatic centring) |
| 4 | focus | OFF, STOP, PLUS, MINUS, SLEW, CAL1, CAL2 |
| 5 | dome | OFF, STOP, PLUS, MINUS, SLEW_PLUS, SLEW_MINUS, **AUTO_STOP (6)**, AUTO_PLUS, AUTO_MINUS, CALIBRATION |
| 6 | dome slit | UNDEF, OPENING, CLOSING, OPEN, **CLOSE (4)** |
| 7 | tube flap | UNDEF, OPENING, CLOSING, OPEN, CLOSE |
| 8 | mirror flap | UNDEF, OPENING, CLOSING, OPEN, CLOSE |
| 9 | bitfield | b0 HA, b1 Dec, b2 dome, b3 focus, b4 aberration, b5 precession, b6 refraction, b7 model, b8 guide mode |

observe_batch counts a slew as finished when `glst[1] == 4` (TRACK) and `glst[5] == 6`
(dome AUTO_STOP), and `trrd` matches the target to within 10" with the right side.
Parking counts as finished when `glst[1..3] == 3 1 1`.

### ASCOL commands seen in the clients

| cmd | meaning |
|---|---|
| `TEON 0/1` | telescope off/on |
| `OION 0/1` | oil off/on |
| `TETR 0/1` | tracking off/on |
| `TSRA ra dec pos` | set RA/Dec target (`HHMMSS.ss ±DDMMSS.ss 0/1`) |
| `TGRA 1` / `TGRA 0` | go to RA/Dec / stop slew |
| `TSHA ha da` | set hour-axis/dec-axis target, degrees (HA -180..330, DA -90..270) |
| `TGHA 1` | go to HA/DA |
| `TEHC 0/1`, `TEDC 0/1` | HA / Dec axis calibration |
| `TSGV ra dec` | set guide corrections (ints) |
| `TSCA`, `TSCP`, `TSCR`, `TSCM` `0/1` | aberration, precession, refraction, model corrections |
| `TSCS n` | select pointing model |
| `TSGM 0/1` | guide mode |
| `DOSO 0/1` | dome slit close/open |
| `FTOC 0/1` | tube flap close/open |
| `FMOC 0/1` | mirror flap close/open |
| `DOSA az` + `DOGA` | dome set absolute + go |
| `DOSR d` + `DOGR` | dome set relative + go |
| `DOAM` / `DOCA` / `DOST` | dome automatic / calibrate / stop |
| `FOSA x` + `FOGA` | telescope focus set absolute + go |
| `FOSR d` + `FOGR` | telescope focus set relative + go |
| `FOCA` / `FOST` | focus calibrate / stop |

Park sequence (fiber_control_client): `DOST, TETR 0, DOSO 0, FTOC 0, FMOC 0, DOSA 0,
DOGA, TSHA 10 80, TGHA 1, TEHC 1, TEDC 1, TSHA 90 45, TGHA 1, TEON 0, OION 0`. It waits
on the GLST after each of `TGHA 1`, `TEHC 1`, `TEDC 1` and `DOSO 0`.

---

## What the RTS2 drivers rely on

- **Coordinates are J2000.** A logged preset of eta UMi (`161730.27 754519.23`)
  is its catalogue J2000 position. The TCS applies precession, aberration,
  refraction and its pointing model (GLST bits 4-7), and `trrd` is reported
  in the same frame as the preset.
- **The sign of a positive declination is optional.** observe_batch sends
  none, so the drivers send none either.
- **`telescope_set_coordinates ()` doesn't apply the preset immediately.**
  observe_batch sleeps 2 s before `TGRA 1`. The teld instead waits until
  `tsra` shows the preset.
- **The dome.** With the dome in automatic mode (GLST dome 6-8), it is in
  place once it reads AUTO_STOP (6).

Not known yet:
- the units of `trgv`/`trus`;
- what GLST bits 0-3 mean;
- whether the TCS refuses `TGRA 1` below its own limits.

Whatever powers the instruments (an ASCOL command, a relay, a plug) must
never be switched by these drivers. They send only the commands listed above.
