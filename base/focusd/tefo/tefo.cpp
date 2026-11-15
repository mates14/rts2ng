/**
 * Driver for TEFO focuser by Universal Scientific Technologies.
 * Copyright (C) 2021 Jan Strobl
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

// base note: no vendor SDK involved - TEFO talks a plain UDP text protocol
// (see sendCommand() below), so this needed rts2core::ConnUDP (+ its
// ConnNoSend base), part of the still-unfinished "Connection transport
// variants" task - ported into the kernel alongside this driver
// (kernel/include+src/connnosend.{h,cpp}, connudp.{h,cpp} - flattened from
// classic include/connnosend.h + include/connection/udp.h, matching this
// port's flat kernel/include/ convention). connnotify/conntcp/connfork
// remain deferred - nothing ported so far needs them.

#include "focusd.h"

#include "connudp.h"

#define MOTOR_STATE_IDLE	0
#define MOTOR_STATE_MOVING	1
#define MOTOR_STATE_CALIBRATING	2

#define LAST_MOVEMENT_RESULT_OK		0
#define LAST_MOVEMENT_RESULT_FAIL	1

#define UDP_RETRY_ATTEMPTS	5
#define UDP_RESPONSE_TIMEOUT	0.5

// sendCommand () results besides 0 (OK)
#define TEFO_ERROR		-1
#define TEFO_NO_RESPONSE	-2

// how long [s] a move waits for the TEFO daemon to start and to home (D50 2026-11-15: 1:51 after its start)
#define TEFO_READY_TIMEOUT	300.0


namespace rts2focusd
{

/**
 * Driver for TEFO focuser by Universal Scientific Technologies.
 * https://github.com/UniversalScientificTechnologies/TelescopeFocuser
 * Relates to the "responsive" version.
 *
 * @author Jan Strobl
 */
class TEFO:public Focusd
{
	public:
		TEFO (int argc, char **argv);
		virtual ~ TEFO (void);

	protected:
		virtual int isFocusing ();
		virtual bool isAtStartPosition ();

		virtual int processOption (int in_opt);
		virtual int initHardware ();
		virtual int initValues ();
		virtual int info ();
		virtual int setTo (double num);
		virtual double tcOffset () { return 0.;};

	private:
		rts2core::ConnUDP *connTEFO;
		HostString *host;
		unsigned short messageID;
		unsigned short motorState;
		unsigned short last_movement_result;
		bool lastMovementKnown;		// false while the daemon reports "None" (no movement since it started)
		unsigned short position_actual;
		unsigned short position_set;
		float time_to_moveend;

		// a move asked for before the daemon was ready, -1 when none; it waits until pendingDeadline
		int pendingTarget;
		double pendingDeadline;

		/**
		 * @param attempts	how many times to send it when no response comes (UDP_RESPONSE_TIMEOUT each)
		 * @param quiet		do not log a missing response
		 *
		 * @return 0 on success, TEFO_NO_RESPONSE when the daemon did not answer, TEFO_ERROR on a wrong reply
		 */
		int sendCommand (const char *command, int attempts = UDP_RETRY_ATTEMPTS, bool quiet = false);

		// the daemon homed and finished its own move after it (to the position in its config): idle, with a known
		// last movement result
		bool isReady () { return motorState == MOTOR_STATE_IDLE && lastMovementKnown; }

		// send the move to num: M, or CM (calibrate first) when the daemon was restarted, or when forced
		int sendMove (int num, bool calibrate = false);
};

};

using namespace rts2focusd;

TEFO::TEFO (int argc, char **argv):Focusd (argc, argv)
{
	host = nullptr;
	connTEFO = nullptr;
	lastMovementKnown = false;
	pendingTarget = -1;
	pendingDeadline = 0;

	setFocusExtent(1, 1000);	// It's hardcoded in the daemon's code at the moment

	addOption ('t', "tefo_server", 1, "IP and port (separated by :) of the TEFO daemon");
}

TEFO::~TEFO (void)
{
	delete connTEFO;
	delete host;
}

int TEFO::processOption (int in_opt)
{
	switch (in_opt)
	{
		case 't':
			host = new HostString (optarg, "5000");
			break;
		default:
			return Focusd::processOption (in_opt);
	}
	return 0;
}

int TEFO::initHardware ()
{
	if (host == nullptr)
	{
		logStream (MESSAGE_ERROR) << "You must specify IP:port of the TEFO daemon (-t option)." << sendLog;
		return -1;
	}

	connTEFO = new rts2core::ConnUDP (host->getPort (), this, host->getHostname());
	connTEFO->setDebug (getDebug ());

	int ret = connTEFO->init();
	if (ret)
		return ret;

	return 0;
}

int TEFO::initValues ()
{
	focType = std::string ("TEFO");
	messageID = 0;
	return Focusd::initValues ();
}

int TEFO::sendCommand (const char *command, int attempts, bool quiet)
{
	int ret = 0;
	int attempt = 0;
	char message[20], response[80];
	unsigned short response_ID;
	char response_command[10], request_status[10], motor_status[12], last_result[12];

	messageID++;
	sprintf (message, "%i %s", messageID, command);

	if (getDebug ())
		logStream (MESSAGE_DEBUG) << "message: '" << message << "'" << sendLog;
	while (ret <= 0 && attempt < attempts)
	{
		attempt++;
		ret = connTEFO->sendReceive ((const char *)message, response, 79, 0, UDP_RESPONSE_TIMEOUT);
	}
	if (ret<1)
	{
		if (!quiet)
			logStream (MESSAGE_ERROR) << "Didn't get response from focuser daemon..." << sendLog;
		return TEFO_NO_RESPONSE;
	}

	response[ret] = 0;
	if (getDebug ())
		logStream (MESSAGE_DEBUG) << "response: '" << response << "'" << sendLog;
	// <ID_int16> <command> <request-status_string> <motor-status_string> <last-movement-result_int> <actual-ABS-position_int> <set-ABS-position_int> <time-to-moveend_float>
	// base note: the last movement result is "None" until the daemon's first movement (its homing at the start);
	// classic read it as a number and failed on every reply then [JS 2026-11-15]
	ret = sscanf (response, "%hu %9s %9s %11s %11s %hu %hu %f", &response_ID, response_command, request_status, motor_status, last_result, &position_actual, &position_set, &time_to_moveend);
	if (ret !=8)
	{
		logStream (MESSAGE_ERROR) << "Cannot parse reply from focuser, reply was: '" << response << "', sscanf returned " << ret << sendLog;
		return TEFO_ERROR;
	}
	lastMovementKnown = strcmp (last_result, "None") != 0;
	last_movement_result = lastMovementKnown ? atoi (last_result) : LAST_MOVEMENT_RESULT_OK;

	if (response_ID != messageID)
	{
		logStream (MESSAGE_ERROR) << "responseID is not the same as messageID?!?" << sendLog;
		return TEFO_ERROR;
	}

	if (strcmp(request_status, "wrong") == 0)
	{
		logStream (MESSAGE_ERROR) << "request_status reported as wrong in daemon's response!" << sendLog;
		return TEFO_ERROR;
	}

	if (strcmp(motor_status, "idle") == 0)
		motorState = MOTOR_STATE_IDLE;
	else if (strcmp(motor_status, "moving") == 0)
		motorState = MOTOR_STATE_MOVING;
	else
		motorState = MOTOR_STATE_CALIBRATING;

	return 0;
}

int TEFO::info ()
{
	int ret;

	ret = sendCommand ("S");
	if (ret)
		return -1;

	position->setValueInteger ((int) position_actual);

	return Focusd::info ();
}

int TEFO::setTo (double num)
{
	int ret;

	ret = sendCommand ("S", UDP_RETRY_ATTEMPTS, true);
	if (ret == TEFO_ERROR)
		return -1;

	// base note: classic refused a move while the daemon was not running or homing, which is what it does after a
	// reboot (both sites 2026-11-15: the start position failed, F0 stayed at 240 instead of 760). The move now
	// waits for the daemon, up to TEFO_READY_TIMEOUT, in the focusing state (no exposure meanwhile); isFocusing ()
	// sends it. [JS 2026-11-15]
	if (ret == TEFO_NO_RESPONSE || !isReady ())
	{
		pendingTarget = (int) num;
		pendingDeadline = getNow () + TEFO_READY_TIMEOUT;
		logStream (MESSAGE_INFO) << "focuser daemon " << (ret == TEFO_NO_RESPONSE ? "does not answer" : "is not ready (homing)") << ", the move to " << pendingTarget << " waits for it (at most " << TEFO_READY_TIMEOUT << " s)" << sendLog;
		return 0;
	}
	position->setValueInteger ((int) position_actual);
	pendingTarget = -1;

	return sendMove ((int) num);
}

int TEFO::sendMove (int num, bool calibrate)
{
	char command[20];

	if (calibrate || (motorState == MOTOR_STATE_IDLE && last_movement_result == LAST_MOVEMENT_RESULT_OK && position_actual != position_set))
	{
		// it seems like the TEFO daemon has been restarted, we have to recalibrate!
		sprintf (command, "CM%i", num);
	}
	else
	{
		sprintf (command, "M%i", num);
	}

	return sendCommand (command) ? -1 : 0;
}

int TEFO::isFocusing ()
{
	int ret;

	if (pendingTarget >= 0)
	{
		// one attempt and no log while waiting: the main loop is not held 2.5 s at every poll
		// a wrong reply (a late answer to an unanswered request has the old ID) is waited out as no answer
		ret = sendCommand ("S", 1, true);
		if (ret == 0)
			position->setValueInteger ((int) position_actual);
		bool late = getNow () > pendingDeadline;
		// a daemon that is idle but reports no movement past the deadline did not home by itself: calibrate first
		if (ret == 0 && (isReady () || (late && motorState == MOTOR_STATE_IDLE)))
		{
			int num = pendingTarget;
			pendingTarget = -1;
			logStream (MESSAGE_INFO) << "focuser daemon ready, moving to " << num << sendLog;
			if (sendMove (num, !isReady ()))
				return -1;
			return USEC_SEC / 10;
		}
		if (late)
		{
			logStream (MESSAGE_ERROR) << "focuser daemon not ready within " << TEFO_READY_TIMEOUT << " s, the move to " << pendingTarget << " is given up" << sendLog;
			pendingTarget = -1;
			return -1;
		}
		return ret == 0 ? USEC_SEC / 10 : USEC_SEC;
	}

	ret = info ();
	if (ret)
		return ret;

	if (motorState == MOTOR_STATE_MOVING)
		return (int) (USEC_SEC * time_to_moveend);
	if (motorState == MOTOR_STATE_CALIBRATING)
		return (int) (USEC_SEC / 10);

	// the only left case is motorState == MOTOR_STATE_IDLE
	if (last_movement_result == LAST_MOVEMENT_RESULT_FAIL || position_actual != position_set)
		return -1;	// error

	return -2;	// OK, we are in place
}

bool TEFO::isAtStartPosition ()
{
	return false;
}

int main (int argc, char **argv)
{
	TEFO device (argc, argv);
	return device.run ();
}
