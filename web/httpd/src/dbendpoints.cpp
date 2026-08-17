#include "dbendpoints.h"

#ifdef WEB_HAVE_DB

#include "jsonvalue.h"

#include "rts2db/target.h"
#include "rts2db/observationset.h"
#include "rts2db/imageset.h"
#include "configuration.h"

#include <libnova/libnova.h>
#include <ctime>
#include <map>

using namespace rts2web;

namespace
{

/**
 * Ported from classic's lib/rts2json/nightdur.cpp getNightDuration() -
 * kept file-local (not in dbendpoints.h) since it's purely an input-
 * normalization helper for the two night-based endpoints below, not
 * something httpd.cpp needs to call directly. Fills in the missing
 * year/month/day components with sensible "whole range" defaults so a
 * partially-specified night (e.g. year+month only) still produces a
 * well-defined [from, from+duration) window, then resolves the actual
 * night boundary via Configuration::getNight() (astronomical-night
 * start, not local midnight).
 */
void getNightDuration (int year, int month, int day, time_t &from, int64_t &duration)
{
	if (year <= 0)
	{
		year = 2000;
		month = day = 1;
		duration = 1000LL * 365 * 86400;
	}
	else if (month <= 0)
	{
		month = day = 1;
		duration = 365 * 86400;
	}
	else if (day <= 0)
	{
		day = 1;
		duration = 31 * 86400;
	}
	else
	{
		duration = 86400;
	}
	from = rts2core::Configuration::instance ()->getNight (year, month, day);
}

/**
 * Best-effort inverse of what handlePreview()/checkPreviewCache() do
 * with imagesDir+relPath (preview.cpp: fullPath = imagesDir + "/" +
 * relPath) - strips the imagesDir prefix off a DB-stored absolute image
 * path so the result is directly usable as a /preview/<result> path.
 * Returns "" (not an error - just "no preview link available") when
 * imagesDir isn't configured or the stored path doesn't actually fall
 * under it, e.g. a differently-laid-out archive or an imagesDir that
 * wasn't passed on this daemon's command line.
 */
std::string computePreviewPath (const std::string &imagesDir, const char *absPath)
{
	if (!absPath || imagesDir.empty ())
		return "";
	std::string prefix = imagesDir;
	if (prefix.back () != '/')
		prefix += '/';
	std::string ap (absPath);
	if (ap.compare (0, prefix.size (), prefix) == 0)
		return ap.substr (prefix.size ());
	return "";
}

}

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

void rts2web::dbCurrentNight (std::ostringstream &os)
{
	// Same "which night does this timestamp belong to" computation
	// Configuration::getNight()'s own zero-arg overload does internally
	// (base/kernel/include/configuration.h) - duplicated here only
	// because that overload returns a time_t, not the year/month/day
	// this endpoint needs to hand straight to dbNightDetail()/
	// dbSearchImagesByNight().
	time_t t = rts2core::Configuration::instance ()->getNight (time (nullptr));
	struct tm tm_s;
	gmtime_r (&t, &tm_s);
	os << "{\"year\":" << (tm_s.tm_year + 1900) << ",\"month\":" << (tm_s.tm_mon + 1) << ",\"day\":" << tm_s.tm_mday << "}";
}

void rts2web::dbNightsSummary (int year, int month, int day, std::ostringstream &os)
{
	// Same call classic's Night::callAPI() makes for its "incomplete
	// date" branch - hour/minutes stay at ObservationSetDate::load()'s
	// defaults, matching upstream exactly (see dbendpoints.h's comment
	// on why this file doesn't try to "fix" that default).
	rts2db::ObservationSetDate as;
	as.load (year, month, day);

	const char *level = year <= 0 ? "year" : month <= 0 ? "month" : day <= 0 ? "day" : "hour";

	os << "{\"level\":\"" << level << "\",\"entries\":[";
	bool first = true;
	for (rts2db::ObservationSetDate::iterator iter = as.begin (); iter != as.end (); iter++)
	{
		if (!first)
			os << ",";
		first = false;
		os << "{\"key\":" << iter->first << ",\"observations\":" << iter->second.c << ",\"images\":" << iter->second.i << ",\"goodImages\":" << iter->second.gi << ",\"timeOnSky\":";
		jsonNumber (iter->second.tt, os);
		os << "}";
	}
	os << "]}";
}

void rts2web::dbNightDetail (int year, int month, int day, std::ostringstream &os)
{
	time_t from;
	int64_t duration;
	getNightDuration (year, month, day, from, duration);
	time_t end = from + duration;

	rts2db::ObservationSet obsSet;
	obsSet.loadTime (&from, &end);

	os << "[";
	bool first = true;
	for (rts2db::ObservationSet::iterator iter = obsSet.begin (); iter != obsSet.end (); iter++)
	{
		if (!first)
			os << ",";
		first = false;
		os << "{\"id\":" << iter->getObsId () << ",\"targetId\":" << iter->getTargetId () << ",\"targetName\":";
		jsonString (iter->getTargetName ().c_str (), os);
		os << ",\"slew\":";
		jsonNumber (iter->getObsSlew (), os);
		os << ",\"start\":";
		jsonNumber (iter->getObsStart (), os);
		os << ",\"end\":";
		jsonNumber (iter->getObsEnd (), os);
		os << ",\"images\":" << iter->getNumberOfImages () << ",\"goodImages\":" << iter->getNumberOfGoodImages () << ",\"timeOnSky\":";
		jsonNumber (iter->getTimeOnSky (), os);
		os << "}";
	}
	os << "]";
}

/** Shared by dbSearchImagesByTarget/dbSearchImagesByNight - both just
 * differ in how the ImageSet subclass is constructed/loaded. */
// Image::getTargetName() (base/kernel/src/image.cpp) is only ever
// backed by the DB row's numeric target_id, not a name column - when
// unset (always, for an ImageSet built straight from DB rows, see
// ImageSkyDb's DB-row constructor calling setTargetHeaders() with
// targetName left null), it lazily *opens the actual FITS file on disk*
// to read the OBJECT header. Calling it once per image in a loop over a
// whole night is exactly the kind of thing that looks fine against a
// handful of local test images and then falls over against a real
// archive: found live against lascaux's production data, a single
// 6585-image night took 6+ seconds and monopolized the worker pool
// shared with every other DB/preview request for that whole time
// (visible as an unrelated concurrent request 502ing through Apache's
// proxy timeout, and the dashboard's WebSocket looking like it dropped).
// Fixed by resolving target names once per unique target_id (already
// free - the DB row already carries it) via a request-local cache
// instead of once per image; a night typically reuses a handful of
// targets across thousands of images, so this turns thousands of FITS
// opens into at most a handful of target DB lookups.
static std::string resolveTargetName (int targetId, std::map <int, std::string> &nameCache)
{
	std::map <int, std::string>::iterator cached = nameCache.find (targetId);
	if (cached != nameCache.end ())
		return cached->second;

	std::string name;
	try
	{
		rts2db::Target *tar = createTarget (targetId, rts2core::Configuration::instance ()->getObserver (), rts2core::Configuration::instance ()->getObservatoryAltitude ());
		name = tar->getTargetName () ? tar->getTargetName () : "";
		delete tar;
	}
	catch (rts2core::Error &er)
	{
		// Target row itself gone/unloadable - leave name empty rather
		// than failing the whole image listing over one bad target.
	}
	nameCache[targetId] = name;
	return name;
}

static void writeImageSetJson (rts2db::ImageSet &is, const std::string &imagesDir, std::ostringstream &os)
{
	std::map <int, std::string> nameCache;

	os << "[";
	bool first = true;
	for (rts2db::ImageSet::iterator iter = is.begin (); iter != is.end (); iter++)
	{
		if (!first)
			os << ",";
		first = false;

		rts2image::Image *img = *iter;
		os << "{\"path\":";
		jsonString (img->getFileName (), os);
		os << ",\"previewPath\":";
		jsonString (computePreviewPath (imagesDir, img->getFileName ()).c_str (), os);
		os << ",\"obsId\":" << img->getObsId () << ",\"targetId\":" << img->getTargetId () << ",\"targetName\":";
		jsonString (resolveTargetName (img->getTargetId (), nameCache).c_str (), os);
		os << ",\"cameraName\":";
		jsonString (img->getCameraName (), os);
		os << ",\"exposureStart\":";
		jsonNumber (img->getExposureStart (), os);
		os << "}";
	}
	os << "]";
}

void rts2web::dbSearchImagesByTarget (const std::string &imagesDir, int targetId, std::ostringstream &os)
{
	// Same existence check as dbListObservations() - a bad target id
	// should be a 400, not silently "no images found".
	rts2db::Target *tar = createTarget (targetId, rts2core::Configuration::instance ()->getObserver (), rts2core::Configuration::instance ()->getObservatoryAltitude ());
	delete tar;

	rts2db::ImageSetTarget is (targetId);
	is.load ();
	writeImageSetJson (is, imagesDir, os);
}

void rts2web::dbSearchImagesByNight (const std::string &imagesDir, int year, int month, int day, std::ostringstream &os)
{
	time_t from;
	int64_t duration;
	getNightDuration (year, month, day, from, duration);
	time_t end = from + duration;

	rts2db::ImageSetDate is (from, end);
	is.load ();
	writeImageSetJson (is, imagesDir, os);
}

#endif // WEB_HAVE_DB
