#include "jsonvalue.h"

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
	if (std::isnan (d))
		os << "null";
	else
		os << d;
}

void rts2web::jsonValue (Value *value, std::ostringstream &os)
{
	jsonString (value->getName ().c_str (), os);
	os << ":";
	switch (value->getValueBaseType ())
	{
		case RTS2_VALUE_STRING:
			jsonString (value->getValue (), os);
			break;
		case RTS2_VALUE_DOUBLE:
		case RTS2_VALUE_FLOAT:
		case RTS2_VALUE_TIME:
			jsonNumber (value->getValueDouble (), os);
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
	os << "}";
}
