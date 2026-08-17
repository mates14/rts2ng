#pragma once

#include "value.h"
#include "connection.h"

#include <sstream>

namespace rts2web
{

/**
 * JSON string encoding helpers and RTS2 Value -> JSON serialization.
 *
 * Freshly written for web (not a mechanical port of classic
 * lib/rts2json/jsonvalue.cpp), simplified for what's actually needed so
 * far: no "extended" ([flags,value,error,warning,description]) mode, no
 * array/stat/rectangle value rendering yet - added when a real endpoint
 * needs them (STATUS.md task 2 follow-up). One deliberate improvement
 * over classic: string values are actually JSON-escaped here (classic's
 * sendValue()/jsonValue() wrote raw device/value strings straight into
 * the response with no escaping - a value or device name containing a
 * literal `"` would corrupt the JSON output).
 */

/** Write a JSON-escaped copy of s (no surrounding quotes) to os.
 * s == nullptr is treated as "" - see the .cpp for why that's a real
 * case, not just defensive padding. */
void jsonEscape (const char *s, std::ostringstream &os);

/** Write a JSON string literal (with surrounding quotes) to os.
 * s == nullptr is treated as "" (see jsonEscape()). */
void jsonString (const char *s, std::ostringstream &os);

/** Write a JSON number to os - NaN becomes `null` (bare `nan`/`inf`
 * aren't valid JSON tokens; strict parsers like JS's JSON.parse() reject
 * them outright) rather than the value's actual textual meaning
 * (unknown/not-applicable, which null already conveys). Used for
 * anything that can legitimately be NaN - a target's current RA/Dec
 * (e.g. a target type with no fixed position) being the case that
 * surfaced this. */
void jsonNumber (double d, std::ostringstream &os);

/** Write "name":value for a single Value to os. */
void jsonValue (rts2core::Value *value, std::ostringstream &os);

/** Write {"name":value,...} for every value on a connection to os. */
void sendConnectionValues (rts2core::Connection *conn, std::ostringstream &os);

}
