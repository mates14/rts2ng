-- 2026-10-19 [JS]: a target without coordinates is stored as NULL in
-- tar_ra/tar_dec. Classic wrote it as float NaN (rts2-newtarget with an
-- empty RA DEC), and its seed gave the special targets 0 0 - a real
-- position at RA 0, Dec 0, where a plain target (type 'o', 3 "Focusing
-- frames") would really slew. The scheduler tests for NULL and took NaN
-- for a position. Converts only rows where the meaning is clear:
-- NaN or half-set rows, and 0 0 on the special targets (tar_id < 20,
-- their own types) and on the planets (whose position is computed).
-- Other targets at exactly 0 0 are left alone: check them by hand with
--   SELECT tar_id, type_id, tar_name FROM targets WHERE tar_ra = 0 AND tar_dec = 0;
-- Safe to run again.
BEGIN;

UPDATE targets SET tar_ra = NULL, tar_dec = NULL
	WHERE tar_ra = 'NaN' OR tar_dec = 'NaN'
	OR (tar_ra IS NULL) <> (tar_dec IS NULL);

UPDATE targets SET tar_ra = NULL, tar_dec = NULL
	WHERE tar_ra = 0 AND tar_dec = 0
	AND ((tar_id < 20 AND type_id IN ('d', 'f', 'o', 'm', 'c', 'p', 'W', 'I', 'A'))
		OR type_id = 'L');

COMMIT;
