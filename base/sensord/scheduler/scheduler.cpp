/*
 * Scheduler trigger: runs an external scheduler when a telescope gets ready
 * to observe, on the run command, and periodically while one is ready.
 * Copyright (C) 2026 Martin Jelinek <mates@lascaux.asu.cas.cz>
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

#include "sensord.h"
#include "connfork.h"
#include "centralstate.h"

#include <cmath>
#include <unistd.h>
#include <string>
#include <vector>

#define OPT_TELESCOPE   OPT_LOCAL + 900
#define OPT_COMMAND     OPT_LOCAL + 901
#define OPT_INTERVAL    OPT_LOCAL + 902
#define OPT_TIMEOUT     OPT_LOCAL + 903
#define OPT_READY_HOLD  OPT_LOCAL + 904

namespace rts2sensord
{

class Scheduler;

/**
 * One run of the scheduler command, through /bin/sh -c. The command is
 * expected to send its own output to a log: every line it writes to stderr
 * would otherwise become an rts2 error message.
 */
class SchedulerRun:public rts2core::ConnFork
{
	public:
		SchedulerRun (Scheduler *_master, const char *_command, int _timeout);
		virtual ~SchedulerRun ();

		// the daemon is going away, or the run never started: do not report its end
		void detach () { owner = NULL; }

	protected:
		virtual int newProcess ();

	private:
		Scheduler *owner;
		std::string command;
};

/**
 * Starts the (multi-telescope) scheduler when it can make a difference.
 *
 * The daemon connects to the centrald of every telescope the scheduler
 * plans for (one --server each; rts2-start adds the local one), and keeps
 * each telescope's state apart. Block keeps only one master state, and
 * from a centrald that is not the master it passes on no state but off,
 * standby and bad weather.
 *
 * A run starts when a telescope becomes ready (ON, at dusk or night), on
 * the run command, and every interval while a telescope is ready. Only one
 * run at a time: a trigger during a run is held, and a single new run
 * starts as soon as the current one ends, so the plan reflects the latest
 * state without killing a run halfway.
 *
 * A telescope becoming ready while nothing runs waits ready_hold (30 s)
 * first, so that the other one, switched on a moment later, gets into the
 * same plan. Of 155 nights both were switched on within 30 min of each
 * other (2025-09 to 2026-10), the second followed within 30 s in 87, the
 * median lag was 25 s. The run command and the interval do not wait.
 *
 * @author Martin Jelinek <mates@lascaux.asu.cas.cz>
 */
class Scheduler:public Sensor
{
	public:
		Scheduler (int argc, char **argv);
		virtual ~Scheduler ();

		virtual int setMasterState (rts2core::Connection *_conn, rts2_status_t new_state);
		virtual void centraldConnBroken (rts2core::Connection *conn);

		virtual int commandAuthorized (rts2core::Connection *conn);

		void runEnded (SchedulerRun *run);

	protected:
		virtual int processOption (int opt);
		virtual int initHardware ();
		virtual int info ();
		virtual int idle ();
		virtual void endRunLoop ();

	private:
		struct Telescope
		{
			std::string name;
			std::string host;
			rts2core::Connection *conn;
			bool ready;
			rts2core::ValueString *state;
			rts2core::ValueBool *available;
		};

		std::vector <Telescope> telescopes;
		const char *command;

		SchedulerRun *currentRun;

		rts2core::ValueBool *running;
		rts2core::ValueBool *pending;
		rts2core::ValueString *pendingReason;
		rts2core::ValueTime *lastStart;
		rts2core::ValueTime *lastEnd;
		rts2core::ValueString *lastReason;
		rts2core::ValueInteger *runs;
		rts2core::ValueDouble *interval;
		rts2core::ValueInteger *timeout;
		rts2core::ValueDouble *readyHold;
		rts2core::ValueTime *holdUntil;
		std::string holdReason;

		void setReady (Telescope &t, bool ready);
		void triggerReady (const std::string &reason);
		void trigger (const std::string &reason);
		void startRun (const std::string &reason);
		bool anyReady ();
};

}

using namespace rts2sensord;

SchedulerRun::SchedulerRun (Scheduler *_master, const char *_command, int _timeout):rts2core::ConnFork (_master, "/bin/sh", false, false, _timeout)
{
	owner = _master;
	command = _command;
}

SchedulerRun::~SchedulerRun ()
{
	if (owner)
		owner->runEnded (this);
}

int SchedulerRun::newProcess ()
{
	// The child inherits every socket of the daemon (forkedInstance () closes
	// only the listening one): a run held all centrald and client connections
	// open, so a connection SCH dropped stayed up for its peer until the run
	// ended. Keep only stdin/stdout/stderr, which ConnFork set to its pipes.
	closefrom (3);
	execl ("/bin/sh", "sh", "-c", command.c_str (), (char *) NULL);
	return -1;
}

Scheduler::Scheduler (int argc, char **argv):Sensor (argc, argv)
{
	command = NULL;
	currentRun = NULL;

	createValue (running, "running", "scheduler is running", false);
	running->setValueBool (false);
	createValue (pending, "pending", "another run is to start when this one ends", false);
	pending->setValueBool (false);
	createValue (pendingReason, "pending_reason", "what asked for the pending run", false);
	createValue (lastStart, "last_start", "start of the last run", false);
	createValue (lastEnd, "last_end", "end of the last run", false);
	createValue (lastReason, "last_reason", "what started the last run", false);
	createValue (runs, "runs", "number of runs since start", false);
	runs->setValueInteger (0);

	createValue (interval, "interval", "[s] run at least this often while a telescope is ready", false, RTS2_VALUE_WRITABLE | RTS2_VALUE_AUTOSAVE);
	interval->setValueDouble (1200);
	createValue (timeout, "timeout", "[s] kill a run that takes longer", false, RTS2_VALUE_WRITABLE | RTS2_VALUE_AUTOSAVE);
	timeout->setValueInteger (900);
	createValue (readyHold, "ready_hold", "[s] wait after a telescope gets ready, for the other to follow", false, RTS2_VALUE_WRITABLE | RTS2_VALUE_AUTOSAVE);
	readyHold->setValueDouble (30);
	createValue (holdUntil, "hold_until", "a run held for a telescope getting ready starts then", false);

	addOption (OPT_TELESCOPE, "telescope", 1, "name:centrald, one per telescope (centrald as given to --server)");
	addOption (OPT_COMMAND, "command", 1, "scheduler command, run by /bin/sh -c; it should log to a file");
	addOption (OPT_INTERVAL, "interval", 1, "run at least every that many seconds while a telescope is ready (1200)");
	addOption (OPT_TIMEOUT, "timeout", 1, "kill a run after that many seconds (900)");
	addOption (OPT_READY_HOLD, "ready-hold", 1, "wait that many seconds after a telescope gets ready, for the other to follow (30)");

	// state changes arrive at once, this is only for the periodic run
	setIdleInfoInterval (10);
	// idle () releases a held run; 10 s (the default) is coarse for a 30 s hold
	setTimeout (USEC_SEC);
}

Scheduler::~Scheduler ()
{
	// Block deletes the connections after this destructor has run
	if (currentRun)
		currentRun->detach ();
}

int Scheduler::processOption (int opt)
{
	switch (opt)
	{
		case OPT_TELESCOPE:
		{
			const char *sep = strchr (optarg, ':');
			if (sep == NULL || sep == optarg || *(sep + 1) == '\0')
			{
				std::cerr << "--telescope expects name:centrald, not " << optarg << std::endl;
				return -1;
			}
			Telescope t;
			t.name = std::string (optarg, sep - optarg);
			t.host = std::string (sep + 1);
			t.conn = NULL;
			t.ready = false;
			createValue (t.state, (t.name + "_state").c_str (), (t.name + " centrald state").c_str (), false);
			createValue (t.available, (t.name + "_ready").c_str (), (t.name + " is ON at dusk or night").c_str (), false);
			t.available->setValueBool (false);
			telescopes.push_back (t);
			break;
		}
		case OPT_COMMAND:
			command = optarg;
			break;
		case OPT_INTERVAL:
			interval->setValueCharArr (optarg);
			break;
		case OPT_TIMEOUT:
			timeout->setValueCharArr (optarg);
			break;
		case OPT_READY_HOLD:
			readyHold->setValueCharArr (optarg);
			break;
		default:
			return Sensor::processOption (opt);
	}
	return 0;
}

int Scheduler::initHardware ()
{
	if (command == NULL)
	{
		logStream (MESSAGE_ERROR) << "no --command given" << sendLog;
		return -1;
	}
	if (telescopes.empty ())
	{
		logStream (MESSAGE_ERROR) << "no --telescope given" << sendLog;
		return -1;
	}
	for (std::vector <Telescope>::iterator iter = telescopes.begin (); iter != telescopes.end (); iter++)
	{
		iter->conn = getCentraldConn (iter->host.c_str ());
		if (iter->conn == NULL)
		{
			logStream (MESSAGE_ERROR) << "telescope " << iter->name << ": no --server " << iter->host << sendLog;
			return -1;
		}
	}
	return 0;
}

int Scheduler::setMasterState (rts2core::Connection *_conn, rts2_status_t new_state)
{
	for (std::vector <Telescope>::iterator iter = telescopes.begin (); iter != telescopes.end (); iter++)
	{
		if (iter->conn != _conn)
			continue;

		bool ready = (new_state & SERVERD_ONOFF_MASK) == SERVERD_ON
			&& ((new_state & SERVERD_STATUS_MASK) == SERVERD_DUSK || (new_state & SERVERD_STATUS_MASK) == SERVERD_NIGHT);

		iter->state->setValueString (rts2core::CentralState::getString (new_state));
		sendValueAll (iter->state);
		setReady (*iter, ready);
	}
	return Sensor::setMasterState (_conn, new_state);
}

void Scheduler::centraldConnBroken (rts2core::Connection *conn)
{
	// a telescope we cannot see is not ready; reconnecting (60 s later) to
	// an ON centrald is then a transition to ready, and plans again
	for (std::vector <Telescope>::iterator iter = telescopes.begin (); iter != telescopes.end (); iter++)
	{
		if (iter->conn != conn)
			continue;
		iter->state->setValueString ("no connection");
		sendValueAll (iter->state);
		setReady (*iter, false);
	}
	Sensor::centraldConnBroken (conn);
}

void Scheduler::setReady (Telescope &t, bool ready)
{
	if (ready == t.ready)
		return;
	t.ready = ready;
	t.available->setValueBool (ready);
	sendValueAll (t.available);
	if (ready)
		triggerReady (t.name + " ready");
}

void Scheduler::triggerReady (const std::string &reason)
{
	// a run going on, or a hold of no length: the usual path
	if (currentRun != NULL || !(readyHold->getValueDouble () > 0))
	{
		trigger (reason);
		return;
	}
	if (!std::isnan (holdUntil->getValueDouble ()))
	{
		// the other telescope followed within the hold: one plan for both
		holdReason += ", " + reason;
		logStream (MESSAGE_INFO) << reason << ": joins the held run" << sendLog;
		return;
	}
	holdUntil->setValueDouble (getNow () + readyHold->getValueDouble ());
	holdReason = reason;
	sendValueAll (holdUntil);
	logStream (MESSAGE_INFO) << reason << ": planning in " << (int) readyHold->getValueDouble () << " s, so that a telescope switched on meanwhile joins" << sendLog;
}

int Scheduler::commandAuthorized (rts2core::Connection *conn)
{
	if (conn->isCommand ("run"))
	{
		if (!conn->paramEnd ())
			return DEVDEM_E_PARAMSNUM;
		trigger ("run command");
		return 0;
	}
	return Sensor::commandAuthorized (conn);
}

int Scheduler::info ()
{
	if (currentRun == NULL && !pending->getValueBool () && std::isnan (holdUntil->getValueDouble ()) && anyReady ()
		&& (std::isnan (lastStart->getValueDouble ()) || getNow () - lastStart->getValueDouble () >= interval->getValueDouble ()))
		trigger ("interval");
	return Sensor::info ();
}

void Scheduler::trigger (const std::string &reason)
{
	if (currentRun == NULL)
	{
		std::string r = reason;
		if (!std::isnan (holdUntil->getValueDouble ()))
		{
			// run command or interval during a hold: the held trigger goes along now
			r += ", " + holdReason;
			holdUntil->setValueDouble (NAN);
			sendValueAll (holdUntil);
		}
		startRun (r);
		return;
	}
	if (!pending->getValueBool ())
		logStream (MESSAGE_INFO) << reason << ": the scheduler is running, will run again when it ends" << sendLog;
	pending->setValueBool (true);
	pendingReason->setValueString (reason);
	sendValueAll (pending);
	sendValueAll (pendingReason);
}

void Scheduler::startRun (const std::string &reason)
{
	SchedulerRun *run = new SchedulerRun (this, command, timeout->getValueInteger ());
	// lastStart before init: a run that fails to start must not be retried at once
	lastStart->setValueDouble (getNow ());
	lastReason->setValueString (reason);
	sendValueAll (lastStart);
	sendValueAll (lastReason);

	if (run->init ())
	{
		logStream (MESSAGE_ERROR) << "cannot start the scheduler (" << reason << ")" << sendLog;
		run->detach ();
		delete run;
		return;
	}
	currentRun = run;
	addConnection (run);

	running->setValueBool (true);
	runs->inc ();
	sendValueAll (running);
	sendValueAll (runs);
	logStream (MESSAGE_INFO) << "scheduler started: " << reason << sendLog;
}

void Scheduler::runEnded (SchedulerRun *run)
{
	if (run != currentRun)
		return;
	currentRun = NULL;

	if (running->getValueBool ())
	{
		lastEnd->setValueDouble (getNow ());
		running->setValueBool (false);
		sendValueAll (lastEnd);
		sendValueAll (running);
		logStream (MESSAGE_INFO) << "scheduler ended after " << (int) (getNow () - lastStart->getValueDouble ()) << " s" << sendLog;
	}
	// a pending run starts from idle (), not from inside the connection's destructor
}

void Scheduler::endRunLoop ()
{
	// Daemon::endRunLoop () exits straight from the signal handler, so the
	// run's destructor (SIGTERM to its group) never runs: kill it here, or a
	// restarted daemon could plan alongside an orphan. The scheduler commits
	// its upload in one transaction, so a killed run leaves the plan as it was.
	if (currentRun)
		currentRun->terminate ();
	Sensor::endRunLoop ();
}

int Scheduler::idle ()
{
	if (currentRun == NULL && !std::isnan (holdUntil->getValueDouble ()) && getNow () >= holdUntil->getValueDouble ())
	{
		holdUntil->setValueDouble (NAN);
		sendValueAll (holdUntil);
		startRun (holdReason);
	}
	if (currentRun == NULL && pending->getValueBool ())
	{
		std::string reason = pendingReason->getValueString ();
		pending->setValueBool (false);
		pendingReason->setValueString ("");
		sendValueAll (pending);
		sendValueAll (pendingReason);
		startRun (reason);
	}
	return Sensor::idle ();
}

bool Scheduler::anyReady ()
{
	for (std::vector <Telescope>::iterator iter = telescopes.begin (); iter != telescopes.end (); iter++)
		if (iter->ready)
			return true;
	return false;
}

int main (int argc, char **argv)
{
	Scheduler device (argc, argv);
	return device.run ();
}
