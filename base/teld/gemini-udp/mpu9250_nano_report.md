# MPU-9250 USB sensor — usage reference

An Arduino Nano (CH340 USB-serial chip) with an MPU-9250 9-axis IMU. It appears on the PC as a serial port and answers single-character commands with one line of sensor readings.

## Connection

- Port: Linux typically `/dev/ttyUSB0` (user needs to be in the `dialout` group); Windows `COMx`.
- **115200 baud, 8N1**, no flow control.
- Opening the port resets the board. Wait for the line `# READY` (typically 1–2 s, mostly the bootloader delay) before sending commands; anything sent earlier is lost.
- Only one program can have the port open at a time.

## Commands

Single characters, no line ending needed, case-sensitive. CR/LF are ignored.

| Send | Reply |
|---|---|
| `r` | raw values — one line, numbers only |
| `c` | values converted to physical units — one line, `key=value` pairs |
| anything else | `# r=raw data, c=computed data, other=this help` |

Every reply line ends with `\r\n`. **Lines starting with `#` are never data** (status, help, errors) — skip them. Possible such lines:

- `# READY` — initialisation done
- `# WARN magnetometer not available, wia=0x..` — the magnetometer fields will be `NA`
- `# ERR mpu init failed, whoami=0x..` — sensor not responding (it is retried on the next `r`/`c`)
- `# ERR i2c mpu`, `# ERR i2c mag` — a read failed; no data line is sent for that request

## Raw output (`r`)

11 space-separated fields, always in this order:

```
ax ay az gx gy gz t mx my mz movf
```

Example:

```
-180 132 16441 -160 70 20 2134 83 -201 -278 0
```

| # | Field | Meaning | Type | Conversion to physical units |
|---|---|---|---|---|
| 1–3 | `ax ay az` | acceleration | int16 | ÷ 16384 → g (range ±2 g; 1 g ≈ 9.81 m/s²) |
| 4–6 | `gx gy gz` | angular rate | int16 | ÷ 131 → °/s (range ±250 °/s) |
| 7 | `t` | chip temperature | int16 | ÷ 333.87 + 21 → °C |
| 8–10 | `mx my mz` | magnetic field | int16 | × 0.15 → µT (range ±4912 µT) |
| 11 | `movf` | magnetometer overflow | 0/1 | — |

If the magnetometer is not available, fields 8–11 are `NA`.

## Computed output (`c`)

The same quantities already converted. Example:

```
ax=0.0123 ay=-0.0081 az=1.0034 gx=-1.221 gy=0.534 gz=0.153 t=27.41 mx=12.45 my=-30.15 mz=-41.70 movf=0
```

Units: g, °/s, °C, µT. The only difference from converting raw values yourself: the magnetometer values in `c` also include the chip's factory sensitivity adjustment (ASA, typically a correction of a few percent per axis). That factor is not available through the raw output.

## What the values mean — important for interpretation

**Accelerometer**
- Measures gravity plus motion. At rest the vector magnitude is ≈ 1 g; at rest and flat with the chip's Z axis up: `az ≈ +1 g`, `ax ≈ ay ≈ 0`. Usable for tilt.
- Not calibrated; small offsets and scale errors are expected.

**Gyroscope**
- Should be 0 at rest, but each axis has an uncorrected bias (typically on the order of 1 °/s). For accurate use: average a few hundred readings while the device is still and subtract that per-axis offset.

**Temperature**
- Die temperature of the sensor chip, not ambient temperature.

**Magnetometer**
- **Different axis orientation** from accelerometer/gyroscope. To express it in the accel/gyro frame:
  - X_accelframe = `my`
  - Y_accelframe = `mx`
  - Z_accelframe = −`mz`
- Not calibrated: values include offsets from the board and nearby magnetic/ferrous objects (hard/soft-iron effects). Earth's field alone is roughly 25–65 µT. A calibration is needed before using it as a compass.
- Updated internally at 100 Hz; each reply returns the latest valid sample, so it can be up to ~10 ms old and may repeat when polling faster than 100 Hz.
- `movf=1`: the field was out of range (e.g. a magnet very close); the previous mag values are kept.

## Sensor timing

- Accelerometer and gyroscope are sampled internally at 200 Hz, low-pass filtered at about 41–45 Hz. The values are read at the moment the command arrives.
- Each request returns one line within a few milliseconds; polling at tens of Hz is no problem.

## Minimal Python example (pyserial)

```python
import serial

FIELDS = ["ax", "ay", "az", "gx", "gy", "gz", "t", "mx", "my", "mz", "movf"]

with serial.Serial("/dev/ttyUSB0", 115200, timeout=3) as s:
    while s.readline().decode().strip() != "# READY":   # wait for boot
        pass
    s.write(b"r")
    line = s.readline().decode().strip()
    if not line.startswith("#"):
        raw = dict(zip(FIELDS, line.split()))
        ax_g = int(raw["ax"]) / 16384
        gz_dps = int(raw["gz"]) / 131
        print(raw, ax_g, gz_dps)
```
