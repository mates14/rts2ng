/*
 * Executor queue.
 * Copyright (C) 2010,2011      Petr Kubanek, Institute of Physics <kubanek@fzu.cz>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#pragma once

#include "rts2db/devicedb.h"
#include "rts2db/queues.h"
#include "rts2db/target.h"

namespace rts2plan
{

typedef enum { REMOVED_TIMES_EXPIRED = -1, REMOVED_STARTED = 1, REMOVED_NEXT_NEEDED = 2 } removed_t;

/**
 * Target queue entry information.
 *
 * @author Petr Kubanek <kubanek@fzu.cz>
 */
class QueuedTarget:public rts2db::QueueEntry
{
	public:
		QueuedTarget (unsigned int queue_id, rts2db::Target * _target, double _t_start = NAN, double _t_end = NAN, bool persistent = true);

		QueuedTarget (unsigned int queue_id, unsigned int qid, struct ln_lnlat_posn *observer, double obs_altitude);

		/**
		 * Copy constructor.
		 */
		QueuedTarget (const QueuedTarget &qt):rts2db::QueueEntry (qt)
		{
			target = qt.target;
			unobservable_reported = qt.unobservable_reported;
		}

		QueuedTarget (const QueuedTarget &qt, rts2db::Target *_target):rts2db::QueueEntry (qt)
		{
			target = _target;
			unobservable_reported = false;
		}

		~QueuedTarget () {}

		/**
		 * Return true if target observation times does not expired - e.g. t_start is nan or t_start <= now.
		 */
		bool notExpired (double now);

		rts2db::Target *target;

		bool unobservable_reported;
};

/**
 * Executor queue - a plain FIFO list of QueuedTarget entries, the
 * "current + next" state rts2-executor-lite tracks.
 *
 * db note: classic (and the initial faithful port) had this split into
 * an abstract `TargetQueue` base plus `ExecutorQueue`/`SimulQueueTargets`
 * subclasses, supporting 6 pluggable sort modes (FIFO/CIRCULAR/HIGHEST/
 * WESTEAST/WESTEAST_MERIDIAN/OUT_OF_LIMITS), repeat/requeue scheduling
 * (rep_n/rep_separation), a wire-editable multi-queue apparatus
 * (Queues/activeQueue - only one queue, "next", was ever actually
 * created), and direct wire-protocol queue editing (addFirst/moveIndex/
 * updateIndexTimes/queueFromConn/queueFromConnQids - none of which
 * executor.cpp ever called; they existed for the classic C++ selector,
 * which is dead). All of that scheduling intelligence has been removed
 * per an explicit user decision (see the rts2_executor_lite_plan memory):
 * it belongs to the external Python queuer, not the execution engine.
 * What remains here is exactly what the name says - a queue that holds
 * targets in the order they were added, filters out expired/unobservable
 * ones, and reports the front entry as "next". `selectNextObservation()`/
 * `EVENT_NEXT_START`/`EVENT_NEXT_END` and `getMaximalDuration()` (and the
 * `rts2script::getMaximalScriptDuration()` extension it was the only
 * caller of, see scriptduration.h) were also dropped in this pass - they
 * turned out (grep-verified) to be dead code already, called only by the
 * classic C++ selector's simulation path, never by rts2-executor itself.
 * Per-queue DB persistence (the `queue_id >= 0` / `rts2db::Queue` load-
 * or-create branch) was dropped too: the only `ExecutorQueue` ever
 * constructed uses `queue_id = -1` (a "virtual", non-DB-backed queue), so
 * that branch was already unreachable dead code.
 *
 * `isAboveHorizon()` also no longer checks per-target soft `Constraints`
 * (Moon distance, airmass, etc.) - only the hard physical horizon. See the
 * comment on that method for the reasoning.
 *
 * @author Petr Kubanek <kubanek@fzu.cz>
 */
class ExecutorQueue:public std::list <QueuedTarget>
{
	public:
		ExecutorQueue (rts2db::DeviceDb *_master, const char *name, struct ln_lnlat_posn **_observer, double _altitude, int queue_id);
		~ExecutorQueue ();

		/**
		 * Add new queue entry on front of the queue.
		 *
		 * @return Added queue ID.
		 */
		int addFront (rts2db::Target *nt, double t_start = NAN, double t_end = NAN);

		/**
		 * Add new queue entry at the back of the queue.
		 *
		 * @return Added queue ID.
		 */
		int addTarget (rts2db::Target *nt, double t_start = NAN, double t_end = NAN, bool persistent = true);

		/**
		 * Remove entry with given index from the queue.
		 */
		int removeIndex (int index);

		/**
		 * Do not delete pointer to this target, as it is used somewhere else.
		 */
		void setCurrentTarget (rts2db::Target *ct) { currentTarget = ct; }

		void clearNext ();

		/**
		 * Find target represented by given class.
		 */
		const ExecutorQueue::iterator findTarget (rts2db::Target *tar);

		/**
		 * Find the first target in the queue by target ID.
		 */
		const ExecutorQueue::iterator findTarget (int tar_id);

		/**
		 * Filter expired/unobservable entries, and put the queue in an order
		 * where front() is the target that should be observed next. Must be
		 * called (via this or filter()) before reading front() to decide the
		 * next target.
		 *
		 * @param now  time (seconds from 1/1/1970) when observations should start
		 */
		void beforeChange (double now);

		/**
		 * Runs queue filter, remove expired observations.
		 *
		 * @return true if front queue target can be observed, false otherwise
		 */
		bool filter (double now);

		/**
		 * Load target queue from database.
		 *
		 * @param queue_id     ID of queue (its number).
		 */
		void load (int queue_id);

		/**
		 * Update values from the target list. Must be called after queue content changed.
		 */
		void updateVals ();

		friend std::ostream & operator << (std::ostream &os, const ExecutorQueue *eq)
		{
			os << "enabled " << eq->queueEnabled->getValueBool () << " contains";
			for (ExecutorQueue::const_iterator iter = eq->begin (); iter != eq->end (); iter++)
				os << " " << iter->target->getTargetID () << "(" << LibnovaDateDouble (iter->t_start) << " to " << LibnovaDateDouble (iter->t_end) << ")";
			return os;
		}

		const rts2core::ValueBool * getEnabledValue () { return queueEnabled; }

	private:
		rts2db::DeviceDb *master;
		struct ln_lnlat_posn **observer;
		double obs_altitude;

		bool isAboveHorizon (QueuedTarget &tar, double &JD);

		/*
		 * Remove observation requests which expired. Expired requests are:
		 *  - request with set start_time or end_time, which expired (is in past)
		 *  - any request with end time in past
		 *  - requests with started observations
		 */
		void filterExpired (double now);

		/*
		 * Put currently-unobservable (below horizon or violating constraints)
		 * entries on the back of the queue instead of the front.
		 */
		void filterUnobservable (double now, std::list <QueuedTarget> &skipped);

		// remove target with debug entry why it was removed from the queue
		ExecutorQueue::iterator removeEntry (ExecutorQueue::iterator &iter, const removed_t reason);

		// find target with given index
		ExecutorQueue::iterator findIndex (int index);

		rts2core::IntegerArray *nextIds;
		rts2core::StringArray *nextNames;
		rts2core::TimeArray *nextStartTimes;
		rts2core::TimeArray *nextEndTimes;
		rts2core::IntegerArray *queueEntry;

		rts2core::IntegerArray *removedIds;
		rts2core::StringArray *removedNames;
		rts2core::TimeArray *removedTimes;
		rts2core::IntegerArray *removedWhy;
		rts2core::IntegerArray *removedQueueEntry;

		rts2core::IntegerArray *executedIds;
		rts2core::StringArray *executedNames;
		rts2core::TimeArray *executedTimes;
		rts2core::IntegerArray *executedQueueEntry;

		rts2core::ValueBool *queueEnabled;

		int queue_id;

		std::string queue_name;

		rts2db::Target *currentTarget;
};

}
