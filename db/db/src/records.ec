/*
 * Recorded telemetry (recvals + records_* tables).
 * Copyright (C) 2026 Petr Kubanek <petr@kubanek.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "rts2db/records.h"
#include "rts2db/sqlerror.h"
#include "rts2db/devicedb.h"

#include "value.h"

#include <cstring>
#include <sstream>

EXEC SQL INCLUDE sqlca;

using namespace rts2db;

// recvals.device_name/value_name are varchar(25) in classic's schema and
// stay that way here - a longer name is refused rather than silently
// truncated into a second, colliding recvals row.
#define RECVAL_NAME_LEN   25

namespace
{

/**
 * Which records_* table holds samples of this value type, and the SQL
 * expression that reads its `value` column back as a double.
 *
 * records_boolean stores a real boolean, which avg()/min()/max() refuse
 * to work on - hence the ::int cast, applied on read only (the write side
 * has a typed host variable and needs no cast).
 */
const char *recordTable (int valueType)
{
	switch (valueType & RTS2_BASE_TYPE)
	{
		case RTS2_VALUE_DOUBLE:
		case RTS2_VALUE_FLOAT:
		case RTS2_VALUE_TIME:
			return "records_double";
		case RTS2_VALUE_INTEGER:
		case RTS2_VALUE_LONGINT:
		case RTS2_VALUE_SELECTION:
			return "records_integer";
		case RTS2_VALUE_BOOL:
			return "records_boolean";
		default:
			return nullptr;
	}
}

const char *recordValueExpr (int valueType)
{
	return (valueType & RTS2_BASE_TYPE) == RTS2_VALUE_BOOL ? "value::int" : "value";
}

const char *tableForType (int valueType)
{
	const char *table = recordTable (valueType);
	if (table == nullptr)
	{
		std::ostringstream os;
		os << "value type " << (valueType & RTS2_BASE_TYPE) << " has no records table - only numeric and boolean values are recorded";
		throw SqlError (os.str ().c_str ());
	}
	return table;
}

}

int rts2db::findRecvalId (const char *device, const char *value, int *valueType)
{
	if (checkDbConnection ())
		throw SqlError ();

	// A name that cannot be stored cannot match a stored row either, and
	// saying so here keeps getRecvalId() below from creating a truncated
	// duplicate on every call.
	if (strlen (device) > RECVAL_NAME_LEN || strlen (value) > RECVAL_NAME_LEN)
		throw SqlError ("device or value name is too long to record (max 25 characters)");

	EXEC SQL BEGIN DECLARE SECTION;
	int db_recval_id;
	int db_value_type;
	VARCHAR db_device[RECVAL_NAME_LEN + 1];
	VARCHAR db_value[RECVAL_NAME_LEN + 1];
	EXEC SQL END DECLARE SECTION;

	db_device.len = strlen (device);
	memcpy (db_device.arr, device, db_device.len);
	db_value.len = strlen (value);
	memcpy (db_value.arr, value, db_value.len);

	EXEC SQL SELECT
		recval_id,
		value_type
	INTO
		:db_recval_id,
		:db_value_type
	FROM
		recvals
	WHERE
		device_name = :db_device AND value_name = :db_value;

	if (sqlca.sqlcode)
	{
		if (sqlca.sqlcode == ECPG_NOT_FOUND)
		{
			EXEC SQL ROLLBACK;
			return -1;
		}
		SqlError err;
		EXEC SQL ROLLBACK;
		throw err;
	}

	EXEC SQL COMMIT;

	if (valueType != nullptr)
		*valueType = db_value_type;
	return db_recval_id;
}

int rts2db::getRecvalId (const char *device, const char *value, int valueType)
{
	int recvalId = findRecvalId (device, value);
	if (recvalId >= 0)
		return recvalId;

	if (checkDbConnection ())
		throw SqlError ();

	EXEC SQL BEGIN DECLARE SECTION;
	int db_recval_id;
	VARCHAR db_device[RECVAL_NAME_LEN + 1];
	VARCHAR db_value[RECVAL_NAME_LEN + 1];
	int db_value_type = valueType;
	EXEC SQL END DECLARE SECTION;

	db_device.len = strlen (device);
	memcpy (db_device.arr, device, db_device.len);
	db_value.len = strlen (value);
	memcpy (db_value.arr, value, db_value.len);

	// recval_id has no DEFAULT in classic's schema - the id comes from
	// the recval_ids sequence, same as classic's own recording code did.
	EXEC SQL SELECT nextval ('recval_ids') INTO :db_recval_id;
	if (sqlca.sqlcode)
	{
		SqlError err;
		EXEC SQL ROLLBACK;
		throw err;
	}

	EXEC SQL INSERT INTO recvals
		(recval_id, device_name, value_name, value_type)
	VALUES
		(:db_recval_id, :db_device, :db_value, :db_value_type);
	if (sqlca.sqlcode)
	{
		SqlError err;
		EXEC SQL ROLLBACK;
		throw err;
	}

	EXEC SQL COMMIT;
	return db_recval_id;
}

void rts2db::recordValue (int recvalId, int valueType, double t, double value)
{
	if (checkDbConnection ())
		throw SqlError ();

	EXEC SQL BEGIN DECLARE SECTION;
	int db_recval_id = recvalId;
	double db_rectime = t;
	double db_double = value;
	int db_int = (int) value;
	bool db_bool = value != 0;
	EXEC SQL END DECLARE SECTION;

	// ON CONFLICT DO NOTHING, not an error: records_* have a UNIQUE
	// (recval_id, rectime) index, and two samples landing on the same
	// timestamp is a duplicate to drop, not a failure worth propagating
	// up into the recorder's value-changed path.
	switch (valueType & RTS2_BASE_TYPE)
	{
		case RTS2_VALUE_DOUBLE:
		case RTS2_VALUE_FLOAT:
		case RTS2_VALUE_TIME:
			EXEC SQL INSERT INTO records_double
				(recval_id, rectime, value)
			VALUES
				(:db_recval_id, to_timestamp (:db_rectime), :db_double)
			ON CONFLICT DO NOTHING;
			break;
		case RTS2_VALUE_INTEGER:
		case RTS2_VALUE_LONGINT:
		case RTS2_VALUE_SELECTION:
			EXEC SQL INSERT INTO records_integer
				(recval_id, rectime, value)
			VALUES
				(:db_recval_id, to_timestamp (:db_rectime), :db_int)
			ON CONFLICT DO NOTHING;
			break;
		case RTS2_VALUE_BOOL:
			EXEC SQL INSERT INTO records_boolean
				(recval_id, rectime, value)
			VALUES
				(:db_recval_id, to_timestamp (:db_rectime), :db_bool)
			ON CONFLICT DO NOTHING;
			break;
		default:
			tableForType (valueType);	 // throws, always
			return;
	}

	if (sqlca.sqlcode)
	{
		SqlError err;
		EXEC SQL ROLLBACK;
		throw err;
	}
	EXEC SQL COMMIT;
}

void rts2db::loadRecords (int recvalId, int valueType, double from, double to, int maxPoints, std::vector <RecordEntry> &records)
{
	if (checkDbConnection ())
		throw SqlError ();

	const char *table = tableForType (valueType);
	const char *valueExpr = recordValueExpr (valueType);

	if (!(from < to))
		throw SqlError ("record range must start before it ends");

	EXEC SQL BEGIN DECLARE SECTION;
	char *rec_stmp_c;
	double db_rectime;
	double db_avg;
	double db_min;
	double db_max;
	long db_count;
	int db_avg_ind;
	int db_min_ind;
	int db_max_ind;
	EXEC SQL END DECLARE SECTION;

	std::ostringstream _os;
	_os.precision (17);
	if (maxPoints > 0)
	{
		// Bucketed in the database, not in the caller: a month of 60 s
		// telemetry is ~43000 rows, and neither the HTTP response nor a
		// graph a thousand pixels wide has any use for them. min/max per
		// bucket are carried alongside the average so a downsampled plot
		// can still show a spike instead of averaging it into nothing.
		double bucket = (to - from) / maxPoints;
		_os << "SELECT EXTRACT (EPOCH FROM min(rectime)),"
			"avg(" << valueExpr << ")::float8,"
			"min(" << valueExpr << ")::float8,"
			"max(" << valueExpr << ")::float8,"
			"count(*)"
			" FROM " << table <<
			" WHERE recval_id = " << recvalId <<
			" AND rectime >= to_timestamp (" << from << ")"
			" AND rectime <= to_timestamp (" << to << ")"
			" GROUP BY floor (EXTRACT (EPOCH FROM rectime) / " << bucket << ")"
			" ORDER BY 1 ASC;";
	}
	else
	{
		_os << "SELECT EXTRACT (EPOCH FROM rectime),"
			<< valueExpr << "::float8,"
			<< valueExpr << "::float8,"
			<< valueExpr << "::float8,"
			"1"
			" FROM " << table <<
			" WHERE recval_id = " << recvalId <<
			" AND rectime >= to_timestamp (" << from << ")"
			" AND rectime <= to_timestamp (" << to << ")"
			" ORDER BY rectime ASC;";
	}

	rec_stmp_c = new char[_os.str ().length () + 1];
	strcpy (rec_stmp_c, _os.str ().c_str ());

	EXEC SQL PREPARE rec_stmp FROM :rec_stmp_c;

	delete[] rec_stmp_c;

	if (sqlca.sqlcode)
	{
		SqlError err;
		EXEC SQL ROLLBACK;
		throw err;
	}

	EXEC SQL DECLARE rec_cur CURSOR FOR rec_stmp;

	EXEC SQL OPEN rec_cur;
	if (sqlca.sqlcode)
	{
		SqlError err;
		EXEC SQL ROLLBACK;
		throw err;
	}

	while (1)
	{
		EXEC SQL FETCH next FROM rec_cur INTO
			:db_rectime,
			:db_avg :db_avg_ind,
			:db_min :db_min_ind,
			:db_max :db_max_ind,
			:db_count;
		if (sqlca.sqlcode)
			break;
		// a NULL sample (the column is nullable) becomes NAN, the same
		// "unknown, not zero" convention the rest of this tree uses
		records.push_back (RecordEntry (db_rectime,
			db_avg_ind < 0 ? NAN : db_avg,
			db_min_ind < 0 ? NAN : db_min,
			db_max_ind < 0 ? NAN : db_max,
			db_count));
	}

	EXEC SQL CLOSE rec_cur;
	EXEC SQL COMMIT;
}

void RecvalsSet::load (bool withExtent)
{
	if (checkDbConnection ())
		throw SqlError ();

	EXEC SQL BEGIN DECLARE SECTION;
	int db_recval_id;
	VARCHAR db_device[RECVAL_NAME_LEN + 1];
	VARCHAR db_value[RECVAL_NAME_LEN + 1];
	int db_value_type;
	int db_device_ind;

	char *ext_stmp_c;
	double db_time_from;
	double db_time_to;
	long db_samples;
	int db_time_from_ind;
	int db_time_to_ind;
	EXEC SQL END DECLARE SECTION;

	clear ();

	EXEC SQL DECLARE recvals_cur CURSOR FOR
		SELECT
			recval_id,
			device_name,
			value_name,
			value_type
		FROM
			recvals
		ORDER BY
			device_name ASC, value_name ASC;

	EXEC SQL OPEN recvals_cur;
	if (sqlca.sqlcode)
	{
		SqlError err;
		EXEC SQL ROLLBACK;
		throw err;
	}

	while (1)
	{
		EXEC SQL FETCH next FROM recvals_cur INTO
			:db_recval_id,
			:db_device :db_device_ind,
			:db_value,
			:db_value_type;
		if (sqlca.sqlcode)
			break;
		db_device.arr[db_device_ind < 0 ? 0 : db_device.len] = '\0';
		db_value.arr[db_value.len] = '\0';
		push_back (Recval (db_recval_id, (char *) db_device.arr, (char *) db_value.arr, db_value_type));
	}

	EXEC SQL CLOSE recvals_cur;
	EXEC SQL COMMIT;

	if (!withExtent)
		return;

	// Deliberately after the cursor above is closed rather than inside
	// its loop: the table each row needs is decided by that row's value
	// type, so this is a second, dynamic statement per entry, and running
	// it while the outer cursor is still open would nest two statements
	// on one connection for no reason.
	for (RecvalsSet::iterator iter = begin (); iter != end (); iter++)
	{
		const char *table = recordTable (iter->valueType);
		if (table == nullptr)
			continue;				 // no samples table - nothing to measure

		std::ostringstream _os;
		_os.precision (17);
		_os << "SELECT EXTRACT (EPOCH FROM min(rectime)), EXTRACT (EPOCH FROM max(rectime)), count(*)"
			" FROM " << table <<
			" WHERE recval_id = " << iter->recvalId << ";";

		ext_stmp_c = new char[_os.str ().length () + 1];
		strcpy (ext_stmp_c, _os.str ().c_str ());

		EXEC SQL PREPARE ext_stmp FROM :ext_stmp_c;

		delete[] ext_stmp_c;

		if (sqlca.sqlcode)
		{
			SqlError err;
			EXEC SQL ROLLBACK;
			throw err;
		}

		EXEC SQL EXECUTE ext_stmp INTO
			:db_time_from :db_time_from_ind,
			:db_time_to :db_time_to_ind,
			:db_samples;
		if (sqlca.sqlcode)
		{
			SqlError err;
			EXEC SQL ROLLBACK;
			throw err;
		}

		iter->timeFrom = db_time_from_ind < 0 ? NAN : db_time_from;
		iter->timeTo = db_time_to_ind < 0 ? NAN : db_time_to;
		iter->samples = db_samples;

		EXEC SQL COMMIT;
	}
}
