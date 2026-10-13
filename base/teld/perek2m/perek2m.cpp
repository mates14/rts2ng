/*
 * Perek 2m telescope (Ondrejov) through Jan Fuchs' telescoped.
 * Copyright (C) 2026 Martin Jelinek
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

#include "teld.h"
#include "configuration.h"
#include "perek2m.h"

#include <cmath>

#define OPT_TCS_HOST       OPT_LOCAL + 2301
#define OPT_TCS_PORT       OPT_LOCAL + 2302
#define OPT_POLL           OPT_LOCAL + 2303
#define OPT_RPC_TIMEOUT    OPT_LOCAL + 2304
#define OPT_READONLY       OPT_LOCAL + 2305
#define OPT_PARK_HA        OPT_LOCAL + 2306
#define OPT_PARK_DA        OPT_LOCAL + 2307

#define EVENT_PEREK2M_POLL  RTS2_LOCAL_EVENT + 1230

// TRRD within this of the target counts as arrived (observe_batch uses 10")
#define ARRIVE_TOLERANCE   (10.0 / 3600.0)
// TSRA echoing the preset: the reply carries 0.01 s / 0.1"
#define PRESET_TOLERANCE   (1.0 / 3600.0)
// park position reached, axis degrees
#define PARK_TOLERANCE     0.05

namespace rts2teld
{

/**
 * The Perek 2m TCS points by itself: it takes catalogue (J2000)
 * coordinates and applies precession, aberration, refraction and its own
 * pointing model. So this driver sends the J2000 origin plus RTS2 offsets
 * and astrometric corrections, and leaves the rest of RTS2's correction
 * chain unused. A slew is telescope_set_coordinates () and then "TGRA 1"
 * once TSRA shows the preset; it ends when the TCS tracks on the target
 * and, when the dome follows the telescope, the dome has stopped.
 *
 * The 2m is shared with observers who drive it from Fuky's clients. The
 * driver commands nothing on its own; it reports whatever the TCS does.
 * Power (oil, TEON), flaps and the slit belong to rts2-dome-perek2m. Park
 * only moves to the park position (HA/Dec axes); the 2m has no mandatory
 * idle position.
 */
class Perek2m:public Telescope
{
	public:
		Perek2m (int argc, char **argv);
		virtual ~Perek2m ();

	protected:
		virtual int processOption (int in_opt);
		virtual int initValues ();
		virtual int init ();
		virtual int info ();
		virtual void postEvent (rts2core::Event *event);

		virtual int commandAuthorized (rts2core::Connection *conn);
		virtual int setValue (rts2core::Value *oldValue, rts2core::Value *newValue);

		virtual int startResync ();
		virtual int isMoving ();
		virtual int stopMove ();
		virtual int startPark ();
		virtual int isParking ();
		virtual int endPark () { return 0; }

		virtual double estimateTargetTime () { return 120; }

		virtual int abortMoveTracking ();

	private:
		perek2m::Tcs tcs;
		perek2m::TcsInfo tcsInfo;
		bool tcsConnected;

		// a slew in progress: first wait for TSRA to show the preset, then TGRA 1
		enum { MOVE_NONE, MOVE_PRESET, MOVE_SLEW } moveStage;
		double moveRa, moveDec;
		int moveSide;
		double moveStarted;
		// telescope seen leaving TRACK/STOP since TGRA 1
		bool moveSeenSlewing;
		// the slew was stopped by abortMoveTracking (): isMoving () reports a failure
		bool moveAborted;

		rts2core::ValueString *tcsUrl;
		rts2core::ValueBool *tcsConn;
		rts2core::ValueBool *readOnly;
		rts2core::ValueString *lastError;
		rts2core::ValueString *tcsUt;

		rts2core::ValueString *glstRaw;
		rts2core::ValueString *oilState;
		rts2core::ValueString *telState;
		rts2core::ValueString *haState;
		rts2core::ValueString *daState;
		rts2core::ValueString *focState;
		rts2core::ValueInteger *glstBits;

		rts2core::ValueRaDec *tcsPos;
		rts2core::ValueRaDec *tcsTarget;
		rts2core::ValueString *tcsObject;
		rts2core::ValueString *pierSide;
		rts2core::ValueSelection *moveSideSel;
		rts2core::ValueDouble *haAxis;
		rts2core::ValueDouble *daAxis;
		rts2core::ValueDouble *guideRa;
		rts2core::ValueDouble *guideDec;
		rts2core::ValueDouble *userRa;
		rts2core::ValueDouble *userDec;

		rts2core::ValueBool *tcsTracking;
		rts2core::ValueBool *corAberration;
		rts2core::ValueBool *corPrecession;
		rts2core::ValueBool *corRefraction;
		rts2core::ValueBool *corModel;
		rts2core::ValueBool *guideMode;
		rts2core::ValueInteger *modelNumber;
		rts2core::ValueDouble *telFocus;

		rts2core::ValueDouble *parkHa;
		rts2core::ValueDouble *parkDa;
		rts2core::ValueDouble *moveTimeout;

		rts2core::ValueString *ascolAnswer;

		// send a command that must be accepted; logs and records a refusal
		int tcsCommand (const std::string &cmd);
		void recordError (const std::string &err);

		bool targetReached ();
		bool domeFollowing () { return tcsInfo.glst[perek2m::GLST_DOME] >= DOME_ST_AUTO_STOP && tcsInfo.glst[perek2m::GLST_DOME] <= DOME_ST_AUTO_STOP + 2; }
};

}

using namespace rts2teld;

Perek2m::Perek2m (int argc, char **argv):Telescope (argc, argv, false, false)
{
	tcsConnected = false;
	moveStage = MOVE_NONE;
	moveRa = moveDec = NAN;
	moveSide = 0;
	moveStarted = 0;
	moveSeenSlewing = false;
	moveAborted = false;

	createValue (tcsUrl, "tcs_url", "telescoped XML-RPC server", false);
	createValue (tcsConn, "tcs_conn", "telescoped is answering", false);
	createValue (readOnly, "tcs_readonly", "monitor only: every command is refused (--readonly sets it at start)", false, RTS2_VALUE_WRITABLE);
	readOnly->setValueBool (false);
	createValue (lastError, "last_error", "last refused or failed TCS command", false);
	createValue (tcsUt, "tcs_ut", "TCS time (UT)", false);

	createValue (glstRaw, "GLST", "raw TCS global state", false);
	createValue (oilState, "OIL", "oil system", true);
	createValue (telState, "TELSTATE", "telescope state (TCS)", true);
	createValue (haState, "HA_AXIS", "hour axis state", false);
	createValue (daState, "DA_AXIS", "declination axis state", false);
	createValue (focState, "FOC_STATE", "telescope focus state", false);
	createValue (glstBits, "glst_bits", "GLST bitfield: HA, Dec, dome, focus, aberration, precession, refraction, model, guide", false);

	createValue (tcsPos, "TCS_POS", "position reported by the TCS (J2000)", true);
	createValue (tcsTarget, "TCS_TAR", "target preset in the TCS (J2000)", true);
	createValue (tcsObject, "TCS_OBJ", "object name preset in the TCS", false);
	createValue (pierSide, "TELSIDE", "telescope position: east or west", true);
	createValue (moveSideSel, "move_side", "side used for RTS2 slews: keep the current one, or force", false, RTS2_VALUE_WRITABLE);
	moveSideSel->addSelVal ("current");
	moveSideSel->addSelVal ("east");
	moveSideSel->addSelVal ("west");
	moveSideSel->setValueInteger (0);
	createValue (haAxis, "HA_AXPOS", "[deg] hour axis position", true, RTS2_DT_DEGREES);
	createValue (daAxis, "DA_AXPOS", "[deg] declination axis position", true, RTS2_DT_DEGREES);
	createValue (guideRa, "GUIDE_RA", "guide correction RA (TRGV)", false);
	createValue (guideDec, "GUIDE_DEC", "guide correction Dec (TRGV)", false);
	createValue (userRa, "USER_RA", "user speed RA (TRUS)", false);
	createValue (userDec, "USER_DEC", "user speed Dec (TRUS)", false);

	createValue (tcsTracking, "TCS_TRACK", "TCS tracking (TETR)", true, RTS2_VALUE_WRITABLE);
	createValue (corAberration, "cor_aberration", "TCS applies aberration (TSCA)", false, RTS2_VALUE_WRITABLE);
	createValue (corPrecession, "cor_precession", "TCS applies precession (TSCP)", false, RTS2_VALUE_WRITABLE);
	createValue (corRefraction, "cor_refraction", "TCS applies refraction (TSCR)", false, RTS2_VALUE_WRITABLE);
	createValue (corModel, "cor_model", "TCS applies its pointing model (TSCM)", false, RTS2_VALUE_WRITABLE);
	createValue (guideMode, "guide_mode", "TCS guide mode (TSGM)", false, RTS2_VALUE_WRITABLE);
	createValue (modelNumber, "TCS_MODEL", "TCS pointing model number (TSCS)", true, RTS2_VALUE_WRITABLE);
	createValue (telFocus, "TELFOCUS", "telescope (secondary) focus position (FOSA)", true, RTS2_VALUE_WRITABLE);

	createValue (parkHa, "park_ha", "[deg] park position, hour axis", false, RTS2_VALUE_WRITABLE | RTS2_DT_DEGREES);
	parkHa->setValueDouble (90);
	createValue (parkDa, "park_da", "[deg] park position, declination axis", false, RTS2_VALUE_WRITABLE | RTS2_DT_DEGREES);
	parkDa->setValueDouble (45);
	createValue (moveTimeout, "move_timeout", "[s] a slew or park not finished by then fails", false, RTS2_VALUE_WRITABLE);
	moveTimeout->setValueDouble (600);

	createValue (ascolAnswer, "ascol_answer", "answer to the last ascol command", false);

	addOption (OPT_TCS_HOST, "tcs-host", 1, "telescoped host (default 192.168.193.195)");
	addOption (OPT_TCS_PORT, "tcs-port", 1, "telescoped port (default 9999)");
	addOption (OPT_POLL, "poll-interval", 1, "[s] state poll interval (default 1)");
	addOption (OPT_RPC_TIMEOUT, "rpc-timeout", 1, "[s] XML-RPC call timeout (default 5)");
	addOption (OPT_READONLY, "readonly", 0, "monitor only: refuse every command");
	addOption (OPT_PARK_HA, "park-ha", 1, "[deg] park hour axis position (default 90)");
	addOption (OPT_PARK_DA, "park-da", 1, "[deg] park declination axis position (default 45)");
}

Perek2m::~Perek2m ()
{
	tcs.stop ();
}

int Perek2m::processOption (int in_opt)
{
	switch (in_opt)
	{
		case OPT_TCS_HOST:
			tcs.setHost (optarg);
			break;
		case OPT_TCS_PORT:
			tcs.setPort (atoi (optarg));
			break;
		case OPT_POLL:
			tcs.setPollInterval (atof (optarg));
			break;
		case OPT_RPC_TIMEOUT:
			tcs.setTimeout (atof (optarg));
			break;
		case OPT_READONLY:
			readOnly->setValueBool (true);
			break;
		case OPT_PARK_HA:
			parkHa->setValueDouble (atof (optarg));
			break;
		case OPT_PARK_DA:
			parkDa->setValueDouble (atof (optarg));
			break;
		default:
			return Telescope::processOption (in_opt);
	}
	return 0;
}

int Perek2m::initValues ()
{
	rts2core::Configuration *config = rts2core::Configuration::instance ();
	if (config->loadFile ())
		return -1;
	setTelLongLat (config->getObserver ()->lng, config->getObserver ()->lat);
	setTelAltitude (config->getObservatoryAltitude ());
	return Telescope::initValues ();
}

int Perek2m::init ()
{
	int ret = Telescope::init ();
	if (ret)
		return ret;
	tcsUrl->setValueString (tcs.getUrl ());
	if (readOnly->getValueBool ())
		logStream (MESSAGE_INFO) << "read-only: the TCS is monitored, every command is refused" << sendLog;
	tcs.start ();
	addTimer (1, new rts2core::Event (EVENT_PEREK2M_POLL));
	return 0;
}

void Perek2m::recordError (const std::string &err)
{
	logStream (MESSAGE_ERROR) << err << sendLog;
	lastError->setValueString (err);
	sendValueAll (lastError);
}

int Perek2m::tcsCommand (const std::string &cmd)
{
	if (readOnly->getValueBool ())
	{
		recordError (cmd + " refused: read-only");
		return -1;
	}
	std::string err;
	if (tcs.command (cmd, err))
	{
		recordError (err);
		return -1;
	}
	logStream (MESSAGE_DEBUG) << "TCS " << cmd << sendLog;
	return 0;
}

int Perek2m::info ()
{
	bool conn = tcs.getInfo (tcsInfo);

	if (conn != tcsConnected)
	{
		tcsConnected = conn;
		if (conn)
		{
			logStream (MESSAGE_INFO) << "telescoped " << tcs.getUrl () << " is answering" << sendLog;
			maskState (DEVICE_ERROR_MASK, 0, "TCS answering");
		}
		else
		{
			logStream (MESSAGE_ERROR) << "telescoped " << tcs.getUrl () << " is not answering: " << tcs.getLastError () << sendLog;
			maskState (DEVICE_ERROR_MASK, DEVICE_ERROR_HW, "TCS not answering");
		}
	}
	tcsConn->setValueBool (conn);

	if (!tcsInfo.valid)
		return Telescope::info ();

	using namespace perek2m;

	tcsUt->setValueString (tcsInfo.ut);
	std::string g;
	for (int i = 0; i < GLST_FIELDS; i++)
		g += (i ? " " : "") + std::to_string (tcsInfo.glst[i]);
	glstRaw->setValueString (g);
	oilState->setValueString (glstName (GLST_OIL, tcsInfo.glst[GLST_OIL]));
	telState->setValueString (glstName (GLST_TEL, tcsInfo.glst[GLST_TEL]));
	haState->setValueString (glstName (GLST_HA, tcsInfo.glst[GLST_HA]));
	daState->setValueString (glstName (GLST_DA, tcsInfo.glst[GLST_DA]));
	focState->setValueString (glstName (GLST_FOCUS, tcsInfo.glst[GLST_FOCUS]));
	int bits = tcsInfo.glst[GLST_BITS];
	glstBits->setValueInteger (bits);
	if (bits >= 0)
	{
		corAberration->setValueBool (bits & 0x10);
		corPrecession->setValueBool (bits & 0x20);
		corRefraction->setValueBool (bits & 0x40);
		corModel->setValueBool (bits & 0x80);
		guideMode->setValueBool (bits & 0x100);
	}
	tcsTracking->setValueBool (tcsInfo.glst[GLST_TEL] == TEL_ST_TRACK);

	tcsPos->setValueRaDec (tcsInfo.ra, tcsInfo.dec);
	tcsTarget->setValueRaDec (tcsInfo.tarRa, tcsInfo.tarDec);
	tcsObject->setValueString (tcsInfo.object);
	pierSide->setValueString (tcsInfo.side == 0 ? "east" : (tcsInfo.side == 1 ? "west" : "unknown"));
	if (tcsInfo.side >= 0)
		telFlip->setValueInteger (tcsInfo.side);
	haAxis->setValueDouble (tcsInfo.haAxis);
	daAxis->setValueDouble (tcsInfo.decAxis);
	guideRa->setValueDouble (tcsInfo.guideRa);
	guideDec->setValueDouble (tcsInfo.guideDec);
	userRa->setValueDouble (tcsInfo.userRa);
	userDec->setValueDouble (tcsInfo.userDec);
	modelNumber->setValueInteger (tcsInfo.model);
	telFocus->setValueDouble (tcsInfo.focus);

	// TEL is the mount's apparent position: TRRD is J2000, so precess it
	// (aberration and nutation are below what TEL is used for)
	if (!std::isnan (tcsInfo.ra) && !std::isnan (tcsInfo.dec))
	{
		struct ln_equ_posn j2000, now;
		j2000.ra = tcsInfo.ra;
		j2000.dec = tcsInfo.dec;
		ln_get_equ_prec (&j2000, ln_get_julian_from_sys (), &now);
		setTelRaDec (now.ra, now.dec);
	}

	return Telescope::info ();
}

void Perek2m::postEvent (rts2core::Event *event)
{
	if (event->getType () == EVENT_PEREK2M_POLL)
	{
		// distribute the TCS state every second; the base's own info ()
		// runs only every refresh_idle (60 s) while nothing moves
		infoAll ();
		addTimer (1, event);
		return;
	}
	Telescope::postEvent (event);
}

int Perek2m::startResync ()
{
	// the snapshot from the last poll, not the last info (): a command can
	// arrive right after the TCS changed state
	info ();
	if (readOnly->getValueBool ())
	{
		recordError ("move refused: read-only");
		return -1;
	}
	if (!tcsConnected || !tcsInfo.valid)
	{
		recordError ("move refused: telescoped is not answering");
		return -1;
	}
	if (!tcsInfo.ready () && !tcsInfo.slewing ())
	{
		recordError (std::string ("move refused: telescope is ") + perek2m::glstName (perek2m::GLST_TEL, tcsInfo.glst[perek2m::GLST_TEL]) + " (power it on with the dome open sequence first)");
		return -1;
	}

	// J2000 origin with RTS2 offsets, plus the astrometric corrections the
	// base applied on top of the apparent target (small, so the same in J2000)
	struct ln_equ_posn pos, tar, telTar;
	getOrigin (&pos);
	applyOffsets (&pos);
	getTarget (&tar);
	getTelTargetRaDec (&telTar);
	if (!std::isnan (telTar.ra) && !std::isnan (tar.ra))
	{
		double dra = telTar.ra - tar.ra;
		if (dra > 180)
			dra -= 360;
		else if (dra < -180)
			dra += 360;
		pos.ra += dra;
		pos.dec += telTar.dec - tar.dec;
	}
	pos.ra = ln_range_degrees (pos.ra);

	int side;
	switch (moveSideSel->getValueInteger ())
	{
		case 1:
			side = 0;
			break;
		case 2:
			side = 1;
			break;
		default:
			side = tcsInfo.side >= 0 ? tcsInfo.side : 0;
	}

	std::string err;
	if (tcs.setCoordinates (pos.ra, pos.dec, side, "RTS2", err))
	{
		recordError (err);
		return -1;
	}

	moveRa = pos.ra;
	moveDec = pos.dec;
	moveSide = side;
	moveStarted = getNow ();
	moveSeenSlewing = false;
	moveAborted = false;
	moveStage = MOVE_PRESET;

	logStream (MESSAGE_INFO) << "preset " << perek2m::formatRa (pos.ra) << " " << perek2m::formatDec (pos.dec) << " side " << (side ? "west" : "east") << sendLog;
	return 0;
}

bool Perek2m::targetReached ()
{
	if (std::isnan (tcsInfo.ra) || std::isnan (tcsInfo.dec))
		return false;
	double dra = fabs (tcsInfo.ra - moveRa);
	if (dra > 180)
		dra = 360 - dra;
	return dra * cos (ln_deg_to_rad (moveDec)) <= ARRIVE_TOLERANCE
		&& fabs (tcsInfo.dec - moveDec) <= ARRIVE_TOLERANCE
		&& tcsInfo.side == moveSide;
}

int Perek2m::isMoving ()
{
	info ();

	if (moveStage == MOVE_NONE)
	{
		if (moveAborted)
		{
			moveAborted = false;
			return -1;
		}
		return -2;
	}

	if (!tcsConnected)
	{
		recordError ("move failed: telescoped stopped answering");
		moveStage = MOVE_NONE;
		return -1;
	}

	if (getNow () - moveStarted > moveTimeout->getValueDouble ())
	{
		recordError ("move failed: not on target after " + std::to_string ((int) moveTimeout->getValueDouble ()) + " s");
		std::string err;
		tcs.command ("TGRA 0", err);
		moveStage = MOVE_NONE;
		return -1;
	}

	if (moveStage == MOVE_PRESET)
	{
		// observe_batch sleeps 2 s here; wait until TSRA shows the preset instead
		double dra = fabs (tcsInfo.tarRa - moveRa);
		if (dra > 180)
			dra = 360 - dra;
		if (std::isnan (dra) || dra > PRESET_TOLERANCE || fabs (tcsInfo.tarDec - moveDec) > PRESET_TOLERANCE || tcsInfo.tarSide != moveSide)
		{
			if (getNow () - moveStarted > 15)
			{
				recordError ("move failed: the TCS did not take the preset");
				moveStage = MOVE_NONE;
				return -1;
			}
			return USEC_SEC / 4;
		}
		if (tcsCommand ("TGRA 1"))
		{
			moveStage = MOVE_NONE;
			return -1;
		}
		moveStage = MOVE_SLEW;
		return USEC_SEC;
	}

	// MOVE_SLEW
	if (tcsInfo.slewing ())
	{
		moveSeenSlewing = true;
		return USEC_SEC;
	}

	int st = tcsInfo.glst[perek2m::GLST_TEL];
	if (st == TEL_ST_TRACK && targetReached ())
	{
		// the dome follows the telescope: wait until it stops as well
		if (domeFollowing () && tcsInfo.glst[perek2m::GLST_DOME] != DOME_ST_AUTO_STOP)
			return USEC_SEC;
		moveStage = MOVE_NONE;
		return -2;
	}

	// stopped or powered off after it slewed, or never started
	if (st != TEL_ST_TRACK && st != TEL_ST_STOP)
	{
		recordError (std::string ("move failed: telescope went to ") + perek2m::glstName (perek2m::GLST_TEL, st));
		moveStage = MOVE_NONE;
		return -1;
	}
	if (st == TEL_ST_STOP && moveSeenSlewing)
	{
		recordError ("move failed: telescope stopped off target");
		moveStage = MOVE_NONE;
		return -1;
	}
	return USEC_SEC;
}

int Perek2m::stopMove ()
{
	if (moveStage == MOVE_NONE)
		return 0;
	moveStage = MOVE_NONE;
	if (readOnly->getValueBool ())
		return 0;
	return tcsCommand ("TGRA 0");
}

int Perek2m::abortMoveTracking ()
{
	// base note: Telescope::info () calls this whenever TEL is below the hard
	// horizon, and the default stops tracking. The 2m has its own limits and
	// is often moved by hand, so RTS2 only aborts its own slew; it never
	// stops the TCS for a position it did not command. Before the first poll
	// TEL is NaN, which must not count as below the horizon either.
	if (moveStage == MOVE_NONE || !tcsInfo.valid)
		return 1;
	logStream (MESSAGE_ERROR) << "slew below the horizon, stopping it" << sendLog;
	stopMove ();
	moveAborted = true;
	return 0;
}

int Perek2m::startPark ()
{
	// the snapshot from the last poll, not the last info (): a command can
	// arrive right after the TCS changed state
	info ();
	if (readOnly->getValueBool ())
	{
		recordError ("park refused: read-only");
		return -1;
	}
	if (!tcsConnected || !tcsInfo.ready ())
	{
		recordError ("park refused: telescope is not ready");
		return -1;
	}
	moveStage = MOVE_NONE;
	char cmd[80];
	snprintf (cmd, sizeof (cmd), "TSHA %.4f %.4f", parkHa->getValueDouble (), parkDa->getValueDouble ());
	if (tcsCommand ("TETR 0") || tcsCommand (cmd) || tcsCommand ("TGHA 1"))
		return -1;
	moveStarted = getNow ();
	return 0;
}

int Perek2m::isParking ()
{
	info ();
	if (!tcsConnected)
	{
		recordError ("park failed: telescoped stopped answering");
		return -1;
	}
	if (getNow () - moveStarted > moveTimeout->getValueDouble ())
	{
		recordError ("park failed: not at the park position after " + std::to_string ((int) moveTimeout->getValueDouble ()) + " s");
		return -1;
	}
	// observe_batch: parked when GLST telescope/HA/Dec are 3 1 1; also check
	// the axes, since that is true before the move starts too
	if (tcsInfo.glst[perek2m::GLST_TEL] == TEL_ST_STOP
		&& tcsInfo.glst[perek2m::GLST_HA] == AXIS_POSITION
		&& tcsInfo.glst[perek2m::GLST_DA] == AXIS_POSITION
		&& fabs (tcsInfo.haAxis - parkHa->getValueDouble ()) <= PARK_TOLERANCE
		&& fabs (tcsInfo.decAxis - parkDa->getValueDouble ()) <= PARK_TOLERANCE)
		return -2;
	return USEC_SEC;
}

int Perek2m::setValue (rts2core::Value *oldValue, rts2core::Value *newValue)
{
	if (oldValue == readOnly)
	{
		// switching it is local: nothing is sent to the TCS
		logStream (MESSAGE_INFO) << (((rts2core::ValueBool *) newValue)->getValueBool () ? "read-only: every command is refused from now on" : "read-only off: commands go to the TCS") << sendLog;
		return 0;
	}
	char cmd[80];
	if (oldValue == tcsTracking)
		return tcsCommand (((rts2core::ValueBool *) newValue)->getValueBool () ? "TETR 1" : "TETR 0") ? -2 : 0;
	if (oldValue == corAberration)
		return tcsCommand (((rts2core::ValueBool *) newValue)->getValueBool () ? "TSCA 1" : "TSCA 0") ? -2 : 0;
	if (oldValue == corPrecession)
		return tcsCommand (((rts2core::ValueBool *) newValue)->getValueBool () ? "TSCP 1" : "TSCP 0") ? -2 : 0;
	if (oldValue == corRefraction)
		return tcsCommand (((rts2core::ValueBool *) newValue)->getValueBool () ? "TSCR 1" : "TSCR 0") ? -2 : 0;
	if (oldValue == corModel)
		return tcsCommand (((rts2core::ValueBool *) newValue)->getValueBool () ? "TSCM 1" : "TSCM 0") ? -2 : 0;
	if (oldValue == guideMode)
		return tcsCommand (((rts2core::ValueBool *) newValue)->getValueBool () ? "TSGM 1" : "TSGM 0") ? -2 : 0;
	if (oldValue == modelNumber)
	{
		snprintf (cmd, sizeof (cmd), "TSCS %d", newValue->getValueInteger ());
		return tcsCommand (cmd) ? -2 : 0;
	}
	if (oldValue == telFocus)
	{
		snprintf (cmd, sizeof (cmd), "FOSA %.2f", newValue->getValueDouble ());
		return (tcsCommand (cmd) || tcsCommand ("FOGA")) ? -2 : 0;
	}
	return Telescope::setValue (oldValue, newValue);
}

int Perek2m::commandAuthorized (rts2core::Connection *conn)
{
	if (conn->isCommand ("ascol"))
	{
		// the rest of the line is the raw ASCOL command
		char *cmd;
		if (conn->paramNextString (&cmd))
			return -2;
		std::string full = cmd;
		while (!conn->paramEnd ())
		{
			if (conn->paramNextString (&cmd))
				return -2;
			full += std::string (" ") + cmd;
		}
		if (readOnly->getValueBool ())
		{
			conn->sendCommandEnd (DEVDEM_E_PARAMSVAL, "read-only");
			return -1;
		}
		try
		{
			std::string ans = tcs.execute (full);
			ascolAnswer->setValueString (ans);
			sendValueAll (ascolAnswer);
			logStream (MESSAGE_INFO) << "ascol " << full << " => " << ans << sendLog;
			return 0;
		}
		catch (rts2core::XmlRpcError &er)
		{
			recordError ("ascol " + full + " failed: " + er.what ());
			conn->sendCommandEnd (DEVDEM_E_HW, er.what ());
			return -1;
		}
	}
	return Telescope::commandAuthorized (conn);
}

int main (int argc, char **argv)
{
	Perek2m device (argc, argv);
	return device.run ();
}
