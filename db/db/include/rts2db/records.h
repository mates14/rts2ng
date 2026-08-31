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
#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace rts2db
{

/**
 * Access to the telemetry history: the `recvals` catalogue of recorded
 * device/value pairs and the `records_double`/`_integer`/`_boolean`
 * sample tables behind it.
 *
 * db note: new in this tree, but the *schema* is classic's, unchanged and
 * deliberately so - a site upgrading from classic (D50, 2026-08-31) has
 * years of history in these tables and must keep reading it. What is new
 * is who writes them: classic's rts2-xmlrpcd recorded values itself, as a
 * side job of the web daemon, driven by the `<record/>` entries of its
 * event XML. Here recording is rts2-recordd's job (db/recordd/) and
 * rts2-httpd only reads, which is why this lives in `db` and not in
 * `web` - neither end owns it.
 *
 * The tables carry `rectime` as `timestamp without time zone`, written
 * and read the way every other time in this tree is (`to_timestamp()` in,
 * `EXTRACT (EPOCH FROM ...)` out), so a sample time is a plain ctime
 * double everywhere above this layer.
 */

/**
 * One row of `recvals` - a device/value pair that is being recorded.
 */
class Recval
{
	public:
		Recval (int _recvalId, const char *_device, const char *_value, int _valueType)
		{
			recvalId = _recvalId;
			device = _device;
			value = _value;
			valueType = _valueType;
			timeFrom = NAN;
			timeTo = NAN;
			samples = 0;
		}

		int recvalId;
		std::string device;
		std::string value;

		/** RTS2 value type as recorded - Value::getValueType(), whose
		 * RTS2_BASE_TYPE bits decide which records_* table holds the
		 * samples. */
		int valueType;

		/** Extent of the stored samples; NAN/0 unless load() was asked
		 * to fill them in. */
		double timeFrom;
		double timeTo;
		long samples;
};

class RecvalsSet:public std::vector <Recval>
{
	public:
		/**
		 * Load the whole recvals catalogue.
		 *
		 * @param withExtent  also fill in each entry's timeFrom/timeTo/
		 *   samples. That is one aggregate query per row, over an index -
		 *   cheap for the handful of values a site records, but it is the
		 *   caller's call, since a bare listing doesn't need it.
		 */
		void load (bool withExtent = false);
};

/**
 * One point of a loaded series. When the load was not bucketed, avg is
 * the sample itself and min == max == avg with n == 1; when it was,
 * min/max carry the spread inside the bucket, which is what makes a
 * downsampled graph honest about a spike instead of averaging it away.
 */
class RecordEntry
{
	public:
		RecordEntry (double _t, double _avg, double _min, double _max, long _n)
		{
			t = _t;
			avg = _avg;
			min = _min;
			max = _max;
			n = _n;
		}

		double t;
		double avg;
		double min;
		double max;
		long n;
};

/**
 * recvals row id for a device/value pair, or -1 when this pair has never
 * been recorded. valueType, when not nullptr, receives the type the row
 * was created with - a reader needs it to know which records_* table the
 * samples are in.
 *
 * The lookup-only half of getRecvalId(), split out because rts2-httpd
 * must never create a recvals row: an /api/db/records call naming a
 * device that is not recorded is a 400, not a reason to catalogue it.
 *
 * @throw SqlError on any database problem.
 */
int findRecvalId (const char *device, const char *value, int *valueType = nullptr);

/**
 * recvals row id for a device/value pair, creating the row (from the
 * recval_ids sequence) when this pair has never been recorded before.
 *
 * @throw SqlError on any database problem.
 */
int getRecvalId (const char *device, const char *value, int valueType);

/**
 * Store one sample. valueType picks the table: double/float/time go to
 * records_double, integer/selection to records_integer, bool to
 * records_boolean; anything else throws, since there is no table for it
 * (a string value has no history worth graphing, and classic never
 * recorded one either).
 *
 * @throw SqlError on any database problem.
 */
void recordValue (int recvalId, int valueType, double t, double value);

/**
 * Load a series between two times.
 *
 * @param maxPoints  when > 0 and the range holds more samples than this,
 *   average them into that many equal-length time buckets in the database
 *   rather than shipping every row to the caller. A month of 60 s
 *   telemetry is ~43000 rows; a graph 1000 pixels wide cannot show them
 *   and a browser should not have to parse them.
 *
 * @throw SqlError on any database problem.
 */
void loadRecords (int recvalId, int valueType, double from, double to, int maxPoints, std::vector <RecordEntry> &records);

}
