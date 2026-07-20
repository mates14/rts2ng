/*
 * Command classes.
 * Copyright (C) 2003-2007 Petr Kubanek <petr@kubanek.net>
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

// base note: the classic command.h declares ~50 concrete Command
// subclasses (CommandMove, CommandFilter, CommandExposure, CommandMirror,
// CommandIntegrate, CommandExecNext, CommandQueue*, ...), each tied to a
// specific DevClient* type (camera/telescope/cupola/focuser/mirror/phot -
// task 6+ and beyond, effectively driver-tier concerns per the design
// artifacts for this project, not core bus plumbing). Ported here: the
// base Command class, the four classes centrald auth needs
// (Rts2CentraldCommand, CommandSendKey, CommandAuthorize, CommandKey), and
// CommandStatusInfo/CommandDeviceStatus - those two look driver-adjacent by
// name but actually take a Connection* (not a DevClient*) and implement the
// core status_info/device_status protocol handshake that
// Connection::sendCommand() itself issues, so they're core, not deferred.
// Also added CommandChangeValue (task: rts2-mon) - it only builds a
// PROTO_SET_VALUE wire string from a value name/operator/operand(s), no
// DevClient dependency, needed by the value-editing dialogs in
// base/monitor/nvaluebox.cpp.
// Port the other ~45 alongside whatever DevClient/driver work needs them -
// grep the classic ../include/command.h for the full list when that time
// comes.
//
// This also let go of the classic header's "#include block.h" - the base
// Command class only stores a Block* (never calls a method on it), so a
// forward declaration is enough; the reduced Command does not need Block
// to exist yet, unlike the trimmed set still needing a full Connection.

#include "connection.h"
#include "object.h"
#include "event.h"

/**
 * @defgroup RTS2Command RTS2 commands
 */

/** Send command again to device. @ingroup RTS2Command */
#define RTS2_COMMAND_REQUE      -5

/**
 * Miscelanus flag for sending command while exposure is in progress.
 */
#define BOP_WHILE_STATE         0x00001000

/**
 * Call-in-progress mask
 */
#define BOP_CIP_MASK            0x00006000

#define COMMAND_INFO            "info"
#define COMMAND_TELD_MOVE       "move"
#define COMMAND_TELD_MOVE_PM    "move_pm"
#define COMMAND_TELD_MOVE_EPOCH  "move_epoch"
#define COMMAND_TELD_MOVE_EPOCH_PM  "move_epoch_pm"
#define COMMAND_TELD_HADEC      "hadec"
#define COMMAND_TELD_ALTAZ      "altaz"
#define COMMAND_TELD_ALTAZ_NC   "nc_altaz"
#define COMMAND_TELD_MOVE_MPEC  "move_mpec"
#define COMMAND_TELD_MOVE_TLE   "move_tle"
#define COMMAND_TELD_PARK       "park"
#define COMMAND_TELD_PEEK       "peek"
#define COMMAND_TELD_GETDUT1    "getdut1"
#define COMMAND_TELD_WEATHER    "weather"
#define COMMAND_DATA_IN_FITS    "fits_data"
#define COMMAND_FITS_STAT       "fits_statistics"
#define COMMAND_CCD_EXPOSURE    "expose"
#define COMMAND_CCD_EXPOSURE_NO_CHECKS  "expose_no_checks"
#define COMMAND_CCD_SHIFTSTORE  "shiftstore"
#define COMMAND_OPEN       "open"
#define COMMAND_CLOSE      "close"
#define COMMAND_STOP       "stop"
#define COMMAND_CUPOLA_AZ       "az"
#define COMMAND_CUPOLA_MOVE     "move"
#define COMMAND_CUPOLA_SYNCTEL  "synctel"
#define COMMAND_CUPOLA_PARK     "park"
#define COMMAND_PARALLACTIC_UPDATE   "pa_update"
#define COMMAND_ROTATOR_AUTO         "autorotate"
#define COMMAND_ROTATOR_AUTOSTOP     "autostop"
#define COMMAND_ROTATOR_PARK         "park"

/**
 * Defines CIP (Command In Progress) states. Commands which waits on component or RTS2
 * to reach given state uses this to control their execution.
 */
enum cip_state_t
{
	CIP_NOT_CALLED = 0x00000000, //! The command does not use CIP.
	CIP_WAIT       = 0x00002000, //! The command is waiting for status update.
	CIP_RUN        = 0x00004000, //! The status update have run, but does not fullfill command request. Command is still waiting for correct state.
	CIP_RETURN     = 0x00006000	 //! Command is waiting for return from device status wait.
};

namespace rts2core
{

class Block;
class DevClientTelescope;
class DevClientFocus;
class DevClientCamera;
class DevClientPhot;

/**
 * Base class which represents commands send over network to other component.
 * This object is usually send through Connection::queCommand to connection,
 * which process it, wait for the other side to reply, pass return code to
 * Connection::commandReturn callback and delete it.
 *
 * @see Connection
 *
 * @ingroup RTS2Block
 * @ingroup RTS2Command
 */
class Command
{
	public:
		Command (Block * _owner);
		Command (Block * _owner, const char *_text);
		Command (Block * _owner, std::ostringstream &_os);
		Command (Command * _command);
		Command (Command & _command);
		virtual ~ Command (void);

		void setCommand (std::ostringstream &_os);
		void setCommand (const char * _text);

		void setConnection (Connection * conn) { connection = conn; }

		Connection * getConnection () { return connection; }

		virtual int send ();
		int commandReturn (int status, Connection * conn);

		char * getText () { return text; }

		void setBopMask (int _bopMask) { bopMask = _bopMask; }

		int getBopMask () { return bopMask; }

		void setOriginator (Object * _originator) { originator = _originator; }

		bool isOriginator (Object * testOriginator) { return originator == testOriginator; }

		cip_state_t getStatusCallProgress () { return (cip_state_t) (bopMask & BOP_CIP_MASK); }

		void setStatusCallProgress (cip_state_t call_progress)
		{
			bopMask = (bopMask & ~BOP_CIP_MASK) | (call_progress & BOP_CIP_MASK);
		}

		virtual int commandReturnOK (Connection * conn);
		virtual int commandReturnQued (Connection * conn);
		virtual int commandReturnFailed (int status, Connection * conn);

		virtual void deleteConnection (Connection * conn);
	protected:
		Block * owner;
		Connection * connection;
		char * text;
	private:
		int bopMask;
		Object * originator;
};

/**
 * Command send to central daemon.
 *
 * @ingroup RTS2Command
 */
class Rts2CentraldCommand:public Command
{

	public:
		Rts2CentraldCommand (Block * _owner, char *_text)
			:Command (_owner, _text)
		{
		}
};

/**
 * Send and process authorization request.
 *
 * @ingroup RTS2Command
 */
class CommandSendKey:public Command
{
	private:
		int key;
		int centrald_id;
		int centrald_num;
	public:
		CommandSendKey (Block * _master, int _centrald_id, int _centrald_num, int _key);
		virtual int send ();

		virtual int commandReturnOK (Connection * conn)
		{
			connection->setConnState (CONN_AUTH_OK);
			return -1;
		}
		virtual int commandReturnFailed (int status, Connection * conn)
		{
			connection->setConnState (CONN_AUTH_FAILED);
			return -1;
		}
};

/**
 * Send authorization query to centrald daemon.
 *
 * @ingroup RTS2Command
 */
class CommandAuthorize:public Command
{
	public:
		CommandAuthorize (Block * _master, int centralId, int key);
		virtual int commandReturnFailed (int status, Connection * conn)
		{
			logStream (MESSAGE_ERROR) << "authentification failed for connection " << conn->getName ()
				<< " centrald num " << conn->getCentraldNum ()
				<< " centrald id " << conn->getCentraldId ()
				<< sendLog;
			return -1;
		}
};

/**
 * Send key request to centrald.
 *
 * @ingroup RTS2Command
 */
class CommandKey:public Command
{
	public:
		CommandKey (Block * _master, const char * device_name);
};

/**
 * Send status info command to central server.
 *
 * When this command return, device status is updated, so updateStatusWait from
 * control_conn is called.
 *
 * @ingroup RTS2Command
 */
class CommandStatusInfo:public Command
{
	private:
		Connection * control_conn;
	public:
		CommandStatusInfo (Block * master, Connection * _control_conn);
		virtual int commandReturnOK (Connection * conn);
		virtual int commandReturnFailed (int status, Connection * conn);

		const char * getCentralName()
		{
			return control_conn->getName ();
		}

		virtual void deleteConnection (Connection * conn);
};

/**
 * Send device_status command instead of status_info command.
 *
 * @ingroup RTS2Command
 */
class CommandDeviceStatus:public CommandStatusInfo
{
	public:
		CommandDeviceStatus (Block * master, Connection * _control_conn);
};

/**
 * Set message mask on centrald connections - which message severities
 * this component wants to receive.
 *
 * @ingroup RTS2Command
 */
class CommandMessageMask:public Command
{
	public:
		CommandMessageMask (Block * _master, int _mask);
};

/**
 * Change a value on a device. Used by CLI clients (rts2-mon) to send edited
 * value contents over the wire - PROTO_SET_VALUE followed by name, operator
 * and one or more operands depending on the value's type.
 *
 * @ingroup RTS2Command
 */
class CommandChangeValue:public Command
{
	public:
		CommandChangeValue (Block * _master, std::string _valName, char op, int _operand);
		CommandChangeValue (Block * _master, std::string _valName, char op, long int _operand);
		CommandChangeValue (Block * _master, std::string _valName, char op, float _operand);
		CommandChangeValue (Block * _master, std::string _valName, char op, double _operand);
		CommandChangeValue (Block * _master, std::string _valName, char op, double _operand1, double _operand2);
		CommandChangeValue (Block * _master, std::string _valName, char op, int _operand1, int _operand2, int _operand3);
		CommandChangeValue (Block * _master, std::string _valName, char op, bool _operand);
		/**
		 * Change rectangle value.
		 */
		CommandChangeValue (Block * _master, std::string _valName, char op, int x, int y, int w, int h);
		/**
		 * Create command to change value from string.
		 *
		 * @param raw If true, string will be send without escaping.
		 */
		CommandChangeValue (Block * _master, std::string _valName, char op, std::string _operand, bool raw = false);
};

/**
 * Ask a cupola to synchronize to the telescope's current target position.
 * Sent via Block::queueCommandForType(DEVICE_TYPE_CUPOLA, ...) - no
 * DevClient dependency (unlike CommandCupolaNotMove, which is tied to
 * DevClientCupola and stays deferred), so it's core to
 * Telescope::startCupolaSync(), called unconditionally on every telescope
 * movement regardless of whether a cupola is actually connected.
 *
 * @ingroup RTS2Command
 */
class CommandCupolaSyncTel:public Command
{
	public:
		CommandCupolaSyncTel (Block * _master, double ra, double dec);
};

/**
 * Move telescope to given RA/DEC. Needed by DevClientTelescopeExec
 * (rts2script/execcli) - task 25.
 *
 * @ingroup RTS2Command
 */
class CommandMove:public Command
{
	public:
		CommandMove (Block * _master, DevClientTelescope * _tel, double ra, double dec);
		virtual int commandReturnFailed (int status, Connection * conn);
	protected:
		CommandMove (Block * _master, DevClientTelescope * _tel);
	private:
		DevClientTelescope *tel;
};

/**
 * Move telescope without modelling corrections.
 *
 * @ingroup RTS2Command
 */
class CommandMoveUnmodelled:public CommandMove
{
	public:
		CommandMoveUnmodelled (Block * _master, DevClientTelescope * _tel, double ra, double dec);
};

/**
 * Move telescope to MPEC one-line.
 *
 * @ingroup RTS2Command
 */
class CommandMoveMpec:public CommandMove
{
	public:
		CommandMoveMpec (Block * _master, DevClientTelescope * _tel, double ra, double dec, std::string mpec_oneline);
};

/**
 * Move telescope to TLE.
 *
 * @ingroup RTS2Command
 */
class CommandMoveTle:public CommandMove
{
	public:
		CommandMoveTle (Block * _master, DevClientTelescope * _tel, double ra, double dec, std::string tle1, std::string tle2);
};

/**
 * Move telescope to fixed location.
 *
 * @ingroup RTS2Command
 */
class CommandMoveFixed:public CommandMove
{
	public:
		CommandMoveFixed (Block * _master, DevClientTelescope * _tel, double ra, double dec);
};

/**
 * Command for telescope movement in HA/Dec coordinates.
 *
 * @ingroup RTS2Command
 */
class CommandMoveHaDec:public CommandMove
{
	public:
		CommandMoveHaDec (Block * _master, DevClientTelescope * _tel, double ha, double dec);
};

/**
 * Command for telescope movement in alt az.
 *
 * @ingroup RTS2Command
 */
class CommandMoveAltAz:public CommandMove
{
	public:
		CommandMoveAltAz (Block * _master, DevClientTelescope * _tel, double alt, double az);
};

class CommandResyncMove:public CommandMove
{
	public:
		CommandResyncMove (Block * _master, DevClientTelescope * _tel, double ra, double dec);
};

/**
 * Send astrometry correction (mark/image/observation IDs + RA/DEC/position
 * error) to a telescope connection.
 *
 * @ingroup RTS2Command
 */
class CommandCorrect:public Command
{
	public:
		CommandCorrect (Block * _master, int corr_mark, int corr_img, int corr_obs, int img_id, int obs_id, double ra_corr, double dec_corr, double pos_err);
};

class CommandStartGuide:public Command
{
	public:
		CommandStartGuide (Block * _master, char dir, double dir_dist);
};

class CommandStopGuideAll:public Command
{
	public:
		CommandStopGuideAll (Block * _master):Command (_master)
		{
			setCommand ("stop_guide_all");
		}
};

/**
 * Report FITS image statistics (average/min/max/sum/mode) back to the
 * device that produced the exposure. Needed by DevClientCameraImage
 * (rts2fits/devcliimg) - task 23/24.
 *
 * @ingroup RTS2Command
 */
class CommandFitsStat:public Command
{
	public:
		CommandFitsStat (Block * _master, double average, double min, double max, double sum, double mode);
};

/**
 * Ask a telescope to change (offset) its position by a given RA/DEC delta.
 * Needed by DevClientTelescopeImage (rts2fits/devcliimg) - task 23/24.
 *
 * @ingroup RTS2Command
 */
class CommandChange:public Command
{
	DevClientTelescope *tel;
	public:
		CommandChange (Block * _master, double ra, double dec);
		CommandChange (DevClientTelescope * _tel, double ra, double dec);
		CommandChange (CommandChange * _command, DevClientTelescope * _tel);
		virtual int commandReturnFailed (int status, Connection * conn);
};

/**
 * Ask a device to run its info command. Needed by DevClientCameraImage/
 * DevClientWriteImage (rts2fits/devcliimg) - task 23/24.
 *
 * @ingroup RTS2Command
 */
class CommandInfo:public Command
{
	public:
		CommandInfo (Block * _master);
		virtual int commandReturnOK (Connection * conn);
		virtual int commandReturnFailed (int status, Connection * conn);
};

/**
 * Change focuser position by a relative step count (FOC_TOFFS +=).
 * Needed by DevClientFocusFoc (rts2fits/devclifoc) - task 23/24.
 *
 * @ingroup RTS2Command
 */
class CommandChangeFocus:public Command
{
	private:
		DevClientFocus * focuser;
		DevClientCamera * camera;
		void change (int _steps);
	public:
		CommandChangeFocus (DevClientFocus * _focuser, int _steps);
		CommandChangeFocus (DevClientCamera * _camera, int _steps);
		virtual int commandReturnFailed (int status, Connection * conn);
};

/**
 * Ask a photometer to integrate, optionally after a filter change. Needed
 * by DevClientPhotFoc (rts2fits/devclifoc) - task 23/24.
 *
 * @ingroup RTS2Command
 */
class CommandIntegrate:public Command
{
	private:
		DevClientPhot * phot;
	public:
		CommandIntegrate (DevClientPhot * _phot, int _filter,
			float _exp, int _count);
		CommandIntegrate (DevClientPhot * _phot, float _exp,
			int _count);
		virtual int commandReturnFailed (int status, Connection * conn);
};

/**
 * Base for commands which need a DevClientCamera to construct (device is
 * derived from the camera's own connection). Needed by `CommandBox`/
 * `CommandCenter` (element.cpp) - task 25.
 *
 * @ingroup RTS2Command
 */
class CommandCameraSettings:public Command
{
	public:
		CommandCameraSettings (DevClientCamera * camera);
};

/**
 * Start exposure on camera. Needed by element.cpp
 * (`ElementSequence`/`ElementImage`) - task 25.
 *
 * @ingroup RTS2Command
 */
class CommandExposure:public Command
{
	public:
		CommandExposure (Block * _master, DevClientCamera * _camera, int _bopMask);

		virtual int commandReturnOK (Connection *conn);
		virtual int commandReturnFailed (int status, Connection * conn);
	private:
		DevClientCamera * camera;
};

/**
 * Shift-and-store exposure sequence commands. Needed by element.cpp
 * (`ElementShiftStoreStart`/`Progress`/`End`) - task 25.
 *
 * @ingroup RTS2Command
 */
class CommandShiftStart: public Command
{
	public:
		CommandShiftStart (Block * _master, float expTime, int _bopMask);
};

class CommandShiftProgress: public Command
{
	public:
		CommandShiftProgress (Block * _master, int shift, float expTime, int _bopMask);
};

class CommandShiftEnd: public Command
{
	public:
		CommandShiftEnd (Block * _master, int shift, float expTime, int _bopMask);
};

/**
 * Set camera windowing/centering. Needed by element.cpp
 * (`ElementBox`/`ElementCenter`) - task 25.
 *
 * @ingroup RTS2Command
 */
class CommandBox:public CommandCameraSettings
{
	public:
		CommandBox (DevClientCamera * _camera, int x, int y, int w, int h);
};

class CommandCenter:public CommandCameraSettings
{
	public:
		CommandCenter (DevClientCamera * _camera, int width, int height);
};

/**
 * Queue a target on a selector's queue at a given time. Needed by
 * elementexe.cpp (the `requeue` script command) - task 25.
 *
 * @ingroup RTS2Command
 */
class CommandQueueAt:public Command
{
	public:
		CommandQueueAt (Block * _master, const char *queue, int tar_id, double t_start, double t_end);
};

/**
 * Kill all running/queued commands, optionally without triggering
 * script_ends. Needed by DevScript (devscript.cpp) - task 25.
 *
 * @ingroup RTS2Command
 */
class CommandKillAll:public Command
{
	public:
		CommandKillAll (Block * _master);
};

class CommandKillAllWithoutScriptEnds:public Command
{
	public:
		CommandKillAllWithoutScriptEnds (Block * _master);
};

/**
 * Signal a script has ended. Needed by DevScript (devscript.cpp) -
 * task 25.
 *
 * @ingroup RTS2Command
 */
class CommandScriptEnds:public Command
{
	public:
		CommandScriptEnds (Block * _master);
};

}
