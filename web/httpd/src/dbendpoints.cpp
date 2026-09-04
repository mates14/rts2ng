#include "dbendpoints.h"

#ifdef WEB_HAVE_DB

#include "jsonvalue.h"

#include "rts2db/target.h"
#include "rts2db/targetell.h"
#include "rts2db/observationset.h"
#include "rts2db/imageset.h"
#include "rts2db/scheduling.h"
#include "rts2db/targetscripts.h"
#include "rts2db/records.h"
#include "objectcheck.h"
#include "riseset.h"
#include "status.h"
#include "timestamp.h"
#include "configuration.h"

#include <libnova/libnova.h>
#include <ctime>
#include <map>
#include <mutex>
#include <vector>

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
	if (upd.info && ell)
	{
		// A plain `info` on an elliptical target would call
		// setTargetInfo() directly, bypassing orbitFromMPC() entirely -
		// silently decoupling the stored text from the orbit/name/type
		// it's supposed to drive. Must go through `mpec` instead.
		delete tar;
		throw rts2core::Error ("info cannot be set directly on an elliptical target - its tar_info is the MPC orbital line, set via mpec instead");
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

void rts2web::dbNewTargetId (int after, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	int id = rts2db::newTargetId (after);
	os << "{\"id\":" << id << "}";
}

void rts2web::dbCreateTarget (int targetId, const std::string &type, const TargetUpdate &upd, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	bool elliptical = (type == "elliptical");

	if (upd.name && upd.name->length () > 150)
		throw rts2core::Error ("name is too long (max 150 characters)");
	if (upd.comment && upd.comment->length () > 2000)
		throw rts2core::Error ("comment is too long (max 2000 characters)");
	if (upd.info && elliptical)
		throw rts2core::Error ("info cannot be set directly on an elliptical target - its tar_info is the MPC orbital line, set via mpec instead");

	rts2db::Target *tar;
	if (elliptical)
	{
		if (!upd.mpec)
			throw rts2core::Error ("mpec is required to create an elliptical target");
		rts2db::EllTarget *ell = new rts2db::EllTarget ();
		// orbitFromMPC() also derives and sets name/info/type from the
		// parsed line - same as dbUpdateTarget()'s mpec handling.
		if (ell->orbitFromMPC (upd.mpec->c_str ()))
		{
			delete ell;
			throw rts2core::Error (std::string ("cannot parse mpec as an MPC minor-planet or comet one-line element: ") + *upd.mpec);
		}
		tar = ell;
	}
	else
	{
		if (!upd.ra || !upd.dec)
		{
			throw rts2core::Error ("ra and dec are required to create an equatorial target");
		}
		rts2db::ConstTarget *ct = new rts2db::ConstTarget ();
		// TYPE_OPORTUNITY ('O') - the generic "just observe this" type
		// already used elsewhere for ad-hoc science targets (confirmed
		// against the user's own sch/database.py query, which explicitly
		// selects type_id='O' as its "opportunity targets" case).
		// Terrestrial/Alt-Az remains out of scope - see STATUS.md.
		ct->setTargetType (TYPE_OPORTUNITY);
		ct->setPosition (*upd.ra, *upd.dec);
		if (upd.pmRa || upd.pmDec)
			ct->setProperMotion (upd.pmRa ? *upd.pmRa : 0, upd.pmDec ? *upd.pmDec : 0);
		tar = ct;
	}

	if (upd.name)
		tar->setTargetName (upd.name->c_str ());
	if (upd.comment)
		tar->setTargetComment (upd.comment->c_str ());
	if (upd.info)
		tar->setTargetInfo (*upd.info);
	if (upd.priority)
		tar->setTargetPriority (*upd.priority);
	// A newly-created target defaults enabled unless the caller
	// explicitly said otherwise - matches Target::Target()'s own
	// tar_enabled default (true) rather than inventing a different one.
	tar->setTargetEnabled (upd.enabled ? *upd.enabled : true, false);

	int ret = tar->saveWithID (true, targetId);
	delete tar;
	if (ret)
		throw rts2core::Error ("failed to create target - see the daemon log for details");

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

void rts2web::dbListScripts (int targetId, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	// Same existence check as dbGetScheduling() - a bad target id should
	// be a 400, not a misleadingly empty script map.
	rts2db::Target *tar = createTarget (targetId, rts2core::Configuration::instance ()->getObserver (), rts2core::Configuration::instance ()->getObservatoryAltitude ());
	delete tar;

	std::map <std::string, std::string> scripts = rts2db::TargetScripts::listForTarget (targetId);

	os << "{\"tarId\":" << targetId << ",\"scripts\":{";
	bool first = true;
	for (std::map <std::string, std::string>::iterator iter = scripts.begin (); iter != scripts.end (); iter++)
	{
		if (!first)
			os << ",";
		first = false;
		jsonString (iter->first.c_str (), os);
		os << ":";
		jsonString (iter->second.c_str (), os);
	}
	os << "}}";
}

void rts2web::dbSaveScript (int targetId, const std::string &camera, const std::string &script, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	rts2db::Target *tar = createTarget (targetId, rts2core::Configuration::instance ()->getObserver (), rts2core::Configuration::instance ()->getObservatoryAltitude ());
	try
	{
		tar->setScript (camera.c_str (), script.c_str ());
	}
	catch (rts2core::Error &er)
	{
		delete tar;
		throw;
	}
	delete tar;

	os << "{\"tarId\":" << targetId << ",\"camera\":";
	jsonString (camera.c_str (), os);
	os << ",\"script\":";
	jsonString (script.c_str (), os);
	os << "}";
}

void rts2web::dbDeleteScript (int targetId, const std::string &camera, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	rts2db::Target *tar = createTarget (targetId, rts2core::Configuration::instance ()->getObserver (), rts2core::Configuration::instance ()->getObservatoryAltitude ());
	try
	{
		tar->deleteScript (camera.c_str ());
	}
	catch (rts2core::Error &er)
	{
		delete tar;
		throw;
	}
	delete tar;

	os << "{\"tarId\":" << targetId << ",\"camera\":";
	jsonString (camera.c_str (), os);
	os << ",\"deleted\":true}";
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
		// Not `os << getObsStart ()`: these are ctime doubles, and the
		// stream operator's six significant digits round them to the
		// nearest ~1000 s - see jsonNumber()'s comment.
		os << "{\"id\":" << iter->getObsId () << ",\"start\":";
		jsonTime (iter->getObsStart (), os);
		os << ",\"end\":";
		jsonTime (iter->getObsEnd (), os);
		os << "}";
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
		jsonTime (iter->getObsSlew (), os);
		os << ",\"start\":";
		jsonTime (iter->getObsStart (), os);
		os << ",\"end\":";
		jsonTime (iter->getObsEnd (), os);
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
		jsonTime (img->getExposureStart (), os);
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

void rts2web::dbListRecvals (std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	rts2db::RecvalsSet recvals;
	recvals.load (true);

	os << "[";
	bool first = true;
	for (rts2db::RecvalsSet::iterator iter = recvals.begin (); iter != recvals.end (); iter++)
	{
		if (!first)
			os << ",";
		first = false;
		os << "{\"id\":" << iter->recvalId << ",\"device\":";
		jsonString (iter->device.c_str (), os);
		os << ",\"value\":";
		jsonString (iter->value.c_str (), os);
		os << ",\"type\":" << iter->valueType << ",\"from\":";
		jsonTime (iter->timeFrom, os);
		os << ",\"to\":";
		jsonTime (iter->timeTo, os);
		os << ",\"samples\":" << iter->samples << "}";
	}
	os << "]";
}

void rts2web::dbRecords (const std::string &device, const std::string &value, double from, double to, int maxPoints, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	int valueType = 0;
	int recvalId = rts2db::findRecvalId (device.c_str (), value.c_str (), &valueType);
	if (recvalId < 0)
		throw rts2core::Error ("no recorded value " + device + "." + value + " - see /api/db/recvals for what is recorded");

	std::vector <rts2db::RecordEntry> records;
	rts2db::loadRecords (recvalId, valueType, from, to, maxPoints, records);

	os << "{\"id\":" << recvalId << ",\"device\":";
	jsonString (device.c_str (), os);
	os << ",\"value\":";
	jsonString (value.c_str (), os);
	os << ",\"type\":" << valueType << ",\"from\":";
	jsonTime (from, os);
	os << ",\"to\":";
	jsonTime (to, os);
	os << ",\"points\":[";
	bool first = true;
	for (std::vector <rts2db::RecordEntry>::iterator iter = records.begin (); iter != records.end (); iter++)
	{
		if (!first)
			os << ",";
		first = false;
		os << "[";
		jsonTime (iter->t, os);
		os << ",";
		jsonNumber (iter->avg, os);
		os << ",";
		jsonNumber (iter->min, os);
		os << ",";
		jsonNumber (iter->max, os);
		os << "," << iter->n << "]";
	}
	os << "]}";
}

namespace
{

/**
 * Walks centrald's own next_event() state machine (riseset.h) forward
 * from local noon on the day containing `noon`, recording where DUSK/
 * NIGHT/MORNING begin - sunset, the night_horizon crossing in, the
 * night_horizon crossing out, and sunrise. Factored out of
 * dbTargetAltitude() so dbTargetVisibilityYear() (one call per day of a
 * year, rather than one per request) agrees with it on where night
 * starts and ends instead of keeping a second copy of the loop.
 *
 * Returns false (not an error) when no sunset/sunrise pair was found for
 * this day at this observer - a polar day/night, or a horizon
 * configuration that never crosses. Callers decide what that means: a
 * single night with none is nothing to plot (an error), a day inside a
 * year with none is just a day to skip.
 */
bool findNightBoundaries (struct ln_lnlat_posn *observer, time_t noon,
	double nightHorizon, double dayHorizon, int eveningTime, int morningTime,
	time_t &sunset, time_t &nightStart, time_t &nightEnd, time_t &sunrise)
{
	sunset = nightStart = nightEnd = sunrise = 0;
	rts2_status_t currType = -1, nextType = -1;
	time_t cursor = noon;
	for (int i = 0; i < 24 && !(sunset && sunrise); i++)
	{
		time_t probe = cursor + 1;
		time_t evTime = cursor;
		next_event (observer, &probe, &currType, &nextType, &evTime, nightHorizon, dayHorizon, eveningTime, morningTime);
		if (evTime <= cursor)
			break;					 // no progress - refuse to spin
		switch (currType)
		{
			case SERVERD_DUSK:
				if (!sunset)
					sunset = cursor;
				break;
			case SERVERD_NIGHT:
				if (!nightStart)
				{
					nightStart = cursor;
					nightEnd = evTime;
				}
				break;
			case SERVERD_MORNING:
				if (!sunrise)
					sunrise = cursor;
				break;
		}
		cursor = evTime;
	}
	return sunset != 0 && sunrise != 0 && sunrise > sunset;
}

}

void rts2web::dbTargetAltitude (int targetId, double fixedRa, double fixedDec, double refTime, int points, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	rts2core::Configuration *config = rts2core::Configuration::instance ();
	struct ln_lnlat_posn *observer = config->getObserver ();

	// The same [observatory] keys, with the same defaults, that
	// centrald's initValues() reads - not a second opinion on when night
	// is. (An operator can raise centrald's night_horizon value at
	// runtime, since it is writable there; this reads the configured
	// one, which is what a plan for a future night should be based on
	// anyway.)
	double nightHorizon = config->getDoubleDefault ("observatory", "night_horizon", -10);
	double dayHorizon = config->getDoubleDefault ("observatory", "day_horizon", 0);
	int eveningTime = config->getIntegerDefault ("observatory", "evening_time", 7200);
	int morningTime = config->getIntegerDefault ("observatory", "morning_time", 1800);

	// A night belongs to the day it starts on, so everything is anchored
	// to local noon: asking at 23:00 and at 03:00 has to describe the
	// same night, and an explicit date=YYYY-MM-DD (passed in as that
	// day's local noon) then needs no special case here.
	time_t ref = (time_t) refTime;
	struct tm tmRef;
	localtime_r (&ref, &tmRef);
	if (tmRef.tm_hour < 12)
		ref -= 86400;
	localtime_r (&ref, &tmRef);
	tmRef.tm_hour = 12;
	tmRef.tm_min = 0;
	tmRef.tm_sec = 0;
	tmRef.tm_isdst = -1;
	time_t noon = mktime (&tmRef);

	// Walk centrald's own state machine (riseset.h's next_event, via the
	// shared findNightBoundaries() above) forward from noon: DUSK starts
	// at sunset, NIGHT at the night_horizon crossing, DAWN at the end of
	// night, MORNING at sunrise.
	time_t sunset, nightStart, nightEnd, sunrise;
	if (!findNightBoundaries (observer, noon, nightHorizon, dayHorizon, eveningTime, morningTime, sunset, nightStart, nightEnd, sunrise))
		throw rts2core::Error ("cannot find a sunset/sunrise pair for this date at this observatory - a polar day or night, or a horizon configuration that never crosses");

	if (points < 10)
		points = 10;
	if (points > 2000)
		points = 2000;

	rts2db::Target *tar = nullptr;
	if (targetId >= 0)
	{
		// throws rts2db::SqlError for a nonexistent id, same as every
		// other target endpoint here
		tar = createTarget (targetId, observer, config->getObservatoryAltitude ());
	}

	ObjectCheck *checker = config->getObjectChecker ();

	std::ostringstream pts;
	bool first = true;
	try
	{
		for (int i = 0; i < points; i++)
		{
			double t = sunset + (double) (sunrise - sunset) * i / (points - 1);
			double JD = Timestamp (t).getJD ();

			struct ln_equ_posn equ;
			struct ln_hrz_posn hrz;
			if (tar)
			{
				// per-sample position, so an elliptical/GRB/planet target
				// traces its real path across the night
				tar->getPosition (&equ, JD);
				tar->getAltAz (&hrz, JD, observer);
			}
			else
			{
				equ.ra = fixedRa;
				equ.dec = fixedDec;
				ln_get_hrz_from_equ (&equ, observer, JD, &hrz);
			}

			struct ln_equ_posn moonEqu, sunEqu;
			struct ln_hrz_posn moonHrz, sunHrz;
			ln_get_lunar_equ_coords (JD, &moonEqu);
			ln_get_hrz_from_equ (&moonEqu, observer, JD, &moonHrz);
			ln_get_solar_equ_coords (JD, &sunEqu);
			ln_get_hrz_from_equ (&sunEqu, observer, JD, &sunHrz);

			// The horizon at *this moment's* azimuth - the whole point of
			// plotting it against time rather than drawing one fixed
			// limit line: a target setting into a hill is only visible
			// until it reaches that hill's altitude at that azimuth.
			double horizonAlt = checker->getHorizonHeight (&hrz, 0);

			if (!first)
				pts << ",";
			first = false;
			pts << "[";
			jsonTime (t, pts);
			pts << ",";
			jsonNumber (hrz.alt, pts);
			pts << ",";
			jsonNumber (hrz.az, pts);
			pts << ",";
			jsonNumber (horizonAlt, pts);
			pts << ",";
			jsonNumber (moonHrz.alt, pts);
			pts << ",";
			jsonNumber (ln_get_angular_separation (&equ, &moonEqu), pts);
			pts << ",";
			jsonNumber (sunHrz.alt, pts);
			pts << "]";
		}

		os << "{\"id\":" << (tar ? tar->getTargetID () : -1) << ",\"name\":";
		jsonString (tar ? tar->getTargetName () : "", os);
		os << ",\"sunset\":";
		jsonTime (sunset, os);
		os << ",\"sunrise\":";
		jsonTime (sunrise, os);
		os << ",\"nightStart\":";
		if (nightStart)
			jsonTime (nightStart, os);
		else
			os << "null";
		os << ",\"nightEnd\":";
		if (nightEnd)
			jsonTime (nightEnd, os);
		else
			os << "null";
		os << ",\"nightHorizon\":";
		jsonNumber (nightHorizon, os);
		os << ",\"dayHorizon\":";
		jsonNumber (dayHorizon, os);
		// illumination halfway through the night, which is what "how
		// bright is the Moon tonight" means for planning
		os << ",\"moonDisk\":";
		jsonNumber (ln_get_lunar_disk (Timestamp ((sunset + sunrise) / 2.0).getJD ()), os);
		os << ",\"points\":[" << pts.str () << "]}";
	}
	catch (...)
	{
		delete tar;
		throw;
	}
	delete tar;
}

void rts2web::dbTargetVisibilityYear (int targetId, double fixedRa, double fixedDec, int year, std::ostringstream &os)
{
	std::lock_guard <std::mutex> dbLock (dbAccessMutex);

	rts2core::Configuration *config = rts2core::Configuration::instance ();
	struct ln_lnlat_posn *observer = config->getObserver ();

	// Same [observatory] keys as dbTargetAltitude(), same reason: a plan
	// for the whole year is based on the configured thresholds, not
	// whatever an operator may have nudged centrald's live value to.
	double nightHorizon = config->getDoubleDefault ("observatory", "night_horizon", -10);
	double dayHorizon = config->getDoubleDefault ("observatory", "day_horizon", 0);
	int eveningTime = config->getIntegerDefault ("observatory", "evening_time", 7200);
	int morningTime = config->getIntegerDefault ("observatory", "morning_time", 1800);

	rts2db::Target *tar = nullptr;
	if (targetId >= 0)
	{
		// throws rts2db::SqlError for a nonexistent id, same as every
		// other target endpoint here
		tar = createTarget (targetId, observer, config->getObservatoryAltitude ());
	}

	ObjectCheck *checker = config->getObjectChecker ();

	// Walked one calendar day at a time via struct tm (not by adding
	// 86400 to a time_t): re-deriving each day's noon through mktime()
	// lets libc apply that day's own DST offset, so the two days a year
	// DST actually changes don't leave every following noon off by an
	// hour until the next transition corrects it back.
	struct tm tmDay;
	memset (&tmDay, 0, sizeof (tmDay));
	tmDay.tm_year = year - 1900;
	tmDay.tm_mon = 0;
	tmDay.tm_mday = 1;
	tmDay.tm_hour = 12;
	tmDay.tm_isdst = -1;

	std::ostringstream days;
	bool first = true;
	try
	{
		for (int i = 0; i < 366; i++)
		{
			time_t dayNoon = mktime (&tmDay);
			struct tm normalized;
			localtime_r (&dayNoon, &normalized);
			if (i > 0 && normalized.tm_year != year - 1900)
				break;						 // wrapped into next year - done

			// advance to the next calendar day for the following
			// iteration before anything below can `continue` past it
			tmDay = normalized;
			tmDay.tm_mday += 1;
			tmDay.tm_hour = 12;
			tmDay.tm_min = 0;
			tmDay.tm_sec = 0;
			tmDay.tm_isdst = -1;

			time_t sunset, nightStart, nightEnd, sunrise;
			if (!findNightBoundaries (observer, dayNoon, nightHorizon, dayHorizon, eveningTime, morningTime, sunset, nightStart, nightEnd, sunrise))
				continue;					 // polar day/night here on this date - just skip it

			// ~10 min resolution: dbTargetAltitude() samples one night at
			// up to 2000 points for an interactive per-pixel plot; this
			// endpoint samples every night of a year, so a coarser fixed
			// step keeps the whole response cheap without losing
			// anything a one-point-per-day plot could show anyway.
			int nSamples = (int) ((sunrise - sunset) / 600);
			if (nSamples < 10)
				nSamples = 10;

			// A circumpolar (or near-circumpolar) target sweeps through
			// every azimuth over the course of one night, so it can duck
			// behind a real horizon obstruction and reappear more than
			// once - the visible portion of a night is a set of
			// intervals, not always one contiguous "rise to set" span.
			// Collapsing it to first-up/last-up (an earlier version of
			// this endpoint did) silently painted the dip in between as
			// still visible.
			std::vector<time_t> windowStart, windowEnd;
			bool wasUp = false;
			time_t curStart = 0, lastT = sunset;
			double peakAlt = -90, peakTime = sunset;

			for (int s = 0; s <= nSamples; s++)
			{
				double t = sunset + (double) (sunrise - sunset) * s / nSamples;
				double JD = Timestamp (t).getJD ();

				struct ln_equ_posn equ;
				struct ln_hrz_posn hrz;
				if (tar)
				{
					tar->getPosition (&equ, JD);
					tar->getAltAz (&hrz, JD, observer);
				}
				else
				{
					equ.ra = fixedRa;
					equ.dec = fixedDec;
					ln_get_hrz_from_equ (&equ, observer, JD, &hrz);
				}

				if (hrz.alt > peakAlt)
				{
					peakAlt = hrz.alt;
					peakTime = t;
				}

				// the horizon at *this moment's* azimuth, same reason as
				// dbTargetAltitude(): a target the telescope loses behind
				// a hill isn't "up" just because it is above 0 degrees
				bool up = hrz.alt > checker->getHorizonHeight (&hrz, 0);
				if (up && !wasUp)
					curStart = (time_t) t;
				else if (!up && wasUp)
				{
					windowStart.push_back (curStart);
					windowEnd.push_back ((time_t) t);
				}
				wasUp = up;
				lastT = (time_t) t;
			}
			if (wasUp)
			{
				windowStart.push_back (curStart);
				windowEnd.push_back (lastT);
			}

			if (!first)
				days << ",";
			first = false;
			days << "[";
			jsonTime (dayNoon, days);
			days << ",";
			jsonTime (sunset, days);
			days << ",";
			if (nightStart)
				jsonTime (nightStart, days);
			else
				days << "null";
			days << ",";
			if (nightEnd)
				jsonTime (nightEnd, days);
			else
				days << "null";
			days << ",";
			jsonTime (sunrise, days);
			days << ",";
			jsonNumber (peakAlt, days);
			days << ",";
			jsonTime (peakTime, days);
			days << ",[";
			for (size_t w = 0; w < windowStart.size (); w++)
			{
				if (w)
					days << ",";
				days << "[";
				jsonTime (windowStart[w], days);
				days << ",";
				jsonTime (windowEnd[w], days);
				days << "]";
			}
			days << "]]";
		}

		os << "{\"id\":" << (tar ? tar->getTargetID () : -1) << ",\"name\":";
		jsonString (tar ? tar->getTargetName () : "", os);
		os << ",\"year\":" << year << ",\"days\":[" << days.str () << "]}";
	}
	catch (...)
	{
		delete tar;
		throw;
	}
	delete tar;
}

#endif // WEB_HAVE_DB
