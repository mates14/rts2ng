#pragma once

// Only meaningful (and only compiled) when WEB_HAVE_DB is set - see
// web/CMakeLists.txt's WEB_WITH_DB option and STATUS.md task 7.

#ifdef WEB_HAVE_DB

#include <sstream>
#include <string>

namespace rts2web
{

/**
 * STATUS.md task 7: DB-bound endpoints (target/observation history,
 * night reports, image search) via ../db's already-ported rts2db
 * (TargetSet/Target/ObservationSet/ObservationSetDate/ImageSet) -
 * genuinely reused, not reimplemented as raw SQL. Each function writes a
 * JSON body to os; a target/observation that doesn't exist, or any other
 * DB-layer failure, is reported by throwing rts2core::Error - caught
 * inside the worker job that calls it (see httpd.cpp's handleDb()),
 * since as of 2026-08-17 every one of these runs on the worker pool, not
 * inline on the main thread (found necessary, not just cautious, by
 * live-testing the target-listing endpoints against a real 15,753-row
 * production database - see STATUS.md).
 *
 * Night reports and image search were deferred out of the first task-7
 * pass because the local test database had no observations/images to
 * test them against meaningfully - both now exist, using real data
 * confirmed present in this session's local test DB and (for the
 * concrete path-handling questions) reasoned through against how
 * rts2db::ImageSet actually stores/returns file paths, not guessed.
 */

/** GET /api/db/targets - every target: id, name, type, current ra/dec. */
void dbListTargets (std::ostringstream &os);

/** GET /api/db/target?id=N - one target's full detail. Throws
 * rts2core::Error if id doesn't exist. */
void dbGetTarget (int targetId, std::ostringstream &os);

/** GET /api/db/observations?id=N - every observation of target id
 * (empty array, not an error, if the target exists but was never
 * observed - the common case on a fresh test DB). Throws
 * rts2core::Error if the target itself doesn't exist. */
void dbListObservations (int targetId, std::ostringstream &os);

/**
 * GET /api/db/nights?year=&month=&day= - aggregated observation/image
 * counts, grouped by whichever date component is left unspecified
 * (a hierarchical year -> month -> day drill-down, mirroring classic's
 * rts2db::ObservationSetDate model exactly). Any of year/month/day may
 * be omitted (pass -1) to browse at that level.
 */
void dbNightsSummary (int year, int month, int day, std::ostringstream &os);

/**
 * GET /api/db/night?year=&month=&day= - every observation during one
 * specific night (astronomical-night boundaries, not the calendar day -
 * see getNightDuration() in the .cpp). All three parameters required.
 */
void dbNightDetail (int year, int month, int day, std::ostringstream &os);

/**
 * GET /api/db/images?target=N - every archived image of target N.
 * imagesDir is HttpD's --images-dir (task 3) - used to additionally
 * compute a "previewPath" relative to it (directly usable with
 * GET /preview/<previewPath>) whenever the DB's stored absolute path
 * actually falls under imagesDir; left as an empty string otherwise
 * (e.g. imagesDir not configured, or the site's archive lives somewhere
 * else) rather than guessed at.
 */
void dbSearchImagesByTarget (const std::string &imagesDir, int targetId, std::ostringstream &os);

/**
 * GET /api/db/images?year=&month=&day= - every archived image from one
 * specific night (same night-boundary computation as dbNightDetail()).
 * All three parameters required. See dbSearchImagesByTarget() for what
 * imagesDir/previewPath mean.
 */
void dbSearchImagesByNight (const std::string &imagesDir, int year, int month, int day, std::ostringstream &os);

}

#endif // WEB_HAVE_DB
