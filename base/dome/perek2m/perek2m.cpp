/*
 * Perek 2m dome (Ondrejov): slit, flaps, dome rotation and the telescope
 * power sequence, through Jan Fuchs' telescoped.
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

#include "dome.h"
#include "perek2m.h"

#include <cmath>

#define OPT_TCS_HOST       OPT_LOCAL + 2311
#define OPT_TCS_PORT       OPT_LOCAL + 2312
#define OPT_POLL           OPT_LOCAL + 2313
#define OPT_RPC_TIMEOUT    OPT_LOCAL + 2314
#define OPT_READONLY       OPT_LOCAL + 2315
#define OPT_PARK_HA        OPT_LOCAL + 2316
#define OPT_PARK_DA        OPT_LOCAL + 2317
#define OPT_KEEP_POWER     OPT_LOCAL + 2318

#define EVENT_PEREK2M_DOME_POLL  RTS2_LOCAL_EVENT + 1231

// park position reached, axis degrees
#define PARK_TOLERANCE     0.05
// dome at its close azimuth, degrees
#define DOME_AZ_TOLERANCE  1.0

using namespace perek2m;

namespace rts2dome
{

/**
 * The RTS2 dome of the Perek 2m. Open and close are the whole evening and
 * morning (or bad weather) sequences, since on the 2m the slit is useless
 * without the telescope powered and the flaps open:
 *
 * open:  oil on (OION 1), wait for ON; telescope on (TEON 1), wait for
 *        STOP; mirror and tube flaps and slit open (FMOC 1, FTOC 1,
 *        DOSO 1), wait for OPEN; dome rotation automatic (DOAM).
 * close: at once, slit and flaps close (DOSO 0, FTOC 0, FMOC 0), the dome
 *        stops and turns to its close azimuth (DOST, DOSA, DOGA) and the
 *        telescope goes to its park position (TETR 0, TSHA, TGHA 1),
 *        finishing whatever it did; once all of that is done, telescope
 *        off (TEON 0) and, when the telescope is OFF, oil off (OION 0).
 *
 * Nothing else is switched: in particular never the instruments' power.
 *
 * The 2m is shared with observers using Fuky's clients, so the dome is
 * built with inhibit_auto_close: auto_close starts false, and only explicit
 * open/close commands act until it is set. With auto_close false, neither
 * centrald's state changes nor bad weather move anything.
 */
class Perek2m:public Dome
{
	public:
		Perek2m (int argc, char **argv);
		virtual ~Perek2m ();

	protected:
		virtual int processOption (int in_opt);
		virtual int init ();
		virtual int info ();
		virtual void postEvent (rts2core::Event *event);
		virtual int setValue (rts2core::Value *oldValue, rts2core::Value *newValue);

		virtual int startOpen ();
		virtual long isOpened ();
		virtual int endOpen ();
		virtual int startClose ();
		virtual long isClosed ();
		virtual int endClose ();
		virtual int stop ();

	private:
		Tcs tcs;
		TcsInfo tcsInfo;
		bool tcsConnected;

		enum { SEQ_NONE, SEQ_OPEN, SEQ_CLOSE } seq;
		int seqStep;
		double seqStarted;
		double stepStarted;

		rts2core::ValueString *tcsUrl;
		rts2core::ValueBool *tcsConn;
		rts2core::ValueBool *readOnly;
		rts2core::ValueString *lastError;
		rts2core::ValueString *sequence;
		rts2core::ValueDouble *seqTimeout;

		rts2core::ValueString *oilState;
		rts2core::ValueString *telState;
		rts2core::ValueString *slitState;
		rts2core::ValueString *tubeState;
		rts2core::ValueString *mirrorState;
		rts2core::ValueString *domeState;
		rts2core::ValueDouble *domeAz;

		rts2core::ValueBool *slitOpen;
		rts2core::ValueBool *tubeOpen;
		rts2core::ValueBool *mirrorOpen;
		rts2core::ValueBool *domeAuto;
		rts2core::ValueDouble *domeTarget;

		rts2core::ValueDouble *parkHa;
		rts2core::ValueDouble *parkDa;
		rts2core::ValueDouble *closeAz;
		rts2core::ValueBool *powerOff;

		int tcsCommand (const std::string &cmd);
		void recordError (const std::string &err);
		void setSequence (const char *what);

		bool coversOpen () { return tcsInfo.glst[GLST_SLIT] == COVER_OPEN && tcsInfo.glst[GLST_TUBE_FLAP] == COVER_OPEN && tcsInfo.glst[GLST_MIRROR_FLAP] == COVER_OPEN; }
		bool coversClosed () { return tcsInfo.glst[GLST_SLIT] == COVER_CLOSE && tcsInfo.glst[GLST_TUBE_FLAP] == COVER_CLOSE && tcsInfo.glst[GLST_MIRROR_FLAP] == COVER_CLOSE; }
		bool powered () { return tcsInfo.glst[GLST_OIL] == OIL_ON && tcsInfo.ready (); }
		bool poweredOff () { return tcsInfo.glst[GLST_OIL] == OIL_OFF && tcsInfo.glst[GLST_TEL] == TEL_ST_OFF; }
		bool parked ();
		bool domeStopped ();

		// a step waits for its condition at most this long
		bool seqTimedOut ();
};

}

using namespace rts2dome;

Perek2m::Perek2m (int argc, char **argv):Dome (argc, argv, DEVICE_TYPE_DOME, true)
{
	tcsConnected = false;
	seq = SEQ_NONE;
	seqStep = 0;
	seqStarted = stepStarted = 0;

	createValue (tcsUrl, "tcs_url", "telescoped XML-RPC server", false);
	createValue (tcsConn, "tcs_conn", "telescoped is answering", false);
	createValue (readOnly, "tcs_readonly", "monitor only: every command is refused (--readonly sets it at start)", false, RTS2_VALUE_WRITABLE);
	readOnly->setValueBool (false);
	createValue (lastError, "last_error", "last refused or failed TCS command", false);
	createValue (sequence, "sequence", "open/close sequence step", false);
	sequence->setValueString ("idle");
	createValue (seqTimeout, "seq_timeout", "[s] an open or close sequence not finished by then fails", false, RTS2_VALUE_WRITABLE);
	seqTimeout->setValueDouble (900);

	createValue (oilState, "OIL", "oil system", true);
	createValue (telState, "TELSTATE", "telescope state (TCS)", false);
	createValue (slitState, "SLIT", "dome slit", true);
	createValue (tubeState, "TUBEFLAP", "tube flap", true);
	createValue (mirrorState, "MIRRFLAP", "mirror flap", true);
	createValue (domeState, "DOMESTAT", "dome rotation", true);
	createValue (domeAz, "DOME_AZ", "[deg] dome azimuth", true, RTS2_DT_DEGREES);

	createValue (slitOpen, "slit_open", "slit: open (DOSO)", false, RTS2_VALUE_WRITABLE);
	createValue (tubeOpen, "tube_flap_open", "tube flap: open (FTOC)", false, RTS2_VALUE_WRITABLE);
	createValue (mirrorOpen, "mirror_flap_open", "mirror flap: open (FMOC)", false, RTS2_VALUE_WRITABLE);
	createValue (domeAuto, "dome_auto", "dome follows the telescope (DOAM / DOST)", false, RTS2_VALUE_WRITABLE);
	createValue (domeTarget, "dome_goto", "[deg] turn the dome to this azimuth (DOSA, DOGA)", false, RTS2_VALUE_WRITABLE | RTS2_DT_DEGREES);

	createValue (parkHa, "park_ha", "[deg] telescope park position for close, hour axis", false, RTS2_VALUE_WRITABLE | RTS2_DT_DEGREES);
	parkHa->setValueDouble (90);
	createValue (parkDa, "park_da", "[deg] telescope park position for close, declination axis", false, RTS2_VALUE_WRITABLE | RTS2_DT_DEGREES);
	parkDa->setValueDouble (45);
	createValue (closeAz, "close_az", "[deg] dome azimuth for close (NaN: leave the dome where it is)", false, RTS2_VALUE_WRITABLE | RTS2_DT_DEGREES);
	closeAz->setValueDouble (0);
	createValue (powerOff, "power_off", "close switches the telescope and the oil off", false, RTS2_VALUE_WRITABLE);
	powerOff->setValueBool (true);

	addOption (OPT_TCS_HOST, "tcs-host", 1, "telescoped host (default 192.168.193.195)");
	addOption (OPT_TCS_PORT, "tcs-port", 1, "telescoped port (default 9999)");
	addOption (OPT_POLL, "poll-interval", 1, "[s] state poll interval (default 1)");
	addOption (OPT_RPC_TIMEOUT, "rpc-timeout", 1, "[s] XML-RPC call timeout (default 5)");
	addOption (OPT_READONLY, "readonly", 0, "monitor only: refuse every command");
	addOption (OPT_PARK_HA, "park-ha", 1, "[deg] telescope park hour axis position (default 90)");
	addOption (OPT_PARK_DA, "park-da", 1, "[deg] telescope park declination axis position (default 45)");
	addOption (OPT_KEEP_POWER, "keep-power", 0, "close leaves the telescope and the oil on");
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
		case OPT_KEEP_POWER:
			powerOff->setValueBool (false);
			break;
		default:
			return Dome::processOption (in_opt);
	}
	return 0;
}

int Perek2m::init ()
{
	int ret = Dome::init ();
	if (ret)
		return ret;
	tcsUrl->setValueString (tcs.getUrl ());
	if (readOnly->getValueBool ())
		logStream (MESSAGE_INFO) << "read-only: the dome is monitored, every command is refused" << sendLog;
	tcs.start ();
	addTimer (1, new rts2core::Event (EVENT_PEREK2M_DOME_POLL));
	return 0;
}

void Perek2m::recordError (const std::string &err)
{
	logStream (MESSAGE_ERROR) << err << sendLog;
	lastError->setValueString (err);
	sendValueAll (lastError);
}

void Perek2m::setSequence (const char *what)
{
	sequence->setValueString (what);
	sendValueAll (sequence);
	logStream (MESSAGE_DEBUG) << "sequence: " << what << sendLog;
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

	if (tcsInfo.valid)
	{
		oilState->setValueString (glstName (GLST_OIL, tcsInfo.glst[GLST_OIL]));
		telState->setValueString (glstName (GLST_TEL, tcsInfo.glst[GLST_TEL]));
		slitState->setValueString (glstName (GLST_SLIT, tcsInfo.glst[GLST_SLIT]));
		tubeState->setValueString (glstName (GLST_TUBE_FLAP, tcsInfo.glst[GLST_TUBE_FLAP]));
		mirrorState->setValueString (glstName (GLST_MIRROR_FLAP, tcsInfo.glst[GLST_MIRROR_FLAP]));
		domeState->setValueString (glstName (GLST_DOME, tcsInfo.glst[GLST_DOME]));
		domeAz->setValueDouble (tcsInfo.domeAz);
		slitOpen->setValueBool (tcsInfo.glst[GLST_SLIT] == COVER_OPEN || tcsInfo.glst[GLST_SLIT] == COVER_OPENING);
		tubeOpen->setValueBool (tcsInfo.glst[GLST_TUBE_FLAP] == COVER_OPEN || tcsInfo.glst[GLST_TUBE_FLAP] == COVER_OPENING);
		mirrorOpen->setValueBool (tcsInfo.glst[GLST_MIRROR_FLAP] == COVER_OPEN || tcsInfo.glst[GLST_MIRROR_FLAP] == COVER_OPENING);
		int d = tcsInfo.glst[GLST_DOME];
		domeAuto->setValueBool (d >= DOME_ST_AUTO_STOP && d <= DOME_ST_AUTO_STOP + 2);
	}
	return Dome::info ();
}

void Perek2m::postEvent (rts2core::Event *event)
{
	if (event->getType () == EVENT_PEREK2M_DOME_POLL)
	{
		infoAll ();
		addTimer (1, event);
		return;
	}
	Dome::postEvent (event);
}

int Perek2m::setValue (rts2core::Value *oldValue, rts2core::Value *newValue)
{
	if (oldValue == readOnly)
	{
		// switching it is local: nothing is sent to the TCS
		logStream (MESSAGE_INFO) << (((rts2core::ValueBool *) newValue)->getValueBool () ? "read-only: every command is refused from now on" : "read-only off: commands go to the TCS") << sendLog;
		return 0;
	}
	if (oldValue == slitOpen)
		return tcsCommand (((rts2core::ValueBool *) newValue)->getValueBool () ? "DOSO 1" : "DOSO 0") ? -2 : 0;
	if (oldValue == tubeOpen)
		return tcsCommand (((rts2core::ValueBool *) newValue)->getValueBool () ? "FTOC 1" : "FTOC 0") ? -2 : 0;
	if (oldValue == mirrorOpen)
		return tcsCommand (((rts2core::ValueBool *) newValue)->getValueBool () ? "FMOC 1" : "FMOC 0") ? -2 : 0;
	if (oldValue == domeAuto)
		return tcsCommand (((rts2core::ValueBool *) newValue)->getValueBool () ? "DOAM" : "DOST") ? -2 : 0;
	if (oldValue == domeTarget)
	{
		char cmd[50];
		snprintf (cmd, sizeof (cmd), "DOSA %.2f", ln_range_degrees (newValue->getValueDouble ()));
		return (tcsCommand ("DOST") || tcsCommand (cmd) || tcsCommand ("DOGA")) ? -2 : 0;
	}
	return Dome::setValue (oldValue, newValue);
}

bool Perek2m::parked ()
{
	return tcsInfo.glst[GLST_TEL] == TEL_ST_STOP
		&& tcsInfo.glst[GLST_HA] == AXIS_POSITION
		&& tcsInfo.glst[GLST_DA] == AXIS_POSITION
		&& fabs (tcsInfo.haAxis - parkHa->getValueDouble ()) <= PARK_TOLERANCE
		&& fabs (tcsInfo.decAxis - parkDa->getValueDouble ()) <= PARK_TOLERANCE;
}

bool Perek2m::domeStopped ()
{
	int d = tcsInfo.glst[GLST_DOME];
	if (d != DOME_ST_STOP && d != DOME_ST_OFF)
		return false;
	if (std::isnan (closeAz->getValueDouble ()))
		return true;
	double diff = fabs (ln_range_degrees (tcsInfo.domeAz - closeAz->getValueDouble ()));
	return diff <= DOME_AZ_TOLERANCE || diff >= 360 - DOME_AZ_TOLERANCE;
}

bool Perek2m::seqTimedOut ()
{
	if (getNow () - seqStarted <= seqTimeout->getValueDouble ())
		return false;
	recordError (std::string (seq == SEQ_OPEN ? "open" : "close") + " sequence not finished after " + std::to_string ((int) seqTimeout->getValueDouble ()) + " s, at: " + sequence->getValueString ());
	return true;
}

int Perek2m::startOpen ()
{
	if (readOnly->getValueBool ())
	{
		recordError ("open refused: read-only");
		return -1;
	}
	info ();
	if (!tcsConnected)
	{
		recordError ("open refused: telescoped is not answering");
		return -1;
	}
	seq = SEQ_OPEN;
	seqStep = 0;
	seqStarted = stepStarted = getNow ();
	setSequence ("open: starting");
	return 0;
}

long Perek2m::isOpened ()
{
	info ();
	if (seq != SEQ_OPEN)
		// outside the sequence: opened is the slit and flaps open and the telescope powered
		return (tcsInfo.valid && coversOpen () && powered ()) ? -2 : 0;

	if (!tcsConnected)
	{
		recordError ("open failed: telescoped stopped answering");
		seq = SEQ_NONE;
		setSequence ("idle (open failed)");
		return -1;
	}
	if (seqTimedOut ())
	{
		seq = SEQ_NONE;
		setSequence ("idle (open timed out)");
		return -1;
	}

	switch (seqStep)
	{
		case 0:
			// oil
			if (tcsInfo.glst[GLST_OIL] != OIL_ON)
			{
				if (tcsInfo.glst[GLST_OIL] == OIL_OFF || tcsInfo.glst[GLST_OIL] == OIL_OFF_DELAY)
				{
					if (tcsCommand ("OION 1"))
						break;
				}
				setSequence ("open: oil starting");
			}
			seqStep = 1;
			return USEC_SEC;
		case 1:
			if (tcsInfo.glst[GLST_OIL] != OIL_ON)
				return USEC_SEC;
			// telescope power
			if (!tcsInfo.ready () && !tcsInfo.slewing ())
			{
				if (tcsCommand ("TEON 1"))
					break;
				setSequence ("open: telescope powering on");
			}
			seqStep = 2;
			return USEC_SEC;
		case 2:
			if (!tcsInfo.ready () && !tcsInfo.slewing ())
				return USEC_SEC;
			// flaps and slit
			if (tcsInfo.glst[GLST_MIRROR_FLAP] != COVER_OPEN && tcsCommand ("FMOC 1"))
				break;
			if (tcsInfo.glst[GLST_TUBE_FLAP] != COVER_OPEN && tcsCommand ("FTOC 1"))
				break;
			if (tcsInfo.glst[GLST_SLIT] != COVER_OPEN && tcsCommand ("DOSO 1"))
				break;
			setSequence ("open: slit and flaps opening");
			seqStep = 3;
			return USEC_SEC;
		case 3:
			if (!coversOpen ())
				return USEC_SEC;
			// the dome follows the telescope from now on
			if (tcsCommand ("DOAM"))
				break;
			seq = SEQ_NONE;
			setSequence ("idle (opened)");
			return -2;
	}
	// a command was refused
	seq = SEQ_NONE;
	setSequence ("idle (open failed)");
	return -1;
}

int Perek2m::endOpen ()
{
	return 0;
}

int Perek2m::startClose ()
{
	if (readOnly->getValueBool ())
	{
		recordError ("close refused: read-only");
		return -1;
	}
	info ();
	if (!tcsConnected)
	{
		recordError ("close refused: telescoped is not answering");
		return -1;
	}

	// everything that protects the telescope starts at once; a refused
	// command is recorded, but the others are still sent
	int failed = 0;
	failed += tcsCommand ("DOSO 0") ? 1 : 0;
	failed += tcsCommand ("FTOC 0") ? 1 : 0;
	failed += tcsCommand ("FMOC 0") ? 1 : 0;
	failed += tcsCommand ("DOST") ? 1 : 0;
	if (!std::isnan (closeAz->getValueDouble ()))
	{
		char cmd[50];
		snprintf (cmd, sizeof (cmd), "DOSA %.2f", ln_range_degrees (closeAz->getValueDouble ()));
		failed += (tcsCommand (cmd) || tcsCommand ("DOGA")) ? 1 : 0;
	}
	// park, finishing whatever the telescope does; nothing to do when it is off
	if (tcsInfo.ready () || tcsInfo.slewing ())
	{
		char cmd[80];
		snprintf (cmd, sizeof (cmd), "TSHA %.4f %.4f", parkHa->getValueDouble (), parkDa->getValueDouble ());
		if (tcsInfo.slewing ())
			failed += tcsCommand ("TGRA 0") ? 1 : 0;
		failed += (tcsCommand ("TETR 0") || tcsCommand (cmd) || tcsCommand ("TGHA 1")) ? 1 : 0;
	}
	if (failed)
		logStream (MESSAGE_WARNING) << failed << " close command(s) refused, waiting for what was accepted" << sendLog;

	seq = SEQ_CLOSE;
	seqStep = 0;
	seqStarted = stepStarted = getNow ();
	setSequence ("close: slit, flaps, dome, park");
	return 0;
}

long Perek2m::isClosed ()
{
	info ();
	if (seq != SEQ_CLOSE)
	{
		// outside the sequence: closed is the slit and flaps closed and,
		// unless power is kept, the telescope and oil off
		if (!tcsInfo.valid)
			return 0;
		return (coversClosed () && (!powerOff->getValueBool () || poweredOff ())) ? -2 : 0;
	}

	if (!tcsConnected)
	{
		recordError ("close failed: telescoped stopped answering");
		seq = SEQ_NONE;
		setSequence ("idle (close failed)");
		return -1;
	}
	if (seqTimedOut ())
	{
		seq = SEQ_NONE;
		setSequence ("idle (close timed out)");
		return -1;
	}

	switch (seqStep)
	{
		case 0:
		{
			bool telDone = parked () || tcsInfo.glst[GLST_TEL] == TEL_ST_OFF || tcsInfo.glst[GLST_TEL] == TEL_ST_OFF_WAIT;
			if (!coversClosed () || !domeStopped () || !telDone)
				return USEC_SEC;
			if (!powerOff->getValueBool ())
			{
				seq = SEQ_NONE;
				setSequence ("idle (closed, power kept)");
				return -2;
			}
			if (tcsInfo.glst[GLST_TEL] != TEL_ST_OFF && tcsInfo.glst[GLST_TEL] != TEL_ST_OFF_WAIT)
			{
				if (tcsCommand ("TEON 0"))
					break;
			}
			setSequence ("close: telescope powering off");
			seqStep = 1;
			return USEC_SEC;
		}
		case 1:
			// oil only once the telescope is OFF
			if (tcsInfo.glst[GLST_TEL] != TEL_ST_OFF)
				return USEC_SEC;
			if (tcsInfo.glst[GLST_OIL] != OIL_OFF && tcsInfo.glst[GLST_OIL] != OIL_OFF_DELAY)
			{
				if (tcsCommand ("OION 0"))
					break;
			}
			setSequence ("close: oil off");
			seqStep = 2;
			return USEC_SEC;
		case 2:
			if (tcsInfo.glst[GLST_OIL] != OIL_OFF)
				return USEC_SEC;
			seq = SEQ_NONE;
			setSequence ("idle (closed)");
			return -2;
	}
	seq = SEQ_NONE;
	setSequence ("idle (close failed)");
	return -1;
}

int Perek2m::endClose ()
{
	return 0;
}

int Perek2m::stop ()
{
	// stops the sequence and the dome rotation; slit, flaps and the
	// telescope finish what they were told
	seq = SEQ_NONE;
	setSequence ("idle (stopped)");
	if (readOnly->getValueBool ())
		return 0;
	return tcsCommand ("DOST");
}

int main (int argc, char **argv)
{
	Perek2m device (argc, argv);
	return device.run ();
}
