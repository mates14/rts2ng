# Losmandy Gemini Protocol Reference Handbook

_A consolidated, implementer-facing reference to the Losmandy Gemini telescope-mount control
protocol: the UDP transport used by Gemini-2, and the LX200-like + "native" serial command sets
it carries. Parts 1-7 were compiled from the vendor-published protocol/command-reference documents
and user manuals listed below (no reverse-engineering, no assumptions from any existing driver
implementation); Part 8 is the deliberate exception - field experience from this driver and the two
existing functional Gemini drivers it was ported from/alongside. The source documents themselves
aren't kept in this repo (several large PDFs/HTML files - Görlich/Kanevsky/Simpson's UDP Protocol
Specification v1.2; the Level 4/5/6 Serial Interface Command Description references; the Level 4
and pre-Level-4 full user manuals) - this file is the processed distillate._

**Purpose**: this document exists to brief an AI coding agent (or a human) implementing
`rts2-teld-gemini-udp`, an RTS2 driver that talks to a Gemini-2 mount over the UDP protocol
described below. It is organized so that the transport layer, the command syntax, and the full
command tables can each be consulted independently.

**Critical scoping fact**: the UDP transport (Part 1) only ever existed for Gemini-2. Everything
Gemini-1-specific in this handbook (Part 5) is about the *serial command set*, which is carried
unchanged *inside* Gemini-2's UDP datagrams — i.e. useful as a fallback explanation of a command's
older behavior, not as a separate transport to implement.

The Level 6 document is cumulative: it documents the full command set as of L6 and annotates each
entry with when it was introduced (`From L4, V1.0 up`, `NEW in L5`, `NEW in L6`, `Obsoleted!`,
etc.). Those annotations are preserved verbatim in the tables below. Where Level 6 silently drops
something Level 5 documented, it is called out explicitly (see Part 5).

---
## Part 1 — UDP Transport Protocol (v1.2)

Source: `Gemini_UDP_Protocol_Specification_1.2.txt` (Görlich/Kanevsky/Simpson, March 2018).

### Access

Gemini listens for UDP datagrams on **port 11110** by default (configurable via Gemini's built-in
web app, Network Settings page). There is **no authentication, login, or access control** — reaching
the port is sufficient to command the mount.

### Datagram format

The Gemini protocol occupies the UDP payload (`Data` field), starting at byte offset 8 of the
overall UDP packet (i.e. after the standard 8-byte UDP header that your OS/socket stack handles).
Three fields, all within the payload:

| Field | Offset (in payload) | Size | Purpose |
|---|---|---|---|
| `DatagramNumber` | 0 | 4 bytes | **Request**: a client-unique sequence number, incrementing by 1 per datagram sent, starting at 0. **Response**: echoes the `DatagramNumber` of the command this is a response to. |
| `LastDatagramNumber` | 4 | 4 bytes | **Request**: always 0, *except* in a NACK datagram (see below). **Response**: normally 0; on a response to a NACK, holds the `DatagramNumber` of the last command Gemini actually received. |
| `GeminiData` | 8 | up to 255 bytes | The Gemini serial command string (request) or its response (response), **NULL-terminated (`0x00`)**. The 255-byte cap includes the terminating NULL. |

Integers are 4-byte fields (endianness not stated in the spec beyond the .NET example using
`BitConverter.GetBytes`, i.e. little-endian on the reference platform — verify against a live unit
if this matters for your implementation).

### Basic request/response cycle

1. Open a UDP socket, no handshake.
2. For each command: increment `DatagramNumber`, build the payload, send, wait for a response
   datagram, extract `GeminiData`.
3. Close the socket when done.

Example — sending `:GR#` alone:
- Request payload: `DatagramNumber=N`, `LastDatagramNumber=0`, `GeminiData=":GR#\0"` (9 bytes of data field).
- Response payload: `DatagramNumber=N` (echoed), `LastDatagramNumber=0`, `GeminiData="13:45:23#\0"`.

### Batching multiple serial commands in one datagram

Multiple NULL-terminated Gemini commands can be concatenated in a single `GeminiData` field, e.g.
`:GR#:GD#:GS#:GVP#\0`. Gemini processes them **in order** and concatenates their responses in the
same order into one response datagram: `13:45:23#75:34:09#09:56:09#Losmandy Gemini#\0`.

This batching is not just a convenience — it is the mechanism used to guarantee ordered, atomic
delivery of multi-command sequences under UDP's unordered/unreliable delivery (see "Critical
sequences" below).

### Commands with no serial response (ACK substitution)

Some serial commands normally produce no response at all over a wire connection (e.g. `:Q#`, `:RS#`).
Over UDP, **every command datagram still gets a response datagram** — Gemini substitutes a
single **ACK byte (`0x06`)** followed by NULL when there is no real command response to send. A
client must always wait for *some* response datagram.

If a batch contains a mix of response-producing and response-less commands, the response-less
command contributes nothing to the concatenated response (its completion is implied by the
presence of the other commands' responses) — the ACK substitution only happens when the *entire*
datagram's commands produce no response.

### Datagram reliability — ordering

UDP does not guarantee arrival order. Where the underlying serial protocol requires strict command
ordering (e.g. `:Sr` must precede `:Sd` in an equatorial slew sequence), you must either:

1. Serialize at the application level — never have more than one datagram in flight, waiting for
   each response before sending the next; or
2. Place the whole ordered sequence into **one datagram** (see batching above) — Gemini processes
   a single datagram's commands strictly in the order given, and the datagram either arrives whole
   or is retransmitted whole.

Losmandy recommends doing **both**. See "Critical sequences" below for the specific command groups
that must be batched this way.

### Datagram reliability — loss recovery (NACK/resync)

Every command datagram gets a response datagram, even if the underlying serial command has no
output. If a client's receive-timeout fires with no response, it cannot tell whether (a) the
command never reached Gemini, or (b) Gemini processed it but the response was lost. Resolve this
with a **NACK** datagram:

- NACK request (9 bytes total, **no NULL terminator**, and its `DatagramNumber` must be a **new**,
  never-before-used sequence number — not a retransmission of the timed-out one):
  - 4 bytes `DatagramNumber` (new value)
  - 4 bytes `LastDatagramNumber` = 0
  - 1 byte NACK (`0x15`)
- Gemini's response to a NACK:
  - 4 bytes `DatagramNumber` (echoes the NACK's number)
  - 4 bytes `LastDatagramNumber` = the `DatagramNumber` of the **last command Gemini actually
    received**
  - The response to that last-received command
  - NULL terminator

Client logic on receiving the NACK response:
- If `LastDatagramNumber` in the response **equals** the `DatagramNumber` of the command that
  timed out → Gemini did receive and process it; the response was lost in transit and is now
  available in this NACK-response datagram. Done, no retransmit needed.
- If it **differs** → Gemini never received the original command. Resend it under a **new**
  `DatagramNumber` and treat the reply to that resend as authoritative.

If several consecutive NACK round-trips also time out, the client should give up and surface a
"lost communication with Gemini" error to the caller rather than retrying indefinitely.

### Critical sequences (must be sent as a single batched datagram)

**LX200-like command pairs/sequences:**
- `:Os` with `:Od`
- `:OS` with `:OR`
- `:RC` with `:MA`
- `:RC` / `:RG` / `:RM` / `:Rm` / `:RS` with `:Me` / `:Mw` / `:Mn` / `:Ms` / `:Ma` / `:Mi` / `:Mg`
- `:SG` with `:SL` / `:SC`
- `:Sr` with `:ON` with `:Sd` — this exact order (`Sr`, then `ON`, then `Sd`) is recommended
- `:Sz` with `:ON` with `:Sa`

**Native command ranges** (batch together any commands you're setting together that fall in the
same range):
- `>1:` .. `>26:`
- `>100:` and `>110:`
- `>120:` .. `>172:`
- `>201:` .. `>211:`
- `>220:` .. `>223:`
- `>411:` .. `>415:`
- `>501:` .. `>504:`
- `>801:` .. `>815:`

You don't have to send *every* command in a range in one datagram — only the ones you *are*
sending as part of one logical operation. E.g. setting mount type + worm ratios (range 1–26),
move/slew speeds (range 120–172), and resetting the PE counter (range 501–504) as one logical
"reconfigure" operation should be sent as three datagrams (one per range touched), not fewer or
more mixed across ranges.

### Macro command: ENQ (0x05)

Sending a datagram whose `GeminiData` is just the single **ENQ byte (`0x05`)** returns a composite
status/coordinates response in one round-trip — this is also exposed as native command `>81:` (see
Part 4). Fields, semicolon-separated:

```
PRA;PDEC;RA;DEC;HA;AZ;EL;<rate>;<pier-side>;<sidereal-time>;<tracking-time-left>;<state-bits>;<RA-servo-lag>;<DEC-servo-lag>;<RA-PWM-duty>;<DEC-PWM-duty>;
```

Example from the spec:
```
1152000;1152000;0.907784;+90.000000;+6.000001;180.000000;+33.818611;N;N;N;E;6.907785;0;32;26327.898667;1;01060100;0;0;0;0;
```

Field meanings (as documented):
- `PRA`, `PDEC` — coordinates as **integer** values (physical step/encoder positions)
- `RA`, `DEC`, `HA`, `AZ`, `EL` — coordinates as **double** values
- movement rate — one of `N`/`T`/`G`/`C`/`S` (see `:Gv#`, native id `130`)
- side of pier — `W`/`E` (see `:Gm#`)
- sidereal time
- tracking time left until the Safety Limit is reached
- state bits (see native id `<99`, and `<98` for the extended status word — the exact
  correspondence between this field's on-the-wire width and the `<99`/`<98` decimal bitmasks is
  **not fully spelled out** in the source doc; treat the raw value as opaque unless/until verified
  against a live unit)
- servo lags in both axes (see native ids `245`/`246`)
- PWM duty in both axes (see native ids `247`/`248`)

The spec notes "future versions of the UDP protocol are expected to extend this command set" —
i.e. treat the exact field count/order as versioned, and prefer parsing leniently (split on `;`,
don't assume a fixed field count) if robustness matters more than strictness.

### Native command checksum (relevant to UDP payload construction)

Native commands (`<id:...#` / `>id:value...#`) carry a mandatory checksum character — computed and
verified independently of the UDP layer, but every native command sent over UDP must have a valid
one or Gemini won't act on it. See Part 4 for the full algorithm.

---
## Part 2 — Serial Command Syntax Overview

Gemini exposes two parallel command families over the same serial (or UDP-tunneled-serial)
channel:

1. **LX200-like commands** — Meade LX200-compatible commands plus Losmandy extensions. Syntax:
   `:<letters><params>#`. Values in `<>` are placeholders to be replaced by actual values; `{}`
   shows alternative characters (e.g. `{+-}` means a literal `+` or `-`). Responses, where present,
   are ASCII, terminated with `#`.
2. **Native commands** — Gemini-specific `Get`/`Set` access to internal parameters by numeric ID,
   documented in Part 4.

### Precision modes

Several LX200 Get-coordinate commands (`:GR#`, `:GD#`, `:GA#`, `:GZ#`, etc.) change their reply
format depending on a global precision mode, toggled/queried via `:U#` (toggle Low/High) and
`:u#` (select Double, L5+) and read back via `:P#`:

- **Low Precision**: short form, e.g. `<hh>:<mm>.<m>#`
- **High Precision** (default at startup): `<hh>:<mm>:<ss>#`
- **Double Precision** (`NEW in L5`): signed floating point, 6 digits after the decimal point,
  e.g. `{+-}<hh>.<hhhhhh>#`. Also applies to "Set" parameters sent in this mode.

A driver should pick one precision mode at connect time (Double Precision is the least ambiguous
to parse) and set it explicitly rather than relying on Gemini's default, since the default varies
by command and firmware level.

### Native command syntax and checksum

```
Get:  <<id>:<checksum>#            ->  <parameter value><checksum>#
Set:  ><id>:<parameter value><checksum>#   ->  (no response payload beyond ACK-substitution)
```

(from L2 up). The `id` is interpreted as an integer with leading zeros ignored. Multiple parameter
values within one command are separated by hyphens (per the general syntax note) or semicolons
(per individual command descriptions — follow the specific command's documented separator).

**Checksum algorithm** (documented once, in the Level 6 doc, applies from L2 up):

1. Take every transmitted character of the command **including** the leading `<` or `>` sign and
   the colon `:` (i.e. everything except the checksum character itself and the trailing `#`).
2. XOR all those bytes together (bytewise XOR).
3. Clear the high bit of the result (mask with `0x7F`, i.e. modulo 128).
4. Add 64 (`0x40`).

The result is a single printable ASCII character appended just before the terminating `#`.
Undefined IDs are silently ignored on Set; on Get they return just a bare `#`. A command sent with
an incorrect checksum **is not executed**. In Debug Mode, Gemini's hand-controller display shows
expected-vs-received checksum (hex) on mismatch — useful for bring-up testing against a real unit.

Worked example from the doc (quoted verbatim; the source HTML's own formatting here is a little
inconsistent, treat as illustrative rather than a byte-exact test vector):
> Get the Mount Type: `<0:v` and `<00:F#` are equivalent. `<1:w#` and `<2:t#` and `<3:u#` will
> deliver the same result, e.g. the string `1q#` if the mount type is set to GM-8 or `2r#` if G-11
> is selected.

(Note: `<1:...`, `<2:...`, `<3:...` above are querying *different* native IDs — `1`, `2`, `3` — that
are all aliases/related sub-fields of the same underlying Mount Type value as `<0:`; this is a
documented quirk, not a checksum artifact.)

### Coordinate/angle notations in use

Two different degree-minute notations appear across the tables below, both from the *same* vendor
document set — implementers should handle both:

- LX200-style commands mostly use a literal **degree sign** (`°`, byte `0xDF` in Low Precision
  mode specifically per the `:GD#` entry) between degrees and minutes, e.g. `{+-}<dd>°<mm>#`.
- Native commands (safety limits, flip points, home position, etc.) use a **literal lowercase
  `d`** as the degree/minutes separator instead, e.g. `<ddd>d<mm>` (so `002d30` = 2°30').

---
## Part 3 — LX200-like Command Reference (Level 6, Version 1.02)

This is the **current, cumulative** command table — the authoritative source for what a Gemini-2
mount running L6 firmware understands. Each row is quoted (near-)verbatim from the official
`Gemini Level 6, Version 1.02 Serial Interface Command Description`. Provenance annotations
(`From L4, V1.0 up`, `NEW in L5`, `NEW in L6`, `Obsoleted!`, etc.) are preserved in the Remarks
column — they tell you the **minimum firmware level** required for a command, or that it has been
superseded.

Category headers below (`Synchronize`, `Focus Control`, `Get Information`, `Park`, `Move`,
`Precision Guiding`, `Object/Observing/Output`, `Precession and Refraction`, `Precision`,
`Quit Moving`, `Rate`, `Set`, `Site Select`) match the source document's own grouping.


#### Handshake / ACK

| Command | Returns | Remarks |
|---|---|---|
| `0x06 (ACK char)` | B# while the initial startup message is being displayed , b# while waiting for the selection of the Startup Mode, S# during a Cold Start or G# if startup was completed with an equatorial mount selected, A# if startup was completed with an Alt/Az mount selected. NEW in L5.1 | Usable for testing the serial link and determining the type of mount (German Equatorial or Alt/Az). During Startup, with a "b#" being returned, the connected device can select the startup mode by sending either a bC# for selecting the Cold Start, bW# for selecting the Warm Start and bR# for selecting the Warm Restart. |

#### Synchronize

| Command | Returns | Remarks |
|---|---|---|
| `:CE<character>#` | &lt;character&gt;# | This commands echoes the given character, followed by a hash mark. It can be used to synchronize the serial data exchange. NEW in L5 |
| `:Cm#` | No object!# or &lt;object name&gt;# | The string "No object!#" is returned if the mount is not aligned or no object was selected, otherwise the name of the selected object used is returned. This command does an "Additional Alignment", (re)calculating the current pointing model parameters and synchronizing to the position of the selected object. |
| `:CM#` | No object!# or &lt;object name&gt;# | The string "No object!#" is returned if the mount is not aligned or no object was selected, otherwise the name of the selected object used is returned. The position (RA and DEC) is synchronized to the position of the object by setting the Index or Flip parameters of the current model. |
| `:C<n>#` | &lt;n&gt;# | Select a pointing model. Currently two models (n=0, n=1) are supported. NEW in L5 |
| `:Cc#` | &lt;n&gt;# | Selects the currently active pointing model for I/O access. Currently two models (n=0, n=1) are supported. NEW in L5 |
| `:C?#` | &lt;n&gt;# | Returns the number of the currently active pointing model. Currently two models (n=0, n=1) are supported. NEW in L5 |
| `:CI#` | No object!# or &lt;object name&gt;# | The string "No object!#" is returned if the mount is not aligned or no object was selected, otherwise the name of the selected object used is returned. An Initial Align is done. The currently selected model is reset and the mount is synchronized to the selected object. NEW in L5 |
| `:CR#` | &lt;n&gt;# | The currently selected model &lt;n&gt; is reset. NEW in L5 |
| `:CU#` | &lt;n&gt;# | The last alignment of the currently selected model is reset. NEW in L5 |

#### Focus Control

| Command | Returns | Remarks |
|---|---|---|
| `:F+#` |  | Focus In |
| `:F-#` |  | Focus Out |
| `:FQ#` |  | Stop focusing |
| `:FF#` |  | Focus Fast |
| `:FM#` |  | Focus Medium |
| `:FS#` |  | Focus Slow |

#### Get Information

| Command | Returns | Remarks |
|---|---|---|
| `:GA#` | In Double Precision mode: {+-}&lt;dd&gt;.&lt;dddddd&gt;# In High Precision mode: {+-}&lt;dd&gt;:&lt;mm&gt;:&lt;ss&gt;# In Low Precision mode: {+-}&lt;dd&gt;°&lt;mm&gt;# | Get Altitude (from L1, V2.0 up) |
| `:GB#` | &lt;n&gt;# | Get LED Display Brightness Value(from L1, V2.0 up) n=0: 100% n=6: 6.6% n=7: blank display n=8: test mode (all pixels lit). |
| `:GC#` | &lt;mm&gt;/&lt;dd&gt;/&lt;yy&gt;# | Local Calendar Date, month mm, days dd and years yy separated by slashes. |
| `:Gc#` | (24)# | Clock Format |
| `:GD#` | In Double Precision mode: {+-}&lt;dd&gt;.&lt;dddddd&gt;# In High Precision mode: {+-}&lt;dd&gt;:&lt;mm&gt;:&lt;ss&gt;# In Low Precision mode: {+-}&lt;dd&gt;°&lt;mm&gt;# | Apparent (refraction included) Declination the telescope is pointing to, to the equinox of the date. Except during GoTo operations, the coordinates are corrected according to the pointing model. Signed degrees (-90 to +90), minutes, seconds. The degree sign in Low Precision mode is the character 0xDF. |
| `:Gd#` | In Double Precision mode: {+-}&lt;dd&gt;.&lt;dddddd&gt;# In High Precision mode: {+-}&lt;dd&gt;:&lt;mm&gt;:&lt;ss&gt;# In Low Precision mode: {+-}&lt;dd&gt;°&lt;mm&gt;# | Selected object's declination. New in L5.1 |
| `:GE#` | &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# | Get Alarm time (from L1, V2.0 up) |
| `:GG#` | {+-}&lt;hh&gt;# or {+-}&lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# | Get the difference between local time and UTC (from L1, V2.0 up). If your local time is earlier than UTC this command will return a positive value, if later than UTC the value is negative. The extended format with minutes and seconds is new in L5. Minutes and seconds will be omitted if both are zero. |
| `:Gg#` | {+-}&lt;ddd&gt;°&lt;mm&gt;# In Double Precision mode: {+-}&lt;dd&gt;.&lt;dddddd&gt;# | Get Site Longitude (from L1, V2.0 up) |
| `:GH#` | [-]&lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# In Double Precision mode: {+-}&lt;hh&gt;.&lt;hhhhhh&gt;# | Hour Angle the telescope is pointing to. From L4, V1.0 up. |
| `:Gh#` | &lt;dd&gt;*# | Minimum elevation limit. NEW in L5 |
| `:GI#` | &lt;Information Buffer Content&gt;# | Get the content of the information buffer. Up to 256 characters, followed by a hash mark. NEW in L5 |
| `:GL#` | &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# In Double Precision mode: {+-}&lt;hh&gt;.&lt;hhhhhh&gt;# | Civil Time (UTC time from the internal Real Time Clock + UTC offset), hours, minutes, seconds in 24-hour format. |
| `:Gl#` | &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# In Double Precision mode: {+-}&lt;hh&gt;.&lt;hhhhhh&gt;# | UTC time from the internal Real Time Clock, hours, minutes, seconds in 24-hour format. NEW in L6 |
| `:Gm#` | {EW}# | Get Telescope RA Axis' Mount's Side. E# for East or W# for West side is replied. From L4, V1.0 up. |
| `:GM#` | &lt;name string&gt;# | Name (up to 15 characters) of the first site stored. |
| `:GN#` | &lt;name string&gt;# | Name (up to 15 characters) of the second site stored. |
| `:GO#` | &lt;name string&gt;# | Name (up to 15 characters) of the third site stored. |
| `:GP#` | &lt;name string&gt;# | Name (up to 15 characters) of the fourth site stored. |
| `:Gp#` | {LU}# | Get Telescope DEC Axis side. L# for Lower or U# for Upper half circle of physical step addresses. Note: Changing half circles at one of the poles (+-90 degrees declination) is a "meridian flip". NEW in L6. |
| `:GR#` | In Double Precision mode: {+-}&lt;hh&gt;.&lt;hhhhhh&gt;# High Precision mode: &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# Low Precision mode: &lt;hh&gt;:&lt;mm&gt;.&lt;m&gt;# | Apparent (refraction included) Right Ascension the telescope is pointing to, to the equinox of the date. Despite during GoTo operations, the coordinates are corrected according to the pointing model. Hours (0 to 24), minutes, seconds or tenth of minutes. |
| `:Gr#` | In Double Precision mode: {+-}&lt;dd&gt;.&lt;dddddd&gt;# In High Precision mode: {+-}&lt;dd&gt;:&lt;mm&gt;:&lt;ss&gt;# In Low Precision mode: {+-}&lt;dd&gt;°&lt;mm&gt;# | Selected object's right ascension. New in L5.1 |
| `:GS#` | &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# In Double Precision mode: {+-}&lt;hh&gt;.&lt;hhhhhh&gt;# | Sidereal Time. From L4, V1.0 up. |
| `:Gt#` | {+-}&lt;dd&gt;°&lt;mm&gt;# In Double Precision mode: {+-}&lt;hh&gt;.&lt;hhhhhh&gt;# | Get Site Latitude (from L1, V2.0 up). |
| `:GV#` | &lt;l&gt;&lt;vv&gt;# | Get Software Level l(one digit) and Version vv(two digits) |
| `:GVD#` | &lt;mm&gt; &lt;dd&gt; &lt;yyyy&gt;# | Get Software Built Date (from L4, V1.0 up) |
| `:GVN#` | &lt;l&gt;.&lt;vv&gt;# | Get Software Level l(one digit) and Version vv(two digits) (from L4, V1.0 up) |
| `:GVP#` | Losmandy Gemini# | Product String (from L4, V1.0 up) |
| `:GVT#` | &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# | Get Software Built Time (from L4, V1.0 up) |
| `:Gv#` | N (for "no movement") T (for Tracking) G (for Guiding) C (for Centering) S (for Slewing) ! for Stall | Get Maximum Velocity of both axes. |
| `:GW#` | N (for "no movement") T (for Tracking) G (for Guiding) C (for Centering) S (for Slewing) ! for Stall | Get Velocity RA New in L5. |
| `:Gw#` | N (for "no movement") T (for Tracking) G (for Guiding) C (for Centering) S (for Slewing) ! for Stall | Get Velocity DEC New in L5. |
| `:Gu#` | N (for "no tracking") T (for Tracking) G (for Guiding) C (for Centering) S (for Slewing) ! for Stall | Get Velocity RA, DEC (2 characters) New in L5. |
| `:GZ#` | In Double Precision mode: {+-}&lt;hh&gt;.&lt;hhhhhh&gt;# In High Precision mode: &lt;ddd&gt;:&lt;mm&gt;:&lt;ss&gt;# In Low Precision mode: &lt;ddd&gt;°&lt;mm&gt;# | Get Azimuth. From North over East. (From L4, V1.0 up) |

#### Park

| Command | Returns | Remarks |
|---|---|---|
| `:hP#` |  | Park at Home Position. The Home Position defaults to the celestial pole visible at the given hemisphere (north or south) but can be set by the user at the Gemini. |
| `:hH#` |  | Set Home Position for Park to current position. New in L5. |
| `:hC#` |  | Park at the Startup Position. This position is the position required for a Cold or Warm Start, pointing to the celestial pole of the given hemisphere (north or south), with the counterweight pointing downwards (CWD position). From L4, V1.0 up. |
| `:hZ#` |  | Park at Zenith. New in L5. |
| `:hN#` |  | Sleep Telescope: stop tracking, blank displays. |
| `:hW#` |  | Wake Up Telescope, resume tracking. |
| `:h?#` | 2: Park operation in progress 1: Park operation completed. The mount is parked. 0: No Park command received or Park operation failed. | Parking Status Inquiry |

#### Move

_Note: the directions mentioned depend upon the hemisphere of the observing site and the side of the mount the telescope actually is. Directions do not change when crossing one of the poles._

| Command | Returns | Remarks |
|---|---|---|
| `:MA#` | 0 1Object below horizon.# 2No object selected.# 3Manual Control.# | Slew to an object. The object selection had to be done by sending the Sz and Sa commands with the horizontal (Azimuth and Altitude) object coordinates. This command will be rejected while the system is in Manual Mode, f.i. identifying or selecting an object from the internal databases. From L4, V1.0 up. |
| `:MF<n>` | # | Move Find: for 0 &lt; n &lt;256 move n arcmin in a Meander Search pattern at centering speed. Can be interrupted by :Q#. If not interrupted, Move Find will return to the start position after 6 cycles. For n=0 the position is changed ("wobbled") shaping an X with 5 arcmin legs moving at a quarter of the centering speed to detect faint objects. |
| `:ML#` | # | Move Lock: Slew commands :MS# and :MA# will be suppressed, error code 3 (Manual Control) will be returned if these commands are sent. |
| `:Ml#` | # | Move Unlock: Slew commands :MS# or :MA# will be allowed to be executed again. |
| `:Mf#` | 0 1Object below horizon.# 3Manual Control.# 4Position unreachable.# | Do a meridian flip and slew to the current coordinates. |
| `:MM#` | 0 1Object below horizon.# 2No object selected.# 3Manual Control.# 4Position unreachable.# 5Not aligned.# 6Outside Limits.# 7Rejected - Mount is parked!# | Slew to an object, doing a meridian flip if possible. Selection has had to be done locally (from Gemini's databases) or by sending the Sr and Sd commands with the equatorial object coordinates. This command will be rejected while the system is in Manual Mode, f.i. identifying or selecting an object from the internal databases. |
| `:MS#` | 0 1Object below horizon.# 2No object selected.# 3Manual Control.# 4Position unreachable.# 5Not aligned.# 6Outside Limits.# 7Rejected - Mount is parked!# | Slew to an object. Selection has had to be done locally (from Gemini's databases) or by sending the Sr and Sd commands with the equatorial object coordinates. This command will be rejected while the system is in Manual Mode, f.i. identifying or selecting an object from the internal databases. |
| `:M<0..9>#` | See return code of :MS# | NEW in L6. Move to a Solar System object. 0: Sun, 1: Mercury, 2: Venus, 3: Earth Moon, 4: Mars, 5: Jupiter. 6: Saturn, 7: Uranus, 8: Neptune, 9: Pluto. |
| `:Me#` |  | Move eastwards at the selected speed rate. |
| `:Mw#` |  | Move westwards at the selected speed rate. |
| `:Mn#` |  | Move northwards at the selected speed rate. |
| `:Ms#` |  | Move southwards at the selected speed rate. |
| `:MP[+-]<RA steps>;[+-]<DEC steps>#` | See return code of :MS# | NEW in L6. Move axes by a certain amount of motor encoder ticks (steps). If a sign is specified, move to a position relative to the current position. Without a sign, the position is an absolute value. The RA position has to be within the safety limits. |
| `:Mp[+-]<RA steps>;[+-]<DEC steps>#` | See return code of :MS# | NEW in L6. Move axes by a certain amount of motor encoder ticks (steps) by absolute servo moves. If a sign is specified, move to a position relative to the current position. Without a sign, the position is an absolute value. The RA position has to be within the safety limits. The step addresses have to be in the allowed range. For testing purposes logging can be enabled, see commands 930ff. |
| `:mi<RA steps>;<DEC steps>#` |  | Changed in L6! Move axes by the signed amount of motor encoder ticks using relative servo moves. The resulting target addresses have to fit into the allowed step range. For testing purposes logging can be enabled, see commands 930ff. |
| `:mm<step multiplier>#` |  | Obsoleted! Step multiplier for the :mi command up to L4. The step count parameter of the :mi command was multiplied by this factor. |

#### Precision Guiding

| Command | Returns | Remarks |
|---|---|---|
| `:Ma<direction><arcsecs>#` |  | Moves into &lt;direction&gt; "e", "w", "n", "s" for &lt;arcsecs&gt; arc seconds. &lt;arcsecs&gt; are converted into motor encoder ticks, in L4 the result must not exceed 255 or will be cut off modulo 256. |
| `:Mi<direction><steps>#` |  | Moves into &lt;direction&gt; "e", "w", "n", "s" for &lt;steps&gt; (1 &lt;= steps &lt;= 255) motor encoder ticks. |
| `:Mg<direction><time>#` |  | Moves into &lt;direction&gt; "e", "w", "n", "s" for &lt;time&gt; milliseconds. &lt;time&gt; is converted into motor encoder ticks, in L4 the result must not exceed 255 or will be cut off modulo 256. |

#### Object/Observing/Output

| Command | Returns | Remarks |
|---|---|---|
| `:OC#` |  | Clears the Observing Log. |
| `:OI<catalog-id><object-id>#` |  | Select an object object-id from Gemini's internal databases catalog-id. Catalog-id is a character selecting one of the contiguous catalogues: '1': Messier, '2': NGC, '3': IC, '4': Sh2, '7': SAO, ':': LDN, ';': LBN. Object-id is a numeric designation of the object in the catalogue; it can be followed by an extension character for NGC and IC catalogues. |
| `:OO#` | &lt;current display content&gt;# | Ask for the current content of the output display line (up to 32 characters followed by a hash mark). New in L5. |
| `:Oo#` | &lt;previous display content&gt;# | Ask for the current content of the output display line (up to 32 characters followed by a hash mark). New in L5. |
| `:ON<name>#` |  | Tells the Gemini system the name or identification of the selected object. If this command is not used, the name defaults to "PC Object". Using this command is recommended between the :Sr and :Sd commands for equatorial coordinates or the :Sz and :Sa commands for horizontal coordinates respectively. From L4, V1.0 up. |
| `:OR#` | &lt;log entry&gt;# | Reads the next line from the Observing Log. |
| `:OS#` |  | Points to the beginning of the Observing Log. |
| `:Oc#` |  | Delete all User Catalogue entries. |
| `:Od<object line>#` |  | Download a User Catalogue entry to the Gemini. The object line consist of the object name (up to 10 ASCII characters), a comma ',' as delimiter, Right Ascension &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;, Declination {+-}&lt;dd&gt;:&lt;mm&gt;:&lt;ss&gt;. The coordinates have to be given for the epoch 2000.0. |
| `:On#` | &lt;n&gt;# | 0 &lt;= n &lt;= 4096: Read current number of Gemini's User Catalogue entries. |
| `:Or#` | &lt;object line&gt;# | Upload a User Catalogue entry from Gemini. |
| `:Os#` |  | Points to the beginning of the User Catalogue (for downloading). |

#### Precession and Refraction

| Command | Returns | Remarks |
|---|---|---|
| `:p<0..7>#` |  | Combination of three bits: xx1 Precess received coordinates from JD2000 to JNOW x1x Refract received coordinates 1xx Precess to be transmitted coordinates backwards JNOW to JD2000 1xx new in L6 |
| `:p?#` | 0..7 | Combination of three bits: xx1 Precess received coordinates from JD2000 to JNOW x1x Refract received coordinates 1xx Precess to be transmitted coordinates backwards JNOW to JD2000 New in L6 |

#### Precision

| Command | Returns | Remarks |
|---|---|---|
| `:P#` | DBL PRECISION or HIGH PRECISION or LOW PRECISION | All strings are 14 characters long (there are 2 blanks between LOW or DBL and PRECISION). |
| `:U#` |  | Toggle between Low Precision (short) and High Precision (long) mode. Gemini is in High Precision mode after starting up. |
| `:u#` |  | NEW in L5 Select the Double Precision mode. Values will be displayed in signed floating point format with 6 digits after the decimal point. "Set" parameters can be send the same way. |

#### Quit Moving

| Command | Returns | Remarks |
|---|---|---|
| `:Q#` |  | Quit all movements mentioned below. |
| `:Qe#` |  | Quit movement eastwards. |
| `:Qw#` |  | Quit movement westwards. |
| `:Qn#` |  | Quit movement northwards. |
| `:Qs#` |  | Quit movement southwards. |

#### Rate

| Command | Returns | Remarks |
|---|---|---|
| `:RC#` |  | Rate Centre. Subsequent Move commands will move at Centering Speed. |
| `:RG#` |  | Rate Guide. Subsequent Move commands will move at Guiding Speed. |
| `:RM#` |  | Rate Move. Subsequent Move commands will move at Move/Find Speed. |
| `:Rm[<sign>][<value>]#` |  | Set Move Rate, either to an absolute value (if no sign was specified) given or (if a sign was given) as increment/decrement to the current rate. If no value is specified, Move Rate is set to the default value 50. |
| `:RS#` |  | Rate Slew. Subsequent Move commands will move at Slewing Speed. |
| `:R?#` | G#, C#, M# or S# | Currently selected speed for Move commands is returned as one of the characters G, C, M or S followed by a hash mark. New in L5. |

#### Set

| Command | Returns | Remarks |
|---|---|---|
| `:Sa{+-}<dd>{*°}<mm># or :Sa{+-}<dd>{*°:}<mm>:<ss>#` | 0 if invalid or 1 if valid | Sets the object's altitude. A negative sign is ignored. Values greater than 90 degrees are set to 90 degrees. It is important that the :Sz# command has been send prior. If the coordinate selection is valid the object status is set to "Selected". From L4, V1.0 up. |
| `:SB<n>#` |  | Set LED Display Brightness Value(from L1, V2.0 up) n=0: 100% n=6: 6.6% n=7: blank display n=8: test mode (all pixels lit). |
| `:SC<mm>/<dd>/<yy>#` | 0 if invalid or 1Updating planetary data# &lt;24 blanks&gt;# | Set Calendar Date: months mm, days dd, year yy of the civil time according to the timezone set. The internal calender/clock uses GMT. |
| `:Sc<mm>/<dd>/<yy>#` | 0 if invalid or 1 &lt;24 blanks&gt;# | Set UTC Calendar Date: months mm, days dd, year yy of UTC time The internal calender/clock uses GMT. Usi:ng :Sc and :Su avoids problems with time zone settings. NEW in L5.3 |
| `:Sd{+-}<dd>{*°}<mm># or :Sd{+-}<dd>{*°:}<mm>:<ss>#` | 0 if invalid or 1 if valid | Sets the object's declination. It is important that the :Sr# command has been send prior. Internal calculations are done that may take up to 0.5 seconds. If the coordinate selection is valid the object status is set to "Selected". |
| `:SE<hh>:<mm>:<ss>#` | 1 | Set Alarm Time from the civil time hours hh, minutes mm and seconds ss. The timezone has to be set before using this command. |
| `:SG{+-}hh#` | 1 | Set the number of hours by which your local time differs from UTC. If your local time is earlier than UTC set a positive value, if later than UTC set a negative value. The time difference has to be set before setting the calendar date (SC) and local time (SL), since the Real Time Clock is running at UTC. |
| `:S0<name string>#` | 1 | Set name of site #0. The minimum length of site name strings is 1 byte, the maximum length 15 bytes. New in L5. |
| `:SM<name string>#` | 1 | Set name of the first site stored. The minimum length of site name strings is 1 byte, the maximum length 15 bytes. |
| `:SN<name string>#` | 1 | Set name of the second site stored. |
| `:SO<name string>#` | 1 | Set name of the third site stored. |
| `:SP<name string>#` | 1 | Set name of the forth site stored. |
| `:SL<hh>:<mm>:<ss>#` | 1 | Set RTC Time from the civil time hours hh, minutes mm and seconds ss. The timezone has to be set before using this command. |
| `:Sl<hh>:<mm>:<ss>#` | 1 | Set RTC Time from UTC time hours hh, minutes mm and seconds ss. NEW in L6 |
| `:Sg{+-}<ddd>*<mm>#` | 1 if valid | Sets the longitude of the observing site to ddd degrees and mm minutes. The longitude has to be specified positively for western latitudes (west of Greenwich, the plus sign may be omitted) and negatively for eastern longitudes. Alternatively, 360 degrees may be added to eastern longitudes. |
| `:Sh<dd>#` | 1 if valid, 0 if invalid | Set Minimum Elevation for GoTo operations in degrees. |
| `:Sp#` | No object!# or 1 if object coordinates were set. | Precess coordinate transmitted by means of :Sr and :Sd to the equinox of the date. |
| `:Sr<hh>:<mm>.<m># or :Sr<hh>:<mm>:<ss>#` | 0 if invalid or 1 if valid | Sets the object's Right Ascension and the object status to "Not Selected". The :Sd# command has to follow to complete the selection. The subsequent use of the :ON...# command is recommended. |
| `:St{+-}<dd>*<mm>#` | 1 if valid | Sets the latitude of the observing site to dd degrees, mm minutes. The minus sign indicates southern latitudes, the positive sign may be omitted. |
| `:Sw<n>#` | 1 if valid | Sets the Slewing rate for the Move commands |
| `:SU<hh>:<mm>:<ss>#` | 1 | Set RTC Time from UTC time hours hh, minutes mm and seconds ss. The timezone has to be set before using this command. New in L5. |
| `:Sz<ddd>{*°}<mm># or :Sz<ddd>{*°:}<mm>:<ss>#` | 0 if invalid or 1 if valid | Sets the object's azimuth. From L4, V1.0 up. |

#### Site Select

| Command | Returns | Remarks |
|---|---|---|
| `:W<n>#` |  | Select Site n with 0&lt;=n&lt;=4. |
| `:W?#` | 0&lt;=n&lt;=4. | Query currecntly selected site n |
| `:WQ#` |  | Query a connected GPS receiver. |
## Part 4 — Native Command Reference (Level 6, Version 1.02)

Syntax and checksum: see Part 2. Table columns:

- **Id** — the numeric id (as it literally appears in the source; some rows document a small
  family of related ids together, e.g. `130, 131..137`)
- **Get Parameters** — parameters to send with a Get (`<id:...#`), when the Get itself takes
  arguments (most Gets take none beyond the id + checksum)
- **Set Parameters** — parameters/range to send with a Set (`>id:value...#`)
- **Get Return Values** — what a Get returns
- **Description / Remarks** — full text, including level provenance

Functional-area headers (bold rows) are an organizational aid added for this handbook — the
source document lists all ids in one continuous table, ordered numerically. The row for id
`41, 42..44` is a documented exception: it appears **only** in the Level 5 v2.1 table (marked
`NEW in L5.2`) and is **absent from the published Level 6 v1.02 table**. That is most likely a
documentation gap rather than a removed feature (nothing else in the L5→L6 diff suggests native
commands get removed), but it has not been independently confirmed — treat it as tentative.

| Id | Get Parameters | Set Parameters | Get Return Values | Description / Remarks |
|---|---|---|---|---|
| **Mount Type & Custom-Mount Parameters** | | | | |
| `0..8` |  |  | 0: Custom Mount 1: GM-8 2: G-11 3: HGM-200 4: MI-250 5: Titan 6: Titan50 7: G-10 8: G-12 | Mount Type. Custom mount support (Id 0 and also commands 21..28) was introduced in L4. Attention: The CI-700 entry was replaced by the MI-250. CI-700 can be supported as a custom mount. |
| `10, 11..15` |  |  | 10: Neither use encoder nor end switches 11: Use Encoder 12: Test Encoder 13: Ignore Encoder 14: Use end switches 15: Don't use end switches | Axis Encoder port status. 10 can be used for requesting. |
| **Gear Ratios & Motor Encoder Resolution** | | | | |
| `21` |  | {+-}80..720 | {+-}80..720 | RA worm gear ratio. The sign indicates the direction. Note: The 1,296,00 arcsec of a circle divided by the product of worm gear ratio, spur gear ratio and motor encoder resolution define the step size per encoder tick. Gemini L4 supports step sizes from 0.2 arcsec/tick to 2.5 arcsec/tick. Combinations exceeding this range are not allowed. |
| `22` |  | {+-}80..720 | {+-}80..720 | DEC worm gear ratio. The sign indicates the direction. See note for command 21. |
| `23` |  | 10..150 | 10..150 | RA spur gear ratio. See notes for commands 21 and 27. |
| `24` |  | 10..150 | 10..150 | DEC spur gear ratio. See note for command 21. |
| `25` |  | 100..2048 | 100..2048 | RA motor encoder resolution. See notes for commands 21 and 27. |
| `26` |  | 100..2048 | 100..2048 | DEC motor encoder resolution. See note for command 21. |
| `27` |  |  | 2000..25600 | Number of RA steps for one worm revolution (since is a product of spur ratio and motor encoder ratio,this command can only be used for reading out the maximum step count, not for setting it). |
| `28` |  |  | 2000..25600 | Amount of DEC steps for one worm revolution (since is a product of spur ratio and motor encoder ratio,this command can only be used for reading out the maximum step count, not for setting it). |
| `33` |  | 10..150 | 10..150 | RA spur gear ratio in "double" format. See notes for commands 21 and 27. |
| `34` |  | 10..150 | 10..150 | DEC spur gear ratio in "double" format. See note for command 21. |
| **Startup / Parking Behavior** | | | | |
| `41, 42..44` |  |  | 42: Warm Start 43: Warm Restart 44: Ask | Default Parking mode. 41 can be used for requesting, 41: Cold Start. Documented in the Level 5 v2.1 spec; **missing from the published Level 6 v1.02 table** (likely a documentation omission, not a removed feature) &mdash; verify against a real L6 unit before relying on it. |
| **Hardware / Firmware Identification** | | | | |
| `51` |  |  | 0 .. 1 | Gemini mainboard version. Returns '0' for the original rectangular unit, '1' for the square "mini" controler. NEW in L5.2 |
| `60` |  |  | 0 .. 1 | Supress Alarm Beeps. '0': Alarm Beep is active, '1': Alarm Beep is off. NEW in L6 |
| `70` |  | 12.000.000 +- 0..11.717 | 12.000.000 +- 0..11.717 | Fine tune the 12 Mhz CPU frequency with about one promille maximum deviation. The tracking divisores are recalculated. NEW in L6.02 |
| **Status Inquiry (ENQ macro, state bits)** | | | | |
| `81` |  |  |  | Enquiry to get a coordinates and states string. See separate ENQ macro description. NEW in L5.1 |
| `91` |  | 0 or 1 | 0 or 1 | Native Command checksum behavior. 0: MSB bit is cleared (modulo 128), 1: MSB bit is not cleared. NEW in L5 |
| `92` |  | 0 .. 2 | 0 .. 2 | Park behavior. 0: Startup and every Move command (including pushing a hand controller button) wakes up the mount (default), 1: Only GoTo and Unpark commands wake up, 2: Only a WakeUp command wakes up the mount. NEW in L5.1 |
| `96` |  |  | 1: Configuration data reloaded from SD card, 2: SRAM variables structure was changed by the new firmware version, 4: Wrong SRAM checksum, f.i. after changing battery, 8: reserved, 16: RTC and location updated by GPS, 32: RTC had to be initialized, 64: reserved, 128: reserved. | Startup circumstances, a bitwise combination of several indications regarding the static ram (SRAM) containing the setup and the real-time clock RTC (both running on battery while Gemini is powered off) NEW in L5.1 |
| `97` |  |  | Eight revision characters 1: Site, 2: Date/Time, 3: Mount Parameter, 4: Display content, 5: Modelling parameters, 6: Speeds, 7: Park, 8: reserved. | State Check. This string of characters can be requested periodically to be compared with a former state. Whenever one of these characters was changed, the corresponding serial commands can be used to get the latest information. The characters are initialized to a '0' (0x30), will be incremented up to '~' (0x7E) and will then start at '0' again. There can be multiple changes between requests. NEW in L5 |
| `98` |  |  | Decimal sum of 1: Telescope is tracking in Alt/Az mode, 2: A GoTo operation is yet to be started, 4: GoTo to physical step address, 8: Servo Position GoTo is ongoing, | Extended Status Inquiry. NEW in L6 |
| `99` |  |  | Decimal sum of 1: Telescope is Aligned, 2: Modelling in use, 4: Object is selected, 8: GoTo operation is ongoing, 16: RA limit reached, 32: Gemini assumes object coordinates to refer to J2000.0 and precesses them to the equinox of the date. | Status Inquiry. |
| **RA/DEC Axis Encoders** | | | | |
| `100` |  | {+-}2048..32768 | {+-}2048..32768 | Nominal Encoder Resolution in RA. |
| `101` |  |  | 0..Encoder Resolution RA-1 | Get Encoder Value RA. |
| `110` |  | {+-}2048..32768 | {+-}2048..32768 | Nominal Encoder Resolution in DEC. |
| `111` |  |  | 0..Encoder Resolution DEC-1 | Get Encoder Value DEC. |
| **Speeds (Manual, GoTo, Move, Guiding, Centering)** | | | | |
| `120` |  | 20..2000 | 20..2000 | Manual Slewing Speed. |
| `121` |  | 20..2000 | 20..2000 | Manual Slewing Speed in RA. New in L5. |
| `122` |  | 20..2000 | 20..2000 | Manual Slewing Speed in DEC. New in L5. |
| `130, 131..137` |  |  | 131: Sidereal 132: King Rate 133: Lunar 134: Solar 135: Terrestrial Mode 136: Closed Loop 137: Comet/User Defined | Tracking Rate. 130 can be used for requesting. |
| `140` |  | 20..2000 | 20..2000 | GoTo Slewing Speed (for both axes). |
| `141` |  | 20..2000 | 20..2000 | GoTo Slewing Speed in RA. New in L5. |
| `142` |  | 20..2000 | 20..2000 | GoTo Slewing Speed in DEC. New in L5. |
| `145` |  | 20..2000 | 20..2000 | Move Speed (for both axes). New in L5. |
| `146` |  | 20..2000 | 20..2000 | Move Speed in RA. New in L5. |
| `147` |  | 20..2000 | 20..2000 | Move Speed in DEC. New in L5. |
| `150` |  | 0.2..0.8 | 0.2..0.8 | Guiding Speed (for both axes). |
| `151` |  | 0.2..0.8 | 0.2..0.8 | Guiding Speed in RA. New in L5. |
| `152` |  | 0.2..0.8 | 0.2..0.8 | Guiding Speed in DEC. New in L5. |
| `160, 161..163` |  |  | 161: Visual Mode 162: Photo Mode 163: All Speeds | Classical Hand Controller Mode. 160 can be used for requesting. |
| `170` |  | 1..255 | 1..255 | Centering Speed (for both axes). |
| `171` |  | 1..255 | 1..255 | Centering Speed in RA. New in L5. . |
| `172` |  | 1..255 | 1..255 | Centering Speed in DEC. New in L5. |
| **Alarm Mode & RA Motor State** | | | | |
| `180, 181..182` |  |  | 181: Alarm Off 182: Alarm On | Alarm Mode. 180 can be used for requesting. |
| `190, 191..192` |  |  | 191: RA Motor stopped. 192: RA Motor moving. | RA Motor Movement. Command 190 can be used to obtain the current status, 191 for stopping and 192 for restarting the tracking. |
| **TVC** | | | | |
| `200` |  | 0..255 | 0..255 | TVC Step Count. L6: Please be aware that the distance moved by TVC differs between 1x and 4x mode because of the different step sizes. |
| `201` |  | {+-}0..65535 | {+-}0..65535 | Modelling Parameter A (Polar Axis Misalignment in Azimuth), in seconds of arc. |
| `202` |  | {+-}0..65535 | {+-}0..65535 | Modelling Parameter E (Polar Axis Misalignment in Elevation), in seconds of arc. |
| `203` |  | {+-}0..65535 | {+-}0..65535 | Modelling Parameter NP (Axes Non-Perpendicularity at the Pole), in seconds of arc. |
| `204` |  | {+-}0..65535 | {+-}0..65535 | Modelling Parameter NE (Axes Non-Perpendicularity at the Equator), in seconds of arc. |
| `205` |  | {+-}0..65535 | {+-}0..65535 | Modelling Parameter IH (Index Error in Hour Angle), in seconds of arc. |
| `206` |  | {+-}0..65535 | {+-}0..65535 | Modelling Parameter ID (Index Error in Declination), in seconds of arc. |
| `207` |  | {+-}0..65535 | {+-}0..65535 | Modelling Parameter FR (Mirror Flop/Gear Play in RA), in seconds of arc. |
| `208` |  | {+-}0..65535 | {+-}0..65535 | Modelling Parameter FD (Mirror Flop/Gear Play in Declination), in seconds of arc. |
| `209` |  | {+-}0..65535 | {+-}0..65535 | Modelling Parameter CF (Counterweight &amp; RA axis Flexure), in seconds of arc. |
| `211` |  | {+-}0..65535 | {+-}0..65535 | Modelling Parameter TF (Tube Flexure), in seconds of arc. |
| **Safety Limits, GoTo Limits, Flip Points** | | | | |
| `220` |  |  | &lt;ddd&gt;d&lt;mm&gt;; &lt;ddd&gt;d&lt;mm&gt; | Set the respective Safety Limit to the current position. The Get function returns the eastern and western safety limits currently set. Note: Gemini will automatically compensate if you change hemispheres by swapping the eastern and western limits. This is because the mount is oriented northwards in the northern hemisphere and southwards in the southern hemisphere and so the side of the mount that faces east in the northern hemisphere will face west in the southern hemisphere and vice versa. |
| `221` |  | &lt;ddd&gt;d&lt;mm&gt; | &lt;ddd&gt;d&lt;mm&gt; | Get/Set eastern Safety Limit with respect to the meridian in degrees ddd and minutes mm. See note at command 220. |
| `222` |  | &lt;ddd&gt;d&lt;mm&gt; | &lt;ddd&gt;d&lt;mm&gt; | Get/Set western Safety Limit with respect to the meridian in degrees ddd and minutes mm. See note at command 220. |
| `223` |  | &lt;ddd&gt;d&lt;mm&gt; | &lt;ddd&gt;d&lt;mm&gt; | Get/Set Western GoTo limit (with respect to the meridian) in degrees ddd and minutes mm. GoTo operations will include a meridian flip if necessary to stay outside this limit. Note: If the RA angles usable for GoTo operations (East Safety Limit to Western GoTo limit are not sufficient to point to any location, GoTo operations to unreachable locations will be refused and the hand controller will display "Interrupted". A zero value (000d00) indicates that the GoTo Limit wasn't set yet and the default (002d30, allowing for at least 10 minutes tracking the object) is to be used. |
| `225` |  |  | &lt;seconds&gt; | Get Amount of steps (motor encoder ticks) to Western Safety Limit. NEW in L5 |
| `226` |  |  | &lt;seconds&gt; | Get Tracking Time to Western Safety Limit in seconds. NEW in L5 |
| `227` |  | &lt;ddd&gt;d&lt;mm&gt; | &lt;ddd&gt;d&lt;mm&gt; | Get/Set the Eastern Flip Point position with respect to the meridian in degrees ddd and minutes mm. NEW in L6 |
| `228` |  | &lt;ddd&gt;d&lt;mm&gt; | &lt;ddd&gt;d&lt;mm&gt; | Get/Set the Western Flip Point Position with respect to the meridian in degrees ddd and minutes mm. NEW in L6 |
| `229` |  | Sum 0..3 of bits 0, 1: 1: Use Flip Point East, 2: Use Flip Point West, | Sum of bits 0, 1: 1: Use Flip Point East, 2: Use Flip Point West, | Activate or Deactivate the Meridian Flip Points. NEW in L6 |
| `230` |  |  | &lt;east&gt;;&lt;west&gt; | Get physical Safety Limits in clusters of 256 motor encoder ticks. See note at command 221. |
| `231` |  |  | &lt;east&gt;;&lt;west&gt; | Get Amount of steps (motor encoder ticks) to the Safety Limits. NEW in L5 |
| **Physical Axis Position & Servo Diagnostics** | | | | |
| `235` |  |  | &lt;ra_clusters&gt;;&lt;dec_clusters&gt; | Get current physical RA and DEC axes position in clusters of 256 motor encoder ticks. |
| `236` |  |  | 0..255;0..255 | Get the remainders of the current physical RA and DEC axes position clusters. |
| `237` |  |  | &lt;ra_clusters&gt;;&lt;dec_clusters&gt; | Get the size of a half physical circle in clusters of 256 motor encoder ticks. |
| `238` |  |  | &lt;ra ticks&gt;;&lt;dec ticks&gt; | Get the size of a half physical circle in motor encoder ticks. NEW in L5 |
| `239` |  |  | &lt;ra ticks&gt;;&lt;dec ticks&gt; | Get current physical RA and DEC axes position in motor encoder ticks. NEW in L5 |
| `245` |  |  | &lt;ra lag&gt; | Get current physical RA servo motor offset, If the result is always zero, this command may require a new servo motor firmware. In L5, the lag was limited to +-390 steps, in L6 with new servo motor firmware it can exceed these values. NEW in L5.1 Extended in L6 |
| `246` |  |  | &lt;dec lag&gt; | Get current physical DEC servo motor offset, If the result is always zero, this command may require a new servo motor firmware. In L5, the lag was limited to +-390 steps, in L6 with new servo motor firmware it can exceed these values. NEW in L5.1 Extended in L6 |
| `247` |  |  | &lt;ra duty&gt; | Get current RA servo motor PWM duty, -100..100. If the result is always zero, this command may require a new servo motor firmware. NEW in L5.1 |
| `248` |  |  | &lt;dec duty&gt; | Get current DEC servo motor PWM duty, -100..100. If the result is always zero, this command may require a new servo motor firmware. NEW in L5.1 |
| `250` |  |  | &lt;Home_HA&gt;;&lt;Home DEC&gt; | Get/Set HA and DEC home position physical coordinates in arcsecs, 0..1,296,000-1. If the home position was not set, the result is zero. NEW in L6 |
| `251` |  |  | &lt;Home_HA&gt; | Get/Set HA home position in degrees and minutes. If the home position was not set, the Get result is 000d00. NEW in L6 |
| `252` |  |  | &lt;Home_DEC&gt; | Get/Set home position in degrees and minutes . If the home position was not set, the Get result is 000d00. NEW in L6 |
| **Feature Port / Encoder Port I/O** | | | | |
| `311` |  | 0..15 | 0..63 | Feature Port Status. 4 bits (0..15) can be used for setting input/output bits, 6 bits (including two additional input only bits 16 and 32, extending the range to 0..63) are available for input. |
| `312` |  |  | 0..15 | Encoder Port Status. 4 bits (0..15) can be used for reading or setting input/output bits if it is not intended to connect mount axis encoders but to use these channels alternatively. |
| **Battery Voltage** | | | | |
| `321` |  |  | Floating point value | Main battery voltage in volts. NEW in L5.2 |
| `322` |  |  | Floating point value | Lithium battery voltage in volts. NEW in L5.2 |
| **Servo Firmware / Servo Motor Control Mode** | | | | |
| `400` |  |  | &lt;ra version&gt;;&lt;dec version&gt; | Get current RA and DEC servo firmware version. NEW in L6 |
| `401` |  | 0..3 | 0..3 | Servo motor pointing precision. This command defines the step resolution per motor encoder tick for the RA and/or DEC axis. It can only be used with current servo controller software, version 2 or higher. Bit 0 refers to RA, bit 1 to DEC. A high bit sets the motor encoder resolution to quadrupled mode. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| `402` |  | 0..3 | 0..3 | Download motor control parameters at startup. This function can only be used with current servo controller software, version 2 or higher. Bit 0 refers to RA, bit 1 to DEC. A high bit enables the download. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| `403` |  | 0..3 | 0..3 | Get/Set servo motor control "Proportional on Error" parameter. Bit 0 refers to RA, bit 1 to DEC. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| **Tracking / Guiding Rate Divisors** | | | | |
| `411` |  | Up to L4: 256..65535 L5: 0..4294967295 | Up to L4: 256..65535 L5: 0..4294967295 | RA (comet) tracking rate divisor. Up to L4, the RA timer runs at 1.5 MHz, using this divisor the tracking rate can be adapted to the (mount dependent) speed of an object to be tracked. Attention: up to L4, several internal prescalers may be used for further dividing down the frequency. NEW in L5 The divisor corresponds to a 12.0 MHz timer. |
| `412` |  | Up to L4: {+-}0..65535 L5: {+-}0..2147483647 | Up to L4: {+-}0..65535 L5: {+-}0..2147483647 | DEC comet tracking rate divisor. Attention: Changed Meaning! In L3, there was no timer for DEC comet tracking available, the DEC divisor referred to the number of RA steps to be done for one step in DEC. In L4, the divisor value counts the number of internal timer ticks (at 22.888 18359 Hz) per one step in DEC, independently from RA. This corresponds to 0.657154312 arcsec/tick NEW in L5 The divisor corresponds to a 12.0 MHz timer. The sign indicates the direction. A zero value disables DEC tracking. |
| `413` |  | Up to L4: 256..65535 L5: 0..4294967295 | Up to L4: 256..65535 L5: 0..4294967295 | RA- (slow, eastwards) guiding rate divisor. L4: While guiding eastwards, an additional 4x prescaler is active. L4: A value of zero returned means a timer set to the maximum of 65536. NEW in L5 The divisor corresponds to a 12.0 MHz timer. |
| `414` |  | Up to L4: 256..65535 L5: 0..4294967295 | Up to L4: 256..65535 L5: 0..4294967295 | RA+ (fast, westwards) guiding rate divisor. See description to command 413. NEW in L5: The divisor corresponds to a 12.0 MHz timer. |
| `415` |  | Up to L4: 0..65535 L5: 0..4294967295 | Up to L4: 0..65535 L5: 0..4294967295 | DEC guiding rate divisor. NEW in L5: The divisor corresponds to a 12.0 MHz timer. |
| `416` |  | 0..4G (4294967295) | 0..4G | DEC guiding rate Prescaler. NEW in L5.1 The prescaler multiplies the value of the DEC comet tracking rate divisor to achieve very slow movements in DEC. |
| `421` |  |  | 0..4G | RA sidereal tracking rate divisor. NEW in L5.1 |
| `422` |  |  | 0..4G | Equivalent 1x DEC sidereal tracking rate divisor. NEW in L5.1 |
| `451` |  |  | 0..4G | Current RA East/West rate divisors. NEW in L6 |
| `452` |  |  | 0..4G | Current DEC rate divisor. NEW in L6 |
| `453` |  | 0 or 1 | 0 or 1 | Stop (value 0) or Start the RA motor by switching the PWM unit off/on. NEW in L6 |
| `454` |  | 0 or 1 | 0 or 1 | Stop (value 0) or Start the DEC motor by switching the timer unit off/on. NEW in L6 |
| `460` |  | 0..2 | 0..2 | RA Servo control parameter set. 0: Custom settings, 1: Standard motor, 2: Maxon motor. NEW in L6 |
| **Servo PID Parameters (RA/DEC)** | | | | |
| `461` |  | -1024..1024 | -1024..1024 | RA Servo High Speed Proportional Parameter. NEW in L6 |
| `462` |  | -1024..1024 | -1024..1024 | RA Servo High Speed Integral Parameter. NEW in L6 |
| `463` |  | -1024..1024 | -1024..1024 | RA Servo High Speed Differential Parameter. NEW in L6 |
| `464` |  | -1024..1024 | -1024..1024 | RA Servo Low Speed Proportional Parameter. NEW in L6 |
| `465` |  | -1024..1024 | -1024..1024 | RA Servo Low Speed Integral Parameter. NEW in L6 |
| `466` |  | -1024..1024 | -1024..1024 | RA Servo Low Speed Differential Parameter. NEW in L6 |
| `470` |  | 0..2 | 0..2 | DEC Servo control parameter set. 0: Custom settings, 1: Standard motor, 2: Maxon motor. NEW in L6 |
| `471` |  | -1024..1024 | -1024..1024 | DEC Servo High Speed Proportional Parameter. NEW in L6 |
| `472` |  | -1024..1024 | -1024..1024 | DEC Servo High Speed Integral Parameter. NEW in L6 |
| `473` |  | -1024..1024 | -1024..1024 | DEC Servo High Speed Differential Parameter. NEW in L6 |
| `474` |  | -1024..1024 | -1024..1024 | DEC Servo Low Speed Proportional Parameter. NEW in L6 |
| `475` |  | -1024..1024 | -1024..1024 | DEC Servo Low Speed Integral Parameter. NEW in L6 |
| `476` |  | -1024..1024 | -1024..1024 | DEC Servo Low Speed Differential Parameter. NEW in L6 |
| **Periodic Error Correction (PEC)** | | | | |
| `501` |  |  | 0..PECmax | Current RA PEC counter in steps, from 0 to the maximum step count per worm revolution PECmax. This maximum is the product of RA motor encoder resolution and spur gear ratio. It can be calculated multiplying the return values of the &lt;23 and &lt;25 commands or can be obtained directly by &lt;27. |
| `502` |  |  | 0.2 .. 0.8 | Guiding Speed used for training PEC. Only valid if PEC was trained or PEC data were downloaded, see command 509. The set command can be used without parameters and sets the guiding speed back to the value used for training. |
| `503` |  | 0..PECMax | 0..PECMax | Maximum RA PEC counter in steps, from 0 to the maximum step count PECmax. As default, this maximum is calculated as the product of RA motor encoder resolution and spur gear ratio at startup and whenever mount type or mount parameter are changed. Using this command, PECmax can be set to user defined values, f.i. to allow multiple worm cycles to be recorded. |
| `504` |  | 0..255 | 0..255 | Maximum consecutive RA PEC steps. 0 disables step supervision. Higher values define the maximum count of PEC steps in a row to enforce tiny corrections. After this maximum count is reached, normal tracking speed is reestablished. |
| `505` |  | 0..PECMax | 0..PECMax | PEC Worm Offset. |
| `506` |  | See Note | See Note | Worm Cycle Count Offset. This is an offset value added to the dynamic value derived from the RA step count integer divided by the step count per worm revolution. NEW in L6 |
| `507` |  |  | See Note | Worm Cycle Count. This is a readonly value dynamically value derived from the RA step count integer divided by the step count per worm revolution with an offset added. NEW in L6 |
| `508` |  | 0..1 | 0..1 | Enable (1) or disable (0) PEC playback at boot time, if PEC data are available. NEW in L5.2 |
| `509` |  | 0..63 | 0..63 | PEC status. Decimal sum of: 1: PEC active, 2: freshly trained (not yet altered) PEC data are available as current PEC data, 4: PEC training in progress, 8: PEC training was just completed, 16: PEC training will start soon, 32: PEC data are available. |
| `511` | offset[;0 or 1] | value;offset;repeat count | value;repeat count | Currently used PEC data. [value] can be 0 (=normal tracking),1 (RA-, guiding eastwards) or 8 (RA+, guiding westwards), or the divisor if requested. [offset] ranges from 0 to PECmax-1. [repeat count] indicates the number of equal values starting at the given offset. Extended in L6: The Level 6 PEC scheme is extended compared to L5. If the parameter values 0, 1 and 8 are used, L6 PEC is compatible to L5. In L6, step divisor values for the 12MHz oscillator can be specified directly. The minimal allowed value is 1/10th of the sidereal tracking divisor. A very smooth adaptation to the PEC curve can be reached.To get the 12MHz base tracking divisors use the optional ";1" extension after the offset value. |
| `512` | offset[;0 or 1] | value;offset;repeat count | value;repeat count | Saved PEC data. [value] can be 0 (=normal tracking),1 (RA-, guiding eastwards) or 8 (RA+, guiding westwards), or the divisor if requested. [offset] ranges from 0 to PECmax-1; [repeat count] indicates the number of equal values starting at the given offset. See notes for command 511. Obsoleted in L5 by SD card PEC files. |
| `513` |  | value | value | PEC tracking divisor for unguided steps, based on the 12Mhz clock. NEW in L5.3 |
| `515` |  |  |  | Maximum amount of PEC correction intervals. NEW in L6.0 |
| `516` |  |  |  | Current amount of PEC correction intervals used. NEW in L6.0 |
| `521` |  |  | RA-/slow step count; normal step count; RA+/fast step count | PEC statistics. Three decimal values, summing up the steps at the three speeds. |
| `522` |  |  | value | Get the PEC tracking divisor average calculated for all guiding steps of a PECmax worm period, based on the 12Mhz clock. A difference between this average and the tracking divisor indicate a possible drift. NEW in L5.3 |
| `523` |  |  | value | Get/Set the PEC tracking divisor average calculated for all steps of a PECmax worm period, based on the 12Mhz clock. The value should be equal or near to the tracking divisor. NEW in L5.3 |
| `530` |  |  |  | Start PEC training. The training phase will start about ten seconds after this command was issued. NEW in L5 |
| `531` |  |  |  | Switch PEC replay on, if there are valid PEC data available. NEW in L5 |
| `532` |  |  |  | Switch PEC replay off. NEW in L5 |
| `535` |  |  | '0': No PEC training was ongoing. '1': PEC training was aborted before it started '2': PEC training was aborted | Abort PEC training . NEW in L5 |
| `541` |  |  | value | Calculate a PEC tracking divisor for unguided steps (based on the 12Mhz clock) to compensate any drift guiding corrections may have introduced. NEW in L5.3 |
| `550` |  |  |  | Load/Store PEC data from/to SD card file \PEC\CurrPEC.pec. NEW in L5 |
| `551` | &lt;filename&gt; | &lt;filename&gt; |  | Load/Store PEC data from/to SD card file &lt;filename&gt;. NEW in L5 |
| **Display Language** | | | | |
| `601` |  |  |  | Select English as language for the following display outputs. NEW in L5 |
| `602` |  |  |  | Select German as language for the following display outputs. NEW in L5 |
| `603` |  |  |  | Select French as language for the following display outputs. NEW in L5 |
| `604` |  |  |  | Select Spanish as language for the following display outputs. NEW in L5 |
| **Basic Mount Design (EQ/AltAz)** | | | | |
| `700` |  | Mount Design | Mount Design | Basic mount design: 0: Equatorial, 1: Alt/Az. NEW in L5.1 |
| **Network Configuration (Static & DHCP)** | | | | |
| `801` |  | IPv4 address | IPv4 address | IP version 4 address in decimal dotted notation. Activates new network settings immediately. NEW in L5 |
| `802` |  | IPv4 netmask | IPv4 netmask | IP version 4 netmask in decimal dotted notation. Activates new network settings immediately. NEW in L5 |
| `803` |  | IPv4 default gateway | IPv4 default gateway | IP version 4 gateway address in decimal dotted notation. Activates new network settings immediately. NEW in L5 |
| `804` |  | IPv4 primary name server address | IPv4 primary name server address | IP version 4 name server address in decimal dotted notation. Activates new network settings immediately. NEW in L5 |
| `805` |  | IPv4 secondary name server address | IPv4 secondary name server address | IP version 4 name server address in decimal dotted notation. Activates new network settings immediately. NEW in L5 |
| `809` |  | 0..999 | 0..999 | Timespan (in seconds) for requesting and waiting for a DHCP address assignment at startup. NEW in L6 |
| `810` |  | 0: don't use DHCP 1: use DHCP | 0: don't use DHCP 1: use DHCP | Decides whether DHCP is activated at startup or not. NEW in L5 |
| `811` |  | IPv4 address | IPv4 address | IP version 4 address in decimal dotted notation. NEW in L5 |
| `812` |  | IPv4 netmask | IPv4 netmask | IP version 4 netmask in decimal dotted notation. NEW in L5 |
| `813` |  | IPv4 default gateway | IPv4 default gateway | IP version 4 gateway address in decimal dotted notation. NEW in L5 |
| `814` |  | IPv4 primary name server address | IPv4 primary name server address | IP version 4 name server address in decimal dotted notation. NEW in L5 |
| `815` |  | IPv4 secondary name server address | IPv4 secondary name server address | IP version 4 name server address in decimal dotted notation. NEW in L5 |
| `816` |  | Network Time Protocol server address | IPv4 NTP server address | IP version 4 NTP server address in decimal dotted notation. NEW in L5.2 |
| `818` |  | Ethernet port MAC address | Ethernet port MAC address | Ethernet port MAC address in hexadecimal notation. NEW in L5 |
| `826` |  |  | '0' or '1' to indicate if a asynchronous NTP server query was started. | Query the predefined Network Time Protocol Server. NEW in L5.2 |
| **SD Card Files & Servo Logging** | | | | |
| `900` |  |  | "0#" file not available "1#"file opened. | Check for and open a HC fimware file \HCfirmware\gemhc.bin. |
| `902` |  |  | '0' file not deleted '1'file deleted | Delete a HC fimware file \HCfirmware\gemhc.bin. |
| `910` | filename |  | '0' if file not found '1' if file was opened | Open a file for downloading it |
| `911` |  |  | '0' if error '2' followed by data | Read file data |
| `912` | filename |  | '0' file not deleted '1'file deleted | Delete a file. |
| `915` | filename |  | First file name matching the given pattern. | Lookup a file. Directory and wildcards can be specified. |
| `930` |  | n |  | Create and prepare a servo controller log file for the DEC axis for n lines of logging. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| `931` |  |  |  | Close servo controller log files. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| `933` |  |  |  | Remove all servo controller log files. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| `935` |  | n |  | Create/Activate a servo controller log file for the hour angle axis for n lines of logging. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| `936` |  |  |  | Start servo logging as prepared. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| `937` |  | low limit;high limit. | low limit;high limit. | Get/Set the servo motors allowed movement range of the Hour Angle axis in quadrupled (4x mode) motor encoder steps. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| `938` |  | low limit;high limit. | low limit;high limit. | Get/Set the servo motors allowed movement range of the Declination axis in quadrupled (4x mode) motor encoder steps. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| `939` |  | 0..3 | 0..3 | Use servo moves instead of ARM stepping. Bit 0: RA axis, bit 1: DEC axis. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| **Direct Servo Step-Position Move** | | | | |
| `1001` |  | n | n | Get the step position or trigger a servo move of the HA axis to the step address n. n has to be within the step limits. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| `1002` |  | n | n | Get the step position or trigger a servo move of the DEC axis to the step address n. n has to be within the step limits. This function can only be used with current servo controller software, version 3 or higher. NEW in L6 |
| **Configuration Save/Load & Factory Reset** | | | | |
| `43610` |  |  | '1' file loaded/stored '0'file not loaded/stored | Load/Store the SRAM configuration data from/into the file \config\Gemini.cfg. NEW in L5 |
| `43611` | filename | filename | '1' file loaded/stored '0'file not loaded/stored | Load/Store the SRAM configuration data from/into the file filename. NEW in L5 |
| `43690` |  |  |  | Reset to Losmandy HGM default values. |
| `43691` |  |  |  | Reset to Mountain Instruments default values. |
| **Reboot** | | | | |
| `65533` |  |  |  | Reboot the Gemini controller software, enforcing a Cold Start. |
| `65534` |  |  |  | Reboot the Gemini controller software. |
| `65535` |  |  |  | Reboot the Gemini controller software. |
## Part 5 — Gemini-1 / Legacy (Level 4) Reference

**Why this section exists**: the UDP transport (Part 1) is Gemini-2-only, but the serial command
set it carries evolved continuously from Gemini-1 (Level ≤4) through Gemini-2 (Level 5, Level 6).
The Level 6 document is cumulative and is the primary reference above, but where a detail is
ambiguous, terser, or restructured in L6, the older Level 4 document sometimes states it more
plainly (it was written before later features made the description more general/abstract). Use
this section when Part 3/4 leaves something unclear, and as a historical/compatibility reference
if the driver ever needs to talk to older firmware.

### Confirmed deltas between Level 4 and Level 6

Diffing the full command sets (see method note below) turns up surprisingly few real differences —
the LX200-like and native command sets are almost entirely **additive** from L4 to L6:

- **Mount type count grew**: L4 native id `0` (Mount Type) enumerates `0..6` (Custom, GM-8, G-11,
  HGM-200, MI-250, Titan, Titan50). L6 extends this to `0..8`, adding `7: G-10` and `8: G-12`.
- **Precession/refraction control was consolidated**: L4 (and L5) exposed four separate LX200
  commands `:p0#`/`:p1#`/`:p2#`/`:p3#`, each a fixed combination of "precess?" and "refract?"
  booleans (see the L4 table below for the plain-English meaning of each). L6 replaces these with
  a single parametrized command `:p<0..7>#` / query `:p?#`, extending the same two boolean bits
  plus a **third new bit** (`1xx`: precess *outgoing* coordinates backwards from JNOW to JD2000).
  If you only need the L5-era behavior, `:p<n>#` with `n` in `0..3` is backward-compatible with
  `:p0#`..`:p3#`.
- **`:mi<RA steps>;<DEC steps>#`** changed meaning in L6: up to L4/L5 it was a relative *stepper*
  move scaled by a separate multiplier command `:mm<step multiplier>#`; **`:mm` is `Obsoleted!` in
  L6** and `:mi` itself now performs a relative **servo** move directly (see Part 3, category
  `Move`).
- **Native id `41, 42..44`** (Default Parking mode: Cold Start / Warm Start / Warm Restart / Ask)
  is documented in L5 but missing from the L6 table — see the note in Part 4.
- Everything else present in the L4 tables below has a direct, feature-equivalent (often
  literally identically worded) counterpart in the L6 tables in Parts 3–4, typically annotated
  there with `From L4, V1.0 up`.

(Method note: this was established by extracting the full `Command`/`Id` token sets from all three
official HTML docs — Level 4, Level 5 v2.1, Level 6 v1.02 — normalizing encoding, and diffing them
pairwise. The only non-cosmetic differences found are the ones listed above; a handful of other
apparent diffs were purely due to a corrupted degree-sign byte in one source file's HTML, not real
protocol differences.)

### Full Level 4 tables (Gemini-1 hardware ceiling; also early Gemini-2 pre-L5 firmware)

Quoted from `Gemini Level 4, Version 1.0 Serial Interface Command Description` (the file is named
`Goerlich_Gemini_Protocol.htm` on disk — the filename does not reflect its actual content).
Category naming here differs slightly from L6 (e.g. `Park` is split into `Home Position` /
`Startup Position`, and `Object/Observing/Output` is just `Object/Observing` — L6 added the
`:OO#`/`:Oo#` "Output" display-line commands, hence the renamed category upstream).


#### Handshake / ACK

| Command | Returns | Remarks |
|---|---|---|
| `0x06 (ACK char)` | B# while the initial startup message is being displayed (new in L4), b# while waiting for the selection of the Startup Mode, S# during a Cold Start (new in L4) or G# after completed startup. | Usable for testing the serial link and determining the type of mount (German equatorial). During Startup, with a "b#" being returned, the PC can select the startup mode by sending a bC# for selecting the Cold Start, bW# for selecting the Warm Start and bR# for selecting the Warm Restart. |

#### Synchronize

| Command | Returns | Remarks |
|---|---|---|
| `:Cm#` | No object!# or &lt;object name&gt;# | The string "No object!#" is returned if the mount is not aligned or no object was selected, otherwise the name of the selected object used is returned. This command does an "Additional Alignment", (re)calculating the pointing model parameters and synchronizing to the position given. |
| `:CM#` | No object!# or &lt;object name&gt;# | The string "No object!#" is returned if the mount is not aligned or no object was selected, otherwise the name of the selected object used is returned. The position (RA and DEC) is synchronized to the position of the object without affecting the modelling parameters. |

#### Focus Control

| Command | Returns | Remarks |
|---|---|---|
| `:F+#` |  | Focus In |
| `:F-#` |  | Focus Out |
| `:FQ#` |  | Stop focusing |
| `:FF#` |  | Focus Fast |
| `:FM#` |  | Focus Medium |
| `:FS#` |  | Focus Slow |

#### Get Information

| Command | Returns | Remarks |
|---|---|---|
| `:GA#` | In High Precision mode: {+-}&lt;dd&gt;:&lt;mm&gt;:&lt;ss&gt;# In Low Precision mode: {+-}&lt;dd&gt;°&lt;mm&gt;# | Get Altitude (from L1, V2.0 up) |
| `:GB#` | &lt;n&gt;# | Get LED Display Brightness Value(from L1, V2.0 up) n=0: 100% n=6: 6.6% n=7: blank display n=8: test mode (all pixels lit). |
| `:GC#` | &lt;mm&gt;/&lt;dd&gt;/&lt;yy&gt;# | Local Calendar Date, month mm, days dd and years yy separated by slashes. |
| `:Gc#` | (24)# | Clock Format |
| `:GD#` | In High Precision mode: {+-}&lt;dd&gt;:&lt;mm&gt;:&lt;ss&gt;# In Low Precision mode: {+-}&lt;dd&gt;°&lt;mm&gt;# | Apparent (refraction included) Declination the telescope is pointing to, to the equinox of the date. Except during GoTo operations, the coordinates are corrected according to the pointing model. Signed degrees (-90 to +90), minutes, seconds. The degree sign in Low Precision mode is the character 0xDF. |
| `:GE#` | &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# | Get Alarm time (from L1, V2.0 up) |
| `:GG#` | {+-}&lt;hh&gt;# | Get the number of hours by which your local time differs from UTC (from L1, V2.0 up). If your local time is earlier than UTC this command will return a positive value, if later than UTC the value is negative. |
| `:Gg#` | {+-}&lt;ddd&gt;°&lt;mm&gt;# | Get Site Longitude (from L1, V2.0 up) |
| `:GH#` | [-]&lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# | Hour Angle the telescope is pointing to. From L4, V1.0 up. |
| `:GL#` | &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# | Civil Time (UTC time from the internal Real Time Clock + UTC offset), hours, minutes, seconds in 24-hour format. |
| `:Gm#` | {EW}# | Get Telescope Mount's Side of Meridian. E# for East or W# for West side is replied. From L4, V1.0 up. |
| `:GM#` | &lt;name string&gt;# | Name (up to 15 characters) of the first site stored. |
| `:GN#` | &lt;name string&gt;# | Name (up to 15 characters) of the second site stored. |
| `:GO#` | &lt;name string&gt;# | Name (up to 15 characters) of the third site stored. |
| `:GP#` | &lt;name string&gt;# | Name (up to 15 characters) of the fourth site stored. |
| `:GR#` | High Precision mode: &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# Low Precision mode: &lt;hh&gt;:&lt;mm&gt;.&lt;m&gt;# | Apparent (refraction included) Right Ascension the telescope is pointing to, to the equinox of the date. Despite during GoTo operations, the coordinates are corrected according to the pointing model. Hours (0 to 24), minutes, seconds or tenth of minutes. |
| `:GS#` | &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# | Sidereal Time. From L4, V1.0 up. |
| `:Gt#` | {+-}&lt;dd&gt;°&lt;mm&gt;# | Get Site Latitude (from L1, V2.0 up). |
| `:GV#` | &lt;l&gt;&lt;vv&gt;# | Get Software Level l(one digit) and Version vv(two digits) |
| `:GVD#` | &lt;mm&gt; &lt;dd&gt; &lt;yyyy&gt;# | Get Software Built Date (from L4, V1.0 up) |
| `:GVN#` | &lt;l&gt;.&lt;vv&gt;# | Get Software Level l(one digit) and Version vv(two digits) (from L4, V1.0 up) |
| `:GVP#` | Losmandy Gemini# | Product String (from L4, V1.0 up) |
| `:GVT#` | &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;# | Get Software Built Time (from L4, V1.0 up) |
| `:Gv#` | N (for "no tracking") T (for Tracking) G (for Guiding) C (for Centering) S (for Slewing) | Get Velocity (from L1, V2.0 up) |
| `:GZ#` | In High Precision mode: &lt;ddd&gt;:&lt;mm&gt;:&lt;ss&gt;# In Low Precision mode: &lt;ddd&gt;°&lt;mm&gt;# | Get Azimuth. From North over East. (From L4, V1.0 up) |

#### Home Position

| Command | Returns | Remarks |
|---|---|---|
| `:hP#` |  | Move to Home Position. The Home Position defaults to the celestial pole visible at the given hemisphere (north or south) but can be set by the user at the Gemini. |

#### Startup Position

| Command | Returns | Remarks |
|---|---|---|
| `:hC#` |  | Move to the Startup Position. This position is the position required for a Cold or Warm Start, pointing to the celestial pole of the given hemisphere (north or south), with the counterweight pointing downwards (CWD position). From L4, V1.0 up. |
| `:hN#` |  | Sleep Telescope: stop tracking, blank displays. |
| `:hW#` |  | Wake Up Telescope, restart tracking. |
| `:h?#` | 2: Home/CWD Search in progress 1: Home/CWD Search done 0: Home/CWD Search failed or no :hP# or :hC# command was received. | Move to Home/CWD Position Status Inquiry |

#### Move

_Note: the directions mentioned depend upon the hemisphere of the observing site and the side of the mount the telescope actually is. Directions do not change when crossing one of the poles._

| Command | Returns | Remarks |
|---|---|---|
| `:MA#` | 0 1Object below horizon.# 2No object selected.# 3Manual Control.# | Slew to an object. The object selection had to be done by sending the Sz and Sa commands with the horizontal (Azimuth and Altitude) object coordinates. This command will be rejected while the system is in Manual Mode, f.i. identifying or selecting an object from the internal databases. From L4, V1.0 up. |
| `:MF<n>` | # | Move Find: for 0 &lt;256 move n arcmin in a Meander Search pattern at centering speed. Can be interrupted by :Q#. If not interrupted, Move Find will return to the start position after 6 cycles. For n=0 the position is changed ("wobbled") shaping an X with 5 arcmin legs moving at a quarter of the centering speed to detect faint objects. |
| `:ML#` | # | Move Lock: Slew commands :MS# and :MA# will be suppressed, error code 3 (Manual Control) will be returned. |
| `:Ml#` | # | Move Unlock: Slew commands :MS# or :MA# will be allowed to be executed again. |
| `:Mf#` | 0 1Object below horizon.# 3Manual Control.# 4Position unreachable.# | Do a meridian flip and slew to the current coordinates. |
| `:MM#` | 0 1Object below horizon.# 2No object selected.# 3Manual Control.# 4Position unreachable.# 5Not aligned.# 6Outside Limits.# | Slew to an object, doing a meridian flip if possible. Selection has had to be done locally (from Gemini's databases) or by sending the Sr and Sd commands with the equatorial object coordinates. This command will be rejected while the system is in Manual Mode, f.i. identifying or selecting an object from the internal databases. |
| `:MS#` | 0 1Object below horizon.# 2No object selected.# 3Manual Control.# 4Position unreachable.# 5Not aligned.# 6Outside Limits.# | Slew to an object. Selection has had to be done locally (from Gemini's databases) or by sending the Sr and Sd commands with the equatorial object coordinates. This command will be rejected while the system is in Manual Mode, f.i. identifying or selecting an object from the internal databases. |
| `:Me#` |  | Move eastwards at the selected speed rate. |
| `:Mw#` |  | Move westwards at the selected speed rate. |
| `:Mn#` |  | Move northwards at the selected speed rate. |
| `:Ms#` |  | Move southwards at the selected speed rate. |
| `:mi<RA steps>;<DEC steps>#` |  | Move axes by a certain amount of motor encoder ticks. The parameter value is multiplied by the factor given by the last :mm command (default 1). The allowed parameter range is 0..65535, signed to select the direction. If this range is not sufficient, a prescaler factor can be set by the :mm command. New in L4V1.05. |
| `:mm<step multiplier>#` |  | Step multiplier for the :mi command. The step count parameter of the :mi command is multiplied by this factor. New in L4V1.05. |

#### Precision Guiding

| Command | Returns | Remarks |
|---|---|---|
| `:Ma<direction><arcsecs>#` |  | Moves into &lt;direction&gt; "e", "w", "n", "s" for &lt;arcsecs&gt; arc seconds. &lt;arcsecs&gt; are converted into motor encoder ticks, the result must not exceed 255 or will be cut off modulo 256. |
| `:Mi<direction><steps>#` |  | Moves into &lt;direction&gt; "e", "w", "n", "s" for &lt;steps&gt; (1 &lt;= steps &lt;= 255) motor encoder ticks. |
| `:Mg<direction><time>#` |  | Moves into &lt;direction&gt; "e", "w", "n", "s" for &lt;time&gt; milliseconds. &lt;time&gt; is converted into motor encoder ticks, the result must not exceed 255 or will be cut off modulo 256. |

#### Object/Observing

| Command | Returns | Remarks |
|---|---|---|
| `:OC#` |  | Clears the Observing Log. |
| `:OI<catalog-id><object-id>#` |  | Select an object object-id from Gemini's internal databases catalog-id. Catalog-id is a character selecting one of the contigous catalogues: '1': Messier, '2': NGC, '3': IC, '4': Sh2, '7': SAO, ':': LDN, ';': LBN. Object-id is a numeric designation of the object in the catalogue; it can be followed by an extension character for NGC and IC catalogues. |
| `:ON<name>#` |  | Tells the Gemini system the name or identification of the selected object. If this command is not used, the name defaults to "PC Object". Using this command is recommended between the :Sr and :Sd commands for equatorial coordinates or the :Sz and :Sa commands for horizontal coordinates respectively. From L4, V1.0 up. |
| `:OR#` | &lt;log entry&gt;# | Reads the next line from the Observing Log. |
| `:OS#` |  | Points to the beginning of the Observing Log. |
| `:Oc#` |  | Delete all User Catalog entries. |
| `:Od<object line>#` |  | Download a User Catalog entry to the Gemini. The object line consist of the object name (up to 10 ASCII characters), a comma ',' as delimiter, Right Ascension &lt;hh&gt;:&lt;mm&gt;:&lt;ss&gt;, Declination {+-}&lt;dd&gt;:&lt;mm&gt;:&lt;ss&gt;. The coordinates have to be given for the epoch 2000.0. |
| `:On#` | &lt;n&gt;# | 0 &lt;= n &lt;= 4096: Read current number of Gemini's User Catalog entries. |
| `:Or#` | &lt;object line&gt;# | Upload a User Catalog entry from Gemini. |
| `:Os#` |  | Points to the beginning of the User Catalog (for downloading). |

#### Precession and Refraction

| Command | Returns | Remarks |
|---|---|---|
| `:p0#` |  | No precession calculation necessary in the Gemini. Coordinates transfered to the Gemini are already precessed to the equinox of the date. Refraction is not calculated. |
| `:p1#` |  | Precession calculation is to be done by Gemini. Coordinates transfered to the Gemini refer to the standard epoch J2000.0. Refraction is not calculated. |
| `:p2#` |  | No precession calculation necessary in the Gemini. Coordinates transfered to the Gemini are already precessed to the equinox of the date. Refraction is calculated. From L4, V1.0 up. |
| `:p3#` |  | Precession calculation is to be done by Gemini. Coordinates transfered to the Gemini refer to the standard epoch J2000.0. Refraction is calculated. From L4, V1.0 up. |

#### Precision

| Command | Returns | Remarks |
|---|---|---|
| `:P#` | HIGH PRECISION or LOW PRECISION | Both strings are 14 characters long (there are 2 blanks between LOW and PRECISION). |
| `:U#` |  | Toggle between Low Precision (short) and High Precision (long) mode. The system is in High Precision mode after starting up. |

#### Quit Moving

| Command | Returns | Remarks |
|---|---|---|
| `:Q#` |  | Quit all movements mentioned below. |
| `:Qe#` |  | Quit movement eastwards. |
| `:Qw#` |  | Quit movement westwards. |
| `:Qn#` |  | Quit movement northwards. |
| `:Qs#` |  | Quit movement southwards. |

#### Rate

| Command | Returns | Remarks |
|---|---|---|
| `:RC#` |  | Rate Center. Subsequent Move commands will move at Centering Speed. |
| `:RG#` |  | Rate Guide. Subsequent Move commands will move at Guiding Speed. |
| `:RM#` |  | Rate Move. Subsequent Move commands will move at Centering Speed. |
| `:RS#` |  | Rate Slew. Subsequent Move commands will move at Slewing Speed. |

#### Set

| Command | Returns | Remarks |
|---|---|---|
| `:Sa{+-}<dd>{*°}<mm># or :Sa{+-}<dd>{*°:}<mm>:<ss>#` | 0 if invalid or 1 if valid | Sets the object's altitude. A negative sign is ignored. Values greater than 90 degrees are set to 90 degrees. It is important that the :Sz# command has been send prior. If the coordinate selection is valid the object status is set to "Selected". From L4, V1.0 up. |
| `:SB<n>#` |  | Set LED Display Brightness Value(from L1, V2.0 up) n=0: 100% n=6: 6.6% n=7: blank display n=8: test mode (all pixels lit). |
| `:SC<mm>/<dd>/<yy>#` | 0 if invalid or 1Updating planetary data# &lt;24 blanks&gt;# | Set Calendar Date: months mm, days dd, year yy of the civil time according to the timezone set. The internal calender/clock uses GMT. |
| `:Sd{+-}<dd>{*°}<mm># or :Sd{+-}<dd>{*°:}<mm>:<ss>#` | 0 if invalid or 1 if valid | Sets the object's declination. It is important that the :Sr# command has been send prior. Internal calculations are done that may take up to 0.5 seconds. If the coordinate selection is valid the object status is set to "Selected". |
| `:SE<hh>:<mm>:<ss>#` | 1 | Set Alarm Time from the civil time hours hh, minutes mm and seconds ss. The timezone has to be set before using this command. |
| `:SG{+-}hh#` | 1 | Set the number of hours by which your local time differs from UTC. If your local time is earlier than UTC set a positive value, if later than UTC set a negative value. The time difference has to be set before setting the calendar date (SC) and local time (SL), since the Real Time Clock is running at UTC. |
| `:SM<name string>#` | 1 | Set name of the first site stored. The minimum length of site name strings is 1 byte, the maximum length 15 bytes. |
| `:SN<name string>#` | 1 | Set name of the second site stored. |
| `:SO<name string>#` | 1 | Set name of the third site stored. |
| `:SP<name string>#` | 1 | Set name of the forth site stored. |
| `:SL<hh>:<mm>:<ss>#` | 1 | Set RTC Time from the civil time hours hh, minutes mm and seconds ss. The timezone has to be set before using this command. |
| `:Sg{+-}<ddd>*<mm>#` | 1 if valid | Sets the longitude of the observing site to ddd degrees and mm minutes. The longitude has to be specified positively for western latitudes (west of Greenwich, the plus sign may be omitted) and negatively for eastern longitudes. Alternatively, 360 degrees may be added to eastern longitudes. |
| `:Sp#` | No object!# or 1 if object coordinates were set. | Precess coordinate transmitted by means of :Sr and :Sd to the equinox of the date. |
| `:Sr<hh>:<mm>.<m># or :Sr<hh>:<mm>:<ss>#` | 0 if invalid or 1 if valid | Sets the object's Right Ascension and the object status to "Not Selected". The :Sd# command has to follow to complete the selection. The subsequent use of the :ON...# command is recommended. |
| `:St{+-}<dd>*<mm>#` | 1 if valid | Sets the latitude of the observing site to dd degrees, mm minutes. The minus sign indicates southern latitudes, the positive sign may be omitted. |
| `:Sw<n>#` | 1 if valid | Sets the Slewing rate for the Move commands |
| `:Sz<ddd>{*°}<mm># or :Sz<ddd>{*°:}<mm>:<ss>#` | 0 if invalid or 1 if valid | Sets the object's azimuth. From L4, V1.0 up. |

#### Site Select

| Command | Returns | Remarks |
|---|---|---|
| `:W<n>#` |  | Select stored Site n with 0&lt;=n&lt;=3. |
#### Native Commands (Level 4)

| Id | Get Parameters | Set Parameters | Get Return Values | Description / Remarks |
|---|---|---|---|---|
| **Mount Type & Custom-Mount Parameters** | | | | |
| `0..6` |  |  | 0: Custom Mount 1: GM8 2: G-11 3: HGM-200 4: MI-250 5: Titan 6: Titan50 | Mount Type. Custom mount support (Id 0 and also commands 21..28) is new in L4. Attention: The CI-700 entry was replaced by the MI-250. CI-700 can be supported as a custom mount. |
| `10, 11..15` |  |  | 10: Neither use encoder nor end switches 11: Use Encoder 12: Test Encoder 13: Ignore Encoder 14: Use end switches 15: Don't use end switches | Axis Encoder port status. 10 can be used for requesting. |
| **Gear Ratios & Motor Encoder Resolution** | | | | |
| `21` |  | {+-}80..720 | {+-}80..720 | RA worm gear ratio. The sign indicates the direction. Note: The 1,296,00 arcsec of a circle divided by the product of worm gear ratio, spur gear ratio and motor encoder resolution define the step size per encoder tick. Gemini L4 supports step sizes from 0.2 arcsec/tick to 2.5 arcsec/tick. Combinations exceeding this range are not allowed. |
| `22` |  | {+-}80..720 | {+-}80..720 | DEC worm gear ratio. The sign indicates the direction. See note for command 21. |
| `23` |  | 20..150 | 20..150 | RA spur gear ratio. See notes for commands 21 and 27. |
| `24` |  | 20..150 | 20..150 | DEC spur gear ratio. See note for command 21. |
| `25` |  | 100..2048 | 100..2048 | RA motor encoder resolution. See notes for commands 21 and 27. |
| `26` |  | 100..2048 | 100..2048 | DEC motor encoder resolution. See note for command 21. |
| `27` |  |  | 2000..25600 | Number of RA steps for one worm revolution (since is a product of spur ratio and motor encoder ratio,this command can only be used for reading out the maximum step count, not for setting it). Note: This product must not exceed 25600. If higher values are reported the combination of RA spur gear ratio and motor encoder is invalid. |
| `28` |  |  | 2000..25600 | Amount of DEC steps for one worm revolution (since is a product of spur ratio and motor encoder ratio,this command can only be used for reading out the maximum step count, not for setting it). |
| **Status Inquiry (ENQ macro, state bits)** | | | | |
| `99` |  |  | Decimal sum of 1: Telescope is Aligned, 2: Modeling in use, 4: Object is selected, 8: GoTo operation is performed, 16: RA limit reached, 32: Gemini assumes object coordinates to refer to J2000.0 and precesses them to the equinox of the date. | Status Inquiry. |
| **RA/DEC Axis Encoders** | | | | |
| `100` |  | {+-}2048..32768 | {+-}2048..32768 | Encoder Resolution in RA. |
| `101` |  |  | 0..Encoder Resolution RA-1 | Get Encoder Value RA. New in L4V1.05. |
| `110` |  | {+-}2048..32768 | {+-}2048..32768 | Encoder Resolution in DEC. |
| `111` |  |  | 0..Encoder Resolution DEC-1 | Get Encoder Value DEC. New in L4V1.05. |
| **Speeds (Manual, GoTo, Move, Guiding, Centering)** | | | | |
| `120` |  | 20..2000 | 20..2000 | Manual Slewing Speed. |
| `130, 131..137` |  |  | 131: Sidereal 132: King Rate 133: Lunar 134: Solar 135: Terrestrial Mode 136: Closed Loop 137: Comet/User Defined | Tracking Rate. 130 can be used for requesting. |
| `140` |  | 20..2000 | 20..2000 | GoTo Slewing Speed. |
| `150` |  | 0.2..0.8 | 0.2..0.8 | Guiding Speed. |
| `160, 161..163` |  |  | 161: Visual Mode 162: Photo Mode 163: All Speeds | Hand Controller Mode. 160 can be used for requesting. |
| `170` |  | 1..255 | 1..255 | Centering Speed. |
| **Alarm Mode & RA Motor State** | | | | |
| `180, 181..182` |  |  | 181: Alarm Off 182: Alarm On | Alarm Mode. 180 can be used for requesting. |
| `190, 191..192` |  |  | 191: RA Motor stopped. 192: RA Motor moving. | RA Motor Movement. Command 190 can be used to obtain the current status, 191 for stopping and 192 for restarting the tracking. |
| **TVC** | | | | |
| `200` |  | 0..255 | 0..255 | TVC Step Count. |
| `201` |  | {+-}0..65535 | {+-}0..65535 | Modeling Parameter A (Polar Axis Misalignment in Azimuth), in seconds of arc. |
| `202` |  | {+-}0..65535 | {+-}0..65535 | Modeling Parameter E (Polar Axis Misalignment in Elevation), in seconds of arc. |
| `203` |  | {+-}0..65535 | {+-}0..65535 | Modeling Parameter NP (Axes Non-Perpendicularity at the Pole), in seconds of arc. |
| `204` |  | {+-}0..65535 | {+-}0..65535 | Modeling Parameter NE (Axes Non-Perpendicularity at the Equator), in seconds of arc. |
| `205` |  | {+-}0..65535 | {+-}0..65535 | Modeling Parameter IH (Index Error in Hour Angle), in seconds of arc. |
| `206` |  | {+-}0..65535 | {+-}0..65535 | Modeling Parameter ID (Index Error in Declination), in seconds of arc. |
| `207` |  | {+-}0..65535 | {+-}0..65535 | Modeling Parameter FR (Mirror Flop/Gear Play in RA), in seconds of arc. |
| `208` |  | {+-}0..65535 | {+-}0..65535 | Modeling Parameter FD (Mirror Flop/Gear Play in Declination), in seconds of arc. |
| `209` |  | {+-}0..65535 | {+-}0..65535 | Modeling Parameter CF (Counterweight &amp; RA axis Flexure), in seconds of arc. |
| `211` |  | {+-}0..65535 | {+-}0..65535 | Modeling Parameter TF (Tube Flexure), in seconds of arc. |
| **Safety Limits, GoTo Limits, Flip Points** | | | | |
| `220` |  |  | &lt;ddd&gt;d&lt;mm&gt;; &lt;ddd&gt;d&lt;mm&gt; | Set the respective Safety Limit to the current position. The Get function returns the eastern and western safety limits currently set. Note: Gemini will automatically compensate if you change hemispheres by swapping the eastern and western limits. This is because the mount is oriented northwards in the northern hemisphere and southwards in the southern hemispshere and so the side of the mount that faces east in the northern hemisphere will face west in the southern hemisphere and vice versa. |
| `221` |  | &lt;ddd&gt;d&lt;mm&gt; | &lt;ddd&gt;d&lt;mm&gt; | Get/Set eastern Safety Limit with respect to the meridian in degrees ddd and minutes mm. See note at command 220. |
| `222` |  | &lt;ddd&gt;d&lt;mm&gt; | &lt;ddd&gt;d&lt;mm&gt; | Get/Set western Safety Limit with respect to the meridian in degrees ddd and minutes mm. See note at command 220. |
| `223` |  | &lt;ddd&gt;d&lt;mm&gt; | &lt;ddd&gt;d&lt;mm&gt; | Get/Set Western GoTo limit (with respect to the meridian) in degrees ddd and minutes mm. GoTo operations will include a meridian flip if necessary to stay outside this limit. Note: If the RA angles usable for GoTo operations (East Safety Limit to Western GoTo limit are not sufficient to point to any location, GoTo operations to unreachable locations will be refused and the hand controller will display "Interrupted". A zero value (000d00) indicates that the GoTo Limit wasn't set yet and the default (002d30, allowing for at least 10 minutes tracking the object) is to be used. |
| `230` |  |  | &lt;east&gt;;&lt;west&gt; | Get physical Safety Limits in clusters of 256 motor encoder ticks. See note at command 221. |
| **Physical Axis Position & Servo Diagnostics** | | | | |
| `235` |  |  | &lt;ra_clusters&gt;;&lt;dec_clusters&gt; | Get current physical RA and DEC axes position in clusters of 256 motor encoder ticks. |
| `236` |  |  | 0..255;0..255 | Get the remainders of the current physical RA and DEC axes position clusters. New in L4V1.05. |
| `237` |  |  | &lt;ra_clusters&gt;;&lt;dec_clusters&gt; | Get the size of an half circle of RA;DEC in clusters of 256 motor encoder ticks. New in L4V1.05. |
| **Feature Port / Encoder Port I/O** | | | | |
| `311` |  | 0..15 | 0..63 | Feature Port Status. 4 bits (0..15) can be used for setting input/output bits, 6 bits (including two additional input only bits 16 and 32, extending the range to 0..63) are available for input. |
| `312` |  |  | 0..15 | Encoder Port Status. 4 bits (0..15) can be used for reading or setting input/output bits if it is not intended to connect mount axis encoders but to use these channels alternatively. |
| **Tracking / Guiding Rate Divisors** | | | | |
| `411` |  | 256..65535 | 256..65535 | RA (comet) tracking rate divisor. The RA timer runs at 1.5 MHz, using this divisor the tracking rate can be adapted to the (mount dependent) speed of an object to be tracked. Attention: several internal prescalers may be used for further dividing down the frequency. |
| `412` |  | {+-}0..65535 | {+-}0..65535 | DEC comet tracking rate divisor. Attention: Changed Meaning! In L3, there was no timer for DEC comet tracking available, the DEC divisor referred to the number of RA steps to be done for one step in DEC. New in L4, the divisor value counts the number of internal timer ticks (at 22.888 18359 Hz) per one step in DEC, independently from RA. This corresponds to 0.657154312 arcsec/tick A zero value disables comet tracking DEC movements. |
| `413` |  | 0..65535 | 0..65535 | RA- (slow, eastwards) guiding rate divisor. While guiding eastwards, an additional 4x prescaler is active. A value of zero returned means a timer set to the maximum of 65536. |
| `414` |  | 0..65535 | 0..65535 | RA+ (fast, westwards) guiding rate divisor. |
| `415` |  | 0..65535 | 0..65535 | DEC guiding rate divisor. |
| **Periodic Error Correction (PEC)** | | | | |
| `501` |  | 0..PECmax | 0..PECmax | Current RA PEC counter in steps, from 0 to the maximum step count per worm revolution PECmax. This maximum is the product of RA motor encoder resolution and spur gear ratio. It can be calculated multiplying the return values of the &lt;23 and &lt;25 commands or can be obtained directly by &lt;27. |
| `502` |  |  | 0.2 .. 0.8 | Guiding Speed used for training PEC. Only valid if PEC was trained or PEC data were downloaded, see command 509. The set command can be used without parameters and sets the guiding speed back to the value used for training. |
| `503` |  | 0..25600 | 0..25600 | Maximum RA PEC counter in steps, from 0 to the maximum step count per worm revolution PECmax. This maximum is calculated as the product of RA motor encoder resolution and spur gear ratio at startup and whenever mount type or mount parameter are changed. Using this command, PECmax can be set to user defined values, f.i. to allow multiple worm cycles to be recorded. |
| `504` |  | 0..255 | 0..255 | Maximum consecutive RA PEC steps. 0 disables step supervision. Higher values define the maximum count of PEC steps in a row to enforce tiny corrections. After this maximum count is reached, normal tracking speed is reestablished. |
| `509` |  | 0..63 | 0..63 | PEC status. Decimal sum of: 1: PEC active, 2: freshly trained (not yet altered) PEC data are available as current PEC data, 4: PEC training in progress, 8: PEC training was just completed, 16: PEC training will start soon, 32: PEC data are available. |
| `511` | offset | value;offset;repeat count | value;repeat count | Currently used PEC data. [value] can be 0 (=normal tracking), 1 (RA-, guiding eastwards) or 8 (RA+, guiding westwards). [offset] ranges from 0 to PECmax-1. [repeat count] indicates the number of equal values starting at the given offset. |
| `512` | offset | value;offset;repeat count | value;repeat count | Saved PEC data. [value] can be 0 (=normal tracking), 1 (RA-, guiding eastwards) or 8 (RA+, guiding westwards). [offset] ranges from 0 to PECmax-1; [repeat count] indicates the number of equal values starting at the given offset. |
| `521` |  |  | RA-/slow step count; normal step count; RA+/fast step count | PEC statistics. Three decimal values, summing up the steps at the three speeds. |
| **Configuration Save/Load & Factory Reset** | | | | |
| `43690` |  |  |  | Reset to Losmandy HGM default values. |
| `43691` |  |  |  | Reset to Mountain Instruments default values. |
| **Reboot** | | | | |
| `65533` |  |  |  | Reboot the Gemini controller software, enforcing a Cold Start. |
| `65534` |  |  |  | Reboot the Gemini controller software. |
| `65535` |  |  |  | Reboot the Gemini controller software. |
## Part 6 — Supplementary Conceptual Reference (from the full user manuals)

The command tables in Parts 3–5 document *syntax*. This section documents *behavior and meaning*,
mined from the full user manuals (`GeminiL4UserManual.pdf`, `gemini_manual.pdf`,
`gemini1v2-1.pdf`) rather than the terse command-reference docs. These manuals are older and
menu/hand-controller-focused, but the underlying mount concepts (safety limits, alignment model,
PEC, tracking rates, startup modes) are unchanged in Gemini-2/L5/L6 — only the serial/UDP access
to them is newer. Hardware/cabling/menu-navigation content has been deliberately excluded; only
material relevant to a protocol/driver implementer is kept. Bracketed tags cite the source
manual/section for each fact.

### 6.1 Mount Types

**Level 4 selectable types** (menu list) [GeminiL4UserManual §5.3.10.2.5]:
Losmandy GM-8, Losmandy G-11, HGM 200, M.I. MI-250, Losmandy Titan, L. Titan (50:1), Custom Mount.

**Native command `<0` / `>0` (Mount Type) numeric IDs, Level 4** [GeminiL4UserManual native cmd
table]: `0`=Custom Mount, `1`=GM8, `2`=G-11, `3`=HGM-200, `4`=MI-250, `5`=Titan, `6`=Titan50.
"Custom mount support (Id 0 and also commands 21..28) is new in L4. Attention: The CI-700 entry
was replaced by the MI-250. CI-700 can be supported as a custom mount."

**Older (pre-L4, `gemini_manual.txt`) numbering** differs: `0`=request, `1`=GM8, `2`=G-11,
`3`=HGM-200 *or* MI-250 (shared id!), `4`=CI700, `5`=Titan. (No Titan50/id-6, no true Custom Mount
support — that's new in L4. Not relevant to a Gemini-2 driver, kept for historical-unit
compatibility only.)

**Custom Mount parameter ranges** (menu, L4) [GeminiL4UserManual §5.3.10.2.5]:
- RA & DEC Spur (Spur Gear Ratio): **20 to 150**
- RA & DEC Worm (Worm Gear Ratio): **-720 to +720, excluding -99 to +99**
- RA & DEC Motor Encoder Resolution: **100 to 2048**

(The native-command appendix's worm-ratio range, `{+-}80..720`, is a minor documentation
inconsistency with this menu prose within the same manual — not a firmware behavior difference as
far as could be determined.)

**Step-size formula** [GeminiL4UserManual §5.3.10.2.5]:
```
θ = 1,296,000 / (WormGearRatio × SpurGearRatio × NominalMotorEncoderResolution)   [arcsec/tick]
    valid range: 0.1 ≤ |θ| ≤ 2.5 arcsec/tick
```
Sign of θ indicates tracking direction. Additional constraint: motor-encoder-resolution × spur
ratio must not exceed **26,800** per the menu prose (the native-command appendix instead caps the
derived steps/worm-revolution, `<27`, at **25,600** — again likely rounding/edition drift, not two
different real limits).

`<27`/`<28` (steps per RA/DEC worm revolution) = spur ratio × motor encoder resolution, range
2000..25600, read-only/derived.

### 6.2 Safety Limits

**Default RA safety limits, standard Losmandy mounts** (GM-8, G-11, HGM-200, Titan)
[GeminiL4UserManual §3.3.3, §5.3.10.2.6]: **114° east, 122° west**, measured from the CWD startup
position. (One earlier changelog paragraph, §1.3.11, says "123°" for the west limit — likely a
typo vs. the two later 122° confirmations; treat **122°** as authoritative.)

**MI-250 defaults**: **92° east, 95° west** [GeminiL4UserManual §3.3.3, §5.3.10.2.6].

Defaults are for the **northern hemisphere**; Gemini automatically swaps east/west for the
southern hemisphere [GeminiL4UserManual §3.3.3] (matches the note already in native ids
`220`–`222`, Part 4).

**Relation to CWD**: limits are always an hour-angle offset from the CWD (counterweight-down)
startup position, not from the meridian. Narrowing a limit: start in CWD, slew to the desired new
limit, confirm via "Set Safety Limit". Extending a limit: the drive ramps down ~10° before the old
limit and stops; continuing further moves past it until the true (extended) limit, where the motor
hard-stops and a buzzer sounds continuously until back in range or a new limit is set. **Limits are
stored in battery-backed SRAM** — a "CMOS reset" at startup resets them (and mount type, lat/long)
to factory defaults [GeminiL4UserManual §5.3.10.2.6, §3.3.3].

**"Set GoTo" limit** (L4+; distinct from the hard safety limit) [GeminiL4UserManual §5.3.10.2.6,
§3.3.4]: an hour-angle (from CWD) past which any GoTo forces the OTA to the east side (flipping if
needed), independent of whether the target is technically still inside the hard safety limit.
Default is **0**, meaning "western safety limit − 2.5°" (guarantees ≥10 min tracking margin before
the hard limit). Setting it to **90** = the meridian itself. Worked example: western safety limit
122° → any GoTo target up to 29.5° west of meridian (122 − 90 − 2.5) will *not* force a flip;
beyond that, GoTo forces an eastward flip. This maps directly to native ids `223` (Western GoTo
limit) and `227`/`228` (explicit Flip Points, L6+) in Part 4.

### 6.3 Startup Modes

Three modes offered after the first successful alignment: **Cold Start**, **Warm Start**, **Warm
Restart**. Before any valid model exists (first power-up, or after a CMOS reset), only Cold Start
is offered [gemini1v2-1 §2.3, GeminiL4UserManual §5.1].

- **Cold Start**: discards all previous alignment/model parameters. Required whenever the mount
  was physically moved out of its polar-aligned position. Mount must be in CWD position; UTC
  date/time, longitude, latitude, mount type can be re-entered here.
- **Warm Start**: the mount base itself was *not* moved, but the OTA/clutches may have been.
  **Preserves modeling parameters**, but **resets assumed position to Startup/CWD** — you must
  physically return to CWD before selecting it, and must re-sync on a known star before use (same
  re-sync requirement as Cold Start, but the model itself isn't lost — "you will have to make a
  1-star-align, but after that the modeling will be used immediately").
- **Warm Restart**: use only if **neither mount nor OTA moved** since power-down. Preserves
  **both** modeling parameters **and** positional/alignment data; Gemini computes elapsed sidereal
  time since power-down and comes up already aligned — no CWD requirement, no re-sync. Best for
  permanent installs / multi-night sessions. Caution from the oldest manual: because CWD isn't
  enforced, if the position is actually wrong the mount can slew unexpectedly toward or past a
  safety limit — if it doesn't auto-decelerate ~10° before an expected limit, abort, return to
  CWD, power-cycle, and use Warm Start instead [gemini1v2-1 §2.3].

(No explanation of the `B#`/`b#`/`S#`/`G#`/`A#` ACK-response mnemonics from Part 3 was found in
these three manuals — that mapping is only documented in the command-table docs already covered
in Parts 3–5.)

### 6.4 Meridian Flip

- A German Equatorial mount cannot track continuously through the meridian without risking the OTA
  striking the mount/tripod — a flip (rotating 180° in RA while inverting Dec) is required
  [GeminiL4UserManual §3.3.2].
- **Default GoTo flip behavior**: Gemini goes to a target *without* flipping if reachable within
  the safety-limit-derived GoTo margin from the current side; otherwise it flips before slewing
  [GeminiL4UserManual §3.3.4].
- The same 2.5° margin used in the GoTo-limit default (§6.2) recurs here — it's evidently a fixed
  internal constant, not per-command configurable except by explicitly setting the GoTo/flip-point
  limits.
- Tracking is allowed to continue *past* the GoTo limit (up to the hard safety limit); a
  subsequent fresh GoTo to that same object, however, will now trigger a flip
  [GeminiL4UserManual §5.3.10.2.6].
- Manual/serial-forced early flip: HC QuickMenu "Meridian Flip", or serially `:Mf#`/`:MM#` (Part
  3) — added specifically because plain-Meade-protocol planetarium software has no other way to
  force an early flip except commanding a GoTo past the western safety limit and back
  [GeminiL4UserManual §3.3.4, §1.3.9].
- An audible warning (short beep every ~20 s) sounds some minutes before the western safety limit
  is reached while tracking, prompting manual intervention [GeminiL4UserManual §3.3.3].

### 6.5 Pointing / Alignment Model

**Sync (`:CM#`) vs. Align (`:Cm#`)** [GeminiL4UserManual §1.3.12]:
- `:CM#` ("Synchronize"): shifts the internal coordinate system to reflect the mount's actual
  position at the given object — recalculates only the **Index values (IH, ID)**, not the full
  model [gemini1v2-1 §4.3.3: "the main modelling parameters are not changed, only the Index
  values... are recalculated"]. When no model is active yet, behaves like an Initial Alignment.
- `:Cm#` ("Additional Align"): uses the same positional difference to **refit the whole pointing
  model** (least-squares/Gaussian best fit over all accumulated alignment points).
- Because some PC software only ever issues `:CM#`, Gemini offers
  **Setup→Communication→"Sync or Align"**: "Sync Only" (default: `:CM#`=sync, `:Cm#`=align) vs.
  "Sync→Add. Al." (swaps the meaning so `:CM#`-only software still improves the model over time).

**Modeling parameters** (physical meaning) [GeminiL4UserManual §3.5.3; native ids `201`-`211`,
Part 4]:

| Symbol | Meaning | Native id |
|---|---|---|
| A | Polar axis misalignment in Azimuth | 201 |
| E | Polar axis misalignment in Elevation | 202 |
| NP | Axis non-perpendicularity at the pole | 203 |
| NE | Axis non-perpendicularity at the equator | 204 |
| IH | Index error, Hour Angle | 205 |
| ID | Index error, Declination | 206 |
| FR | Gear play / mirror flop in RA | 207 |
| FD | Gear play / mirror flop in Dec | 208 |
| CF | Counterweight & RA-axis flexure | 209 |
| TF | Tube flexure | 211 (native-appendix only; not named in the L4 menu docs) |

All in seconds of arc.

**Procedure / convergence**:
- First alignment = Synchronize/Initial Alignment (sets position, starts the model).
- Each subsequent Additional Align (center a known object post-GoTo, confirm) refits the whole
  model by least-squares over all accumulated points, and re-syncs to the latest one.
- Model "won't converge on nominal values until at least 4 or 5 alignments on one side of the
  meridian"; use stars with substantially different hour angles for good conditioning
  [GeminiL4UserManual §3.5.1].
- **Gear play (FR/FD)** is only computed from the **first alignment on the opposite side of the
  meridian** after a flip [GeminiL4UserManual §3.5.1; gemini1v2-1 §4.3.2].
- Hard limit: **up to 254 Additional Aligns per side of the meridian**; ~10 alignments typically
  suffice for good pointing accuracy [gemini1v2-1 §4.3.2].
- Bad/near-singular alignment points are rejected outright ("Sorry. Rejected.") if an internal
  determinant is near zero [gemini1v2-1 §4.3.2].
- **Up to 10 complete pointing models** can be stored/reloaded (useful when swapping OTAs on a
  fixed mount) [GeminiL4UserManual §5.3.10.2.8] — this is the menu-level equivalent of the
  `:C<n>#`/`:Cc#`/`:C?#` model-selection commands in Part 3 (L5+).
- With 5+ Additional Aligns, GoTo can stay accurate even with polar alignment off by as much as
  **6°** (visual use only — imaging needs much tighter polar alignment to avoid field rotation)
  [GeminiL4UserManual §3.4.3].
- Assisted Polar-Axis Correction needs several Additional Aligns first; with the mount already
  within ~2° of true pole and ≥5 Additional Aligns, expect to land within **2 arcmin** of true
  polar alignment — but using this function **resets the modeling parameters**, so the model must
  be rebuilt afterward [GeminiL4UserManual §3.4.2].

### 6.6 PEC (Periodic Error Correction)

**Concept** [gemini_manual §4.3.1]: corrects the tracking-speed ripple caused by worm/worm-gear
manufacturing imperfections, which repeats once per worm revolution. Trained by recording the
guide corrections needed to keep a star centered over one full worm revolution, then replaying
those corrections every subsequent revolution.

**Worm period per mount** (= PEC training/replay cycle length) [GeminiL4UserManual §5.3.10.2.4;
gemini_manual §4.3.2.1]:

| Mount | Worm period |
|---|---|
| G-11, HGM-200, MI-250 | ~4 min (239.344658 s) |
| HGM Titan | ~5.33 min (319.1262611 s) |
| GM-8, CI-700 | ~8 min (478.68939 s) |

**Training countdown**: ~10 s to center the guide star before recording starts, then a display
counts down from **240→0** (G-11/HGM-200/MI-250), **320→0** (HGM Titan), **480→0** (GM-8/CI-700) —
i.e. countdown seconds = worm period in seconds. Interruptible; repeatable without limit.

**Data model**: two stored sets, **"current"** (in use) and **"backup"** (previous run, for
combining/averaging). Persists in SRAM across power-down as long as the RA motor stays engaged to
the worm [gemini_manual §4.3.1–4.3.2].

**PEC steps per worm revolution = 6400** (fixed, "Gemini is geared for 6400 steps per revolution
of the worm" — matches native id `503`/`27` PECmax for the standard mounts). Derived arcsec/step:
G-11/HGM-200/MI-250 = **0.5625"/step**; HGM Titan = **0.75"/step**; GM-8/CI-700 = **1.13"/step**.
Steps/sec at sidereal rate: G-11-class **26.737/s**, HGM Titan **20.055/s**, GM-8/CI-700
**13.367/s** [gemini_manual §4.3.2.4].

**Post-processing options** [gemini_manual §4.3.2.2–4.3.2.8]:
- **Clear Data**: wipes both sets, disables PEC (also happens automatically on CMOS reset/Restore
  Defaults).
- **PEC On/Off**: toggle without touching stored data. **PEC always starts OFF at power-up** —
  must be explicitly re-enabled each session (relevant to native id `508`, "enable PEC playback at
  boot", Part 4, which appears to be exactly the setting that overrides this default).
- **Delay Correction** (0–255 steps, 0=disabled): shifts playback timing to compensate for the lag
  between an error occurring and it being recorded (human reaction time / autoguider integration
  time) — avoids the playback fighting a live autoguider.
- **Drift Correction**: removes non-periodic systematic drift (e.g. residual polar misalignment)
  by summing signed corrections over the full worm period and spreading the net bias evenly across
  the data set, without altering the underlying tracking rate. Needed when applying a PEC curve
  trained in a different session/sky region.
- **Smooth Data**: cancels alternating (opposite-sign, adjacent) correction pairs within a
  configurable 2–255-step window, to filter seeing noise.
- **Average Data**: arithmetic mean across up to **255 equally-weighted training runs**.

**Relevant native command IDs** — see Part 4 (ids `501`–`551`); the descriptions there already
match the concepts above (this section is the "why", Part 4 is the "how to call it").

### 6.7 Tracking Rates

**Six selectable rates** [GeminiL4UserManual §1.1, §3.3.1]: Sidereal, Lunar, Solar, Adaptive King
Rate, Closed Loop, Comet/User-Defined.

- **Sidereal**: period **86164.0905 s**. Assumes accurate polar alignment.
- **Lunar / Solar**: not constant — Gemini computes topocentric Sun/Moon position "now" and one
  hour later to derive the instantaneous RA-rate; lunar rate compensates RA only (no Dec
  compensation).
- **Adaptive King Rate**: implements Rev. King's correction — user manually offsets the polar axis
  slightly toward zenith; Gemini automatically varies RA speed as a function of target declination
  and site latitude to compensate for the resulting non-uniform apparent motion.
- **Closed Loop**: compares modeled vs. actual coordinates and issues slow RA/Dec corrections
  **~22 times/second**; tolerant of several degrees of polar misalignment (but still suffers field
  rotation); requires an established pointing model.
- **Comet/User-Defined**: independent RA/Dec divisor rates set directly by user or PC software,
  bypassing all the above; persists until power-down.

**RA divisor math** [GeminiL4UserManual §3.3.1.5]:
```
RADivisor = (1,500,000 / Prescaler) / (StepsPerRev / WormPeriod)
```
(RA timer = 1.5 MHz on pre-L5 firmware — see the "12.0 MHz timer" note on native ids `411`-`422` in
Part 4 for the L5+ equivalent, a different base clock.)

| Mount | Prescaler | StepsPerRev | WormPeriod (s) |
|---|---|---|---|
| G-11, HGM-200, MI-250 | 1 | 6400 | 239.344658 |
| Losmandy Titan | 2 | 6400 | 319.1262611 |
| GM-8, CI-700 | 2 | 6400 | 478.68939 |
| Losmandy Titan 50:1 | 1 | 12800 | 319.1262611 |

Worked example (G-11 sidereal): `RADivisor = (1,500,000/1) / (6400/239.344658) = 56096`.

**Dec divisor** (pre-L5 firmware): a separate **22.8881835938 Hz** internal timer; the divisor
value divides into that frequency to get Dec steps/sec. `0` disables Dec tracking; `1` = max speed
(~22.9 steps/s); higher values slow it down. **Caution**: an older doc excerpt (`gemini_manual`,
native id `412`) instead describes the pre-L4 Dec divisor as counting *RA steps per Dec step*
rather than a fixed-frequency timer — the two descriptions genuinely conflict, most likely because
the underlying firmware behavior changed across editions (matches the "Attention: Changed
Meaning!" callout already present on native id `412` in Part 3/4's L6 table). Trust the L6 table's
own wording (Part 4) over either older manual for current firmware.

**Tracking-rate enum values** (native id `130`/`131..137`, Part 4) — cross-checked consistent:
`131`=Sidereal, `132`=King Rate, `133`=Lunar, `134`=Solar, `135`=Terrestrial Mode (= tracking off,
used by Park — see §6.10), `136`=Closed Loop, `137`=Comet/User Defined.

**Guiding rate / RA-Dec interaction** [GeminiL4UserManual §5.3.10.2.2, §4.2]: guiding range
**0.2–0.8× sidereal**, default **0.5×**. Dec guiding moves at a *constant* speed equal to the
guiding rate; RA guiding is *additive* to the base 1× sidereal rate — RA+ ⇒ (1+g)×, RA− ⇒ (1−g)×.
At max 0.8× guide rate, Dec ≈0.8×, RA ranges 0.2×–1.8×. **There is no true RA reversal at guide
speed** — "the RA drive always tracks the telescope to the west; it just speeds up or slows down
when it receives a guiding correction," only speed modulation, never direction reversal. This
matters for how a driver should interpret/implement `pulse_guide_ra`.

### 6.8 Status / State Bits

No dedicated discussion of a "status byte"/"state bits" beyond the native command tables was found
in these manuals — nothing further explains the composition of the 8-hex-char state-bits field in
the UDP ENQ macro response (Part 1). The manuals do confirm native id `99` ("Status Inquiry") is
**worded identically, unchanged, from the oldest manual through L4 through L6** — i.e. it's a
stable, reliable field across the whole product line:

> **Id 99 — Status Inquiry.** Decimal sum of: `1`=Telescope is Aligned, `2`=Modeling in use,
> `4`=Object is selected, `8`=GoTo operation is performed, `16`=RA limit reached,
> `32`=Gemini assumes object coordinates to refer to J2000.0 and precesses them to the equinox of
> the date.

Related: changing a mount-parameter setting (gear ratios etc.) while running **invalidates
internal position data and sets state to "Unaligned"** — GoTo/Park are then rejected until the
mount is manually returned to Startup Position and Cold Started [GeminiL4UserManual §7.1.6]. A
driver that lets a user change native mount-config commands (Part 4, ids `21`-`34`) live should
expect this and re-poll `<99` afterward rather than assuming alignment survived.

### 6.9 Older-Firmware Deltas Worth Knowing

- Mount-type native-ID numbering differs between the pre-L4 manual (`0..5`, HGM-200/MI-250 share
  an id, no Custom Mount, no Titan50) and L4+ (`0..8`, Custom Mount = 0). Irrelevant for a
  Gemini-2/L4-L6 driver target, but explains historical-unit behavior if ever encountered.
- **"Synchronize" (`:CM#`) is explicitly marked "(new in Version 2)"** in the oldest manual
  [gemini1v2-1 §4.3.3] — Level 1 v2.1 is the first firmware where Sync and Additional-Align exist
  as a distinct pair; earlier Level 1 firmware apparently only had "Initial Alignment"/"Additional
  Alignment". Protocol history only, not relevant to current targets.

### 6.10 Other Behavioral Notes for a Driver Implementer

- **GoTo/Park rejection conditions** [GeminiL4UserManual §7.1.6]: (a) state "Unaligned" after a
  parameter change — must Cold Start; (b) telescope currently outside the RA safety limits — must
  be manually slewed back in range first; (c) safety-limit/GoTo-limit configuration makes the
  target physically unreachable — GoTo silently fails, hand controller shows **"Interrupted"**
  (this is presumably what LX200 error code `6` "Outside Limits" / `4` "Position unreachable" on
  `:MS#`/`:MM#`, Part 3, correspond to).
- **Slew/GoTo speed defaults and ramping** [GeminiL4UserManual §5.3.10.2.2]: default
  Slewing = GoTo = **800× sidereal** (max settable 1200×); GoTo slews **ramp down to Centering
  speed** on final approach to avoid overshoot; manual slews ramp down on button/command release;
  near a safety limit the drive ramps down starting ~10° before the limit, then hard-stops with a
  continuous buzzer if pushed further.
- **Centering speed**: settable up to 255× sidereal.
- **TVC (Time Variable Compensation)** [GeminiL4UserManual §5.3.10.2.3]: Dec backlash compensation
  — injects 0-255 extra high-speed steps whenever Dec direction reverses, to eliminate mechanical
  hysteresis when autoguider or hand-controller Dec guiding reverses direction. Maps to native id
  `200` (TVC Step Count) in Part 4.
- **Autoguider port ↔ Hand-Controller-Mode interaction** [gemini_manual §4.2.1]: the ST-4-style
  autoguider port is **only electrically active in Photographic Mode and All-Speeds Mode**
  (native id `160`/`161..163`, Part 4) — selecting Visual Mode disables it entirely. Serial
  pulse-guide commands (`:Mg#` etc., Part 3) are presumably independent of this hardware gating,
  but a driver relying on hardware-guide-port behavior should account for it. Simultaneous
  opposing directional signals on the autoguider port (Dec+/Dec− or RA+/RA− at once) are rejected
  as **"Autoguider Error."**
- **Park behavior** [GeminiL4UserManual §5.4.5]: "Park @CWD pos!" slews to the Startup Position
  and sets tracking rate to **Terrestrial** (i.e. stops tracking — native tracking-rate value
  `135`). "Park @Home pos!" targets a separately-settable Home Position (defaults to CWD, settable
  from any current position via `Setup→Mount Parameters→Set Home Posit.`, matching native ids
  `250`-`252` and LX200 `:hH#` in Part 3/4). "Stop Tracking!" halts tracking in place without
  slewing anywhere.
- **Timezone sign convention pitfall** [GeminiL4UserManual, troubleshooting]: "Some programs and
  computer clocks may define their timezones with signs (+/-) opposite to those required by
  Gemini" — worth being defensive about when a driver computes UTC from local civil time (`:SG#`,
  Part 3).
- **Debug Mode**: holding any hand-controller key while powering up puts Gemini into Debug Mode,
  which echoes all received serial commands (and checksum mismatches) to the Information Buffer
  (`:GI#`, Part 3) — useful diagnostic hook when bringing up a new UDP transport implementation
  against real hardware [gemini1v2-1 §2.3].
- **GPS caveat**: if geographic location/UTC were supplied by a connected GPS at power-on, values
  entered manually via Setup after that Cold Start are **not applied** until Gemini is
  power-cycled and Cold Started again (this restriction does not apply to values entered *during*
  the Cold Start prompt sequence itself) [GeminiL4UserManual §1.3, §3.5.2, §5.3.9].


## Part 7 — Implementation Notes for the RTS2 Driver

Practical points that fall out of the material above, collected for quick reference:

- **Transport is Gemini-2 only.** Don't try to reuse the UDP layer for Gemini-1 — it doesn't
  exist there. The serial *command* semantics (Parts 3–5) are what carry over.
- **No auth, no framing beyond NULL-termination.** Anyone who can reach UDP/11110 can command the
  mount; if that matters for your deployment, handle it at the network layer (firewall/VPN), not
  in the driver.
- **Always wait for a response datagram**, even for commands with no serial-level output — expect
  the `0x06` ACK substitution and don't treat it as an error.
- **Batch, don't trust ordering.** For any multi-command sequence where order matters (slews,
  native parameter groups — see the critical-sequence list in Part 1), concatenate into one
  `GeminiData` field rather than firing multiple datagrams and hoping they arrive in order.
- **Implement the NACK resync path**, not just a naive timeout-and-retry — retrying blindly risks
  double-executing a command Gemini already processed (most Set commands are idempotent, but GoTo
  and relative-move commands like `:mi`, `:MP`, `:Ma`/`:Mi`/`:Mg` are not).
- **Pick one precision mode at connect and stick to it** (Part 2) — Double Precision (`:u#`) is
  the least error-prone to parse (fixed-format signed float) and avoids the sexagesimal
  string-parsing variability of Low/High precision.
- **Native command checksums are mandatory**, independent of the UDP layer's own framing — a
  malformed checksum is silently *not executed* by Gemini, which will look like a mysteriously
  ignored command if you skip implementing Part 2's checksum algorithm.
- **Use `>81:` / ENQ (`0x05`) for high-frequency polling** (position, tracking state, side of
  pier, servo lag/PWM) instead of separate `:GR#`/`:GD#`/... calls — it's one round-trip instead
  of several, which matters more over UDP where each round-trip carries its own loss/retry risk.
- **`:h?#` / native status ids `<98`/`<99`** are the way to poll GoTo/park completion — don't
  infer completion from silence or from coordinate convergence alone, since GoTo failure modes
  (`Position unreachable`, `Outside Limits`, `Not aligned`, `Rejected - Mount is parked!`) are
  returned as part of the *original* `:MS#`/`:MM#`/`:MA#` response, not asynchronously.
- **Mind the two different degree-notation conventions** (Part 2) when writing parsers — a LX200
  coordinate parser and a native-command safety-limit parser cannot share the same regex.

---
## Part 8 — Field Experience from `rts2-teld-gemini-udp` (this driver)

Everything above this line is vendor-documentation-only, by design (see the top of this file). This
section is the deliberate exception: findings from actually running `rts2-teld-gemini-udp` (this
repo's `base/teld/gemini-udp/`) against a live Titan mount, and from cross-reading the two existing
functional drivers this project was ported from/alongside — the classic RS232 driver
(`base/teld/gemini/gemini.cpp`, itself a port of `src/teld/gemini.cpp`, authored over the years by
Petr Kubánek and Jan Strobl) and the actively-deployed production RS232 driver at the telescope
(`~/gemini2ser.cpp`, Jan Strobl 2018, with later hand-guiding additions). Where this section
disagrees with or sharpens Parts 1–7, that's the point — it's what those parts explicitly excluded.

### 8.1 ENQ macro (0x05) field layout — resolved

Part 1 flags the ENQ macro's field list as not fully reliable ("the exact correspondence...is not
fully spelled out...treat as opaque unless verified against a live unit"). It has now been verified,
two ways:

1. Against the UDP spec's own worked example (`1152000;1152000;0.907784;+90.000000;+6.000001;
   180.000000;+33.818611;N;N;N;E;6.907785;0;32;26327.898667;1;01060100;0;0;0;0;`): field 4 (HA,
   6.000001) + field 2 (RA, 0.907784) = 6.907785, which equals field **11**, not field 9 as the
   spec's own named-field list would suggest if read as a direct 1:1 sequence. Field **10** is `E`,
   a valid pier-side letter.
2. Against many live goto/park/flip sequences this session (see this repo's git history under
   `base/teld/gemini-udp/`) — pier side at field 10 and LST at field 11 held up consistently,
   including through a real, confirmed meridian flip.

**Resolved 0-indexed layout** (`GeminiCaringLoop::parseEnq()`,
`base/teld/src/geminicaringloop.cpp`): `0`=PRA, `1`=PDEC, `2`=RA (hours), `3`=DEC (deg), `4`=HA
(hours), `5`=AZ, `6`=EL, `7`=movement rate (`:Gv#` alphabet — `N` observed for "idle", not an error),
`8`,`9`=unconfirmed (bracketed by two confirmed neighbors — worth investigating live, if it matters,
before guessing labels), `10`=pier side, `11`=LST, **`12`=native id `99` state bits, confirmed
live** (see 8.3 below). Fields **13+** (servo lag ×2, PWM duty ×2, and whatever else trails) remain
captured as an opaque raw string (`GeminiStatus::rawExtended`) and not parsed — same caution the
spec itself advises ("future versions...are expected to extend this command set").

### 8.2 `:MS#` performs a meridian flip in practice

Part 3's table only states flip behavior explicitly for `:MM#` ("doing a meridian flip if possible")
and is silent on it for `:MS#` — an omission, not a documented "no". Live-verified this session: a
fixed-RA Dec walk from +80° to -10° using `:MS#` exclusively crossed a real pier-side flip (W→E)
with zero rejections. `gemini2ser.cpp`'s own `makeMeridianFlip()` uses `:MM#` for the specific
"forced early flip while tracking near the limit" case (matching Part 3's `:Mf#`/`:MM#` mention
under §6.4 as the intentional-early-flip mechanism) but its regular `startResync()` path — like this
driver's — just sends `:MS#` unconditionally and lets Gemini decide, which the live evidence above
confirms works. This driver made the same choice after briefly trying `:MM#` for *all* moves and
hitting a real stuck-mount incident on the very first attempt (see git history) — inconclusive on
`:MM#` itself, but `:MS#` has the deeper track record, so that's what's used.

### 8.3 Native id 99 is genuinely documented, not reverse-engineered

Prior conversation in this project stated register 99's bit meanings were undocumented and
reverse-engineered by whoever wrote `gemini2ser.cpp`/`gemini.cpp` — **that was wrong**, and this
document is the correction. `GeminiL4UserManual.pdf` documents id 99 explicitly, and the same
wording is confirmed unchanged through the L6 native-command table (Part 4, id `99`, line ~541/959
of this file): `1`=Aligned, `2`=Modeling in use, `4`=Object selected, `8`=GoTo ongoing, `16`=RA
limit reached, `32`=J2000→date precession assumed. It's a stable, documented, cross-firmware-version
field — `gemini2ser.cpp`'s use of bit 8 (its `isMoving()`/idle-loop "is the mount busy" check) and
bit 16 (its reactive `makeMeridianFlip()` trigger, on top of the proactive id-226 countdown this
driver already implements) are both using it exactly as documented, not guessing.

**Confirmed live (2026-08, mount parked/idle): a direct `<99:` query and an ENQ poll taken back to
back agree exactly** — `<99:` replied `1` (Aligned only, as expected for an idle parked mount), and
the same ENQ response's field 12 was also `1`. **Id 99's state bits are already present in every
routine ENQ poll this driver already does, at field 12** (see 8.1's resolved layout) — no separate
poll needed. This driver did not parse that field before this finding; `GeminiCaringLoop::
parseEnq()` should be extended to decode it (bit 8 = GoTo ongoing, a more direct move-completion
signal than the current position-stability heuristic; bit 16 = RA limit reached, a reactive backstop
for the proactive id-226 countdown this driver already acts on). Part 7's own advice ("don't infer
completion from...coordinate convergence alone") already recommended this in general; this finding
means it can be had for free, from data already being fetched every cycle.

### 8.4 Three pulse-guide commands with three different units — open question

Part 3's Precision Guiding table documents three distinct commands, easy to conflate:

| Command | Unit | Range |
|---|---|---|
| `:Ma<dir><arcsecs>#` | arcseconds | converted to ticks internally, cut off mod 256 at L4 |
| `:Mi<dir><steps>#` | raw motor-encoder ticks | 1–255, direct |
| `:Mg<dir><time>#` | **milliseconds** | converted to ticks internally, cut off mod 256 at L4 |

`gemini2ser.cpp`'s `performGuide()` sends `:Mi` — raw ticks — and its `pulse_guide_ra`/
`pulse_guide_dec` Values are correctly labeled `"[ticks]"` in its own `createValue()` call. But the
two Python guide scripts that write those values (`python/rts2/guide.py`, `guideccd.py`) compute and
log their output explicitly as **milliseconds** (`'guide: ...pulse=[%d,%d]ms...'`, clamped to
±1000), and pass that number straight through. If `:Mi`'s parameter is genuinely raw ticks as
documented, values above 255 are silently dropped by `performGuide()`'s own guard (`abs(pulseLength)
> 255` → no-op) — meaning corrections in the ~255-1000 "ms" range the script computes never reach
the mount at all, silently. Whether this has actually been a problem in practice (i.e. whether real
guide corrections this driver produces mostly stay under 255 of whatever unit, making it a
non-issue) isn't something this document can determine — that's an empirical question about actual
guiding performance at the telescope, not a protocol one. This driver (`rts2-teld-gemini-udp`) copied
`gemini2ser.cpp`'s `:Mi`-based approach directly (see git history) precisely because it's the
verified-in-production choice; switching to `:Mg` (the unit the guide scripts actually claim to be
using) is a plausible fix but unverified — worth deciding with input from whoever tuned the guide
scripts' `self.factor` constant, since that constant may have been empirically calibrated against
`:Mi`'s actual (ticks) behavior regardless of what the code calls it.

### 8.5 `:p<0-7>#` (precession/refraction) — this driver sends nothing

Confirmed bit semantics (Part 3): bit 0 = precess incoming J2000 coords to now, bit 1 = refract
incoming coords, bit 2 (`NEW in L6`) = precess outgoing coords back to J2000. `gemini2ser.cpp` sends
one of `:p0#`–`:p3#` at init, chosen from its own `calculateAberation()`/`calculatePrecession()`/
`calculateRefraction()` flags (RTS2 base-class Values, gated by CLI options that driver exposes).
`rts2-teld-gemini-udp` never sends `:p#` at all, and never touches those same base-class flags
(which default to `false` in `base/teld/include/teld.h`/`teld.cpp`) — so whatever `:p#` state the
mount was last left in (by `gemini2ser.cpp`, by hand, by factory default) is silently still in
effect, unverified. Applying `gemini2ser.cpp`'s own decision logic to this driver's actual (default,
all-`false`) flag state lands on **`:p3#`** (Gemini precesses and refracts internally, since this
driver's software does neither) — the coherent choice given the current defaults, not yet sent.

### 8.6 `:Sr`/`:Sd` without `:ON` in between — live-verified fine

Part 1's critical-sequence list recommends `:Sr` then `:ON<name>#` then `:Sd`, in that order. This
driver's `GeminiCaringLoop::handleGoto()`/`syncTo()` send `:Sr`+`:Sd` batched together with no `:ON`
in between at all, and this has held up across the whole of this session's live testing (many gotos,
a real flip, a real park). Worth knowing if a future change ever reintroduces per-command sends
instead of batching — the `:ON` omission specifically has not caused an observed problem, but
hasn't been stress-tested against it being *required* either (e.g. under packet loss/resync, where
Gemini's own object-name/selection state might matter more than it has in these clean-network tests).
