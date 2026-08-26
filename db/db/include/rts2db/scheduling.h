/*
 * Scheduling table support - a free-text key=value parameter bag per
 * target (duration=/mag=/snr=/filters=/count=/pscale=/type=...) used by
 * the site's separate Python scheduler (sch/database.py's parse_sinfo()),
 * not by anything in rts2ng's own C++ scheduler/executor.
 *
 * `type=` is the compound-reservation keyword: sch/kernel/reservation.py's
 * CompoundReservation supports 'single' (implicit default - one
 * telescope), 'oneof' (implicit when the target is enabled at more than
 * one telescope with no type= override - "any telescope"), 'and' (every
 * telescope that has this target enabled must observe it, no timing
 * constraint between them - already read by sch/request.py's
 * has_and_type() and acted on by scheduler_core.py), and 'sim' (same as
 * 'and' but starting together - the solver already supports it, but as
 * of this investigation (2026-08-26) nothing in sch/ reads a sinfo
 * keyword for it, i.e. the site's own "simultaneous at both telescopes"
 * feature isn't wired end to end yet). This editor lets sinfo's `type`
 * value be set to whatever the operator wants, including `sim`, but
 * making it actually take effect needs a matching change in sch/
 * (request.py + scheduler_core.py) that's out of this pass's scope - see
 * STATUS.md.
 *
 * db note: the `scheduling` table (tar_id, sinfo) predates this port -
 * it was added directly to production SQL (lascaux/d50) some time after
 * the scheduling.py tooling was written, never through a versioned
 * rts2ng migration, and rts2db had no code for it at all until now (see
 * db/sql/update/rel_1_0_2.sql for the migration that gives fresh rts2ng
 * installs the table; production already has it, just without a
 * primary/unique key on tar_id - see Scheduling::setSinfo()'s comment
 * for how that's handled).
 */

#pragma once

#include <string>

namespace rts2db
{

class Scheduling
{
	public:
		/**
		 * Return the target's scheduling.sinfo string, or "" if it has
		 * no scheduling row yet (not an error - the common case for a
		 * target nobody has set scheduling parameters for).
		 *
		 * @throw rts2core::SqlError on a DB-layer failure other than
		 * "no row" (e.g. connection down).
		 */
		static std::string getSinfo (int tar_id);

		/**
		 * Upsert the target's scheduling.sinfo string.
		 *
		 * @throw rts2core::SqlError on failure.
		 */
		static void setSinfo (int tar_id, const std::string &sinfo);
};

}
