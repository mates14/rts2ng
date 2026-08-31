#include "jsonvalue.h"
#include "valuearray.h"

#include <charconv>
#include <cmath>
#include <cstdio>

using namespace rts2core;

void rts2web::jsonEscape (const char *s, std::ostringstream &os)
{
	// Not every const char* accessor in this codebase is guaranteed
	// non-null the way std::string::c_str() is - e.g.
	// rts2db::Target::getTargetName() is backed by a raw char* that
	// stays nullptr for targets whose load() path never calls
	// setTargetName() (the CalibrationTarget/"Master calibration
	// target" singleton, ID 6, is a real, confirmed example - its
	// load() searches for other calibration targets instead of
	// reading its own identity from the DB). Found by a real crash:
	// dbListTargets() -> jsonString(tar->getTargetName(), os) segfaulted
	// dereferencing a null s here. Treating null as "" is the correct
	// fix at this layer - every caller across the codebase benefits,
	// not just the one call site that happened to trigger it.
	if (s == nullptr)
		return;
	for (const unsigned char *p = (const unsigned char *) s; *p; p++)
	{
		switch (*p)
		{
			case '"':
				os << "\\\"";
				break;
			case '\\':
				os << "\\\\";
				break;
			case '\n':
				os << "\\n";
				break;
			case '\r':
				os << "\\r";
				break;
			case '\t':
				os << "\\t";
				break;
			default:
				if (*p < 0x20)
				{
					char buf[8];
					snprintf (buf, sizeof (buf), "\\u%04x", *p);
					os << buf;
				}
				else
				{
					os << (char) *p;
				}
				break;
		}
	}
}

void rts2web::jsonString (const char *s, std::ostringstream &os)
{
	os << '"';
	jsonEscape (s, os);
	os << '"';
}

void rts2web::jsonNumber (double d, std::ostringstream &os)
{
	// Infinity joins NaN in becoming null: `inf` is no more a valid JSON
	// token than `nan` is, and null is what a strict parser (JS's
	// JSON.parse(), or rts2-jsonclient's own reader) can actually take.
	if (!std::isfinite (d))
	{
		os << "null";
		return;
	}

	// std::to_chars, not `os << d`. The stream operator writes six
	// *significant* digits by default, which silently destroys any large
	// number: a ctime of 1788180123.456 came out as 1.78818e+09 - rounded
	// to the nearest ~1000 seconds - so every message in /api/messages
	// carried the same timestamp, and every RTS2_VALUE_TIME in
	// /api/getall was useless. This is the shortest representation that
	// parses back to exactly the same double, so nothing is lost and
	// 0.1 still prints as 0.1 (setprecision(17) would print
	// 0.10000000000000001).
	char buf[64];
	std::to_chars_result res = std::to_chars (buf, buf + sizeof (buf), d);
	if (res.ec != std::errc ())
	{
		os << "null";
		return;
	}
	os.write (buf, res.ptr - buf);
}

void rts2web::jsonTime (double t, std::ostringstream &os)
{
	// Times go on the wire as ctime seconds, always - never as JD, and
	// never following the process-wide display mode base's --jd/--ctime
	// set (rts2format.h's timeDisplay_t). That mode is for human-facing
	// output; this is a protocol, and its consumers (app.js's
	// `new Date (t * 1000)`, rts2-jsonclient) have to know what they are
	// reading without asking how the daemon was started. It exists as its
	// own function anyway so that a time is a deliberate thing in the
	// serializer rather than an anonymous double, which is precisely how
	// the precision bug above went unnoticed.
	jsonNumber (t, os);
}

// Renders IntegerArray/BoolArray/DoubleArray/TimeArray/StringArray as a
// plain JSON array of their elements. Split out of jsonValue() since it
// needs a different dynamic_cast per base type (the array classes don't
// share a typed element accessor) - see jsonvalue.h's doc comment for
// why this exists at all (a real production bug, not written blind).
static void jsonArrayValue (Value *value, std::ostringstream &os)
{
	os << "[";
	switch (value->getValueBaseType ())
	{
		case RTS2_VALUE_DOUBLE:
		case RTS2_VALUE_FLOAT:
		case RTS2_VALUE_TIME:
		{
			DoubleArray *arr = (DoubleArray *) value;
			bool first = true;
			for (std::vector <double>::iterator iter = arr->valueBegin (); iter != arr->valueEnd (); iter++)
			{
				if (!first)
					os << ",";
				first = false;
				rts2web::jsonNumber (*iter, os);
			}
			break;
		}
		case RTS2_VALUE_STRING:
		{
			StringArray *arr = (StringArray *) value;
			bool first = true;
			for (std::vector <std::string>::iterator iter = arr->valueBegin (); iter != arr->valueEnd (); iter++)
			{
				if (!first)
					os << ",";
				first = false;
				rts2web::jsonString (iter->c_str (), os);
			}
			break;
		}
		case RTS2_VALUE_INTEGER:
		case RTS2_VALUE_LONGINT:
		case RTS2_VALUE_SELECTION:
		case RTS2_VALUE_BOOL:
		default:
		{
			// IntegerArray and BoolArray (BoolArray IS-A IntegerArray,
			// storing 0/1) both iterate as plain ints - bare numbers are
			// valid JSON either way, so no separate bool-array rendering
			// is needed.
			IntegerArray *arr = (IntegerArray *) value;
			bool first = true;
			for (std::vector <int>::iterator iter = arr->valueBegin (); iter != arr->valueEnd (); iter++)
			{
				if (!first)
					os << ",";
				first = false;
				os << *iter;
			}
			break;
		}
	}
	os << "]";
}

void rts2web::jsonValue (Value *value, std::ostringstream &os)
{
	jsonString (value->getName ().c_str (), os);
	os << ":";
	if (value->getValueExtType () & RTS2_VALUE_ARRAY)
	{
		jsonArrayValue (value, os);
		return;
	}
	switch (value->getValueBaseType ())
	{
		case RTS2_VALUE_STRING:
			jsonString (value->getValue (), os);
			break;
		case RTS2_VALUE_DOUBLE:
		case RTS2_VALUE_FLOAT:
			jsonNumber (value->getValueDouble (), os);
			break;
		case RTS2_VALUE_TIME:
			jsonTime (value->getValueDouble (), os);
			break;
		case RTS2_VALUE_INTEGER:
			os << value->getValueInteger ();
			break;
		case RTS2_VALUE_LONGINT:
		case RTS2_VALUE_SELECTION:
		case RTS2_VALUE_BOOL:
			// getValue() already renders these as a bare numeric string
			// (e.g. "0"/"1"), safe to embed unquoted.
			os << value->getValue ();
			break;
		default:
			jsonString (value->getDisplayValue (), os);
			break;
	}
}

void rts2web::sendConnectionValues (Connection *conn, std::ostringstream &os)
{
	os << "{";
	bool first = true;
	for (ValueVector::iterator iter = conn->valueBegin (); iter != conn->valueEnd (); iter++)
	{
		if (!first)
			os << ",";
		first = false;
		jsonValue (*iter, os);
	}
	// The connection's raw RTS2 protocol state (rts2_status_t bitmask -
	// SERVERD_ONOFF_MASK etc, status.h) is not itself a named Value, so
	// it wouldn't otherwise appear here at all - a frontend showing
	// e.g. centrald's on/standby/off needs it. Matches classic's own
	// per-device JSON, which always includes this alongside the value
	// list (lib/rts2json/jsonvalue.cpp in the old tree).
	if (!first)
		os << ",";
	os << "\"state\":" << conn->getState ();
	os << "}";
}
