# Programs (observing programs) - proposal

Status: proposal, 2026-09-30. Nothing implemented yet except the
level-zero target list (`static/targets.html`), which works without it.

## Why

- Group targets by science program: CV monitoring, AGN, Filip's
  transients, ...
- Let a friendly group use a share of the time (e.g. 10 %) and see and edit
  only their own targets without wading through everything else.
- Give a future scheduler the data it needs (who owns a target, what share
  a program gets). The scheduler comes later; first the data has to exist.

Users are only a login permission for now (the web auth file). They are
trusted, and nothing is restricted per program. We work at the level of
programs: a program editor alongside the target editor.

## What the database already has (all from classic RTS2, all unused)

| Piece | Schema | State |
|---|---|---|
| `labels` + `target_labels` | many-to-many free text; `label_type` 1 = PI, 2 = PROGRAM (`LABEL_PROGRAM`, `rts2db/labels.h`) | rts2db code exists; no web UI; `sch/` ignores it |
| `accounts` (`account_share`) + `tickets` (tar_id, account_id, obs_num, sched_from/to, intervals) | classic RTS2 time-share design | only "Default account" row; nothing reads it |
| `users`, `targets_users`, `props` | DB users / per-target users / proposals | dead; web login uses the auth file |
| `targets.tar_priority`, `tar_enabled` | per telescope (each DB separate) | what `sch/` schedules on (type `O`, tar_id 1000-49999) |
| `scheduling.sinfo` | free text `duration=600 type=and ...` | `sch/` reads `duration`, `type` |

Production checked 2026-10-01: `labels` holds a single test entry on D50
(type 1 = PI, "Sergey KArpov") and nothing on SBT. Nothing to migrate.
`programs` can start empty.

## Proposal

1. **New table, not reused labels or accounts.**

   ```sql
   CREATE TABLE programs (
       prog_id          integer PRIMARY KEY,
       prog_name        varchar(150) NOT NULL UNIQUE,
       prog_pi          varchar(150),
       prog_description text,
       prog_share       float,              -- time share, arbitrary units; stored, not yet used
       prog_enabled     boolean NOT NULL DEFAULT true
   );
   ALTER TABLE targets ADD COLUMN prog_id integer REFERENCES programs (prog_id);
   ```

   Labels are free text with no share, and giving them a second meaning
   invites confusion. `accounts` is close in spirit, but it comes with
   `tickets`, which carry their own scheduling semantics we don't want yet.

2. **One program per target** (nullable `targets.prog_id`). This makes it
   unambiguous who owns a target and, later, whose time was used. A target
   genuinely wanted by two programs is better as two targets. Adding a
   column does not break `sch/`, which uses explicit SELECT lists.

3. **Same prog_id on both telescopes.** The program editor writes both
   DBs, like the target editor, so we don't recreate the tar_id mismatch
   problem. The ID is picked free on both (same approach as
   `new-target-id`).

4. **Users stay loose.**

   ```sql
   CREATE TABLE program_users (
       prog_id   integer REFERENCES programs (prog_id),
       usr_login varchar(64) NOT NULL,       -- login name from the web auth file
       PRIMARY KEY (prog_id, usr_login)
   );
   ```

   For now this is only used to default the target list's filter to "my
   programs". Nothing is enforced.

5. **Share is stored, not acted on.** `prog_share` is there for the future
   scheduler; nothing reads it yet.

## Web side

- **Program editor** (`program.html`): name, PI, description, share,
  enabled, members (logins); list of the program's targets. Writes both
  telescopes.
- **Target editor**: a Program dropdown, saved on both telescopes.
- **Target list** (`targets.html`, exists): gains a Program column and
  filter, and defaults to the logged-in user's programs.
- Endpoints (same pattern as the target ones): `api/db/programs`,
  `api/db/program-save`, `api/db/program-create`, plus `prog_id` in
  `target`, `target-save` and the target list.

## Open questions

- One program per target: confirmed? (Proposed: yes.)
- Is the share per telescope or common? (Proposed: one `prog_share` per DB
  row, so it can differ between D50 and SBT if we want.)
- Write endpoints are still GET (Filip's CSRF note); new endpoints should
  be POST from the start.
