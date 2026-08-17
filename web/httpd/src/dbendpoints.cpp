#include "dbendpoints.h"

#ifdef WEB_HAVE_DB

#include "jsonvalue.h"

#include "rts2db/target.h"
#include "rts2db/observationset.h"
#include "configuration.h"

#include <libnova/libnova.h>

using namespace rts2web;

void rts2web::dbListTargets (std::ostringstream &os)
{
	// Mirrors db/db/tools/targetlist.cpp's plain-listing case exactly
	// (new rts2db::TargetSet (targetType) with targetType defaulted to
	// nullptr) - "every target, regardless of type."
	rts2db::TargetSet ts ((const char *) nullptr);
	ts.load ();

	os << "[";
	bool first = true;
	for (rts2db::TargetSet::iterator iter = ts.begin (); iter != ts.end (); iter++)
	{
		if (!first)
			os << ",";
		first = false;

		rts2db::Target *tar = iter->second;
		struct ln_equ_posn pos;
		tar->getPosition (&pos);			 // current JD - see Rts2Target::getPosition(pos)'s default

		os << "{\"id\":" << tar->getTargetID () << ",\"name\":";
		jsonString (tar->getTargetName (), os);
		os << ",\"type\":\"" << tar->getTargetType () << "\",\"ra\":";
		jsonNumber (pos.ra, os);
		os << ",\"dec\":";
		jsonNumber (pos.dec, os);
		os << "}";
	}
	os << "]";
}

void rts2web::dbGetTarget (int targetId, std::ostringstream &os)
{
	// createTarget() throws rts2db::SqlError (a rts2core::Error) if
	// targetId doesn't exist - propagates to httpd.cpp's handleRequest(),
	// caught there the same way as ApiError.
	rts2db::Target *tar = createTarget (targetId, rts2core::Configuration::instance ()->getObserver (), rts2core::Configuration::instance ()->getObservatoryAltitude ());

	struct ln_equ_posn pos;
	tar->getPosition (&pos);

	os << "{\"id\":" << tar->getTargetID () << ",\"name\":";
	jsonString (tar->getTargetName (), os);
	os << ",\"type\":\"" << tar->getTargetType () << "\",\"comment\":";
	jsonString (tar->getTargetComment () ? tar->getTargetComment () : "", os);
	os << ",\"ra\":";
	jsonNumber (pos.ra, os);
	os << ",\"dec\":";
	jsonNumber (pos.dec, os);
	os << "}";

	delete tar;
}

void rts2web::dbListObservations (int targetId, std::ostringstream &os)
{
	// Confirm the target itself exists first - ObservationSet::loadTarget()
	// below would otherwise just silently return an empty set for a bad
	// id, making "target doesn't exist" indistinguishable from "target
	// exists, was never observed" (the latter is the expected, common
	// case on this session's sparse local test DB, not an error).
	rts2db::Target *tar = createTarget (targetId, rts2core::Configuration::instance ()->getObserver (), rts2core::Configuration::instance ()->getObservatoryAltitude ());
	delete tar;

	rts2db::ObservationSet obsSet;
	obsSet.loadTarget (targetId);

	os << "[";
	bool first = true;
	for (rts2db::ObservationSet::iterator iter = obsSet.begin (); iter != obsSet.end (); iter++)
	{
		if (!first)
			os << ",";
		first = false;
		os << "{\"id\":" << iter->getObsId () << ",\"start\":" << iter->getObsStart () << ",\"end\":" << iter->getObsEnd () << "}";
	}
	os << "]";
}

#endif // WEB_HAVE_DB
