-- 2026-08-26: give the "scheduling" table (used by the site's separate
-- Python scheduler in sch/, not by rts2ng's own C++ scheduler/executor -
-- see db/db/include/rts2db/scheduling.h) a proper versioned migration.
-- It already exists on real production DBs (lascaux/d50), added directly
-- to production SQL some time after the sch/ tooling was written, never
-- through a migration file - CREATE TABLE IF NOT EXISTS so this is a
-- no-op there. It's created here WITH a primary key on tar_id, which
-- production's own copy does not have; this migration deliberately does
-- not try to retrofit that onto an already-populated production table
-- (would fail outright if any duplicate tar_id rows exist there, and is
-- a live-schema change that should be a deliberate, separate action, not
-- a side effect of applying this file) - rts2db::Scheduling::setSinfo()
-- is written to behave correctly either way.
CREATE TABLE IF NOT EXISTS scheduling (
	tar_id		integer PRIMARY KEY REFERENCES targets (tar_id),
	sinfo		varchar(2000) NOT NULL DEFAULT ''
);

-- No schema change for `interruptible` (added back in rel_0_8_1.sql) -
-- it already exists on every site's targets table. What changed is only
-- on the rts2db/httpd side: Target::loadTarget()/saveWithID() didn't
-- read or write this column at all until now (a pre-existing gap, not
-- something introduced by this migration).
