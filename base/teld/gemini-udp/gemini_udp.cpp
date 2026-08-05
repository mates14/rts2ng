/*
 * Driver for Losmandy Gemini-2, using the async Gemini UDP protocol.
 * Copyright (C) 2026 Martin Jelinek <mates@iaa.es>
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

// base note: wholly new driver, sibling of base/teld/gemini/ (the RS232
// port of the classic src/teld/gemini.cpp). Deliberately does NOT extend
// TelLX200: that class's tel_read_*/tel_write_* helpers are hard-wired to
// blocking ConnSerial::writeRead() calls, which is exactly the coupling
// this driver exists to avoid.
//
// All mount communication lives in rts2teld::GeminiCaringLoop
// (geminicaringloop.h/.cpp), a dedicated thread ("caring loop" - the
// user's own term from an earlier Python RTS2 mixin) that owns the UDP
// socket and speaks the wire protocol with ordinary blocking calls - see
// that file's base note and UPSTREAM_BUGS.md for why a single-threaded,
// event-loop-integrated design was tried first and abandoned after it
// stack-overflowed on a live mount (sendAndWait() pumped Block::
// oneRunLoop() from inside idle(), which re-entered idle() before its own
// state had advanced - unbounded recursion). This driver's side of that
// boundary is intentionally trivial: idle() copies a GeminiStatus
// snapshot (cheap: mutex lock + struct copy) and reflects it into Values;
// info() does nothing else at all. Network-side clients (rts2-mon,
// multiple simultaneous observers, ...) can therefore never flood the
// mount no matter how often they ask for status, and the RTS2 thread
// itself is never blocked on the network for longer than a mutex lock -
// except for startResync(), which blocks up to 3s on a real
// condition_variable wait (GeminiCaringLoop::gotoRaDec()) to satisfy
// Telescope::startResyncMove()'s synchronous accept/reject contract. That
// one wait is safe for the same reason libmks3's termios VTIME-based
// serial reads are safe (~/paracl/libmks3.c, used by
// base/teld/paramount/paramount.cpp): it's a real OS-level block on a
// dedicated thread, not application code pumping its own dispatcher.

#include "teld.h"
#include "geminicaringloop.h"
#include "hoststring.h"
#include "configuration.h"

#include <libnova/libnova.h>
#include <cmath>

// native Gemini register IDs, see base/teld/gemini/gemini.cpp
#define GEMINI_CMD_RATE_GUIDE    150
#define GEMINI_CMD_RATE_CENTER   170

#define OPT_AUTO_RECOVERY        OPT_LOCAL + 1

namespace rts2teld
{

class GeminiUDP:public Telescope
{
	public:
		GeminiUDP (int argc, char **argv);
		virtual ~GeminiUDP ();

	protected:
		virtual int processOption (int in_opt);
		virtual int initHardware ();
		virtual int initValues ();
		virtual int info ();
		virtual int idle ();
		virtual int setValue (rts2core::Value *oldValue, rts2core::Value *newValue);

		virtual int isMoving ();
		virtual int startResync ();
		virtual int stopMove ();
		virtual int endMove ();

		// :hP#/:h?# - same command pair as base/teld/gemini/gemini.cpp's
		// startPark()/isParking(), ported from the live production driver
		// (~/gemini2ser.cpp), not the classic tree copy.
		virtual int startPark ();
		virtual int isParking ();
		virtual int endPark ();

		// sync (not slew) - tells the mount it's currently at (set_ra,
		// set_dec). Realizes the "setto"/"synccorr" client commands
		// (Telescope::setTo() defaults to -1/unimplemented) - see
		// GeminiCaringLoop::syncTo()'s doc comment for the command choice.
		virtual int setTo (double set_ra, double set_dec);

		// tracking-limit flip-or-park decision, see armLimitAction()'s doc
		// comment for the whole scheme
		virtual void setFullBopState (rts2_status_t new_state);

	private:
		HostString *host;
		GeminiCaringLoop *caring;

		// mirrors of GeminiStatus fields not already covered by
		// Telescope's own telRaDec/telAltAz - see idle()
		// HA/AZ/ALT/LST are NOT duplicated here - Telescope::info() (via
		// infoLST()) already derives and publishes all four from
		// telRaDec + site/time on every call, see teld.cpp:1921-1932
		rts2core::ValueString *pierSideValue;
		rts2core::ValueString *moveRateValue;
		rts2core::ValueLong *praRawValue;
		rts2core::ValueLong *pdecRawValue;
		rts2core::ValueString *extendedStatusValue;
		rts2core::ValueString *lastMoveErrorValue;
		rts2core::ValueString *parkStatusValue;

		// raw, checksum-verified, deliberately unparsed contents of the
		// mount's own configured HA/meridian safety-limit registers - see
		// GeminiUDP::initValues(). Read-only visibility only: nothing in
		// this driver currently pre-validates a target against these
		// before sending it (see STATUS.md's safety-review note) - the
		// only enforcement today is Gemini's own firmware-side rejection.
		rts2core::ValueString *limitBothRawValue;	// native 220 - both limits
		rts2core::ValueString *limitEastRawValue;	// native 221 - eastern safety limit
		rts2core::ValueString *limitWestRawValue;	// native 222 - western safety limit
		rts2core::ValueString *limitWestGotoRawValue;	// native 223 - western goto limit

		// native 226, polled every cycle - seconds of tracking left before
		// Gemini's own firmware hits the western limit and just stops (no
		// flip, no warning of its own). See GeminiCaringLoop::
		// pollTrackingLimit()'s doc comment and armLimitAction() below for
		// what acts on this.
		rts2core::ValueDouble *trackingSecToLimitValue;
		bool trackingLimitWarned;

		// ---- tracking-limit flip-or-park decision ----
		// User-specified policy: if the current target is still reachable
		// (above horizon) once the tracking limit is close, nudge it (a
		// same-target re-goto, which :MS# will flip if needed - see
		// STATUS.md for why :MS# alone, not :MM#); if it's set, park.
		// Either way, this MUST NOT interrupt an in-progress exposure -
		// block new exposures (BOP_EXPOSURE) the moment the decision is
		// armed, then wait for BOP_TEL_MOVE to clear (no camera currently
		// demanding the telescope hold still) before actually moving -
		// same coordination as gemini2ser.cpp's flippingRequested/
		// setFullBopState(), just generalized to cover park too.
		enum LimitAction { LIMIT_ACTION_NONE, LIMIT_ACTION_FLIP, LIMIT_ACTION_PARK } pendingLimitAction;
		bool limitActionInFlight;	// true from the moment the goto/park actually starts until endMove()/endPark() sees it through - keeps the exposure block up for the whole move, not just until it's accepted

		void armLimitAction ();
		void executeLimitAction (LimitAction action);

		// writable mount parameters, native-command backed - see
		// GEMINI_CMD_RATE_GUIDE/CENTER above and
		// GeminiCaringLoop::queueNativeSet(). Defaults match
		// base/teld/gemini/gemini.cpp's own (also-never-read-back)
		// defaults for the same parameters.
		rts2core::ValueFloat *guidingSpeedValue;
		rts2core::ValueInteger *centeringSpeedValue;
		rts2core::ValueFloat *pollIntervalValue;

		// pulse-guide inputs, same name/units convention as
		// base/teld/d50/d50.cpp's guidePulseRA/DEC ("[ms], negative changes
		// direction") - RTS2's guide scripts (python/rts2/guide.py,
		// guideccd.py) write these directly on T0. Realized via
		// GeminiCaringLoop::queuePulseGuide() (the ":Mi" command - see its
		// doc comment; matches gemini2ser.cpp's performGuide() exactly).
		// See setValue() for the direction-sign convention and guard.
		rts2core::ValueInteger *pulseGuideRaValue;
		rts2core::ValueInteger *pulseGuideDecValue;

		// ---- EXPERIMENTAL, off by default (--experimental-auto-recovery)
		// ---- see the long comment at armAutoRecovery()'s definition for
		// why this is scoped down so far from gemini2ser.cpp's own
		// resetMount()/telMotorState machinery, which it's inspired by but
		// does NOT faithfully replicate - this has never been exercised
		// against a real hung mount, over this transport, at all.
		bool autoRecoveryEnabled;
		double disconnectedSince;	// NAN while connected; caring loop poll timestamp of the first poll that came back disconnected, otherwise
		bool autoRecoveryAttempted;	// one attempt per outage - cleared the moment we see connected=true again
		void checkAutoRecovery (const GeminiStatus &st);

		void applyStatus (const GeminiStatus &st);

		// ---- one-shot live self-test, gated by --live-slew-test ----
		bool liveSlewTest;
		bool dryRun;
		double testDecOffset;
		bool returnMode;
		double returnDec;
		enum TestState { TEST_OFF, TEST_WAIT_BASELINE, TEST_SLEW_OUT, TEST_CONFIRM_ARRIVAL, TEST_SLEW_BACK, TEST_CONFIRM_RETURN, TEST_DONE } testState;
		double testStartRa, testStartDec, testTargetRa, testTargetDec;

		void runSelfTest ();
		bool altitudeSafe (double raDeg, double decDeg, double marginDeg);
		bool doGoto (double raDeg, double decDeg, const char *label);

		// ---- pier-side-aware pointing model correction ----
		// naive (uncorrected) target of the move currently in flight, and
		// whether we've already sent the near-arrival correction for it -
		// see checkMoveCorrection()'s doc comment for the whole scheme.
		double pendingMoveNaiveRa, pendingMoveNaiveDec;
		bool moveCorrectionApplied;

		void computeModelCorrection (double raDeg, double decDeg, double lstDeg, char actualPierSide, double &corrRaDeg, double &corrDecDeg);
		void checkMoveCorrection (const GeminiStatus &st);
};

}

using namespace rts2teld;

GeminiUDP::GeminiUDP (int argc, char **argv):Telescope (argc, argv, true, true)
{
	host = nullptr;
	caring = nullptr;

	liveSlewTest = false;
	dryRun = false;
	testDecOffset = 3.0;
	returnMode = false;
	returnDec = NAN;
	testState = TEST_OFF;
	testStartRa = testStartDec = testTargetRa = testTargetDec = NAN;

	pendingMoveNaiveRa = pendingMoveNaiveDec = NAN;
	moveCorrectionApplied = true;	// nothing pending until a move actually starts

	autoRecoveryEnabled = false;
	disconnectedSince = NAN;
	autoRecoveryAttempted = false;

	createValue (pierSideValue, "pier_side", "side of pier (W/E, see :Gm#)", false);
	createValue (moveRateValue, "move_rate", "current movement rate (N/T/G/C/S, see :Gv#)", false);
	createValue (praRawValue, "CNT_RA", "RA axis raw encoder count", true);
	createValue (pdecRawValue, "CNT_DEC", "DEC axis raw encoder count", true);
	createValue (extendedStatusValue, "ext_status_raw", "unverified trailing fields of the 0x05 ENQ macro reply - see geminicaringloop.cpp's parseEnq()", false);
	createValue (lastMoveErrorValue, "last_move_error", "reason the last move failed (arrived nowhere near target, or timed out) - empty if the last move succeeded", false);
	createValue (parkStatusValue, "park_status_raw", "raw :h?# response char (1=parked, 2/space=parking, 0=called without a park in progress)", false);

	createValue (limitBothRawValue, "limit_both_raw", "native 220 (eastern; western safety limit), raw unparsed - NOT currently enforced client-side, see STATUS.md", false);
	createValue (limitEastRawValue, "limit_east_raw", "native 221 (eastern safety limit), raw unparsed - NOT currently enforced client-side, see STATUS.md", false);
	createValue (limitWestRawValue, "limit_west_raw", "native 222 (western safety limit), raw unparsed - NOT currently enforced client-side, see STATUS.md", false);
	createValue (limitWestGotoRawValue, "limit_west_goto_raw", "native 223 (western goto limit), raw unparsed - NOT currently enforced client-side, see STATUS.md", false);
	createValue (trackingSecToLimitValue, "tracking_sec_to_limit", "native 226: seconds of tracking left before Gemini's own firmware hits the western limit and stops - triggers armLimitAction() below 660s", false);
	trackingLimitWarned = false;
	pendingLimitAction = LIMIT_ACTION_NONE;
	limitActionInFlight = false;

	createValue (guidingSpeedValue, "guiding_speed", "speed used for guiding the mount on target", false, RTS2_VALUE_WRITABLE);
	guidingSpeedValue->setValueFloat (0.8);
	createValue (centeringSpeedValue, "centering_speed", "speed used for centering the mount on target", false, RTS2_VALUE_WRITABLE);
	centeringSpeedValue->setValueInteger (2);
	createValue (pollIntervalValue, "poll_interval", "seconds between the caring loop's async status polls", false, RTS2_VALUE_WRITABLE);
	pollIntervalValue->setValueFloat (1.0);

	createValue (pulseGuideRaValue, "pulse_guide_ra", "[ms] time to guide in RA, negative changes direction", false, RTS2_VALUE_WRITABLE);
	pulseGuideRaValue->setValueInteger (0);
	createValue (pulseGuideDecValue, "pulse_guide_dec", "[ms] time to guide in DEC, negative changes direction", false, RTS2_VALUE_WRITABLE);
	pulseGuideDecValue->setValueInteger (0);

	addOption ('e', "gemini-udp", 1, "IP and port (separated by :) of the Gemini-2 mount's UDP interface (default port 11110)");
	addOption ('T', "live-slew-test", 0, "run a one-shot self-test: small Dec-only slew away from the current position, then back. Read current position first, checks altitude margin before moving.");
	addOption ('D', "test-dec-offset", 1, "declination offset in degrees for --live-slew-test (default 3.0, always applied towards the equator)");
	addOption ('N', "dry-run", 0, "with --live-slew-test: compute and log the plan (target, altitude, exact commands) but never send :MS#/:Q# - status polling still runs normally");
	addOption ('R', "return-to-dec", 1, "one-shot mode: slew to this declination (same RA as currently read), confirm arrival, stop tracking, done - no return leg. For restoring a known position after a test.");
	addOption (OPT_AUTO_RECOVERY, "experimental-auto-recovery", 0, "EXPERIMENTAL, off by default: after 60s of total silence from the mount, send one native warm-reboot command. Unverified over UDP - see checkAutoRecovery()'s doc comment before enabling this unattended.");
}

GeminiUDP::~GeminiUDP ()
{
	delete caring;
	delete host;
}

int GeminiUDP::processOption (int in_opt)
{
	switch (in_opt)
	{
		case 'e':
			host = new HostString (optarg, "11110");
			break;
		case 'T':
			liveSlewTest = true;
			break;
		case 'D':
			testDecOffset = atof (optarg);
			break;
		case 'N':
			dryRun = true;
			break;
		case 'R':
			returnMode = true;
			liveSlewTest = true;
			returnDec = atof (optarg);
			break;
		case OPT_AUTO_RECOVERY:
			autoRecoveryEnabled = true;
			break;
		default:
			return Telescope::processOption (in_opt);
	}
	return 0;
}

int GeminiUDP::initHardware ()
{
	if (host == nullptr)
	{
		logStream (MESSAGE_ERROR) << "You must specify IP:port of the Gemini-2 mount's UDP interface (-e option)." << sendLog;
		return -1;
	}

	setIdleInfoInterval (1);

	caring = new GeminiCaringLoop (host->getHostname (), host->getPort ());
	if (!caring->start ())
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: failed to open UDP socket to " << host->getHostname () << ":" << host->getPort () << sendLog;
		return -1;
	}

	return 0;
}

int GeminiUDP::initValues ()
{
	rts2core::Configuration *config = rts2core::Configuration::instance ();
	if (config->loadFile ())
		return -1;

	setTelLongLat (config->getObserver ()->lng, config->getObserver ()->lat);
	setTelAltitude (config->getObservatoryAltitude ());

	if (liveSlewTest)
	{
		testState = TEST_WAIT_BASELINE;
		logStream (MESSAGE_INFO) << "GeminiUDP: --live-slew-test armed" << (returnMode ? " [RETURN MODE]" : "") << sendLog;
	}

	// one-time, read-only: see the raw note at limitBothRawValue's
	// createValue() above for why nothing acts on these yet
	if (caring)
	{
		struct { int id; rts2core::ValueString *value; const char *label; } limits[] = {
			{ 220, limitBothRawValue, "220 (both limits)" },
			{ 221, limitEastRawValue, "221 (eastern safety limit)" },
			{ 222, limitWestRawValue, "222 (western safety limit)" },
			{ 223, limitWestGotoRawValue, "223 (western goto limit)" },
		};
		for (auto &l : limits)
		{
			std::string raw;
			if (caring->readNativeRaw (l.id, raw, 3.0))
			{
				l.value->setValueCharArr (raw.c_str ());
				logStream (MESSAGE_INFO) << "GeminiUDP: native " << l.label << " = \"" << raw << "\"" << sendLog;
			}
			else
			{
				logStream (MESSAGE_ERROR) << "GeminiUDP: failed to read native " << l.label << sendLog;
			}
		}

		// once at startup: every HA/LST-derived decision this driver makes
		// (pier-side prediction, the tracking-limit warning, ...) trusts
		// the mount's own clock - see GeminiCaringLoop::matchTimeUtc()'s
		// doc comment for the exact command sequence (ported from
		// TelLX200::matchTime()).
		std::string err;
		if (caring->matchTimeUtc (err, 3.0))
			logStream (MESSAGE_INFO) << "GeminiUDP: matched mount clock to system UTC" << sendLog;
		else
			logStream (MESSAGE_ERROR) << "GeminiUDP: failed to match mount clock to system UTC: " << err << sendLog;
	}

	return Telescope::initValues ();
}

int GeminiUDP::setValue (rts2core::Value *oldValue, rts2core::Value *newValue)
{
	if (oldValue == guidingSpeedValue)
	{
		if (caring)
			caring->queueNativeSet (GEMINI_CMD_RATE_GUIDE, newValue->getValueDouble ());
		return 0;
	}
	if (oldValue == centeringSpeedValue)
	{
		if (caring)
			caring->queueNativeSet (GEMINI_CMD_RATE_CENTER, (int32_t) newValue->getValueInteger ());
		return 0;
	}
	if (oldValue == pollIntervalValue)
	{
		if (caring)
			caring->setPollInterval (newValue->getValueDouble ());
		return 0;
	}
	if (oldValue == pulseGuideRaValue || oldValue == pulseGuideDecValue)
	{
		int ms = newValue->getValueInteger ();
		// convention and guard match gemini2ser.cpp's performGuide()
		// exactly (the live production driver's verified precedent):
		// positive -> east/north, negative -> west/south; only realized
		// in state == TEL_OBSERVING (not moving, not parked, no other
		// mask bit) and not getBlockMove() - a stricter check than just
		// isTracking(), which alone wouldn't necessarily exclude parked
		char direction = (oldValue == pulseGuideRaValue) ? (ms >= 0 ? 'e' : 'w') : (ms >= 0 ? 'n' : 's');
		bool suitable = caring != nullptr && ms != 0
			&& (getState () & TEL_MASK_MOVING) == TEL_OBSERVING
			&& isTracking () && !getBlockMove ();
		if (suitable)
			caring->queuePulseGuide (direction, (unsigned int) (ms >= 0 ? ms : -ms));
		// always reset to 0 (rather than leaving the just-committed
		// magnitude in place) - the framework only re-invokes this hook
		// when the incoming value actually differs from the stored one
		// (see Daemon::doSetValue()'s isEqual() short-circuit), so leaving
		// e.g. "255" in place would silently swallow the next identical
		// guide pulse. Mutating newValue here means the framework commits
		// 0 as the stored value on our behalf, no separate sendValueAll needed.
		newValue->setValueInteger (0);
		return 0;
	}
	return Telescope::setValue (oldValue, newValue);
}

void GeminiUDP::applyStatus (const GeminiStatus &st)
{
	maskState (DEVICE_ERROR_HW, st.connected ? 0 : DEVICE_ERROR_HW, st.connected ? "mount communication OK" : "no response from mount over UDP");

	if (st.connected)
	{
		disconnectedSince = NAN;
		autoRecoveryAttempted = false;
	}
	else
	{
		if (std::isnan (disconnectedSince))
			disconnectedSince = getNow ();
		checkAutoRecovery (st);
	}

	if (!st.valid)
		return;

	setTelRaDec (st.ra, st.dec);
	// telFlip (MNT_FLIP) drives Telescope::infoUTCLST()'s rotang +180 deg
	// adjustment and FITS headers - gemini2ser.cpp's getFlip() derives it
	// from encoder ticks (native 235) vs. a per-mount decFlipLimit; we
	// already have the mount's own W/E answer from the ENQ macro (:Gm#
	// equivalent), which is more direct - 'E' -> flipped (1), matching
	// that driver's "decTick >= decFlipLimit -> 1" convention (east side
	// of pier = flipped orientation)
	if (st.pierSide == 'E' || st.pierSide == 'W')
		telFlip->setValueInteger (st.pierSide == 'E' ? 1 : 0);
	pierSideValue->setValueCharArr (std::string (1, st.pierSide).c_str ());
	moveRateValue->setValueCharArr (std::string (1, st.moveRate).c_str ());
	praRawValue->setValueLong (st.praRaw);
	pdecRawValue->setValueLong (st.pdecRaw);
	extendedStatusValue->setValueCharArr (st.rawExtended.c_str ());
	lastMoveErrorValue->setValueCharArr (st.moveFailed ? st.moveFailReason.c_str () : "");
	parkStatusValue->setValueCharArr (std::string (1, st.parkStatus).c_str ());
	trackingSecToLimitValue->setValueDouble (st.trackingSecToWestLimit);

	// same 660s (11 min) proactive threshold as gemini2ser.cpp's info().
	// Hysteresis (only re-arms above 900s) so this doesn't re-trigger
	// once a second while sitting under the threshold - armLimitAction()
	// itself only ever fires once per approach (pendingLimitAction /
	// limitActionInFlight guard against re-arming mid-flight too).
	if (!std::isnan (st.trackingSecToWestLimit))
	{
		if (st.trackingSecToWestLimit < 660.0 && !trackingLimitWarned
			&& pendingLimitAction == LIMIT_ACTION_NONE && !limitActionInFlight
			&& isTracking () && (getState () & TEL_MASK_MOVING) != TEL_MOVING)
		{
			trackingLimitWarned = true;
			armLimitAction ();
		}
		else if (st.trackingSecToWestLimit > 900.0)
		{
			trackingLimitWarned = false;
		}
	}

	logStream (MESSAGE_DEBUG) << "GeminiUDP: caring loop poll OK, RA=" << st.ra << " Dec=" << st.dec
		<< " HA=" << st.ha << " AZ=" << st.az << " ALT=" << st.alt << " LST=" << st.lst
		<< " pier=" << st.pierSide << " rate=" << st.moveRate
		<< " trackingSecToLimit=" << st.trackingSecToWestLimit << sendLog;
}

// EXPERIMENTAL, off by default (--experimental-auto-recovery). Loosely
// inspired by gemini2ser.cpp's telMotorState/resetMount() machinery, but
// deliberately NOT a faithful port of it - the two failure modes aren't
// the same thing:
//
// Production's trigger is "the RS232 register-99 query itself failed
// while we expected the mount to be active" - a blocking serial call
// coming back with an I/O error, which on that transport plausibly means
// the mount's firmware (or the USB-serial bridge) has genuinely wedged.
// Its response is a whole state machine: reboot (:65535/:65533 native
// registers, chosen by warm/cold/restart), wait, then re-park and re-
// issue whatever move was interrupted, tracked across many info() cycles.
//
// Our GeminiCaringLoop already retries silently through the datagram
// NACK/resync protocol on every single poll (see sendAndReceive()) -
// that's the direct analogue of production's per-call retry, and it
// already runs unconditionally, not behind this flag. What st.connected
// == false actually means here is "every resync attempt in an entire
// poll cycle failed" - which could be the mount genuinely wedged, or
// could just as easily be a flaky network path, a firewall hiccup, or
// (as happened for real earlier this same project) another client
// stepping on the mount at the same time. Rebooting the mount is not an
// obviously-safe response to any of those other cases.
//
// So this only ports the narrow, low-risk half: notice sustained silence
// and try ONE thing (the same native "warm" reboot register production
// uses by default) - no automatic re-park, no automatic resume of an
// interrupted move, no cold-start option, no retry loop beyond the one
// attempt per outage. Whether a reboot command sent over a UDP socket
// that isn't currently getting ANY response even reaches the mount is
// untested; whether Gemini's UDP listener survives/rebinds after a
// reboot at all is untested. This is here so the behavior exists and is
// visible/logged for a human to evaluate, not because it's been shown to
// help - hence gating it behind an explicit flag, off by default, and
// logging at MESSAGE_CRITICAL when it fires so it's impossible to miss.
void GeminiUDP::checkAutoRecovery (const GeminiStatus &st)
{
	if (!autoRecoveryEnabled || caring == nullptr || autoRecoveryAttempted)
		return;

	constexpr double DISCONNECT_RECOVERY_THRESHOLD_SEC = 60.0;
	double outageSec = getNow () - disconnectedSince;
	if (outageSec < DISCONNECT_RECOVERY_THRESHOLD_SEC)
		return;

	autoRecoveryAttempted = true;
	logStream (MESSAGE_CRITICAL) << "GeminiUDP: EXPERIMENTAL auto-recovery firing - no response from mount for "
		<< outageSec << "s, sending native warm-reboot command (register 65535). This path is unverified "
		<< "over UDP; if it doesn't visibly help within a minute or two, intervene manually rather than "
		<< "waiting on it." << sendLog;
	caring->queueNativeSet (65535, (int32_t) 0);
}

int GeminiUDP::info ()
{
	// deliberately does nothing but reflect whatever the caring loop's
	// last poll wrote into the registry - see base note at the top of
	// this file
	return Telescope::info ();
}

bool GeminiUDP::altitudeSafe (double raDeg, double decDeg, double marginDeg)
{
	struct ln_equ_posn pos;
	pos.ra = raDeg;
	pos.dec = decDeg;
	struct ln_lnlat_posn observer;
	observer.lat = telLatitude->getValueDouble ();
	observer.lng = telLongitude->getValueDouble ();
	struct ln_hrz_posn hrz;
	ln_get_hrz_from_equ (&pos, &observer, ln_get_julian_from_sys (), &hrz);
	logStream (MESSAGE_INFO) << "GeminiUDP: target RA=" << raDeg << " Dec=" << decDeg << " -> alt=" << hrz.alt << " az=" << hrz.az << sendLog;
	return hrz.alt > marginDeg;
}

int GeminiUDP::startResync ()
{
	struct ln_equ_posn pos;
	getTarget (&pos);
	return doGoto (pos.ra, pos.dec, "framework-requested move") ? 0 : -1;
}

bool GeminiUDP::doGoto (double raDeg, double decDeg, const char *label)
{
	if (caring == nullptr)
		return false;

	std::string err;
	bool ok = caring->gotoRaDec (raDeg, decDeg, err, 3.0);
	if (!ok)
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: " << label << " refused: " << err << sendLog;
		return false;
	}
	logStream (MESSAGE_INFO) << "GeminiUDP: " << label << " accepted, RA=" << raDeg << " Dec=" << decDeg << sendLog;

	// arm the near-arrival model correction (checkMoveCorrection(), called
	// from idle()) for this new move - see its doc comment
	pendingMoveNaiveRa = raDeg;
	pendingMoveNaiveDec = decDeg;
	moveCorrectionApplied = false;

	return true;
}

// Works out, for a given real sky target and the pier side Gemini actually
// committed to, what the T-Point-corrected coordinates should be.
//
// The model's terms are evaluated in "unfolded" declination space (see
// base/teld/gemini/gemini.cpp's startResync() and base/teld/src/gem.cpp's
// GEM::sky2counts(), both already using this identity): the same real sky
// point (ra, dec) can be encoded two ways - (ra, dec) itself, or (ra,
// 180-dec / -180-dec) - and which encoding you feed the model determines
// which side's flexure/collimation correction comes out, because the
// model's trig terms evaluate differently outside +-90 even though no
// physical declination is ever actually outside that range.
//
// Standard GEM convention (matches ASCOM/INDI pierEast/pierWest and this
// codebase's own GEM/gemini.cpp): for a target's hour angle HA = LST-RA,
// HA > 0 (past meridian, setting) is naturally reached "pier east", HA < 0
// (before meridian, rising) is naturally reached "pier west" - so (ra,
// dec) as-is corresponds to whichever of E/W matches that sign, and the
// unfolded encoding corresponds to the other one.
void GeminiUDP::computeModelCorrection (double raDeg, double decDeg, double lstDeg, char actualPierSide, double &corrRaDeg, double &corrDecDeg)
{
	double ha = ln_range_degrees (lstDeg - raDeg);
	if (ha > 180.0)
		ha -= 360.0;
	char naturalSide = (ha > 0.0) ? 'E' : 'W';

	double unfoldedDec = decDeg;
	if (actualPierSide != naturalSide && actualPierSide != '?')
		unfoldedDec = decDeg < 0 ? -180.0 - decDeg : 180.0 - decDeg;

	struct ln_equ_posn modelPos;
	modelPos.ra = raDeg;
	modelPos.dec = unfoldedDec;

	struct ln_equ_posn realPos;
	realPos.ra = raDeg;
	realPos.dec = decDeg;
	struct ln_lnlat_posn observer;
	observer.lat = telLatitude->getValueDouble ();
	observer.lng = telLongitude->getValueDouble ();
	struct ln_hrz_posn hrz;
	double jd = ln_get_julian_from_sys ();
	ln_get_hrz_from_equ (&realPos, &observer, jd, &hrz);

	struct ln_equ_posn modelChange;
	computeModel (&modelPos, &hrz, &modelChange, jd, 0);

	corrRaDeg = raDeg - modelChange.ra;
	corrDecDeg = decDeg - modelChange.dec;

	logStream (MESSAGE_INFO) << "GeminiUDP: model correction for actual pier=" << actualPierSide
		<< " (natural=" << naturalSide << " at HA=" << ha << ", "
		<< (actualPierSide != naturalSide ? "flipped" : "direct") << " unfolded Dec=" << unfoldedDec << ")"
		<< " -> dRA=" << modelChange.ra << " dDec=" << modelChange.dec << sendLog;
}

// Sends the model-corrected retarget once a move is close enough to its
// naive destination that the pier side Gemini committed to is trustworthy
// (see the computeModelCorrection() doc comment for why "close enough"
// substitutes for "the flip has already happened, if any" - we don't have
// a way to know that more directly). Relies on the mid-slew retarget
// behavior verified live against real hardware (see STATUS.md): sending a
// new :Sr/:Sd/:MM# while already slewing makes Gemini smoothly redirect,
// no need to wait for arrival or stop first. Reuses
// GeminiCaringLoop::gotoRaDec() directly (not doGoto()) so it does NOT
// re-arm this same correction - doGoto() resets moveCorrectionApplied,
// which would otherwise retrigger forever as the corrected move also
// approaches its own (now-correct) target.
void GeminiUDP::checkMoveCorrection (const GeminiStatus &st)
{
	if (!st.valid || !st.moveInProgress || moveCorrectionApplied || caring == nullptr)
		return;

	double dRa = fabs (ln_range_degrees (st.ra - pendingMoveNaiveRa));
	double dDec = fabs (st.dec - pendingMoveNaiveDec);
	if (dRa > 1.0 || dDec > 1.0)
		return;

	moveCorrectionApplied = true;	// never retry, even if nothing to correct or the correction below fails

	double corrRa, corrDec;
	computeModelCorrection (pendingMoveNaiveRa, pendingMoveNaiveDec, st.lst, st.pierSide, corrRa, corrDec);

	// model is private on Telescope, not reachable from here - but
	// computeModel() itself already no-ops to a zero correction when no
	// model is loaded, so check the result instead of the pointer: skip a
	// pointless identical retarget rather than guess at "is one loaded".
	if (fabs (ln_range_degrees (corrRa - pendingMoveNaiveRa)) < (1.0 / 3600.0) && fabs (corrDec - pendingMoveNaiveDec) < (1.0 / 3600.0))
		return;

	logStream (MESSAGE_INFO) << "GeminiUDP: near arrival (dRA=" << dRa << " dDec=" << dDec << "), applying model correction: naive RA=" << pendingMoveNaiveRa << " Dec=" << pendingMoveNaiveDec
		<< " -> corrected RA=" << corrRa << " Dec=" << corrDec << sendLog;

	std::string err;
	if (!caring->gotoRaDec (corrRa, corrDec, err, 3.0))
		logStream (MESSAGE_ERROR) << "GeminiUDP: model-corrected retarget was refused: " << err << sendLog;
}

int GeminiUDP::setTo (double set_ra, double set_dec)
{
	if (caring == nullptr)
		return -1;

	std::string err;
	if (!caring->syncTo (set_ra, set_dec, err, 3.0))
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: sync refused: " << err << sendLog;
		return -1;
	}

	// re-anchoring to a known-true position makes any previously
	// accumulated incremental correction stale/double-counted - same as
	// gemini2ser.cpp's setTo() calling zeroCorrRaDec() up front
	zeroCorrRaDec ();
	setTelRaDec (set_ra, set_dec);

	logStream (MESSAGE_INFO) << "GeminiUDP: synced to RA=" << set_ra << " Dec=" << set_dec << sendLog;
	return 0;
}

int GeminiUDP::isMoving ()
{
	if (caring == nullptr)
		return -2;
	GeminiStatus st = caring->getStatus ();
	if (st.moveInProgress)
		return USEC_SEC;
	if (st.moveFailed)
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: " << st.moveFailReason << sendLog;
		return -1;
	}
	return -2;
}

int GeminiUDP::stopMove ()
{
	if (caring)
		caring->requestAbort ();
	return 0;
}

int GeminiUDP::startPark ()
{
	if (caring == nullptr)
		return -1;
	// gemini2ser.cpp's startPark() calls stopMove() first - requestPark()
	// does the equivalent (see GeminiCaringLoop::handlePark())
	caring->requestPark ();
	logStream (MESSAGE_INFO) << "GeminiUDP: park requested (:hP#)" << sendLog;
	return 0;
}

int GeminiUDP::isParking ()
{
	if (caring == nullptr)
		return -1;
	GeminiStatus st = caring->getStatus ();
	if (st.parking)
		return USEC_SEC;
	if (st.parkFailed)
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: park failed, :h?# reported '" << st.parkStatus << "' (0 = called without a park command in progress)" << sendLog;
		return -1;
	}
	return -2;
}

int GeminiUDP::endPark ()
{
	// gemini2ser.cpp's endPark() also does matchTime() here when not in
	// SERVERD_NIGHT state - the mount tends to sit parked across a full
	// day between observing runs, so this is the natural place to refresh
	// its clock before it's needed again.
	if (caring && getMasterState () != SERVERD_NIGHT)
	{
		std::string err;
		if (caring->matchTimeUtc (err, 3.0))
			logStream (MESSAGE_INFO) << "GeminiUDP: matched mount clock to system UTC (park exit)" << sendLog;
		else
			logStream (MESSAGE_ERROR) << "GeminiUDP: failed to match mount clock to system UTC: " << err << sendLog;
	}

	if (limitActionInFlight)
	{
		limitActionInFlight = false;
		clearExposure ();
		logStream (MESSAGE_INFO) << "GeminiUDP: tracking-limit park finished, exposures unblocked" << sendLog;
	}
	return 0;
}

int GeminiUDP::endMove ()
{
	int ret = Telescope::endMove ();
	if (limitActionInFlight)
	{
		limitActionInFlight = false;
		clearExposure ();
		logStream (MESSAGE_INFO) << "GeminiUDP: tracking-limit flip nudge finished, exposures unblocked" << sendLog;
	}
	return ret;
}

// Arms the flip-or-park decision (see the LimitAction/limitActionInFlight
// doc comment at their declaration) and blocks new exposures immediately -
// the actual move happens later, from setFullBopState(), once no camera
// is mid-exposure. Deciding flip-vs-park here (not later, at execute time)
// keeps the "is the target still up" check anchored to the moment the
// warning fired, matching this driver's usual "check once, act once"
// discipline rather than re-evaluating repeatedly while waiting.
void GeminiUDP::armLimitAction ()
{
	struct ln_equ_posn tar;
	getTelTargetRaDec (&tar);

	bool stillUp = altitudeSafe (tar.ra, tar.dec, 20.0);
	pendingLimitAction = stillUp ? LIMIT_ACTION_FLIP : LIMIT_ACTION_PARK;

	logStream (MESSAGE_WARNING) << "GeminiUDP: tracking limit approaching ("
		<< trackingSecToLimitValue->getValueDouble () << "s left) - target RA=" << tar.ra << " Dec=" << tar.dec
		<< (stillUp ? " still above horizon, will nudge for a flip" : " has set, will park")
		<< " as soon as no camera is mid-exposure" << sendLog;

	blockExposure ();
}

void GeminiUDP::executeLimitAction (LimitAction action)
{
	bool started = false;

	if (action == LIMIT_ACTION_FLIP)
	{
		struct ln_equ_posn tar;
		getTelTargetRaDec (&tar);
		logStream (MESSAGE_INFO) << "GeminiUDP: tracking-limit flip nudge, re-sending RA=" << tar.ra << " Dec=" << tar.dec << sendLog;
		limitActionInFlight = true;
		if (doGoto (tar.ra, tar.dec, "tracking-limit flip nudge"))
		{
			started = true;
		}
		else
		{
			logStream (MESSAGE_ERROR) << "GeminiUDP: flip nudge was refused, falling back to park" << sendLog;
			action = LIMIT_ACTION_PARK;
		}
	}

	if (!started && action == LIMIT_ACTION_PARK)
	{
		logStream (MESSAGE_INFO) << "GeminiUDP: tracking-limit park" << sendLog;
		limitActionInFlight = true;
		if (startPark () == 0)
			started = true;
	}

	if (!started)
	{
		// neither attempt even started - don't leave exposures blocked
		// forever over this
		logStream (MESSAGE_ERROR) << "GeminiUDP: could not start a flip nudge or a park - releasing the exposure block anyway to avoid a permanent stall" << sendLog;
		limitActionInFlight = false;
		clearExposure ();
	}
}

void GeminiUDP::setFullBopState (rts2_status_t new_state)
{
	Telescope::setFullBopState (new_state);
	if (pendingLimitAction != LIMIT_ACTION_NONE && !(new_state & BOP_TEL_MOVE))
	{
		LimitAction action = pendingLimitAction;
		pendingLimitAction = LIMIT_ACTION_NONE;
		executeLimitAction (action);
	}
}

void GeminiUDP::runSelfTest ()
{
	switch (testState)
	{
		case TEST_OFF:
		case TEST_DONE:
			return;

		case TEST_WAIT_BASELINE:
		{
			GeminiStatus st = caring->getStatus ();
			if (!st.valid)
				return;

			testStartRa = st.ra;
			testStartDec = st.dec;
			testTargetRa = testStartRa;
			if (returnMode)
			{
				testTargetDec = returnDec;
			}
			else
			{
				// always move towards the equator, away from the pole, so
				// a small offset can never itself flirt with the +/-90 edge
				testTargetDec = testStartDec > 0 ? testStartDec - fabs (testDecOffset) : testStartDec + fabs (testDecOffset);
			}

			logStream (MESSAGE_INFO) << "GeminiUDP self-test" << (returnMode ? " [RETURN MODE]" : "") << ": baseline RA=" << testStartRa << " Dec=" << testStartDec
				<< " -> planned target RA=" << testTargetRa << " Dec=" << testTargetDec << sendLog;

			if (!altitudeSafe (testTargetRa, testTargetDec, 30.0))
			{
				logStream (MESSAGE_ERROR) << "GeminiUDP self-test: computed target is not comfortably above horizon, aborting test (no move sent)" << sendLog;
				testState = TEST_DONE;
				return;
			}

			if (dryRun)
			{
				logStream (MESSAGE_INFO) << "GeminiUDP self-test [DRY RUN]: would goto RA=" << testTargetRa << " Dec=" << testTargetDec << " - stopping here, no command sent" << sendLog;
				testState = TEST_DONE;
				return;
			}

			if (!doGoto (testTargetRa, testTargetDec, "self-test outbound slew"))
			{
				testState = TEST_DONE;
				return;
			}
			testState = TEST_SLEW_OUT;
			return;
		}

		case TEST_SLEW_OUT:
		{
			GeminiStatus st = caring->getStatus ();
			if (st.moveInProgress)
				return;
			if (st.moveFailed)
			{
				logStream (MESSAGE_ERROR) << "GeminiUDP self-test: outbound slew FAILED: " << st.moveFailReason << sendLog;
				testState = TEST_DONE;
				return;
			}
			logStream (MESSAGE_INFO) << "GeminiUDP self-test: outbound slew finished, current RA=" << getTelRa () << " Dec=" << getTelDec () << sendLog;
			testState = TEST_CONFIRM_ARRIVAL;
			return;
		}

		case TEST_CONFIRM_ARRIVAL:
		{
			double dRa = fabs (ln_range_degrees (getTelRa () - testTargetRa));
			double dDec = fabs (getTelDec () - testTargetDec);
			logStream (MESSAGE_INFO) << "GeminiUDP self-test: arrival error dRA=" << dRa << " dDec=" << dDec << " deg" << sendLog;

			if (returnMode)
			{
				stopTracking (nullptr);
				stopMove ();
				logStream (MESSAGE_INFO) << "GeminiUDP self-test [RETURN MODE]: DONE, tracking stopped at RA=" << getTelRa () << " Dec=" << getTelDec () << sendLog;
				testState = TEST_DONE;
				return;
			}

			logStream (MESSAGE_INFO) << "GeminiUDP self-test: slewing back to baseline RA=" << testStartRa << " Dec=" << testStartDec << sendLog;
			if (!doGoto (testStartRa, testStartDec, "self-test return slew"))
			{
				logStream (MESSAGE_ERROR) << "GeminiUDP self-test: return slew was REFUSED - mount is left away from its starting position, manual intervention needed" << sendLog;
				testState = TEST_DONE;
				return;
			}
			testState = TEST_SLEW_BACK;
			return;
		}

		case TEST_SLEW_BACK:
		{
			GeminiStatus st = caring->getStatus ();
			if (st.moveInProgress)
				return;
			if (st.moveFailed)
			{
				logStream (MESSAGE_ERROR) << "GeminiUDP self-test: return slew FAILED: " << st.moveFailReason << " - mount is left away from its starting position, manual intervention needed" << sendLog;
				testState = TEST_DONE;
				return;
			}
			logStream (MESSAGE_INFO) << "GeminiUDP self-test: return slew finished, current RA=" << getTelRa () << " Dec=" << getTelDec () << sendLog;
			testState = TEST_CONFIRM_RETURN;
			return;
		}

		case TEST_CONFIRM_RETURN:
		{
			double dRa = fabs (ln_range_degrees (getTelRa () - testStartRa));
			double dDec = fabs (getTelDec () - testStartDec);
			logStream (MESSAGE_INFO) << "GeminiUDP self-test: return error dRA=" << dRa << " dDec=" << dDec << " deg" << sendLog;
			stopTracking (nullptr);
			stopMove ();
			logStream (MESSAGE_INFO) << "GeminiUDP self-test: DONE, tracking stopped, mount left at/near the original position - "
				<< "re-issue your normal park command if it needs to be flagged as parked" << sendLog;
			testState = TEST_DONE;
			return;
		}
	}
}

int GeminiUDP::idle ()
{
	if (caring)
	{
		GeminiStatus st = caring->getStatus ();
		applyStatus (st);
		checkMoveCorrection (st);
	}

	runSelfTest ();

	return Telescope::idle ();
}

int main (int argc, char **argv)
{
	GeminiUDP device = GeminiUDP (argc, argv);
	return device.run ();
}
