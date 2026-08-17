#pragma once

// Only meaningful (and only compiled) when WEB_HAVE_DB is set - see
// web/CMakeLists.txt's WEB_WITH_DB option and STATUS.md task 7.

#ifdef WEB_HAVE_DB

#include <sstream>

namespace rts2web
{

/**
 * STATUS.md task 7: DB-bound endpoints (target/observation history) via
 * ../db's already-ported rts2db (TargetSet/Target/ObservationSet) -
 * genuinely reused, not reimplemented as raw SQL. Each function writes a
 * JSON body to os; a target/observation that doesn't exist is reported
 * by throwing rts2core::Error, caught alongside ApiError in httpd.cpp's
 * handleRequest() (same 400-with-JSON-error-body treatment).
 *
 * Deliberately narrow for this first pass, matching what the local test
 * database can actually exercise meaningfully (real targets, no
 * observations/images yet): target listing/lookup and per-target
 * observation history. Night reports and image search are real classic
 * features but need data this test DB doesn't have to test against -
 * left for a follow-up rather than written blind.
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

}

#endif // WEB_HAVE_DB
