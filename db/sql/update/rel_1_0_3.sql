-- 2026-08-26: scripts.tar_id/camera_name already carries a unique
-- constraint on both real production DBs (lascaux/d50) - added ad hoc,
-- outside any versioned migration (most likely by rts2-configdb's
-- bootstrap). Without it, Target::setScript()'s insert-then-update-on-
-- failure upsert pattern silently degrades into "keeps inserting
-- duplicate rows" on repeated edits of the same (tar_id, camera_name).
-- Guarded so this is a no-op where the constraint (under this exact
-- name) already exists, e.g. real production.
DO $$
BEGIN
	IF NOT EXISTS (
		SELECT 1 FROM information_schema.table_constraints
		WHERE constraint_name = 'scripts_uniq_cam_tar' AND table_name = 'scripts'
	) THEN
		ALTER TABLE scripts ADD CONSTRAINT scripts_uniq_cam_tar UNIQUE (tar_id, camera_name);
	END IF;
END $$;
