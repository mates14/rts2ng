#include "dbendpoints.h"

#ifdef WEB_HAVE_DB

#include "jsonvalue.h"

#include "rts2db/target.h"
#include "rts2db/targetell.h"
#include "rts2db/observationset.h"
#include "rts2db/imageset.h"
#include "rts2db/scheduling.h"
#include "configuration.h"

#include <libnova/libnova.h>
#include <ctime>
#include <map>
#include <mutex>

using namespace rts2web;

namespace
{

/**
 * rts2db's ECPG-generated queries run over a single implicit connection
 * that isn't safe for concurrent use from multiple threads - two
 * queries in flight at once on it corrupt the wire protocol (libpq
 * error messages like "message contents do not agree with length in
 * message type T" / "server sent data without prior row description").
 * Found live: dbendpoints.cpp's functions all run on workerPool's
 * multiple threads (httpd.cpp's handleDb()), and the frontend's own
 * night-detail view fires /api/db/night and /api/db/images concurrently
 * via Promise.all - two DB-touching worker jobs racing on the one
 * connection was never a hypothetical, it happened on the first real
 * concurrent page load. Every public function below takes this before
 * touching rts2db and holds it for its whole body (including the
 * private helpers below, which never take it themselves - std::mutex
 * isn't recursive, only the outermost entry point may lock). This
 * makes DB endpoints line up behind each other instead of running
 * truly in parallel, but they were serialized by the single physical
 * connection anyway; the worker pool's job here is keeping the main
 * bus/WebSocket thread free, not DB-query parallelism.
 */
std::mutex dbAccessMutex;

}

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
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

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

namespace
{

/**
 * Full target detail, shared by dbGetTarget() and dbUpdateTarget()'s
 * post-save response so both return the same shape - the editor page
 * re-renders itself from whichever one it just got. `hasPosition` and
 * `isElliptical` tell the frontend which form fields apply to this
 * target's actual type (see TargetUpdate's doc comment in dbendpoints.h)
 * rather than it having to hardcode a type_id char -> form-shape table
 * of its own.
 */
void writeTargetDetail (rts2db::Target *tar, std::ostringstream &os)
{
	struct ln_equ_posn pos;
	tar->getPosition (&pos);

	rts2db::ConstTarget *ct = dynamic_cast <rts2db::ConstTarget *> (tar);
	bool isElliptical = dynamic_cast <rts2db::EllTarget *> (tar) != nullptr;

	os << "{\"id\":" << tar->getTargetID () << ",\"name\":";
	jsonString (tar->getTargetName (), os);
	os << ",\"type\":\"" << tar->getTargetType () << "\",\"comment\":";
	jsonString (tar->getTargetComment () ? tar->getTargetComment () : "", os);
	os << ",\"info\":";
	jsonString (tar->getTargetInfo (), os);
	os << ",\"priority\":";
	jsonNumber (tar->getTargetPriority (), os);
	os << ",\"bonus\":";
	jsonNumber (tar->getTargetBonus (), os);
	os << ",\"enabled\":" << (tar->getTargetEnabled () ? "true" : "false");
	os << ",\"interruptible\":" << (tar->getInterruptible () ? "true" : "false");
	os << ",\"isElliptical\":" << (isElliptical ? "true" : "false");
	os << ",\"hasPosition\":" << (ct ? "true" : "false");
	os << ",\"ra\":";
	jsonNumber (pos.ra, os);
	os << ",\"dec\":";
	jsonNumber (pos.dec, os);
	os << ",\"pmRa\":";
	if (ct)
	{
		struct ln_equ_posn pm;
		ct->getProperMotion (&pm);
		jsonNumber (pm.ra, os);
		os << ",\"pmDec\":";
		jsonNumber (pm.dec, os);
	}
	else
	{
		os << "null,\"pmDec\":null";
	}
	os << "}";
}

}

void rts2web::dbGetTarget (int targetId, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	// createTarget() throws rts2db::SqlError (a rts2core::Error) if
	// targetId doesn't exist - propagates to httpd.cpp's handleRequest(),
	// caught there the same way as ApiError.
	rts2db::Target *tar = createTarget (targetId, rts2core::Configuration::instance ()->getObserver (), rts2core::Configuration::instance ()->getObservatoryAltitude ());
	writeTargetDetail (tar, os);
	delete tar;
}

void rts2web::dbUpdateTarget (int targetId, const TargetUpdate &upd, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	rts2db::Target *tar = createTarget (targetId, rts2core::Configuration::instance ()->getObserver (), rts2core::Configuration::instance ()->getObservatoryAltitude ());

	rts2db::ConstTarget *ct = dynamic_cast <rts2db::ConstTarget *> (tar);
	rts2db::EllTarget *ell = dynamic_cast <rts2db::EllTarget *> (tar);

	if ((upd.ra || upd.dec || upd.pmRa || upd.pmDec) && !ct)
	{
		delete tar;
		throw rts2core::Error ("ra/dec/pmRa/pmDec only apply to targets with a directly stored equatorial position - this target's position is computed from other data");
	}
	if (upd.mpec && !ell)
	{
		delete tar;
		throw rts2core::Error ("mpec only applies to elliptical (minor planet/comet) targets");
	}

	// Target::saveWithID() now clamps these to the column width rather
	// than overflowing (see target.ec), but silently truncating an
	// over-length value a caller actually sent is a worse API contract
	// than telling them clearly - reject it here instead.
	if (upd.name && upd.name->length () > 150)
	{
		delete tar;
		throw rts2core::Error ("name is too long (max 150 characters)");
	}
	if (upd.comment && upd.comment->length () > 2000)
	{
		delete tar;
		throw rts2core::Error ("comment is too long (max 2000 characters)");
	}

	if (upd.name)
		tar->setTargetName (upd.name->c_str ());
	if (upd.comment)
		tar->setTargetComment (upd.comment->c_str ());
	if (upd.priority)
		tar->setTargetPriority (*upd.priority);
	if (upd.bonus)
		tar->setTargetBonus (*upd.bonus);
	if (upd.enabled)
		tar->setTargetEnabled (*upd.enabled, false);
	if (upd.interruptible)
		tar->setInterruptible (*upd.interruptible);

	if (upd.mpec)
	{
		// orbitFromMPC() also derives and sets name/info/type from the
		// parsed line - apply it before any explicit name override
		// above so an explicit name (if the caller also sent one) wins.
		if (ell->orbitFromMPC (upd.mpec->c_str ()))
		{
			delete tar;
			throw rts2core::Error (std::string ("cannot parse mpec as an MPC minor-planet or comet one-line element: ") + *upd.mpec);
		}
		if (upd.name)
			tar->setTargetName (upd.name->c_str ());
	}
	else if (upd.info)
	{
		tar->setTargetInfo (*upd.info);
	}

	if (ct && (upd.ra || upd.dec))
	{
		struct ln_equ_posn rawPos;
		ct->getRawPosition (&rawPos);
		ct->setPosition (upd.ra ? *upd.ra : rawPos.ra, upd.dec ? *upd.dec : rawPos.dec);
	}
	if (ct && (upd.pmRa || upd.pmDec))
	{
		struct ln_equ_posn rawPm;
		ct->getProperMotion (&rawPm);
		ct->setProperMotion (upd.pmRa ? *upd.pmRa : rawPm.ra, upd.pmDec ? *upd.pmDec : rawPm.dec);
	}

	int ret = tar->save (true);
	delete tar;
	if (ret)
		throw rts2core::Error ("failed to save target - see the daemon log for details");

	// Re-load fresh from the DB rather than trusting the in-memory
	// object post-save, so the response genuinely reflects what's
	// stored (e.g. confirms an mpec-derived name/type actually
	// persisted) instead of assuming save() did exactly what was asked.
	rts2db::Target *fresh = createTarget (targetId, rts2core::Configuration::instance ()->getObserver (), rts2core::Configuration::instance ()->getObservatoryAltitude ());
	writeTargetDetail (fresh, os);
	delete fresh;
}

void rts2web::dbGetScheduling (int targetId, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	// Same existence check as dbListObservations() - a bad target id
	// should be a 400, not a misleading empty-string sinfo.
	rts2db::Target *tar = createTarget (targetId, rts2core::Configuration::instance ()->getObserver (), rts2core::Configuration::instance ()->getObservatoryAltitude ());
	delete tar;

	std::string sinfo = rts2db::Scheduling::getSinfo (targetId);
	os << "{\"tarId\":" << targetId << ",\"sinfo\":";
	jsonString (sinfo.c_str (), os);
	os << "}";
}

void rts2web::dbSaveScheduling (int targetId, const std::string &sinfo, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	rts2db::Target *tar = createTarget (targetId, rts2core::Configuration::instance ()->getObserver (), rts2core::Configuration::instance ()->getObservatoryAltitude ());
	delete tar;

	rts2db::Scheduling::setSinfo (targetId, sinfo);

	os << "{\"tarId\":" << targetId << ",\"sinfo\":";
	jsonString (sinfo.c_str (), os);
	os << "}";
}

void rts2web::dbListObservations (int targetId, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

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
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

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
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

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
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

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
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	time_t from;
	int64_t duration;
	getNightDuration (year, month, day, from, duration);
	time_t end = from + duration;

	rts2db::ImageSetDate is (from, end);
	is.load ();
	writeImageSetJson (is, imagesDir, os);
}

#endif // WEB_HAVE_DB
