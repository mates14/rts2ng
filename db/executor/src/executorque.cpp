/*
 * Executor queue.
 * Copyright (C) 2010,2011     Petr Kubanek, Institute of Physics <kubanek@fzu.cz>
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

#include "rts2script/executorque.h"
#include "rts2db/sqlerror.h"

using namespace rts2plan;

QueuedTarget::QueuedTarget (unsigned int _queue_id, rts2db::Target * _target, double _t_start, double _t_end, bool _persistent):QueueEntry (0, _queue_id)
{
	target = _target;

	qid = nextQid ();
	t_start = _t_start;
	t_end = _t_end;
	tar_id = target->getTargetID ();

	unobservable_reported = false;

	create ();
}

QueuedTarget::QueuedTarget (unsigned int _queue_id, unsigned int _qid, struct ln_lnlat_posn *observer, double obs_altitude):QueueEntry (_qid, _queue_id)
{
	load ();
	target = createTarget (tar_id, observer, obs_altitude);
}

bool QueuedTarget::notExpired (double now)
{
	return (std::isnan (t_start) || t_start <= now) && (std::isnan (t_end) || t_end > now);
}

ExecutorQueue::ExecutorQueue (rts2db::DeviceDb *_master, const char *name, struct ln_lnlat_posn **_observer, double _altitude, int _queue_id):std::list <QueuedTarget> ()
{
	observer = _observer;
	obs_altitude = _altitude;
	master = _master;
	currentTarget = NULL;
	queue_id = _queue_id;
	queue_name = std::string (name);

	std::string sn (name);

	master->createValue (nextIds, (sn + "_ids").c_str (), "next queue IDs", false);
	master->createValue (nextNames, (sn + "_names").c_str (), "next queue names", false);
	master->createValue (nextStartTimes, (sn + "_start").c_str (), "times of element execution", false);
	master->createValue (nextEndTimes, (sn + "_end").c_str (), "times of element execution", false);
	master->createValue (queueEntry, (sn + "_qid").c_str (), "private queue ID", false);

	master->createValue (removedIds, (sn + "_removed_ids").c_str (), "removed observation IDS", false);
	master->createValue (removedNames, (sn + "_removed_names").c_str (), "names of removed IDS", false);
	master->createValue (removedTimes, (sn + "_removed_times").c_str (), "times when target was removed", false);
	master->createValue (removedWhy, (sn + "_removed_why").c_str (), "why target was removed", false);
	master->createValue (removedQueueEntry, (sn + "_removed_qid").c_str (), "queue entry of removed target", false);

	master->createValue (executedIds, (sn + "_executed_ids").c_str (), "ID of executed targets", false);
	master->createValue (executedNames, (sn + "_executed_names").c_str (), "executed targets names", false);
	master->createValue (executedTimes, (sn + "_executed_times").c_str (), "time when target was executed", false);
	master->createValue (executedQueueEntry, (sn + "_executed_qid").c_str (), "queue entry of executed target", false);

	master->createValue (queueEnabled, (sn + "_enabled").c_str (), "enable queue for selection", false, RTS2_VALUE_WRITABLE);
	queueEnabled->setValueBool (true);
}

ExecutorQueue::~ExecutorQueue ()
{
	currentTarget = NULL;
	for (ExecutorQueue::iterator iter = begin (); iter != end (); iter++)
	{
		delete iter->target;
	}
}

int ExecutorQueue::addFront (rts2db::Target *nt, double t_start, double t_end)
{
	QueuedTarget qt (queue_id, nt, t_start, t_end);
	push_front (qt);
	updateVals ();
	return qt.qid;
}

int ExecutorQueue::addTarget (rts2db::Target *nt, double t_start, double t_end, bool persistent)
{
	QueuedTarget qt (queue_id, nt, t_start, t_end, persistent);
	push_back (qt);
	updateVals ();
	return qt.qid;
}

int ExecutorQueue::removeIndex (int index)
{
	ExecutorQueue::iterator iter = findIndex (index);
	if (iter == end ())
		return -1;

	if (iter->target != currentTarget)
		delete iter->target;
	else
		currentTarget = NULL;
	iter->remove ();
	erase (iter);
	updateVals ();
	return 0;
}

void ExecutorQueue::clearNext ()
{
	for (ExecutorQueue::iterator iter = begin (); iter != end (); iter++)
	{
		// do not delete current target
		if (iter->target == currentTarget)
			iter->target = NULL;
		else
			delete iter->target;
		// remove entry from database
		iter->remove ();
	}
	clear ();
	updateVals ();
}

const ExecutorQueue::iterator ExecutorQueue::findTarget (rts2db::Target *tar)
{
	for (ExecutorQueue::iterator iter = begin (); iter != end (); iter++)
	{
		if (iter->target == tar)
			return iter;
	}
	return end ();
}

const ExecutorQueue::iterator ExecutorQueue::findTarget (int tar_id)
{
	for (ExecutorQueue::iterator iter = begin (); iter != end (); iter++)
	{
		if (iter->target->getTargetID () == tar_id)
			return iter;
	}
	return end ();
}

void ExecutorQueue::beforeChange (double now)
{
	filter (now);
}

bool ExecutorQueue::filter (double now)
{
	filterExpired (now);

	bool ret = false;
	std::list <QueuedTarget> skipped;
	filterUnobservable (now, skipped);
	ExecutorQueue::iterator it = begin ();
	if (!empty () && (std::isnan (front ().t_start) || front ().t_start <= now))
	{
		ret = true;
		it++;
	}
	insert (it, skipped.begin (), skipped.end ());
	updateVals ();
	// if front target was not skipped, it can be observed
	return ret;
}

void ExecutorQueue::load (int _queue_id)
{
	std::list <unsigned int> qids = rts2db::queueQids (_queue_id);

	for (std::list <unsigned int>::iterator iter = qids.begin (); iter != qids.end (); iter++)
		push_back (QueuedTarget (_queue_id, *iter, *observer, obs_altitude));

	updateVals ();
}

void ExecutorQueue::filterExpired (double now)
{
	if (empty ())
		return;
	// remove any requests which are before a request with start or end time in the past
	for (ExecutorQueue::iterator iter = begin (); iter != end (); iter++)
	{
		double t_start = iter->t_start;
		double t_end = iter->t_end;
		if ((!std::isnan (t_start) && t_start <= now) || (!std::isnan (t_end) && t_end <= now))
		{
			for (ExecutorQueue::iterator irem = begin (); irem != iter;)
			{
				logStream (MESSAGE_DEBUG) << "target " << iter->target->getTargetName () << " (" << iter->target->getTargetID () << ") has start and end times set (" << LibnovaDateDouble (t_start) << " " << LibnovaDateDouble (t_end) << ", removing previous target " << irem->target->getTargetName () << " (" << irem->target->getTargetID () << ")" << sendLog;
				irem = removeEntry (irem, REMOVED_NEXT_NEEDED);
			}
		}
	}
	// remove requests which must go, either because their end time is in the past
	// or they were observed and should be observed only once
	for (ExecutorQueue::iterator iter = begin (); iter != end ();)
	{
		double t_end = iter->t_end;
		if (!std::isnan (t_end) && t_end <= now)
			iter = removeEntry (iter, REMOVED_TIMES_EXPIRED);
		else if (iter->target->observationStarted ())
			iter = removeEntry (iter, REMOVED_STARTED);
		else
			iter++;
	}
}

void ExecutorQueue::filterUnobservable (double now, std::list <QueuedTarget> &skipped)
{
	if (!empty ())
	{
		time_t n = now;
		double JD = ln_get_julian_from_timet (&n);

		for (ExecutorQueue::iterator iter = begin (); iter != end ();)
		{
			// isAboveHorizon changes jd parameter - we would like to keep the current time
			double tjd = JD;
			if (isAboveHorizon (*iter, tjd))
			{
				iter->unobservable_reported = false;
				return;
			}

			if (!(std::isnan (iter->t_start) && std::isnan (iter->t_end)) && iter->target->observationStarted ())
			{
				logStream (MESSAGE_WARNING) << "target " << iter->target->getTargetName () << " (" << iter->target->getTargetID () << ") was observed, and as it has specified start or end times (" << LibnovaDateDouble (iter->t_start) << " to " << LibnovaDateDouble (iter->t_end) << "), it will be removed" << sendLog;
				iter->remove ();
				iter = erase (iter);
				continue;
			}

			if (iter->unobservable_reported == false)
			{
				logStream (MESSAGE_WARNING) << "Target " << iter->target->getTargetName () << " (" << iter->target->getTargetID () << ") is at " << LibnovaDate (tjd) << " unobservable, putting it on back of the queue" << sendLog;
				iter->unobservable_reported = true;
			}

			skipped.push_back (*iter);
			iter = erase (iter);
		}
	}
}

bool ExecutorQueue::isAboveHorizon (QueuedTarget &qt, double &JD)
{
	// db note: this used to also require
	// `qt.target->getViolatedConstraints (JD, violated) == 0` - i.e. the
	// per-target, file-defined soft Constraints (Moon distance, airmass,
	// etc.). Removed per an explicit user decision (2026-07-15, see the
	// rts2_executor_lite_plan memory): rejecting a target for a soft
	// constraint is a scheduling decision that belongs to the external
	// queuer, not the executor. What's left here is the hard physical
	// check only (`Target::isAboveHorizon()`, backed by the same
	// `rts2core::ObjectCheck`/horizon-file mechanism independently
	// enforced at the mount/kernel level in base/teld) - confirmed as a
	// genuine, already-wired-in backstop before this removal (task #48).
	struct ln_hrz_posn hrz;
	if (!std::isnan (qt.t_start))
	{
		time_t t = qt.t_start;
		double njd = ln_get_julian_from_timet (&t);
		// only change time to calculate conditions when start time is in future
		if (njd > JD)
			JD = njd;
	}
	qt.target->getAltAz (&hrz, JD, *observer);
	return qt.target->isAboveHorizon (&hrz);
}

void ExecutorQueue::updateVals ()
{
	std::vector <int> _id_arr;
	std::vector <std::string> _name_arr;
	std::vector <int> _qid_arr;
	std::vector <double> _start_arr;
	std::vector <double> _end_arr;

	for (ExecutorQueue::iterator iter = begin (); iter != end (); iter++)
	{
		_id_arr.push_back (iter->target->getTargetID ());
		_name_arr.push_back (iter->target->getTargetName ());
		_qid_arr.push_back (iter->qid);
		_start_arr.push_back (iter->t_start);
		_end_arr.push_back (iter->t_end);

		iter->update ();
	}

	nextIds->setValueArray (_id_arr);
	nextNames->setValueArray (_name_arr);
	nextStartTimes->setValueArray (_start_arr);
	nextEndTimes->setValueArray (_end_arr);
	queueEntry->setValueArray (_qid_arr);

	master->sendValueAll (nextIds);
	master->sendValueAll (nextNames);
	master->sendValueAll (nextStartTimes);
	master->sendValueAll (nextEndTimes);
	master->sendValueAll (queueEntry);
}

const char* getTextReason (const removed_t reason)
{
	switch (reason)
	{
		case REMOVED_NEXT_NEEDED:
			return "next target must be observed";
		case REMOVED_TIMES_EXPIRED:
			return "target times expired";
		case REMOVED_STARTED:
			return "target was observed";
	}
	return "unknow reason";
}

ExecutorQueue::iterator ExecutorQueue::removeEntry (ExecutorQueue::iterator &iter, const removed_t reason)
{
	logStream (MESSAGE_WARNING) << "removing target " << iter->target->getTargetName () << " (" << iter->target->getTargetID () << ", start " << LibnovaDateDouble (iter->t_start) << ", end " << LibnovaDateDouble (iter->t_end) << ") because " << getTextReason (reason) << sendLog;

	// add why,..
	if (reason < 0)
	{
		removedIds->addValue (iter->target->getTargetID ());
		removedNames->addValue (iter->target->getTargetName ());
		removedTimes->addValue (getNow ());
		removedWhy->addValue (reason);
		removedQueueEntry->addValue (iter->qid);

		master->sendValueAll (removedIds);
		master->sendValueAll (removedNames);
		master->sendValueAll (removedTimes);
		master->sendValueAll (removedWhy);
		master->sendValueAll (removedQueueEntry);
	}
	else
	{
		executedIds->addValue (iter->target->getTargetID ());
		executedNames->addValue (iter->target->getTargetName ());
		executedTimes->addValue (getNow ());
		executedQueueEntry->addValue (iter->qid);

		master->sendValueAll (executedIds);
		master->sendValueAll (executedNames);
		master->sendValueAll (executedTimes);
		master->sendValueAll (executedQueueEntry);
	}

	if (iter->target != currentTarget)
		delete iter->target;
	else
		currentTarget = NULL;

	iter->remove ();

	return erase (iter);
}

ExecutorQueue::iterator ExecutorQueue::findIndex (int index)
{
	ExecutorQueue::iterator iter;
	if (index < 0)
	{
		iter = end ();
		for (int i = index; i < -1 && iter != begin (); i++)
			iter--;
	}
	else
	{
		iter = begin ();
		for (int i = index; i > 0 && iter != end (); i--)
			iter++;
	}
	return iter;
}
