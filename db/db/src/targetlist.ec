#include "rts2db/targetlist.h"
#include "rts2db/sqlerror.h"
#include "rts2db/devicedb.h"

#include <cmath>

EXEC SQL include sqlca;

using namespace rts2db;

void TargetSummarySet::load ()
{
	if (checkDbConnection ())
		throw SqlError ();

	EXEC SQL BEGIN DECLARE SECTION;
	int db_tar_id;
	char db_type_id;
	VARCHAR db_tar_name[151];
	VARCHAR db_tar_comment[201];
	double db_tar_ra;
	double db_tar_dec;
	bool db_tar_enabled;
	int db_tar_priority;
	VARCHAR db_sinfo[2001];
	long db_obs_count;
	double db_last_obs;

	int db_type_id_ind;
	int db_tar_name_ind;
	int db_tar_comment_ind;
	int db_tar_ra_ind;
	int db_tar_dec_ind;
	int db_tar_priority_ind;
	int db_sinfo_ind;
	int db_last_obs_ind;
	EXEC SQL END DECLARE SECTION;

	clear ();

	// scheduling is reduced to one row per tar_id with DISTINCT ON:
	// production's table has no key on tar_id (see scheduling.h), so a
	// plain join could duplicate a target.
	EXEC SQL DECLARE tarsum_cur CURSOR FOR
		SELECT
			t.tar_id,
			t.type_id,
			t.tar_name,
			substr (t.tar_comment, 1, 200),
			t.tar_ra,
			t.tar_dec,
			t.tar_enabled,
			t.tar_priority,
			s.sinfo,
			coalesce (o.obs_count, 0),
			EXTRACT (EPOCH FROM o.last_obs)
		FROM
			targets t
		LEFT JOIN
			(SELECT DISTINCT ON (tar_id) tar_id, sinfo FROM scheduling ORDER BY tar_id) s
			ON s.tar_id = t.tar_id
		LEFT JOIN
			(SELECT tar_id, count (*) AS obs_count, max (obs_start) AS last_obs FROM observations GROUP BY tar_id) o
			ON o.tar_id = t.tar_id
		ORDER BY
			t.tar_id ASC;

	EXEC SQL OPEN tarsum_cur;
	if (sqlca.sqlcode)
	{
		SqlError err;
		EXEC SQL ROLLBACK;
		throw err;
	}

	while (1)
	{
		EXEC SQL FETCH next FROM tarsum_cur INTO
			:db_tar_id,
			:db_type_id :db_type_id_ind,
			:db_tar_name :db_tar_name_ind,
			:db_tar_comment :db_tar_comment_ind,
			:db_tar_ra :db_tar_ra_ind,
			:db_tar_dec :db_tar_dec_ind,
			:db_tar_enabled,
			:db_tar_priority :db_tar_priority_ind,
			:db_sinfo :db_sinfo_ind,
			:db_obs_count,
			:db_last_obs :db_last_obs_ind;
		if (sqlca.sqlcode)
			break;

		TargetSummary ts;
		ts.id = db_tar_id;
		ts.type = db_type_id_ind < 0 ? '?' : db_type_id;
		if (db_tar_name_ind >= 0)
			ts.name = std::string (db_tar_name.arr, db_tar_name.len);
		if (db_tar_comment_ind >= 0)
			ts.comment = std::string (db_tar_comment.arr, db_tar_comment.len);
		ts.ra = db_tar_ra_ind < 0 ? NAN : db_tar_ra;
		ts.dec = db_tar_dec_ind < 0 ? NAN : db_tar_dec;
		ts.enabled = db_tar_enabled;
		ts.hasPriority = db_tar_priority_ind >= 0;
		ts.priority = ts.hasPriority ? db_tar_priority : 0;
		if (db_sinfo_ind >= 0)
			ts.sinfo = std::string (db_sinfo.arr, db_sinfo.len);
		ts.obsCount = db_obs_count;
		ts.lastObs = db_last_obs_ind < 0 ? NAN : db_last_obs;
		push_back (ts);
	}

	// Anything but "no more rows" ending the loop is a real failure -
	// better an error than a silently truncated list.
	// SqlError's constructor rolls back itself, which also closes the
	// cursor.
	if (sqlca.sqlcode != ECPG_NOT_FOUND)
		throw SqlError ();

	EXEC SQL CLOSE tarsum_cur;
	EXEC SQL COMMIT;
}
