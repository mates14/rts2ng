# D50: is rts2ng ready to replace classic RTS2? (teld-d50 + dome-d50)

Report of 2026-10-09. Scope: the two daemons that must cooperate
without fault at D50 (Ondřejov, host lascaux50) - the mount `T0`
(`rts2-teld-d50`) and the roof `DOME` (`rts2-dome-d50`). If they get it
wrong, the telescope hits the roof or rain gets in.

## Verdict

- **The code is ready to be tried on the hardware, in daytime, supervised.**
  It is not ready for unattended nights until the hardware checks below pass.
- The port of both drivers is **faithful to what runs today**, verified by diff
  against the production source. Production is built from the `/home/torman/rts2`
  working tree: `/usr/local/bin/rts2-teld-d50` is byte-identical to its
  `.libs` build.
- The review found **real interlock bugs that classic has as well**. They are
  fixed in rts2ng, so after the switch the system is safer than classic.
- **The roof side is tested** on a bench, in simulation. **The mount side is
  only compiled.** It needs raw ethernet (root) to the REMOTES boards and the
  real servos, and nothing has exercised it.

## 1. Port faithfulness (before the fixes)

| Component | Compared with production `/home/torman/rts2` |
|---|---|
| `teld/d50.cpp`, `dome/d50.cpp` | identical logic; only one crash fix |
| tgdrive, connremotes, connethernet, connserial, connford, fork | identical |
| domeford | one fix (option-parsing result was ignored) |
| Dome base | identical |
| Telescope base | GPointModel, satellite tracking and cupola/rotator support dropped (D50 uses none); `move_end` semantics changed |
| pointing model | the latitude fix (72b01e0) doesn't affect D50: none of its terms use latitude |
| ERFA | production is built without it, same as rts2ng |

**Crash fix:** a failed REMOTES raw-socket init used to crash classic with a
segfault. Reproduced on production's binary (exit 139); rts2ng exits cleanly.

Dry run of the rts2ng binary with the production command line and config: all
options are accepted, and the defaults, horizon, ceiling and T-Point files load.

## 2. Bugs found and fixed (all present in classic too)

**Commits** (`master`, not pushed):

```
bf69c84 teld/d50: the dome may only close on a mount that is really inside
84c0e95 dome: block telescope moves while the roof opens; d50 waits for a moving mount
2a0ad7e dome/tests: D50 dome bench - the real dome-d50 against a simulated roof
9b8b7c8 kernel: critical messages reach syslog as critical; systemd heartbeat
2ecdc3b packaging: rts2@ units restart crashed and hung daemons; rts2-alertd to Telegram
387b34d teld/d50: waiting for the servos to stop gives up after 30 s
548edd6 dome: a close that cannot start is retried and reported until it can
```

### Roof vs telescope
1. **The encoder cross-check only caught errors in one direction.** Every
   motor-vs-ABS-encoder check was `diff > limit` without `fabs`, so a negative
   mismatch of any size passed. That included the checks allowing
   `inside_dome_limit`, the value the roof closes on. Fixed with `fabs`, and the
   RA difference is now wrapped to ±180°.
2. **The roof could close on a mount that had just started to leave.**
   - The mount cleared `inside_dome_limit` without sending it, so the dome
     learned it only with the next info, up to 5 s later.
   - The dome trusted the value before checking whether the mount was moving.
   - Fixed in three places:
     - the mount sends the value immediately;
     - the dome waits while the mount moves or parks;
     - the mount refuses any move except park while the dome is closing or
       waiting to close.
3. **Failed REMOTES encoder reads used uninitialised values.** The cross-check
   compared garbage. Each read is now marked valid or unknown:
   - "unknown" refuses moves inside the closed dome;
   - after a park it only warns, so it can't keep the roof open in rain;
   - `endPark` re-reads the position at rest.
4. Smaller fixes:
   - waypoint timers left over from a previous move are cancelled;
   - the "return to last safe position" recovery clears `inside_dome_limit`;
   - the worm-frequency reset after guiding compared a pointer and never ran;
   - the tracking lead-ahead was integer 0;
   - the `--help` default of the roof-switch variable (`roof_opened`).

### First target lost at the start of the night (the known issue)
- **What happens:** the dome blocked only exposures while opening, and the
  cameras' `blocked_by` does not list `DOME`, so even that block never reached
  them. The executor's move reached a mount that refused it. Production log,
  2026-10-08 22:36: three moves refused, then C0 exposed the parked position.
- **Fix:** the dome now also raises the telescope-move block (`BOP_TEL_MOVE`)
  while opening. The executor's move waits for the roof. Stopping an opening
  now clears both blocks (classic left the exposure block set).

### Roof reliability
- **A close that didn't reach the end switch was marked closed and left.** It
  is now retried 3 times, then reported as CRITICAL.
- **"Cannot start closing"** (e.g. the mount daemon is down) was reported once
  and, outside bad weather, never retried. It is now CRITICAL, repeated every
  5 min, and the close is retried every 30 s until it starts.
- **Ford board pin writes reported success even when the write failed.** They
  now report failures, and `dome-d50` checks its motor commands.

### Supervision and alerting
- **Emergencies were logged as debug.** rts2ng sent every
  CRITICAL|REPORTIT message (all roof and park emergencies) to syslog at
  priority 7. They now go as `LOG_CRIT`.
- **Hangs went undetected.** `teld-d50` waited forever for the servos to
  report "stopped". It now gives up after 30 s with a CRITICAL error.
- **systemd units:** `rts2@NAME` runs as `Type=notify` with `Restart=always`
  and `WatchdogSec=60`. The kernel sends READY and a heartbeat from the main
  loop, so a crashed *or hung* daemon is restarted.
- **`rts2-alertd`** sends to Telegram every CRITICAL message (with the errors
  before it as context) and every watchdog kill or restart of an `rts2@` unit.

Details for upstream: `base/UPSTREAM_BUGS.md`, section "D50 mount/roof interlock".

## 3. What was tested, and what was not

**Tested:**
- **Dome bench** (`base/dome/tests/d50-bench/run.sh`): the real `rts2-dome-d50`,
  a simulated Ford board on a pty, a private centrald and a dummy mount.
  **8/8 pass:**
  - opening blocks T0 until open (both blocks visible on T0);
  - close with the mount at rest and inside;
  - close requested mid-slew → the roof motor starts only after the mount stops;
  - jammed roof → 3 retries, then CRITICAL;
  - after unjamming → closes;
  - stop mid-opening clears both blocks;
  - mount daemon dead → close refused and reported as CRITICAL;
  - mount back → the roof closes by itself.
- **Classic dome code on the bench:** fails 4 of the original 6 scenarios, so
  the bench catches these bugs.
- **systemd watchdog**, as a real user unit: a SIGSTOP-frozen daemon was
  restarted within 10 s; a healthy one was left running for 40 s.
- **`rts2-alertd`**, in `--dry-run` mode against the bench's emergencies.

**Not tested:**
- Anything of the mount driver against hardware.
- The executor actually holding its move while the roof opens. The mechanism
  was shown on the bench (T0 carries `BOP_TEL_MOVE`), but the executor needs the
  database.
- A real Telegram send (no token on this host yet).

## 4. Checklist for the switch-over

**Before** (any time, no risk to production):
- [ ] Choose the alarm chat: a new, louder chat (recommended), or the
      `private_chat_id` from lascaux `~/gcn/config.json` (host alias `ssh l`).
- [ ] Create `/etc/rts2/alertd` (mode 0600; template `base/packaging/alertd.conf`):
      `[telegram] bot_token = …, chat_id = …` and `[alert] site = D50`.
      Then run `rts2-alertd --test`.
- [ ] Consider adding `DOME` to `blocked_by` of C0/C1/C2 in `/etc/rts2/rts2.ini`
      (helps classic too). Not needed for the rts2ng fix.
- [ ] Fix production's `rts2.service`: it has been *failed* since 2026-09-15
      (`rts2-stop` exited 1), so tonight's daemons are hand-started and
      unsupervised. Run `systemctl status rts2` and `systemctl reset-failed rts2`.

**Switch-over day** (daytime, roof closed, someone on site; classic binaries stay
in `/usr/local/bin` for an immediate rollback):
- [ ] Stop the classic `T0` and `DOME`. Start them with
      `systemctl enable --now rts2@centrald rts2@DOME rts2@T0`, using the same
      `/etc/rts2/devices` lines. Don't also start them via `rts2.service`.
- [ ] Enable alerts: `systemctl enable --now rts2-alertd`.
- [ ] In rts2-mon, read `T0.remotes_abs_encoder_difference`. Its sign was
      never checked in classic, so a routine negative offset larger than
      `limit_remotes_abs_encoder_difference` (0.5°) would now refuse
      closed-dome moves. Fix the encoder offsets if so.
- [ ] Encoder and drive readouts match classic. Then a short slew and a park.
- [ ] After the park: `inside_dome_limit = 1`, and the roof closes.
- [ ] Mount outside the safe zone, then close → the dome sends park, waits, closes.
- [ ] A move sent while the roof waits to close → refused ("only park is allowed now").
- [ ] Kill `T0` with the roof open, then close → refused, Telegram alarm;
      systemd restarts T0 → the roof closes.
- [ ] Slew and tracking hand-off look normal (the lead-ahead is now 0.1 s, about 1.5″).

**First nights:** supervised, with the rollback ready.

## 5. Notes
- `Dome` base changes also apply to `dome-bart` and zelio (the move block
  while opening, close retries, repeated CRITICAL). Agreed as wanted.
- Left alone on purpose: in `isMoving`, `fabs (diffAc) > haCpd * moveTolerance`
  compares against a negative number (`haCpd < 0`), so the mount always
  re-targets while handing over to tracking. Changing that would alter slew
  behaviour, which can't be tested without the hardware.
- Bench usage: `base/dome/tests/d50-bench/README.md`. Rerun it after any change
  to the dome, the mount interlock or the daemon lifecycle.
