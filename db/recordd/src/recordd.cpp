/*
 * rts2-recordd: record selected device values into the observation database.
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

/*
 * db note: new daemon, no classic counterpart of its own - classic
 * recorded telemetry inside rts2-xmlrpcd (the web daemon), driven by the
 * <record/> entries of its event XML, which is why replacing that daemon
 * with rts2-httpd silently lost the cloudmeter graphs at D50. Recording
 * has nothing to do with serving HTTP: it is "watch the bus, write to the
 * database", so it is its own daemon here and rts2-httpd only reads what
 * it wrote (the /api/db/records endpoints).
 *
 * The configuration file is deliberately rts2-logd's format, not classic
 * httpd's event XML: sites already have such files (e.g. the
 * /etc/rts2/cloud that logd on this very machine points at), it is one
 * line per device, and it keeps a libxml2 dependency out of a daemon that
 * has no other use for one.
 *
 *   # device  cadence  values...
 *   CLOUD     60       TEMP_DIFF TEMP_IN TEMP_AMB HEATER
 *
 * Sampling is periodic rather than change-driven, which is the one
 * deliberate difference from classic's <value cadency=""><record/>. A
 * change-driven recorder writes nothing at all for a value that is not
 * changing, so a graph of it has no line where the reading was perfectly
 * steady - exactly when a telemetry plot most wants to show that it was.
 * Every cadence seconds, whatever the bus last reported for the value is
 * what gets stored.
 */

#include "rts2db/devicedb.h"
#include "rts2db/records.h"
#include "rts2db/sqlerror.h"

#include "utilsfunc.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

namespace rts2db
{

/**
 * One configured device/value pair, plus everything the daemon learns
 * about it at runtime.
 */
class RecordedValue
{
	public:
		RecordedValue (const char *_device, const char *_value, double _cadence)
		{
			device = _device;
			value = _value;
			cadence = _cadence;
			nextSample = 0;
			recvalId = -1;
			warnedMissing = false;
			warnedType = false;
		}

		std::string device;
		std::string value;

		/** seconds between samples */
		double cadence;
		/** when this value is due to be sampled again */
		double nextSample;

		/** recvals row, resolved lazily on the first successful sample -
		 * doing it at startup instead would mean creating rows for
		 * values that may never appear, and would tie startup to the
		 * database being up at that exact moment. */
		int recvalId;

		/** "the device/value isn't there" and "its type cannot be
		 * recorded" are both worth saying once, and then never again -
		 * a daemon that runs for months must not fill the log with the
		 * same line every cadence seconds. */
		bool warnedMissing;
		bool warnedType;
};

class RecordD:public rts2db::DeviceDb
{
	public:
		RecordD (int argc, char **argv);

	protected:
		virtual int processArgs (const char *arg);
		virtual int init ();
		virtual int idle ();
		virtual int willConnect (rts2core::NetworkAddress *_addr);
		virtual void usage ();

	private:
		std::vector <std::string> configFiles;
		std::vector <RecordedValue> recorded;

		rts2core::ValueLong *recordsWritten;
		rts2core::ValueLong *recordErrors;
		rts2core::ValueTime *lastRecord;

		int loadConfig (const char *filename);
		void sample (RecordedValue &rec, double now);
		bool isRecordedDevice (const char *device);
};

}

using namespace rts2db;

RecordD::RecordD (int argc, char **argv):rts2db::DeviceDb (argc, argv, DEVICE_TYPE_LOGD, "RECORD")
{
	createValue (recordsWritten, "records", "samples written to the database since start", false);
	createValue (recordErrors, "errors", "database errors while writing samples", false);
	createValue (lastRecord, "last_record", "time of the last sample written", false);

	recordsWritten->setValueLong (0);
	recordErrors->setValueLong (0);
	lastRecord->setValueDouble (NAN);
}

void RecordD::usage ()
{
	std::cout << "Record device values into the observation database, for later graphing" << std::endl
		<< "by rts2-httpd's /api/db/records endpoint. Takes one or more configuration" << std::endl
		<< "files, in rts2-logd's format - one line per device:" << std::endl
		<< std::endl
		<< "  # device  cadence  values..." << std::endl
		<< "  CLOUD     60       TEMP_DIFF TEMP_IN TEMP_AMB" << std::endl
		<< std::endl
		<< "  " << getAppName () << " /etc/rts2/record" << std::endl
		<< "  " << getAppName () << " -d RECORD --database stars /etc/rts2/record" << std::endl;
}

int RecordD::processArgs (const char *arg)
{
	configFiles.push_back (std::string (arg));
	return 0;
}

int RecordD::loadConfig (const char *filename)
{
	std::ifstream ifs (filename);
	if (!ifs.good ())
	{
		logStream (MESSAGE_ERROR) << "cannot open configuration file " << filename << sendLog;
		return -1;
	}

	int ln = 0;
	std::string line;
	while (std::getline (ifs, line))
	{
		ln++;
		size_t comment = line.find ('#');
		if (comment != std::string::npos)
			line = line.substr (0, comment);

		std::istringstream is (line);
		std::string device;
		double cadence;
		if (!(is >> device))
			continue;				 // blank (or comment-only) line

		if (!(is >> cadence) || cadence <= 0)
		{
			logStream (MESSAGE_ERROR) << filename << ":" << ln << ": expected a positive cadence in seconds after the device name" << sendLog;
			return -1;
		}

		std::string value;
		int named = 0;
		while (is >> value)
		{
			recorded.push_back (RecordedValue (device.c_str (), value.c_str (), cadence));
			named++;
		}

		if (named == 0)
		{
			logStream (MESSAGE_ERROR) << filename << ":" << ln << ": no value names listed for device " << device << sendLog;
			return -1;
		}
	}

	return 0;
}

int RecordD::init ()
{
	int ret = rts2db::DeviceDb::init ();
	if (ret)
		return ret;

	if (configFiles.empty ())
	{
		logStream (MESSAGE_ERROR) << "no configuration file given - see " << getAppName () << " --help" << sendLog;
		return -1;
	}

	for (std::vector <std::string>::iterator iter = configFiles.begin (); iter != configFiles.end (); iter++)
	{
		ret = loadConfig (iter->c_str ());
		if (ret)
			return ret;
	}

	if (recorded.empty ())
	{
		logStream (MESSAGE_ERROR) << "configuration lists no values to record" << sendLog;
		return -1;
	}

	// Spread the first sample of each value over its own cadence instead
	// of firing every one of them in the same second at startup - a site
	// recording thirty values would otherwise open the run with thirty
	// simultaneous inserts, and keep doing so on every restart.
	double now = getNow ();
	int i = 0;
	for (std::vector <RecordedValue>::iterator iter = recorded.begin (); iter != recorded.end (); iter++, i++)
		iter->nextSample = now + (iter->cadence * i) / recorded.size ();

	logStream (MESSAGE_INFO) << "recording " << recorded.size () << " value(s)" << sendLog;

	return 0;
}

bool RecordD::isRecordedDevice (const char *device)
{
	for (std::vector <RecordedValue>::iterator iter = recorded.begin (); iter != recorded.end (); iter++)
	{
		if (iter->device == device)
			return true;
	}
	return false;
}

int RecordD::willConnect (rts2core::NetworkAddress *_addr)
{
	// Only the devices actually being recorded from - unlike rts2-httpd,
	// which watches everything, this daemon has a fixed, short list and
	// no reason to hold a connection to anything else. The type/name
	// ordering below is httpd's own rule (lower type, or same type and
	// lower name, initiates), which keeps two devices from both dialing
	// each other; if the other side has the higher type it opens the
	// connection to us instead, so the pair still ends up connected.
	if (!isRecordedDevice (_addr->getName ()))
		return 0;
	if (_addr->getType () < getDeviceType ()
		|| (_addr->getType () == getDeviceType () && strcmp (_addr->getName (), getDeviceName ()) < 0))
		return 1;
	return 0;
}

void RecordD::sample (RecordedValue &rec, double now)
{
	rts2core::Connection *conn = getOpenConnection (rec.device.c_str ());
	if (conn == nullptr)
	{
		if (!rec.warnedMissing)
		{
			logStream (MESSAGE_WARNING) << "device " << rec.device << " is not connected - not recording " << rec.value << " until it appears" << sendLog;
			rec.warnedMissing = true;
		}
		return;
	}

	rts2core::Value *value = conn->getValue (rec.value.c_str ());
	if (value == nullptr)
	{
		if (!rec.warnedMissing)
		{
			logStream (MESSAGE_WARNING) << "device " << rec.device << " has no value " << rec.value << " - not recording it" << sendLog;
			rec.warnedMissing = true;
		}
		return;
	}

	rec.warnedMissing = false;

	switch (value->getValueBaseType ())
	{
		case RTS2_VALUE_DOUBLE:
		case RTS2_VALUE_FLOAT:
		case RTS2_VALUE_TIME:
		case RTS2_VALUE_INTEGER:
		case RTS2_VALUE_LONGINT:
		case RTS2_VALUE_SELECTION:
		case RTS2_VALUE_BOOL:
			break;
		default:
			if (!rec.warnedType)
			{
				logStream (MESSAGE_ERROR) << rec.device << "." << rec.value << " is not a numeric or boolean value - it cannot be recorded" << sendLog;
				rec.warnedType = true;
			}
			return;
	}

	double v = value->getValueDouble ();
	if (std::isnan (v))
		return;						 // a gap in the graph is the honest rendering of "no reading"

	try
	{
		if (rec.recvalId < 0)
			rec.recvalId = getRecvalId (rec.device.c_str (), rec.value.c_str (), value->getValueType ());

		recordValue (rec.recvalId, value->getValueType (), now, v);

		recordsWritten->inc ();
		lastRecord->setValueDouble (now);
		sendValueAll (recordsWritten);
		sendValueAll (lastRecord);
	}
	catch (rts2core::Error &er)
	{
		// Never fatal: a database that is down, restarting, or refusing
		// one statement must not take the daemon with it - the sample is
		// lost, the next one is tried on the next cadence. recvalId is
		// dropped so a failed lookup is retried rather than cached.
		rec.recvalId = -1;
		recordErrors->inc ();
		sendValueAll (recordErrors);
		logStream (MESSAGE_ERROR) << "cannot record " << rec.device << "." << rec.value << ": " << er << sendLog;
	}
}

int RecordD::idle ()
{
	double now = getNow ();
	for (std::vector <RecordedValue>::iterator iter = recorded.begin (); iter != recorded.end (); iter++)
	{
		if (now < iter->nextSample)
			continue;
		sample (*iter, now);
		// from now, not from the due time: a sample that was late (a
		// slow database, a daemon that just started) must not make the
		// next one due immediately.
		iter->nextSample = now + iter->cadence;
	}
	return rts2db::DeviceDb::idle ();
}

int main (int argc, char **argv)
{
	RecordD device (argc, argv);
	return device.run ();
}
