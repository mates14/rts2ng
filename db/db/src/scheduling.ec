#include "rts2db/scheduling.h"
#include "rts2db/sqlerror.h"
#include "rts2db/devicedb.h"

EXEC SQL include sqlca;

using namespace rts2db;

std::string Scheduling::getSinfo (int tar_id)
{
	if (checkDbConnection ())
		throw SqlError ();

	EXEC SQL BEGIN DECLARE SECTION;
	int d_tar_id = tar_id;
	VARCHAR d_sinfo[2000];
	EXEC SQL END DECLARE SECTION;

	EXEC SQL SELECT
		sinfo
	INTO
		:d_sinfo
	FROM
		scheduling
	WHERE
		tar_id = :d_tar_id;

	if (sqlca.sqlcode)
	{
		if (sqlca.sqlcode == ECPG_NOT_FOUND)
		{
			EXEC SQL ROLLBACK;
			return std::string ("");
		}
		// Note: SqlError()'s own constructor issues EXEC SQL ROLLBACK
		// itself, after capturing sqlca's current message/code into the
		// exception - an explicit ROLLBACK here first would overwrite
		// sqlca with the (successful, empty-message) rollback's own
		// result before SqlError() ever reads it, turning every error
		// here into a useless "(#0)" with no message (found by testing
		// this literal code path live, not by inspection).
		throw SqlError ();
	}
	EXEC SQL COMMIT;

	return std::string (d_sinfo.arr, d_sinfo.len);
}

void Scheduling::setSinfo (int tar_id, const std::string &sinfo)
{
	if (checkDbConnection ())
		throw SqlError ();

	EXEC SQL BEGIN DECLARE SECTION;
	int d_tar_id = tar_id;
	VARCHAR d_sinfo[2000];
	EXEC SQL END DECLARE SECTION;

	size_t len = sinfo.length ();
	if (len > 2000)
		throw SqlError ("sinfo is too long (max 2000 characters)");
	memcpy (d_sinfo.arr, sinfo.data (), len);
	d_sinfo.len = len;

	// Delete-then-insert in one transaction rather than UPDATE-or-INSERT:
	// real production's scheduling table (see scheduling.h's file note)
	// has no unique/primary key on tar_id, so an UPDATE could silently
	// leave stray duplicate rows in place instead of ever falling
	// through to INSERT. This is correct either way - it collapses to
	// exactly one row for tar_id whether zero, one, or (on production's
	// unconstrained table) several already existed.
	EXEC SQL DELETE FROM scheduling WHERE tar_id = :d_tar_id;
	// ECPG_NOT_FOUND (sqlcode 100) here just means "0 rows matched" - the
	// expected, common case (no scheduling row existed yet for this
	// target) for a delete-then-insert upsert, not a real error. Found
	// live: ecpg reports a 0-row DELETE the same way it reports a
	// singleton SELECT INTO matching no row, not as sqlcode 0.
	if (sqlca.sqlcode && sqlca.sqlcode != ECPG_NOT_FOUND)
	{
		// Note: SqlError()'s own constructor issues EXEC SQL ROLLBACK
		// itself, after capturing sqlca's current message/code into the
		// exception - an explicit ROLLBACK here first would overwrite
		// sqlca with the (successful, empty-message) rollback's own
		// result before SqlError() ever reads it, turning every error
		// here into a useless "(#0)" with no message (found by testing
		// this literal code path live, not by inspection).
		throw SqlError ();
	}

	EXEC SQL INSERT INTO scheduling (tar_id, sinfo) VALUES (:d_tar_id, :d_sinfo);
	if (sqlca.sqlcode)
	{
		// Note: SqlError()'s own constructor issues EXEC SQL ROLLBACK
		// itself, after capturing sqlca's current message/code into the
		// exception - an explicit ROLLBACK here first would overwrite
		// sqlca with the (successful, empty-message) rollback's own
		// result before SqlError() ever reads it, turning every error
		// here into a useless "(#0)" with no message (found by testing
		// this literal code path live, not by inspection).
		throw SqlError ();
	}
	EXEC SQL COMMIT;
}
