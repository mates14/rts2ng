# rts2-teld-gemini-udp — operating the Gemini-2 mount

What the driver does on its own, what it refuses to do, and what an operator tells it. Written for the
SBT (Small Binocular Telescope) mount after the September 2026 rework. The protocol-level background,
including what was read out of the Gemini-2 firmware, is in `gemini_protocol_reference.md` §8.7–8.8.

**Status:** everything below has been exercised against a simulator only. The dry-run plan at the end
is the first step on the real mount.

---

## 1. Why the driver distrusts the mount

Gemini knows where the telescope points only through its motor encoder counters (native 239). They are
relative: the mount learns their zero at a cold or warm start, when it simply *assumes* the telescope is at
CWD (counterweight down, pointing at the pole). A cold start done anywhere else, a clutch slip, or somebody
moving the mount by hand leaves the counters wrong, and nothing in the mount can notice. Everything
measured in counters is then wrong too: the safety limits, the meridian flip decision, every goto.

The driver therefore keeps its own verdict on the counters, `position_trust`:

| state | meaning | what the driver allows |
|---|---|---|
| `CONFIRMED` | astrometry agreed with the counters | everything |
| `ASSUMED` | nothing suggests a problem, nothing proved it either | everything |
| `LOST` | evidence that the counters are wrong, or a startup nobody vouched for | **no moves, no parking, no tracking** |
| `UNKNOWN` | the driver has not seen the mount start up yet | the mount refuses motion anyway |

`position_reason` always says why. The verdict is kept in the state file (`--position-state`, default
`/var/log/rts2/gemini-udp-position.state`), so restarting the driver neither forgets a LOST position nor
disturbs a healthy mount. Every change to LOST is also written to the incident log
(`--incident-log`, default `/var/log/rts2/gemini-udp-incidents.log`).

Fully automatic recovery without absolute position sensing is impossible, and the driver does not
pretend otherwise. When the counters are in doubt it says so and waits for a human, or for enough
astrometry (section 5).

---

## 2. The `position` command

A single command, with the first word saying what the operator is telling the driver.

| command | use when | what happens |
|---|---|---|
| `position` | any time | logs the current trust, reason, handshake state and axis counters |
| `position ok` | you have looked and the mount's position is fine | → `ASSUMED`; also releases a safety lock |
| `position lost` | you know or suspect the position is wrong | → `LOST`; stops tracking and motion |
| `position unmoved` | the mount waits in its boot menu **and** nothing moved while it was off | answers the menu with *restart* (`bR#`, stored counters kept) → `ASSUMED`; stays `LOST` if the position was already lost before the power cycle |
| `position cwd` | the telescope is **physically at CWD** | answers the boot menu with *cold start* (`bC#`), or reboots a running mount into one (65533) → `ASSUMED` once the counters read CWD; `LOST` if the mount skipped its menu and did not cold-start |
| `position cwd warm` | as above, keeping Gemini's own pointing model | warm start (`bW#` / 65535) |
| `position rezero` | you want a re-zero now from the astrometry collected so far | see section 5 |
| `position abort` | a re-zero is running and has not reached its cold start | stops it → `LOST` |

Commands are sent the usual way, e.g. from `rts2-mon` or `rts2-sendcmd T0 position cwd`.

---

## 3. What happens on its own

### Driver start, or connecting to a mount that is already up

Nothing is sent that changes the mount:

- no `:hW#`: in this firmware it means "unpark and start tracking", which on a parked mount starts
  tracking away from the park position;
- the clock is **read**, and set only if it is more than 2 s off system UTC;
- the parked flag (battery-backed in Gemini) is read, so a parked mount shows as parked;
- the verdict comes from the state file. `LOST` stays `LOST`. Otherwise → `ASSUMED`, unless the counters
  were reset to CWD since the driver last ran.

### The mount reboots or is power-cycled

- **Boot menu with `startup_mode` = `NONE` (the default):** the driver does not answer it. The position
  becomes `LOST` with the reason "waiting in its boot menu", until an operator sends `position unmoved` or
  `position cwd`.
- **`startup_mode` = `RESTART`** (`--startup-mode restart`): answered automatically; the stored counters are
  kept, so the verdict stays what it was.
- **`startup_mode` = `WARM_START` / `COLD_START`:** answered automatically, but the position becomes `LOST`,
  because nobody confirmed the telescope is at CWD.
- **The mount picks a mode by itself** (Gemini's `DefaultBootMode`): if the counters come back exactly at
  CWD it was a cold/warm start → `LOST`; otherwise the counters were kept.

After any silence the driver checks the startup handshake first, so a power cycle is noticed within a
poll or two of the mount answering again. A poll into silence itself takes about 10 s to give up.

### A cold start behind the driver's back

A cold or warm start sets the Dec counter to *exactly* half a circle. If that happens while nothing of the
driver's was moving (hand controller, Gemini web interface, a reboot hidden in a network gap), the
position becomes `LOST`: "the Dec axis counter jumped to exactly CWD while no move was in flight".

### Safety incidents

The watchdog (`safety_enabled`) still stops the mount on:
- unexpected slewing;
- a move heading away from its target;
- a move ending far from its target;
- pointing below `safety_alt_limit`;
- tracking while parked.

The sequence is now **stop → park → locked**. It **never cold-starts** the mount: after an incident nobody
knows where the telescope is, and a cold start would make wherever it is the new CWD. Incidents that cast
doubt on the counters, including a park that did not complete, also make the position `LOST`.
`position ok` releases the lock once you have looked (`safety_locked` can still be cleared as before).

### Move execution failures recover themselves

The failures above (crawl, stall, a move ending short) are the mount failing to *do* a move while its
counters stay sound — distinct from the mount losing track of *where* it is. The driver treats them
differently: it does **not** call a human and does **not** lose trust. It stops, parks to CWD (`:hC#` — the
division of the two sky halves, the pose parks reach reliably, and the best place to start any move from),
and re-sends the target, up to `move_retries` times (default 3). All of it is invisible to the framework,
which sees one move that stays in flight until it arrives or the retries run out (`move_recovery` shows
IDLE / STOPPING / PARKING). The retry itself is a normal slew, run with the full watchdog live; only the stop
and the park to CWD stand the watchdog down, the two phases where the mount cannot go downwards. If the budget
is spent, the mount is left parked at CWD with its position still trusted and the move fails — the scheduler simply moves on and its next target tries again from CWD. A cold
start behind the driver's back, or the boot menu, is *not* this: those still go to LOST and wait for a human,
because parking "to CWD" would drive somewhere wrong when the counters can't be believed.

Every goto sends `:Q#` first by default (`goto_prestop` = STOP), matching `gemini2ser.cpp` — it clears a
stray prior move and leaves the worm tracking, so no sky is lost. Whether the SBT crawl needs more than that
(stopping the worm) is unsettled and is what the mount test checks; until it is, the recovery above is the net
for a goto that crawls, and a park stops the worm first because a park ends stopped anyway.

One consequence for a scheduler: while a move is being recovered the framework sees a single move that stays
in flight for the whole stop/park/retry cycle. In the worst case that is `move_retries` times
(2.5 s settle + up to 180 s park + up to 300 s per slew), about **24 minutes at the default 3** before the move
is finally failed. Every leg is bounded so it always terminates, and a scheduler waiting on one target is a
gentler failure than a mount locked at 3 a.m. — but if an executor seems stuck on a target for many minutes,
`move_recovery` will say why, and `move_retries` is the dial (a CWD park took 16–32 s in the real logs, so most
of that budget is the per-slew timeout, not the park).

---

## 4. Meridian flips: the driver now knows in advance

Gemini decides the pier side of a goto by one rule, read from its firmware:

1. **Keep the current side if the target fits.** The target is first placed on the side of the pier the
   **Dec axis** is on now. If its RA axis position fits strictly inside
   `[CWD − west safety limit + 223 goto limit, CWD + east safety limit]`, the mount stays on that side.
2. **Otherwise flip.** If the target doesn't fit, the mount tries the other side.
3. **Otherwise refuse.** If neither side fits, the reply is `6`.

`:MS#` works exactly this way. `:MM#` tries the two sides in the opposite order.

In hour angle, `:MS#` keeps whatever side the mount is on for targets between `−(E − 90°)` and `W − G − 90°`
(defaults −24° … +30.5°). There is no "natural" side: between those limits the side depends only on where
the mount came from. **Native 223 is measured inside the western safety limit, not from the meridian.**

What the driver does with it:

- **Predicts every goto** from a fresh read of the axis counters right before the slew
  (`goto_prediction`: STAY / FLIP / REFUSE with the window margins). Predictions closer than
  `flip_ambiguity_margin` (0.5°) to a window edge are marked "too close to call", because Gemini's own
  pointing model is not predicted.
- **Checks itself.** After each move the driver compares the side the mount ended on with the prediction.
  Mismatches count in `side_prediction_misses` and are logged. A mismatch *outside* the ambiguity margin
  means the rule or its inputs are wrong — please report those.
- **The tracking-limit flip** is an `:MM#` to the same target. It is sent at most 660 s before the western
  limit, and **not before it is predicted to flip**: the other side of the pier accepts the target only once
  it is east of `−(E − 90°)`, and the tracking side stops at `W − 90°`. If the limits leave no such moment,
  the mount parks instead. The old same-target `:MS#` never flipped at that point.
- **The near-arrival model retarget** is skipped if it would flip the mount (logged as a warning), instead
  of flipping on arrival.
- **Wrong-way safety check:** during a predicted flip it is suspended from the start of the move.
- **`dec_side` and `telFlip`** come from the Dec axis. `pier_side` (the mount's own `:Gm#` answer) is the RA
  axis relative to CWD; it also changes, without any flip, when tracking past 6 h from the meridian.
- **`side_window`** shows the window in degrees from CWD, read after every startup. If flip points are
  enabled in the mount (native 229 ≠ 0) the driver warns: they can force flips it does not predict.

Values: `dec_side`, `side_window`, `goto_prediction`, `flip_ambiguity_margin`, `side_prediction_misses`.

### The SBT mount as configured (seen 2026-09-14)

221 east = 91°, 222 west = 91°, 223 goto limit = 7°, 229 flip points = 2 (western flip point enabled).
At startup the driver logs what that means:

- **E side** (tube east, looking west) accepts gotos to HA > −1°.
- **W side** accepts gotos to HA < −6°; tracking stops at HA +1°.
- **Dead zone:** targets between HA −6° and −1° (about 20 minutes of sky just east of the meridian) are
  refused from **both** sides — Gemini answers `6`. This is the mount's configuration, not a driver fault.
  Reducing 223 below 2°, or widening a safety limit if the mechanics allow it, would close it.
- **A W-side target crossing the meridian** can be flipped only in the last 2° (about 8 minutes) before the
  tracking stop; the driver flips at HA −0.5°, about 6 minutes before it. An exposure still running then
  blocks the flip until it ends, and the mount may reach the limit and stop tracking in the meantime; the flip
  follows once the exposure ends.
- **W→E flips fail, E→W flips work** (tests udp3–udp5, 4 of 4 each way). On every W→E flip the Dec axis slewed
  to the other side while the RA axis did not slew: it stood still or crept at ~0.089°/s (about 21× sidereal).
  The telescope then points at the target's declination 12 h away from where it should be: alt 3.8° in the
  north for a northern target, **alt −49° (tube down, counterweight up)** for a target at Dec −20°. E→W flips
  and same-side moves arrived normally. On this mount a W→E flip is exactly the move whose RA axis has to run
  against the tracking direction (towards CWD and past it), so the leading suspect is Gemini's "stop an axis
  that must reverse before slewing it" step in its goto routine, run while the worm is tracking. The
  production driver (`gemini2ser.cpp`) always sends `:Q#` before a goto.
  - **This is unproven.** `goto_prestop` selects what is sent ahead of every goto: `NONE`, `STOP` (`:Q#`, like
    the production driver — the **default**, no sky lost), or `STOP_TRACKING` (worm off, `:Q#`, wait for the RA
    axis to rest — which costs a tracking gap on every goto, so it is not the default). The default sends only
    `:Q#`, exactly what the known-good `gemini2ser.cpp` does; whether the mount needs the worm stopped is what
    the mount test decides (step H). If it does, the move-recovery net catches any goto that crawls meanwhile.
  - The mechanism, from the polls: on W→E gotos the **Dec axis slews normally and the RA axis never leaves
    centering speed**, while on E→W gotos the RA axis slews first (157° in ~11 s) and Dec follows. With a small
    RA difference that looks like a crawl that nearly gets there; with a large one the Dec axis alone carries
    the tube somewhere dangerous.
  - The driver now stops such a move as soon as the **RA axis itself** shows it: more than 2° of RA axis travel
    still to go and the RA axis counter moving at the mount's **centering rate** — (native 170 + 1) × sidereal,
    0.088°/s on SBT (measured 0.0858–0.0891°/s over four failures) — within ×0.45…×2.2, both over the last 15 s
    and over the last 5 s: faster than a waiting (tracking) axis, far slower than a slewing one, and steady, so
    an axis that waited and has just started to slew does not count. The driver reads native 170 at startup
    (shown in `centering_speed`); changing `centering_speed` moves the band with it. That does not depend on the rate letter the mount reports (across these failures
    it reported `C`, `S` and `N`). A second rule stops a move at centering rate `C` more than 2° from the
    target for 15 s. Either logs "move ended: RA axis not slewing …" and fails the move without a safety
    incident (the counters are fine). The below-horizon watchdog remains the last line.
  - An earlier build skipped the below-horizon watchdog during slews, on the mistaken reading that the low pass
    was the firmware's normal flip path. It is not: with both axes moving a flip passes near the pole. The
    watchdog applies during slews again, and it is what stops this failure (3 polls below `safety_alt_limit`).
- **A stuck goto takes up to 5 minutes to be declared failed** (the move timeout, 300 s; the below-horizon watchdog
  stops a flip that goes wrong long before that). Every move now logs one line saying how it ended: "move ended: arrived / stopped moving / timed
  out / stopped by :Q# after N s, X deg from target, mount rate 'R'".
- **`/var/log/rts2` must exist and be writable** by the user running the driver (it was not; the incident
  report went only to the RTS2 log and the position state was not saved). Create it, or pass
  `--incident-log` and `--position-state`.
- **Flip points:** 229 = 2 enables Gemini's western flip point, which can force flips the prediction does not
  model. Either set 229 to 0, or treat `side_prediction_misses` on the W side with that in mind.

---

## 5. Re-zero from the sky

No Gemini command sets the axis counters; only a cold (or warm) start does, and only to CWD. So a
re-zero is:

1. **Measure** the counter zero error *e* from astrometry.
2. **Step the axes** (`:MP`, absolute counters) to where the counters read CWD − *e*. Physically, that is
   true CWD.
3. **Cold-start there**, so the counters become right.
4. **Put back** Gemini's own model terms other than the index terms, zero RTS2's accumulated corrections,
   and set the position to `CONFIRMED`.

A re-zero takes about 2–3 minutes of telescope time and leaves the mount at CWD, not tracking.

### Where the evidence comes from

The ordinary closed loop, with no human involved: astrometry → `corrwerr` → executor → `correct` to the
mount. The driver looks at each `correct` before the framework handles it; the framework's own
corrections work exactly as before.

- **When a `correct` counts:** only when the framework itself would trust it:
  - the same `MOVE_NUM`, `CORR_IMG` and `CORR_OBS` as when the image was taken;
  - no target offsets (`OFFS` = 0);
  - the mount simply tracking.
- **What it gives:** the telescope then truly points at `OBJ − (ra_err, dec_err)`. That position goes
  through the same pipeline as a goto (corrections as configured, RTS2 pointing model for the current pier
  side), and the firmware's post-cold-start relation turns it into the counter values the mount should
  show. The difference is one sample of *e*.
- **What is kept:** one sample per target, the newest (`sky_evidence`). The framework may reset its
  corrections when the target ends; the samples stay until a re-zero, a mount startup, or `LOST`.
- **Built-in sanity check:** a sample is refused when the same relation applied to the mount's *own*
  reported position is off by more than 60°. That would mean the relation does not fit this mount, and
  the driver will not move the axes on its say-so.

### When it acts

| condition | result |
|---|---|
| newest sample below `rezero_min` (0.25°) | `CONFIRMED` |
| last `rezero_samples` (3) targets agree within `rezero_agree` (0.1°), two of them ≥ `rezero_spread` (15°) apart on the sky, median above `rezero_min`, ≥ `rezero_interval` (4 h) since the last re-zero | **armed** (`rezero_armed`) |
| agreed error above `rezero_max` (30°) | `LOST`: too large to fix automatically |

The spread requirement is the key filter. A zero error is the same everywhere; a pointing-model error or a
bad plate solve is not.

- **With `rezero_auto` = true:** an armed re-zero runs when the **next move to a new target** starts.
  Nothing is exposing at that moment and the field is being left anyway. That move then continues. If the
  re-zero fails, the move fails and the position becomes `LOST`.
- **`position rezero`:** runs it immediately from the median of whatever has been collected, without the
  agreement and spread rules.

**`rezero_auto` is off by default** until a re-zero has been seen working on the mount.

Values: `sky_evidence`, `rezero_armed`, `rezero_state` (IDLE / STOPPING / MOVING / SETTLING / REBOOTING),
`rezero_auto`, `rezero_min`, `rezero_max`, `rezero_samples`, `rezero_agree`, `rezero_spread`,
`rezero_interval`.

**Check before relying on it:** astrometry scripts differ in `ra_err`. `rts2-astrometry.net` sends a plain
RA difference; `rts2-f/astrometry.py` multiplies it by cos(Dec). With the latter, samples taken at different
declinations will not agree and a re-zero never arms (safe, but useless), and the framework's own
corrections are off by the same factor. Check which one `/etc/rts2/img_process` is.

---

## 6. Options and values at a glance

| option | default | |
|---|---|---|
| `--startup-mode none\|restart\|warm\|cold` | `none` | what to answer the boot menu with |
| `--position-state FILE` | `/var/log/rts2/gemini-udp-position.state` | trust kept across driver restarts |
| `--incident-log FILE` | `/var/log/rts2/gemini-udp-incidents.log` | incidents, LOST events, re-zeros |
| `--no-safety-watchdog` | off | as before |
| `--no-safety-coldstart`, `--max-recoveries` | — | accepted, no longer do anything |

| value | writable | |
|---|---|---|
| `position_trust`, `position_reason` | no | section 1 |
| `startup_mode` | yes | NONE / RESTART / WARM_START / COLD_START |
| `mount_ready`, `startup_state`, `clock_matched` | no | handshake state |
| `dec_side`, `pier_side`, `side_window`, `goto_prediction`, `side_prediction_misses` | no | section 4 |
| `flip_ambiguity_margin` | yes | 0.5° |
| `goto_prestop` | yes | NONE / STOP / STOP_TRACKING, sent ahead of every goto (section 4, SBT) |
| `sky_evidence`, `rezero_armed`, `rezero_state` | no | section 5 |
| `move_recovery`, `move_retries` | (retries writable) | move execution-failure recovery (section 3) |
| `rezero_auto`, `rezero_min`, `rezero_max`, `rezero_samples`, `rezero_agree`, `rezero_spread`, `rezero_interval` | yes | section 5 |
| `safety_enabled`, `safety_locked`, `safety_alt_limit`, `wrong_way_margin` | yes | as before |

---

## 7. Dry run on the real mount without sky

Everything except the astrometric evidence can be checked with the dome closed. Have somebody who can see
the telescope, keep a hand on the hand controller's stop, and watch `position_trust`, `position_reason`,
`dec_side`, `pier_side` and `goto_prediction` in `rts2-mon`. Tail the driver log and the incident log.

Run the driver with `--debug` and keep its output (e.g. `rts2-teld-gemini-udp … --debug 2>&1 | tee teld-udp.log`),
and bring the log back with the notes: every goto's prediction ("pier side STAY/FLIP … window margins") and its
check ("goto ended on pier side … as predicted", or a warning when not) are what tell whether the firmware
rule holds on this mount.

### A. Driver restart does not touch the mount

1. Mount up and parked. Restart `rts2-teld-gemini-udp`.
   *Expect:* `ASSUMED` "driver connected to an already running mount"; telescope shown as parked; **no
   motion, tracking stays off**; "mount clock was … off" only if it really was.
2. Same with the mount tracking a target: it keeps tracking undisturbed.

### B. Geometry and the flip rule

1. After startup the log shows the goto windows ("goto windows (hour angle, deg): E side of the pier accepts
   HA > …, W side HA < …", then either the range where `:MS#` keeps the side or a dead-zone warning, and when
   a tracking-limit flip is possible). *Expect* the limits you know the mount has (for SBT, section 4).
2. `dec_side` should match where the tube physically is, and `pier_side` should agree with it for
   ordinary targets.
3. Take the HA limits from those log lines. From the **E** side (tube east, looking west), goto a target
   a few degrees inside the E-side limit: *expect* `goto_prediction` STAY, no flip. Then a few degrees beyond
   it, outside any dead zone: *expect* FLIP.
4. From the **W** side, the same around the W-side limit: STAY inside, FLIP beyond.
4a. If there is a dead zone (SBT: HA −6° … −1°), goto a target inside it: *expect* REFUSE predicted and
   Gemini's refusal (`6`).
5. After each: the log says "ended on pier side … as predicted"; `side_prediction_misses` stays 0.
   Note any "too close to call" cases and what the mount actually did.

### C. LOST blocks, `position ok` releases

1. `position lost`. *Expect:* tracking stops; `move`, `park` and tracking start are refused with "position
   is LOST".
2. `position ok`. *Expect:* `ASSUMED`; moves work again.

### D. Power cycle into the boot menu

1. Power-cycle the Gemini (telescope not touched).
   *Expect:* within ~20 s of it answering again, `LOST` "waiting in its boot menu"; the driver does **not**
   answer the menu.
2. `position unmoved`. *Expect:* the mount restarts with its stored counters, → `ASSUMED`; a goto to a
   known position looks right.
3. Repeat, but send `position lost` before powering off. After `position unmoved` it must stay `LOST`
   ("stored counters were already lost before").

### E. Cold start at CWD on the operator's word

1. Park, and check visually that the telescope really is at CWD.
2. `position cwd`. *Expect:* the mount reboots into a cold start; `ASSUMED` "telescope at CWD — mount cold
   started there"; axis counters exactly at half circles (`position` logs them).

### F. Cold start behind the driver's back

This one deliberately corrupts the counters; do it last, and be ready to recover with E.

1. Move a few degrees away from CWD in Dec.
2. Cold-start the mount from the Gemini web interface.
   *Expect:* `LOST` "the Dec axis counter jumped to exactly CWD while no move was in flight"; moves and
   park refused.
3. Recover: bring the telescope back to CWD with the hand controller, checking visually, then
   `position cwd`.

### H. Why do W→E flips fail?

The log names the build at startup once the build-id line is in; until then note `git log -1` on the machine
running the driver. Keep a hand on stop: a failing W→E flip can point the tube below the horizon before the
watchdog's three polls are up — prefer northern targets (Dec ≥ +40°) for this test, where the failure stays
above the horizon.

The default is `STOP` (`:Q#`), so a W→E flip on the default is the first data point: does it crawl (→ the
recovery parks and retries) or slew? Then vary the setting to find the lightest thing that works:
1. Track a target on the W side. With `goto_prestop` = `NONE`, goto a target that needs a W→E flip. Note
   whether the RA axis slews or crawls.
2. Same start, `goto_prestop` = `STOP` (the default, `:Q#` only).
3. Same start, `goto_prestop` = `STOP_TRACKING` (worm off). If only this one slews, the worm is the cause and
   the tracking-gap cost is justified; if `STOP` or `NONE` already slews, leave the default and avoid the gap.
4. Separate "flip" from "RA axis reversing": from a tracking W-side start, goto a target further **east on
   the same side** (no flip, the RA axis runs against tracking) with `NONE`. If that fails too, the reversal
   is the cause.
5. An E→W flip with each setting should work throughout.
6. For each: the "move ended" line, "stop (:Q#) sent" lines, and **the RA axis rate** from the polls (RA values
   against time — about 0.09°/s means it is not slewing), not just whether the move arrived. The prediction to
   check: `STOP` restores a fast RA slew on W→E and changes nothing about Dec or about E→W.

### G. Re-zero mechanics, without sky (optional)

This checks `:MP`, the cold-start relation and the state machine by feeding **fake** evidence. It leaves
the counters deliberately wrong by the faked amount, so follow it with E.

1. `rts2-sendcmd T0 X ignore_correction = 10` so the framework does not act on the fake corrections.
2. Move to a target at about Dec +40°, HA −1h and let it track. Note `MOVE_NUM`, `CORR_IMG` and
   `CORR_OBS` in `rts2-mon`.
3. `rts2-sendcmd -- T0 correct <MOVE_NUM> <CORR_IMG> <CORR_OBS> 1 1 1.0 0.5 1.1` (a fake RA error of
   1°, Dec 0.5°; the `--` keeps negative numbers from being read as options).
   *Expect:* "re-zero evidence from image 1 …" with an error around −1.0 / ±0.5 deg, and, most
   important, **"mount's own offsets" close to +0.000, +0.000**. If those read tens of degrees, stop:
   the firmware relation does not fit this mount.
4. `position rezero`. *Expect:* `rezero_state` STOPPING → MOVING (the axes step to roughly 1° from CWD) →
   SETTLING (the log gives the residual in arcsec) → REBOOTING (cold start) → IDLE, and `CONFIRMED`
   "re-zeroed from the sky".
5. The counters are now deliberately off by the faked amount: park, check visually how far from CWD it
   ends (about 1° in RA, 0.5° in Dec), then recover with E.
6. `rts2-sendcmd T0 X ignore_correction = 0` (or its configured value).

Please record for each step what happened and the relevant log lines, especially anything that
contradicts the expectations.

---

## 8. On-sky test (when there is sky)

1. With `rezero_auto` off, observe normally. Watch `sky_evidence` fill up: the samples should agree with
   each other at different pointings. With healthy counters, `CONFIRMED` should follow the first solved
   image with a small error.
2. If the samples disagree systematically with declination, check the astrometry script convention
   (section 5).
3. When comfortable, run one `position rezero` by hand on real evidence, then switch `rezero_auto` on.
