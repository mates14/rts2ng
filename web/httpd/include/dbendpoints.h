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

/**
 * Fields accepted by dbUpdateTarget() - a pointer member is non-null iff
 * the corresponding request parameter was actually present, so a save
 * only touches what the caller sent (the frontend only ever sends the
 * fields its form for the target's current type shows) rather than
 * clobbering everything else with defaults.
 *
 * Deliberately no `type` field yet - changing a target's type_id after
 * creation would mean re-instantiating a different rts2db::Target
 * subclass with its own type-specific save semantics (e.g. turning a
 * plain ConstTarget into an EllTarget), not just writing a column. Not
 * supported by this first write-path pass; a target's type is fixed at
 * creation.
 */
struct TargetUpdate
{
	const std::string *name = nullptr;
	const std::string *comment = nullptr;
	// Freeform tar_info edit - only meaningful for target types that
	// don't interpret tar_info themselves (i.e. not TYPE_ELLIPTICAL,
	// which uses `mpec` below instead). Ignored if `mpec` is also set.
	const std::string *info = nullptr;
	// MPC one-line orbital element string - TYPE_ELLIPTICAL only. Parsed
	// via EllTarget::orbitFromMPC(), which also derives the target's
	// name from the designation it parses out - rejected (throws) if it
	// doesn't parse as either an MPC minor-planet or comet line.
	const std::string *mpec = nullptr;
	const float *priority = nullptr;
	const float *bonus = nullptr;
	const bool *enabled = nullptr;
	const bool *interruptible = nullptr;
	// Equatorial coordinates (degrees) - only applicable to targets
	// backed by rts2db::ConstTarget (plain equatorial targets; also
	// TYPE_TERESTIAL, which is a legacy fixed-RA/Dec type despite its
	// name - see STATUS.md's target-editor design note). Rejected for
	// any other target type (e.g. EllTarget, whose position is derived
	// from its orbit, not stored directly).
	const double *ra = nullptr;
	const double *dec = nullptr;
	// Proper motion, arcdeg/year - same ConstTarget-only restriction as ra/dec.
	const double *pmRa = nullptr;
	const double *pmDec = nullptr;
};

/**
 * PUT-style partial update of an existing target's editable fields (see
 * TargetUpdate for what's supported and why). Loads the target via
 * createTarget() (so it round-trips through the correct rts2db::Target
 * subclass for its existing type), applies only the fields present in
 * upd, saves, then writes the fresh post-save detail to os in the same
 * shape as dbGetTarget(). Throws rts2core::Error if the target doesn't
 * exist, if a field is set that doesn't apply to this target's type, if
 * an `mpec` value fails to parse, or if the save itself fails.
 */
void dbUpdateTarget (int targetId, const TargetUpdate &upd, std::ostringstream &os);

/**
 * GET /api/db/scheduling?id=N - the target's scheduling.sinfo string
 * ("" if the target has no scheduling row yet - not an error, that's
 * the common case for a target nobody has configured scheduling
 * parameters for). Throws rts2core::Error if the target itself doesn't
 * exist (same existence check as dbListObservations()).
 */
void dbGetScheduling (int targetId, std::ostringstream &os);

/**
 * POST-style upsert of scheduling.sinfo for a target (the free-text
 * key=value scheduling parameter bag - duration=/mag=/snr=/filters=/
 * count=/pscale=/type=and, per this session's investigation of the
 * user's sch/ scheduler scripts). Confirms the target exists first, same
 * as dbGetScheduling(). Written as delete-then-insert inside one
 * transaction rather than a plain UPDATE-or-INSERT: real production
 * (lascaux)'s scheduling table predates this code and has no unique/
 * primary key on tar_id (added here only for fresh rts2ng installs, see
 * db/sql/update/rel_1_0_2.sql), so this is written to also be correct -
 * collapsing to exactly one row - against that already-deployed,
 * unconstrained shape.
 */
void dbSaveScheduling (int targetId, const std::string &sinfo, std::ostringstream &os);

/**
 * GET /api/db/scripts?id=N - every camera that has a script override
 * stored for this target: {"tarId":N,"scripts":{"C0":"...",...}}. A
 * camera absent from the map is using its device's configured default
 * (rts2.ini's per-camera "script" setting) - see rts2db::Target::
 * getScript()'s doc comment. Throws rts2core::Error if the target
 * itself doesn't exist.
 */
void dbListScripts (int targetId, std::ostringstream &os);

/**
 * POST /api/db/script-save?id=N&camera=C1&script=... - set (upsert)
 * the target's script override for one camera.
 */
void dbSaveScript (int targetId, const std::string &camera, const std::string &script, std::ostringstream &os);

/**
 * POST /api/db/script-delete?id=N&camera=C1 - clear the target's
 * script override for one camera, reverting it to the device's
 * configured default. Not an error if there was no override.
 */
void dbDeleteScript (int targetId, const std::string &camera, std::ostringstream &os);

/** GET /api/db/observations?id=N - every observation of target id
 * (empty array, not an error, if the target exists but was never
 * observed - the common case on a fresh test DB). Throws
 * rts2core::Error if the target itself doesn't exist. */
void dbListObservations (int targetId, std::ostringstream &os);

/**
 * GET /api/db/current-night - {"year":Y,"month":M,"day":D} for "tonight"
 * (Configuration::getNight(), astronomical-night boundary from the
 * current time - no DB access at all). Exists so the frontend's default
 * landing view can jump straight to dbNightDetail()/image search for
 * tonight without first paying for dbNightsSummary()'s unbounded
 * all-time aggregate over every observation/image in the database -
 * found necessary live: that full-table aggregate was what made the
 * dashboard's initial load so slow against lascaux's real archive.
 * Answered synchronously inline in handleDb(), not via the worker pool
 * like the other db endpoints - there's no DB query to block on.
 */
void dbCurrentNight (std::ostringstream &os);

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
