/*
 * Target overview - one row per target with what a listing needs to
 * compare targets against each other (enabled, priority, scheduling
 * sinfo, observation count and last observation), loaded by a single
 * joined query.
 *
 * Deliberately not built on TargetSet: that instantiates a full
 * rts2db::Target subclass per row and would need a second query per
 * target for sinfo and observations - fine for a handful, but production
 * has ~16k targets and the web target list wants all of them at once.
 * Position is the stored tar_ra/tar_dec as is (NAN for targets that have
 * none, e.g. elliptical ones), not a computed current position.
 */

#pragma once

#include <string>
#include <vector>

namespace rts2db
{

class TargetSummary
{
	public:
		int id;
		char type;
		std::string name;
		std::string comment;		 // first 200 characters only
		double ra;					 // NAN when not stored
		double dec;					 // NAN when not stored
		bool enabled;
		int priority;
		bool hasPriority;
		std::string sinfo;			 // "" when the target has no scheduling row
		long obsCount;
		double lastObs;				 // max(obs_start), Unix time; NAN when never observed
};

class TargetSummarySet:public std::vector <TargetSummary>
{
	public:
		/**
		 * Load every target, ordered by tar_id.
		 *
		 * @throw rts2core::SqlError on failure.
		 */
		void load ();
};

}
