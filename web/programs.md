# Programs (observing programs) - design

Status: design, 2026-10-01. Nothing implemented yet except the
level-zero target list (`static/targets.html`), which works without it.
Decisions below were agreed in discussion; open points are marked.

## Goals

1. An outside PI (first case: Petr Pata, FEL CVUT) puts in targets, gets
   a promised fraction of the time, and can see that the telescope works
   for him, without wading through everything else.
2. People who are given time on D50 can set up their own targets.
3. Bring order into our own core and fillup targets.
4. Tell automatically how the time was used, per program and for
   overhead.

## Concepts

- **Program**: a named group of targets with a PI, members (web logins)
  and a **share**, the promised percentage of science time on a telescope.
  A program has no priority of its own.
- **Target priority** (`targets.tar_priority`) changes meaning: it is
  only the relative priority **within the program**. A program cannot
  get more of the telescope by raising its numbers.
- **Fillup** (the sky survey) is a program with no share. It is not
  planned by the scheduler, but its time is reported.
- **GRBs / ToOs** stay outside the shares and override as today.
- **Overhead**: calibrations, focusing, and every failed observation
  (see Accounting).

## Data model

```sql
CREATE TABLE programs (
    prog_id          integer PRIMARY KEY,
    prog_name        varchar(150) NOT NULL UNIQUE,
    prog_pi          varchar(150),
    prog_description text,
    prog_share       float,              -- % of science time; NULL = no share (fillup)
    prog_enabled     boolean NOT NULL DEFAULT true
);
ALTER TABLE targets ADD COLUMN prog_id integer REFERENCES programs (prog_id);

CREATE TABLE program_users (
    prog_id   integer REFERENCES programs (prog_id),
    usr_login varchar(64) NOT NULL,       -- login from the web auth file
    PRIMARY KEY (prog_id, usr_login)
);
```

- New tables, not the classic `labels` (free text, no share) or
  `accounts`/`tickets` (their own scheduling semantics). Production has
  nothing in either worth migrating (checked 2026-10-01: one test label
  on D50, nothing on SBT).
- **One program per target.** A target two programs want becomes two
  targets, so it is always clear whose time was used.
- **Same prog_id on both telescopes**, written to both DBs by the
  program editor (free ID picked on both, like `new-target-id`).
- **Share per telescope**: each DB has its own `programs` row, so a
  program can have 10 % of D50 and nothing on SBT.
- Adding columns doesn't break `sch/`, which uses explicit SELECT lists.

## Scheduling: soft fair-share (`~/sch`)

The scheduler plans one night. Shares are not absolute: the night is
planned roughly in proportion to the shares, corrected by the last
30 days.

**Balance.** For each program with a share, over the last 30 days on
that telescope:

- `T`  = charged science time of all programs with a share
- `O_p = T * s_p / sum(s)`  time owed
- `U_p` = time actually charged to the program (successful visits only, from `usage`)
- `B_p = O_p - U_p`  balance, positive = program is behind

**Tonight's goal.** With `N` = expected science hours tonight, the
program should get about `t_p = N * s_p / sum(s) + B_p` (not below 0).
`sum(s)` runs over programs with something observable tonight, so the
share of a program with nothing up goes to the others.

**Getting the solver to follow it.** The active solver
(`kernel/solver.py`) maximises the sum of `priority * duration *
airmass/time factors`, so priority is a value per second. Effective
priority of a target:

```
eff_i = w_p * tar_priority_i / median(tar_priority of p's candidates tonight)
```

Dividing by the median removes the program's own scale, so the number
of targets and their priority numbers don't buy time. The program
weight `w_p` starts at `s_p`. A linear objective doesn't produce
proportions by itself, so iterate: solve, compare each program's
planned hours with `t_p`, scale `w_p` by `(t_p / planned_p)^a`, solve
again. Stop after a few rounds or when close enough. Open: how long one
solve takes on a full night, which decides the number of rounds.

**The small program case.** A program that wants one 2 h observation
a month gets a small share. Its balance grows night by night until
`t_p` reaches about 2 h, its weight rises, and the solver places it.
Because only successful visits count as used, a failed attempt
leaves the balance in place and it is planned again by itself.

Open: should a program build up balance on nights when none of its
targets were observable at all (seasonal targets)? The 30-day window
limits how much it can build up either way. Proposed: yes, keep it
simple.

## Accounting

### Why not RTS2 observations

An `obs_id` says nothing about success. From the executor's point of
view almost every observation is interrupted. Scripts are written to
fill the slot. Classic `E 10` means "10 s exposures until interrupted",
and guiding depends on it, so this can't change. A script like
`for 60 { E 60 }` that ends a little early is restarted as a new
observation and killed seconds later when the slot ends. Executor bits
(`OBS_BIT_INTERUPED` is never even set) cannot tell these cases apart,
and the executor stays as it is.

### The visit

The unit of accounting is the **visit**: one planned slot from the
`sch/` schedule. A visit contains any number of RTS2 observations,
including the killed restarts, which merge in harmlessly.

The **queuer** (`queue_selector.py`) records visits. It is the
component that knows:

- the planned slot, because it issues next according to the plan;
- that someone else took the telescope (it already detects external
  activity and GRB grace periods);
- that the system left ON (it already follows system state).

Prerequisite: today the queuer deletes executed `queues_targets` rows
(`queue_selector.py:836`), and `telescope_ops.py` clears the queue when
a new schedule is written. The plan is not kept anywhere in the DB.
The queuer writes a row when it starts a slot and completes it when the
slot ends:

```sql
CREATE TABLE visits (
    visit_id    integer PRIMARY KEY,
    tar_id      integer REFERENCES targets (tar_id),
    prog_id     integer,             -- target's program at that moment, frozen
    plan_start  timestamp with time zone,
    plan_end    timestamp with time zone,
    act_start   timestamp with time zone,
    act_end     timestamp with time zone,
    end_reason  varchar(16),         -- time_up, override, system, external, error
    images      integer,             -- science images in the visit
    images_ok   integer,             -- of those, astrometry OK
    exposure_s  float,               -- summed exposure of OK images
    outcome     varchar(16)          -- success, failed; may be revised
);
```

Visits started outside the queuer (a manual `now` from the GUI, a GRB
override) are recorded as `external` with the target they ran, so the
night stays complete.

### Success

- **Delivered** = science images (`obs_subtype = 'S'`, not deleted) of
  the visit's target taken inside `[act_start, act_end]`, whatever
  observation they belong to.
- **Usable image** = astrometry OK (`process_bitfield & ASTROMETRY_OK`).
  This catches "dome open, no stars", which no end reason will.
  Photometry can be added later as a post-processing step, so `outcome`
  may be revised when it runs.
- **Success**: the visit has usable images and got most of its slot,
  either because it ran to `time_up` or because it was cut only late (a
  generous threshold, around 80 %). GRBs are rare, so being generous
  costs little. Revisit the rule if it turns into a problem.
- **Failed**: no usable images, or cut early by an override, the system
  leaving ON, or an error. Charged to overhead (`failed`, with the
  program ID for information). The program's balance stays, so the
  target gets planned again. The data is still handed over.
- Time charged = `act_start` to `act_end` (slew included).

### Nightly summary

From visits (and calibration/focus observations, which are overhead),
a nightly job writes the frozen per-night summary that reports and
the scheduler's balance read:

```sql
CREATE TABLE usage (
    night     date,           -- local date of the evening
    prog_id   integer,        -- 0 for non-program categories
    category  varchar(16),    -- science, failed, fillup, grb, calib, focus,
                              -- manual, idle, closed
    seconds   float,
    PRIMARY KEY (night, prog_id, category)
);
```

`idle` = dome open, system on, nothing observing. `closed` = night time
lost to weather or a closed dome. Both need the dome and system state
history; check what recordd's `records_state` actually holds.

### Old data (diagnostic only)

Without visit records, history can only be estimated. A small script
compares each target's `sinfo` `duration=` with the true observing
period from image timestamps (slew to last image end), grouping a
target's consecutive observations in a night. It is diagnostic, not a
prerequisite: interesting fractions, and it shows how well `duration=`
matches real scripts (see below).

### Slot transitions

Separate from accounting, but the same data helps. The slot is
`duration=` rounded up to the time chunk (60 or 300 s). In the last
chunk the queuer issues next, so `E 10`-style scripts hand over
naturally. A finite script whose true length (slew + script) doesn't
match `duration=` either ends early and restarts into a killed stub,
or runs into the slot boundary. Next can't be issued much earlier,
because Python scripts end as soon as next is no longer their tar_id.
Accounting is unaffected (the stub merges into the visit). The waste is
real, though. Better `duration=` values per script can come from the
queuer's logs (`~/logs/rtspy*.log`), which hold the full history of
slots, nexts and script ends: analyse them into a heuristic for the
true duration of a given script. Later, visits give the same numbers
directly.

## Users and safety

Users are trusted. Safety is still hardened step by step, in this order:

1. Write endpoints become POST with a CSRF check (Filip's note). New
   endpoints are POST from the start.
2. An audit log of edits: who changed which target or program, and
   when. Useful even among trusted users.
3. Server-side check that a program member edits only their program's
   targets. Staff edit everything.
4. Scripts for members come from a template (filters x exposure x
   count) instead of free text.

**Staff vs member**: a login is staff unless marked otherwise (a flag in
the web auth file). This only decides what the user sees first; step 3
is where it starts to restrict.

## Web side

- **Program view** (for the PI; what Petr sees after login): his
  program's targets (enable/disable, edit, add); observations of recent
  nights with outcome, thumbnails and FITS download; time used vs share
  and the current balance; his targets in tonight's plan. No device
  monitor or other programs unless he asks. He built BOOTES, so a few
  quirks are fine, but there should be no clutter.
- **Program editor** (staff): name, PI, description, share per
  telescope, enabled, members, list of targets. Writes both telescopes.
- **Target editor**: Program dropdown, saved on both telescopes.
- **Target list** (`targets.html`, exists): Program column and filter,
  defaulting to the user's programs. A **bulk "assign to program"**
  action on the filtered list, needed to sort thousands of core targets
  (goal 3).
- **Usage report**: per telescope and period, time per program and per
  overhead category, share vs used.
- Endpoints: `api/db/programs`, `program-save`, `program-create`,
  `programs-assign` (bulk), `usage`; `prog_id` in `target`,
  `target-save` and the target list.

## Requests (not now)

The RTS2 target model is built around observing targets again and
again (`sch/` plans each enabled target at most once a night). Outside
requests are often one-shot. No request features are planned now. We
add the ones real programs turn out to need once the program structure
works. Likely first candidate: disable the target after its first
successful observation. Others: a date window, a cadence, a visit
count, airmass/moon limits. They would go into `scheduling` (as `sinfo`
keys or real columns) and need matching support in `sch/`.

## Phases

Accounting comes before any scheduler change, so the fair-share has
real numbers to work with.

1. **Structure.** Programs tables, editor, target dropdown, bulk
   assign; sort our core targets into programs (goal 3).
2. **Accounting.** The queuer writes `visits`; success from astrometry;
   nightly `usage`; usage report. Diagnostic script for old data on
   the side (goal 4).
3. **Petr onboarding.** His program with a share, his targets, the
   program view. Share reported, not yet acted on (goal 1).
4. **Fair-share in `sch/`**, once there are some weeks of `usage`.
   Balance, priority normalisation, iterative weights.
5. **Hardening and D50 guests.** POST/CSRF, audit log, member write
   checks, script templates (goal 2).

## Open points

- Building up balance on nights with nothing observable (proposed: yes).
- Number of reweighting rounds: depends on solve time.
- Success threshold for cut visits (around 80 %); tune once data exists.
- Is astrometry run on all science frames on both telescopes?
- What recordd logs about dome and system state, for `idle`/`closed`.
- Where the fillup survey runs today, so its time can be picked up.
- `~/sch` lives outside this repo. Should it come under version control
  here before phase 4?
