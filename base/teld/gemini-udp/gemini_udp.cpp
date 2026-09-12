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
#include <cstring>
#include <fstream>
#include <sstream>
#include <strings.h>

// native Gemini register IDs, see base/teld/gemini/gemini.cpp
#define GEMINI_CMD_RATE_GUIDE    150
#define GEMINI_CMD_RATE_CENTER   170

// tracking rate, native id 130 reads it back - 131 sidereal, 135
// "terrestrial", which is how both base/teld/gemini/gemini.cpp and the live
// production driver (~/gemini2ser.cpp's startWorm()/stopWorm()) start and
// stop the RA worm. See GeminiUDP::setTracking().
#define GEMINI_CMD_TRACK_SIDEREAL     131
#define GEMINI_CMD_TRACK_TERRESTRIAL  135

#define OPT_AUTO_RECOVERY        OPT_LOCAL + 1
#define OPT_STARTUP_MODE         OPT_LOCAL + 2
#define OPT_INCIDENT_LOG         OPT_LOCAL + 3
#define OPT_NO_SAFETY            OPT_LOCAL + 4
#define OPT_NO_COLDSTART         OPT_LOCAL + 5
#define OPT_MAX_RECOVERIES       OPT_LOCAL + 6

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

		// Gemini needs to be told to run its RA worm - nothing in the
		// framework's own tracking bookkeeping reaches the hardware. Until
		// this existed the driver slewed correctly and then just sat there
		// while the sky moved off the target (reported live from SBT,
		// 2026-09-12). Same two native registers as the production driver's
		// startWorm()/stopWorm().
		virtual int setTracking (int track, bool addTrackingTimer = false, bool send = true, const char *stopMsg = "tracking stopped");

		// "reset" command: reboots the mount in the mode startup_mode
		// selects, exactly as base/teld/gemini/gemini.cpp's resetMount()
		// does over RS232. Also the operator's way out of a safety lock.
		virtual int resetMount ();

		// the framework's own hard-horizon violation hook - routed into the
		// same incident machinery as our own checks, see checkSafety()
		virtual int abortMoveTracking ();

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

		// ---- startup handshake (see GeminiCaringLoop::pollStartupState) ----
		// The mount's boot menu is the whole reason "the new driver reads
		// status fine but ignores every command" was a thing: a Gemini that
		// has been power-cycled and not told which startup mode to use
		// answers queries normally and moves for nobody.
		rts2core::ValueSelection *startupModeValue;	// RESTART / WARM_START / COLD_START, same three as gemini.cpp's next_reset
		rts2core::ValueString *startupStateValue;	// raw handshake character: B/b/S/G/A
		rts2core::ValueBool *mountReadyValue;		// handshake reached 'G'/'A'
		rts2core::ValueBool *clockMatchedValue;		// last post-startup sequence set the mount clock to system UTC
		rts2core::ValueInteger *trackingRateValue;	// native 130 read back: 131 sidereal, 135 terrestrial/off
		unsigned lastStartupCount;			// GeminiStatus::startupCount as of the last time checkStartup() ran

		void checkStartup (const GeminiStatus &st);

		// ---- safety watchdog ----
		// The driver polls the mount once a second anyway, so it is in a
		// position to notice the mount misbehaving; user-specified policy
		// (SBT, 2026-09-12) is that when it does, it should stop, park -
		// which is the one operation that has proven reliable on this mount
		// - and then cold-start the controller, leaving a report behind.
		//
		// Conditions, all of them confirmed over several consecutive polls
		// before they count, so a single odd datagram can't start a recovery:
		//  - the mount is slewing or centering while the driver believes it
		//    is idle (note: a human on the hand controller looks exactly
		//    like this - turn safety_enabled off for on-site work)
		//  - a move in flight is travelling away from its target
		//    (GeminiStatus::moveWrongWay, suspended across a real flip)
		//  - a move ended nowhere near its target (GeminiStatus::moveFailed)
		//  - the mount is pointed below safety_alt_limit
		//  - the mount is tracking while parked - corrected in place first,
		//    escalated only if the correction doesn't stick
		enum SafetyState { SAFETY_OK, SAFETY_STOPPING, SAFETY_PARKING, SAFETY_COLDSTART, SAFETY_LOCKED };
		SafetyState safetyState;
		double safetyStateSince;
		std::string safetyReason;
		bool safetyWantsColdStart;
		unsigned safetyColdStartBaseline;	// startupCount when the cold start was requested - a change means the mount came back

		rts2core::ValueBool *safetyEnabledValue;
		rts2core::ValueBool *safetyLockedValue;		// writable: clearing it is the operator's "I've looked, carry on"
		rts2core::ValueDouble *safetyAltLimitValue;
		rts2core::ValueDouble *wrongWayMarginValue;
		rts2core::ValueString *safetyStateValue;
		rts2core::ValueString *lastIncidentValue;
		rts2core::ValueInteger *incidentCountValue;
		rts2core::ValueInteger *recoveriesUsedValue;

		const char *incidentLogPath;
		bool incidentLogFailed;		// only complain about an unwritable report file once
		int maxRecoveries;
		bool coldStartOnIncident;

		// Counters are in *mount polls*, not idle() ticks: idle() runs
		// whenever the event loop wakes up, which on a busy night is many
		// times a second and always on the same unchanged snapshot. Counting
		// those would turn "three consecutive polls" into "one poll, read
		// three times" and make every threshold here meaningless.
		double lastSafetyPollTimestamp;
		int unexpectedMoveCount;
		int belowHorizonCount;
		int parkedTrackingCount;
		int parkedTrackingCorrections;

		// GeminiStatus::moveFailed and ::moveWrongWay stay set until the
		// next accepted goto clears them - which, after a recovery that
		// ends in a park, may be a long time. Latch both so one bad move
		// is one incident.
		bool moveFailReported;
		bool wrongWayReported;

		void checkSafety (const GeminiStatus &st);
		void runSafetyRecovery (const GeminiStatus &st);
		void triggerIncident (const char *condition, const std::string &detail, bool escalate);
		void setSafetyState (SafetyState newState);
		void appendIncidentLine (const std::string &line);
		void writeIncidentReport (const GeminiStatus &st);

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

	createValue (startupModeValue, "startup_mode", "startup mode picked for the mount's own boot menu, and used by the reset command", false, RTS2_VALUE_WRITABLE);
	startupModeValue->addSelVal ("RESTART");	// bR#
	startupModeValue->addSelVal ("WARM_START");	// bW#
	startupModeValue->addSelVal ("COLD_START");	// bC#
	createValue (startupStateValue, "startup_state", "raw 0x06 handshake reply: B startup message, b boot menu, S cold start running, G/A startup complete", false);
	createValue (mountReadyValue, "mount_ready", "mount finished starting up and will actually act on motion commands", false);
	mountReadyValue->setValueBool (false);
	createValue (clockMatchedValue, "clock_matched", "mount clock was set to system UTC by the last post-startup sequence", false);
	clockMatchedValue->setValueBool (false);
	createValue (trackingRateValue, "tracking_rate", "native 130 read back: 131 sidereal, 132 King, 133 lunar, 134 solar, 135 terrestrial (= not tracking)", false);
	trackingRateValue->setValueInteger (0);
	lastStartupCount = 0;

	createValue (safetyEnabledValue, "safety_enabled", "watch the mount for unexpected movement, wrong-way slews and below-horizon pointing, and stop/park/cold-start it when found", false, RTS2_VALUE_WRITABLE);
	safetyEnabledValue->setValueBool (true);
	createValue (safetyLockedValue, "safety_locked", "an incident recovery finished and the mount is held blocked - set to false once you've looked at the incident log", false, RTS2_VALUE_WRITABLE);
	safetyLockedValue->setValueBool (false);
	createValue (safetyAltLimitValue, "safety_alt_limit", "[deg] altitude below which the mount's own reported position counts as an incident", false, RTS2_VALUE_WRITABLE);
	safetyAltLimitValue->setValueDouble (5.0);
	createValue (wrongWayMarginValue, "wrong_way_margin", "[deg] how far a move in flight may back away from its closest approach to the target before it counts as going the wrong way", false, RTS2_VALUE_WRITABLE);
	wrongWayMarginValue->setValueDouble (15.0);
	createValue (safetyStateValue, "safety_state", "OK, or which stage of an incident recovery is running", false);
	safetyStateValue->setValueCharArr ("OK");
	createValue (lastIncidentValue, "last_incident", "what the last safety incident was", false);
	createValue (incidentCountValue, "incident_count", "safety incidents since this driver started", false);
	incidentCountValue->setValueInteger (0);
	createValue (recoveriesUsedValue, "recoveries_used", "completed automatic recoveries since this driver started - past max_recoveries the mount stays locked", false);
	recoveriesUsedValue->setValueInteger (0);

	safetyState = SAFETY_OK;
	safetyStateSince = NAN;
	safetyWantsColdStart = false;
	safetyColdStartBaseline = 0;
	incidentLogPath = "/var/log/rts2/gemini-udp-incidents.log";
	incidentLogFailed = false;
	maxRecoveries = 3;
	coldStartOnIncident = true;
	lastSafetyPollTimestamp = NAN;
	unexpectedMoveCount = 0;
	belowHorizonCount = 0;
	parkedTrackingCount = 0;
	parkedTrackingCorrections = 0;
	moveFailReported = false;
	wrongWayReported = false;
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
	addOption (OPT_STARTUP_MODE, "startup-mode", 1, "which mode to pick in the mount's boot menu: restart (default), warm or cold");
	addOption (OPT_INCIDENT_LOG, "incident-log", 1, "file to append safety incident reports to (default /var/log/rts2/gemini-udp-incidents.log; incidents are always logged to RTS2 as well)");
	addOption (OPT_NO_SAFETY, "no-safety-watchdog", 0, "do not watch for unexpected mount movement / wrong-way slews / below-horizon pointing (the watchdog is on by default, and can also be toggled at runtime via safety_enabled)");
	addOption (OPT_NO_COLDSTART, "no-safety-coldstart", 0, "on a safety incident stop and park, but do not go on to cold-start the mount");
	addOption (OPT_MAX_RECOVERIES, "max-recoveries", 1, "how many automatic incident recoveries to perform before leaving the mount locked for a human (default 3)");
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
		case OPT_STARTUP_MODE:
			if (!strcasecmp (optarg, "restart"))
				startupModeValue->setValueInteger (GeminiCaringLoop::STARTUP_RESTART);
			else if (!strcasecmp (optarg, "warm"))
				startupModeValue->setValueInteger (GeminiCaringLoop::STARTUP_WARM);
			else if (!strcasecmp (optarg, "cold"))
				startupModeValue->setValueInteger (GeminiCaringLoop::STARTUP_COLD);
			else
			{
				logStream (MESSAGE_ERROR) << "unknown --startup-mode \"" << optarg << "\" - expected restart, warm or cold" << sendLog;
				return -1;
			}
			break;
		case OPT_INCIDENT_LOG:
			incidentLogPath = optarg;
			break;
		case OPT_NO_SAFETY:
			safetyEnabledValue->setValueBool (false);
			break;
		case OPT_NO_COLDSTART:
			coldStartOnIncident = false;
			break;
		case OPT_MAX_RECOVERIES:
			maxRecoveries = atoi (optarg);
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
	caring->setStartupMode ((GeminiCaringLoop::StartupMode) startupModeValue->getValueInteger ());
	caring->setWrongWayMargin (wrongWayMarginValue->getValueDouble ());
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

	// Reading the mount's limit registers and setting its clock used to
	// happen right here, synchronously. Both moved into the caring loop's
	// post-startup sequence (GeminiCaringLoop::runPostStartupSequence()),
	// for two reasons: they have to run again after every *re*start of the
	// mount, not just once at connect - and at connect the mount may well
	// still be sitting in its boot menu, in which case every one of those
	// seven round-trips would simply have failed. checkStartup() below
	// picks the results up off the snapshot instead.

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
	if (oldValue == startupModeValue)
	{
		if (caring)
			caring->setStartupMode ((GeminiCaringLoop::StartupMode) newValue->getValueInteger ());
		return 0;
	}
	if (oldValue == wrongWayMarginValue)
	{
		if (caring)
			caring->setWrongWayMargin (newValue->getValueDouble ());
		return 0;
	}
	if (oldValue == safetyLockedValue)
	{
		// Clearing it is the operator's acknowledgement: they've read the
		// incident log and the mount can be used again. Deliberately the
		// only way out of SAFETY_LOCKED other than restarting the driver
		// (the reset command goes through resetMount(), which clears it
		// too) - an automatic timeout would defeat the point of locking.
		// Setting it is the same lock by hand, for when an operator wants
		// the mount held without waiting for the watchdog to agree.
		bool wantLocked = newValue->getValueInteger () != 0;
		if (!wantLocked && safetyState == SAFETY_LOCKED)
		{
			logStream (MESSAGE_WARNING) << "GeminiUDP: safety lock cleared by operator - movement re-enabled" << sendLog;
			unBlockMove ();
			recoveriesUsedValue->setValueInteger (0);
			sendValueAll (recoveriesUsedValue);
			setSafetyState (SAFETY_OK);
		}
		else if (wantLocked && safetyState == SAFETY_OK)
		{
			logStream (MESSAGE_WARNING) << "GeminiUDP: safety lock set by operator - movement blocked" << sendLog;
			setSafetyState (SAFETY_LOCKED);
		}
		else if (wantLocked != (safetyState == SAFETY_LOCKED))
		{
			// mid-recovery: the sequence owns the lock until it finishes
			logStream (MESSAGE_ERROR) << "GeminiUDP: safety_locked cannot be changed while an incident recovery is running (safety_state="
				<< safetyStateValue->getValue () << ")" << sendLog;
			return -2;
		}
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

// The thing the whole framework-side tracking bookkeeping was missing: a
// command to the mount. Telescope::setTracking() only moves RTS2's own
// TEL_MASK_TRACK state and (re)arms the tracking timer; whether the RA worm
// is actually turning is entirely up to the driver. Without this override
// the driver slewed to targets correctly and then let them drift straight
// back out of the field - reported live from SBT on 2026-09-12.
//
// Both non-zero TRACKING selections ("on" and "sidereal") map to the same
// sidereal rate, which is all this mount is asked for here and exactly what
// the live production driver (~/gemini2ser.cpp's setTracking() ->
// startWorm()/stopWorm()) does with the same two registers.
//
// Note where this gets called from, because it matters for the mount:
// Telescope::endMove() calls startTracking() after every completed slew, so
// tracking is re-asserted on arrival and not just when a client asks.
int GeminiUDP::setTracking (int track, bool addTrackingTimer, bool send, const char *stopMsg)
{
	if (caring)
		caring->queueNativeSet (track ? GEMINI_CMD_TRACK_SIDEREAL : GEMINI_CMD_TRACK_TERRESTRIAL, (int32_t) 1);
	return Telescope::setTracking (track, addTrackingTimer, send, stopMsg);
}

// the "reset" client command. base/teld/gemini/gemini.cpp's resetMount()
// picks between native 65533 (cold) and 65535 (warm) off its next_reset
// selection; startup_mode is the same selection under a name that also
// describes its other job (what to answer the boot menu with).
int GeminiUDP::resetMount ()
{
	if (caring == nullptr)
		return -1;

	bool cold = startupModeValue->getValueInteger () == GeminiCaringLoop::STARTUP_COLD;
	logStream (MESSAGE_WARNING) << "GeminiUDP: rebooting the mount (" << (cold ? "cold start, native 65533" : "warm, native 65535")
		<< ") - it will be unreachable for a while, then brought back up through its boot menu" << sendLog;
	caring->requestReboot (cold);

	// an explicit operator-driven reset is also the way out of a safety
	// lock: they have decided what to do about the incident
	if (safetyState != SAFETY_OK)
	{
		unBlockMove ();
		safetyLockedValue->setValueBool (false);
		sendValueAll (safetyLockedValue);
		recoveriesUsedValue->setValueInteger (0);
		sendValueAll (recoveriesUsedValue);
		setSafetyState (SAFETY_OK);
	}

	return Telescope::resetMount ();
}

// Telescope::infoUTCLST() calls this whenever the telescope's computed
// position falls below the configured hard horizon (and, with no horizon
// file loaded, below alt 0 - see ObjectCheck::getHorizonHeight()). The base
// implementation stops tracking; for this mount that isn't enough, so it
// feeds the same incident machinery as our own below-horizon check.
int GeminiUDP::abortMoveTracking ()
{
	struct ln_hrz_posn hrz;
	getTelAltAz (&hrz);

	// Nothing is actually below any horizon yet. Until the caring loop's
	// first valid poll lands, telRaDec still holds its startup default and
	// the framework's check is comparing a NaN altitude against the horizon
	// - which fails, so infoUTCLST() calls this once per second for the
	// first few seconds of every run. 1 is the documented "abort was not
	// called, temporarily allowed violation" answer: it stops the base
	// class stopping a mount that isn't moving, stops the matching error
	// being logged every second, and - the reason this matters now - stops
	// a NaN opening a safety incident and cold-starting a healthy mount.
	if (caring == nullptr || !caring->getStatus ().valid || std::isnan (hrz.alt))
		return 1;

	int ret = Telescope::abortMoveTracking ();

	std::ostringstream detail;
	detail << "framework hard-horizon violation at alt=" << hrz.alt << " az=" << hrz.az;
	triggerIncident ("mount below the hard horizon", detail.str (), true);

	return ret;
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

	// deliberately ahead of the st.valid gate: while the mount is still in
	// its boot menu there is no valid ENQ status at all, and that is
	// precisely when knowing where it is in its startup matters most
	checkStartup (st);

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
	trackingRateValue->setValueInteger (st.trackingRate);

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

	checkSafety (st);

	logStream (MESSAGE_DEBUG) << "GeminiUDP: caring loop poll OK, RA=" << st.ra << " Dec=" << st.dec
		<< " HA=" << st.ha << " AZ=" << st.az << " ALT=" << st.alt << " LST=" << st.lst
		<< " pier=" << st.pierSide << " rate=" << st.moveRate
		<< " trackingSecToLimit=" << st.trackingSecToWestLimit << sendLog;
}

// Reflects the caring loop's startup handshake, and does the RTS2-thread
// half of "the mount just (re)started": everything here has to be able to
// run more than once, because the mount can be power-cycled, rebooted by
// somebody at the hand controller or over the Gemini web interface, or
// cold-started by our own incident recovery, all while this driver keeps
// running.
void GeminiUDP::checkStartup (const GeminiStatus &st)
{
	char state[2] = { st.startupState, 0 };
	startupStateValue->setValueCharArr (state);

	bool wasReady = mountReadyValue->getValueBool ();
	mountReadyValue->setValueBool (st.startupComplete);

	if (wasReady && !st.startupComplete)
	{
		logStream (MESSAGE_WARNING) << "GeminiUDP: mount is no longer started up (handshake now '" << st.startupState
			<< "') - it has rebooted, or is being rebooted. Motion commands are refused until it is back up." << sendLog;
	}

	if (st.startupCount == lastStartupCount)
		return;

	lastStartupCount = st.startupCount;

	logStream (MESSAGE_INFO) << "GeminiUDP: mount startup complete (handshake '" << st.startupState << "' = "
		<< (st.startupState == 'A' ? "Alt/Az" : "German equatorial") << " mount), startup #" << st.startupCount << sendLog;

	clockMatchedValue->setValueBool (st.clockMatched);
	if (!st.clockMatched)
		logStream (MESSAGE_ERROR) << "GeminiUDP: could not set the mount clock to system UTC - HA/LST-derived decisions (pier side, tracking limit) are only as good as the mount's own clock" << sendLog;

	if (st.limitsValid)
	{
		limitBothRawValue->setValueCharArr (st.limitBothRaw.c_str ());
		limitEastRawValue->setValueCharArr (st.limitEastRaw.c_str ());
		limitWestRawValue->setValueCharArr (st.limitWestRaw.c_str ());
		limitWestGotoRawValue->setValueCharArr (st.limitWestGotoRaw.c_str ());
		logStream (MESSAGE_INFO) << "GeminiUDP: mount limits - 220 both=\"" << st.limitBothRaw
			<< "\" 221 east=\"" << st.limitEastRaw << "\" 222 west=\"" << st.limitWestRaw
			<< "\" 223 west goto=\"" << st.limitWestGotoRaw << "\"" << sendLog;
	}
	else
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: failed to read the mount's limit registers (native 220-223) after startup" << sendLog;
	}

	trackingRateValue->setValueInteger (st.trackingRate);

	// a mount that has just come up is not tracking anything, whatever RTS2
	// still believes - re-assert whichever state the framework is in rather
	// than leaving the two out of step
	if (isTracking ())
	{
		logStream (MESSAGE_INFO) << "GeminiUDP: re-asserting sidereal tracking after mount startup" << sendLog;
		if (caring)
			caring->queueNativeSet (GEMINI_CMD_TRACK_SIDEREAL, (int32_t) 1);
	}
}

// ---- safety watchdog ----
// See the SafetyState declaration for the policy this implements and where
// it came from. Two deliberate choices worth knowing about before changing
// anything here:
//
// Every condition has to hold for SAFETY_CONFIRM_POLLS consecutive polls
// before it counts. The mount is polled over UDP once a second through a
// NACK/resync protocol; a single surprising snapshot is not evidence of
// anything, and the cost of a false positive here is a parked mount and a
// cold-started controller in the middle of the night.
//
// The order of the checks is the order of severity, and the first one to
// fire wins - once an incident is open, safetyState is no longer SAFETY_OK
// and this function returns immediately, so nothing else can start a second
// recovery on top of the first.
void GeminiUDP::checkSafety (const GeminiStatus &st)
{
	constexpr int SAFETY_CONFIRM_POLLS = 3;
	constexpr int PARKED_TRACKING_CORRECTIONS = 3;

	if (!safetyEnabledValue->getValueBool () || !st.valid || safetyState != SAFETY_OK)
		return;

	// one pass per actual mount poll - see lastSafetyPollTimestamp
	if (st.timestamp == lastSafetyPollTimestamp)
		return;
	lastSafetyPollTimestamp = st.timestamp;

	rts2_status_t moving = getState () & TEL_MASK_MOVING;
	bool weCommandedMovement = (moving == TEL_MOVING || moving == TEL_PARKING) || st.moveInProgress;

	// ---- the mount is slewing and we didn't ask it to ----
	// 'S' slewing / 'C' centering only: 'T' is ordinary tracking and 'G' is
	// a guide pulse, both of which are legitimately in flight at moments
	// RTS2's own state doesn't model as "moving". Note that somebody
	// driving the mount from the hand controller is indistinguishable from
	// this - hence safety_enabled being writable at runtime.
	if (!weCommandedMovement && (st.moveRate == 'S' || st.moveRate == 'C'))
		unexpectedMoveCount++;
	else
		unexpectedMoveCount = 0;

	if (unexpectedMoveCount >= SAFETY_CONFIRM_POLLS)
	{
		std::ostringstream detail;
		detail << "mount reports rate '" << st.moveRate << "' for " << unexpectedMoveCount
			<< " consecutive polls while the driver had no move in flight (RA=" << st.ra << " Dec=" << st.dec << " alt=" << st.alt << ")";
		unexpectedMoveCount = 0;
		triggerIncident ("mount moving when it should not be", detail.str (), true);
		return;
	}

	// ---- a move in flight is heading away from its target ----
	if (st.moveWrongWay)
	{
		if (!wrongWayReported)
		{
			wrongWayReported = true;
			std::ostringstream detail;
			detail << "distance to target grew to " << st.moveSeparation << " deg, more than " << wrongWayMarginValue->getValueDouble ()
				<< " deg past its closest approach, over several consecutive polls (RA=" << st.ra << " Dec=" << st.dec << ")";
			triggerIncident ("mount moving the wrong way", detail.str (), true);
			return;
		}
	}
	else
	{
		wrongWayReported = false;
	}

	// ---- a move ended nowhere near its target ----
	// GeminiStatus::moveFailed is sticky until the next accepted goto, so
	// it is latched here too and reported exactly once per move
	if (st.moveFailed)
	{
		if (!moveFailReported)
		{
			moveFailReported = true;
			triggerIncident ("move ended away from its target", st.moveFailReason, true);
			return;
		}
	}
	else
	{
		moveFailReported = false;
	}

	// ---- the mount is pointed below the horizon ----
	// Skipped while parked or parking: the park position is wherever the
	// operator configured it, and a driver that responds to "you asked me
	// to park and now I am parked" by parking again would never stop.
	if (moving != TEL_PARKED && moving != TEL_PARKING && st.alt < safetyAltLimitValue->getValueDouble ())
		belowHorizonCount++;
	else
		belowHorizonCount = 0;

	if (belowHorizonCount >= SAFETY_CONFIRM_POLLS)
	{
		std::ostringstream detail;
		detail << "mount reports alt=" << st.alt << " az=" << st.az << " deg, below the " << safetyAltLimitValue->getValueDouble ()
			<< " deg safety limit, for " << belowHorizonCount << " consecutive polls";
		bool active = weCommandedMovement || isTracking () || st.moveRate != 'N';
		belowHorizonCount = 0;
		// A mount that is simply sitting somewhere low is a bad position,
		// not evidence of a controller fault: park it and leave its
		// alignment alone. One that got there while moving or tracking is
		// a controller we no longer trust, so that one gets the cold start.
		triggerIncident ("mount pointed below the horizon", detail.str (), active);
		return;
	}

	// ---- tracking while parked ----
	// The mildest of the lot, and the one with a cheap fix: Gemini wakes
	// itself up on all sorts of provocations (see native 92), and a parked
	// mount that starts tracking simply drifts off its park position. Try
	// stopping the worm a few times before treating it as a fault.
	if (moving == TEL_PARKED && !isTracking () && (st.moveRate == 'T' || st.trackingRate == GEMINI_CMD_TRACK_SIDEREAL))
		parkedTrackingCount++;
	else
		parkedTrackingCount = 0;

	if (parkedTrackingCount >= SAFETY_CONFIRM_POLLS)
	{
		parkedTrackingCount = 0;
		parkedTrackingCorrections++;
		if (parkedTrackingCorrections <= PARKED_TRACKING_CORRECTIONS)
		{
			logStream (MESSAGE_WARNING) << "GeminiUDP: mount is tracking while parked (rate '" << st.moveRate << "', native 130 = "
				<< st.trackingRate << ") - stopping the worm (attempt " << parkedTrackingCorrections << " of " << PARKED_TRACKING_CORRECTIONS << ")" << sendLog;
			if (caring)
				caring->queueNativeSet (GEMINI_CMD_TRACK_TERRESTRIAL, (int32_t) 1);
		}
		else
		{
			std::ostringstream detail;
			detail << "mount kept tracking while parked after " << PARKED_TRACKING_CORRECTIONS << " attempts to stop the worm (rate '"
				<< st.moveRate << "', native 130 = " << st.trackingRate << ")";
			parkedTrackingCorrections = 0;
			triggerIncident ("mount tracking while parked", detail.str (), true);
		}
		return;
	}
}

void GeminiUDP::setSafetyState (SafetyState newState)
{
	static const char *names[] = { "OK", "STOPPING", "PARKING", "COLDSTART", "LOCKED" };
	safetyState = newState;
	safetyStateSince = getNow ();
	safetyStateValue->setValueCharArr (names[newState]);
	sendValueAll (safetyStateValue);

	if (newState == SAFETY_LOCKED)
	{
		// the mount stays where the recovery left it and refuses to move
		// until a human says otherwise - see setValue()'s safety_locked
		// branch and resetMount()
		setBlockMove ();
		safetyLockedValue->setValueBool (true);
		sendValueAll (safetyLockedValue);
	}
	else if (newState == SAFETY_OK)
	{
		// start the next watch from a clean slate rather than from
		// whatever half-accumulated counts the incident left behind
		unexpectedMoveCount = 0;
		belowHorizonCount = 0;
		parkedTrackingCount = 0;
		parkedTrackingCorrections = 0;
		moveFailReported = false;
		wrongWayReported = false;
		safetyLockedValue->setValueBool (false);
		sendValueAll (safetyLockedValue);
	}
}

// Opens an incident: records it everywhere a human might look for it, then
// gets the mount stopped. The rest of the sequence (park, then optionally
// cold start) runs from runSafetyRecovery() across subsequent idle() ticks -
// it has to, since parking takes a minute or two of real mount movement.
void GeminiUDP::triggerIncident (const char *condition, const std::string &detail, bool escalate)
{
	if (safetyState != SAFETY_OK)
		return;

	safetyReason = std::string (condition) + ": " + detail;
	safetyWantsColdStart = escalate && coldStartOnIncident;

	incidentCountValue->inc ();
	sendValueAll (incidentCountValue);
	lastIncidentValue->setValueCharArr (safetyReason.c_str ());
	sendValueAll (lastIncidentValue);

	logStream (MESSAGE_CRITICAL) << "GeminiUDP: SAFETY INCIDENT #" << incidentCountValue->getValueInteger () << " - " << safetyReason
		<< " -> stopping, then parking" << (safetyWantsColdStart ? ", then cold-starting the mount" : "") << sendLog;

	if (caring)
		writeIncidentReport (caring->getStatus ());

	// stopTracking() calls stopMove() for us, which is requestAbort() -
	// but ask for the abort explicitly too: whatever is going on, the one
	// thing worth spending a datagram on immediately is ":Q#".
	stopTracking ("safety incident");
	if (caring)
		caring->requestAbort ();

	setSafetyState (SAFETY_STOPPING);
}

// Drives the stop -> park -> cold start sequence. Runs from idle(), one
// step per tick, so nothing here may block.
void GeminiUDP::runSafetyRecovery (const GeminiStatus &st)
{
	constexpr double STOP_SETTLE_SEC = 2.0;
	constexpr double PARK_TIMEOUT_SEC = 180.0;
	constexpr double COLDSTART_TIMEOUT_SEC = 300.0;

	if (safetyState == SAFETY_OK || safetyState == SAFETY_LOCKED)
		return;

	double elapsed = getNow () - safetyStateSince;

	switch (safetyState)
	{
		case SAFETY_STOPPING:
		{
			// let ":Q#" reach the mount and the axes wind down before
			// asking for a park - a park issued into a still-slewing mount
			// is exactly the sort of overlapping command this driver
			// already suspects of causing stalled moves
			if (elapsed < STOP_SETTLE_SEC)
				return;

			appendIncidentLine ("stop sent, requesting park");
			// Telescope::startPark(Connection*), not our own startPark()
			// hook - the framework's entry point is what maintains
			// TEL_PARKING/TEL_PARKED, and while TEL_PARKING is set it
			// refuses every other move for us. Qualified because our
			// no-argument startPark() override hides the base overload.
			if (Telescope::startPark (nullptr) != 0)
			{
				logStream (MESSAGE_ERROR) << "GeminiUDP: SAFETY - could not start a park; going straight to " << (safetyWantsColdStart ? "the cold start" : "the safety lock") << sendLog;
				appendIncidentLine ("park could NOT be started");
				setSafetyState (safetyWantsColdStart ? SAFETY_COLDSTART : SAFETY_LOCKED);
				if (safetyState == SAFETY_COLDSTART && caring)
				{
					safetyColdStartBaseline = st.startupCount;
					caring->requestReboot (true);
				}
				return;
			}
			setSafetyState (SAFETY_PARKING);
			return;
		}

		case SAFETY_PARKING:
		{
			bool parked = (getState () & TEL_MASK_MOVING) == TEL_PARKED;
			if (!parked && elapsed < PARK_TIMEOUT_SEC)
				return;

			if (parked)
			{
				logStream (MESSAGE_INFO) << "GeminiUDP: SAFETY - mount parked" << sendLog;
				appendIncidentLine ("parked");
			}
			else
			{
				logStream (MESSAGE_ERROR) << "GeminiUDP: SAFETY - park did not complete within " << PARK_TIMEOUT_SEC << "s" << sendLog;
				appendIncidentLine ("park did NOT complete within the timeout");
			}

			if (safetyWantsColdStart && caring)
			{
				logStream (MESSAGE_WARNING) << "GeminiUDP: SAFETY - cold-starting the mount (native 65533)" << sendLog;
				appendIncidentLine ("cold start requested (native 65533)");
				safetyColdStartBaseline = st.startupCount;
				caring->requestReboot (true);
				setSafetyState (SAFETY_COLDSTART);
			}
			else
			{
				setSafetyState (SAFETY_LOCKED);
			}
			return;
		}

		case SAFETY_COLDSTART:
		{
			// the caring loop walks the mount back up through its boot
			// menu on its own; a bumped startupCount is it saying so
			if (st.startupCount != safetyColdStartBaseline)
			{
				bool mayResume = recoveriesUsedValue->getValueInteger () < maxRecoveries;
				recoveriesUsedValue->inc ();
				sendValueAll (recoveriesUsedValue);

				if (mayResume)
				{
					logStream (MESSAGE_WARNING) << "GeminiUDP: SAFETY - mount is back up after the cold start; recovery "
						<< recoveriesUsedValue->getValueInteger () << " of " << maxRecoveries << ", releasing the mount" << sendLog;
					appendIncidentLine ("mount back up after cold start - released");
					setSafetyState (SAFETY_OK);
				}
				else
				{
					logStream (MESSAGE_CRITICAL) << "GeminiUDP: SAFETY - mount is back up, but " << maxRecoveries
						<< " automatic recoveries have already been used. Holding the mount blocked - look at " << incidentLogPath
						<< ", then clear safety_locked (or send the reset command) to release it." << sendLog;
					appendIncidentLine ("mount back up after cold start - HELD, recovery budget exhausted");
					setSafetyState (SAFETY_LOCKED);
				}
				return;
			}

			if (elapsed > COLDSTART_TIMEOUT_SEC)
			{
				logStream (MESSAGE_CRITICAL) << "GeminiUDP: SAFETY - mount did not come back within " << COLDSTART_TIMEOUT_SEC
					<< "s of the cold start. Holding it blocked - this one needs a human." << sendLog;
				appendIncidentLine ("mount did NOT come back after the cold start");
				setSafetyState (SAFETY_LOCKED);
			}
			return;
		}

		default:
			return;
	}
}

namespace
{
	std::string utcStamp ()
	{
		time_t t = time (nullptr);
		struct tm ts;
		gmtime_r (&t, &ts);
		char buf[32];
		strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &ts);
		return buf;
	}
}

// Appends one timestamped line to the incident log. Every failure here is
// swallowed after a single complaint: an unwritable report file must never
// be the reason a recovery doesn't finish, and everything written here also
// goes to RTS2's own logging, which is where it will actually be noticed.
void GeminiUDP::appendIncidentLine (const std::string &line)
{
	if (incidentLogPath == nullptr || incidentLogFailed)
		return;

	std::ofstream f (incidentLogPath, std::ios::app);
	if (!f.good ())
	{
		incidentLogFailed = true;
		logStream (MESSAGE_ERROR) << "GeminiUDP: cannot write the incident log at " << incidentLogPath
			<< " - create the directory and make it writable by the RTS2 user, or point --incident-log somewhere else. Incidents are still logged to RTS2." << sendLog;
		return;
	}
	f << utcStamp () << "  " << line << "\n";
}

// The full picture at the moment an incident opened - written as one block
// so that whoever reads this file the next morning has the mount's own
// answers, not just our interpretation of them.
void GeminiUDP::writeIncidentReport (const GeminiStatus &st)
{
	std::ostringstream o;
	o << "\n==== " << utcStamp () << "  gemini-udp incident #" << incidentCountValue->getValueInteger () << " ====\n";
	o << "reason:      " << safetyReason << "\n";
	o << "action:      stop -> park" << (safetyWantsColdStart ? " -> cold start" : " (cold start not requested)") << "\n";
	o << "rts2 state:  0x" << std::hex << getState () << std::dec
		<< " tracking=" << (isTracking () ? "yes" : "no")
		<< " block_move=" << (getBlockMove () ? "yes" : "no") << "\n";
	o << "mount:       RA=" << st.ra << " Dec=" << st.dec << " HA=" << st.ha
		<< " AZ=" << st.az << " ALT=" << st.alt << " LST=" << st.lst << "\n";
	o << "             pier=" << st.pierSide << " rate=" << st.moveRate
		<< " startup=" << st.startupState << " tracking_rate=" << st.trackingRate
		<< " park_status=" << st.parkStatus << " connected=" << (st.connected ? "yes" : "no") << "\n";
	o << "             move_in_progress=" << st.moveInProgress << " move_failed=" << st.moveFailed
		<< " move_wrong_way=" << st.moveWrongWay << " pier_changed_in_move=" << st.movePierChanged
		<< " move_separation=" << st.moveSeparation << "\n";
	if (st.moveFailed)
		o << "             move_fail_reason: " << st.moveFailReason << "\n";
	o << "             tracking_sec_to_limit=" << st.trackingSecToWestLimit << "\n";
	o << "             counts: RA=" << st.praRaw << " DEC=" << st.pdecRaw << "\n";
	o << "             limits: 220=\"" << st.limitBothRaw << "\" 221=\"" << st.limitEastRaw
		<< "\" 222=\"" << st.limitWestRaw << "\" 223=\"" << st.limitWestGotoRaw << "\"\n";
	o << "             ext_status_raw: " << st.rawExtended << "\n";

	struct ln_equ_posn tar;
	getTelTargetRaDec (&tar);
	o << "target:      RA=" << tar.ra << " Dec=" << tar.dec << "\n";

	if (incidentLogPath != nullptr && !incidentLogFailed)
	{
		std::ofstream f (incidentLogPath, std::ios::app);
		if (f.good ())
		{
			f << o.str ();
		}
		else
		{
			incidentLogFailed = true;
			logStream (MESSAGE_ERROR) << "GeminiUDP: cannot write the incident log at " << incidentLogPath
				<< " - create the directory and make it writable by the RTS2 user, or point --incident-log somewhere else. The report follows in the RTS2 log instead." << sendLog;
		}
	}

	// always, regardless of the file: this is the record that reaches
	// centrald and whatever is watching it. One RTS2 message per line -
	// a log message is a single protocol field, and embedding newlines in
	// one is asking for a mangled line at the far end.
	std::istringstream lines (o.str ());
	std::string line;
	while (std::getline (lines, line))
	{
		if (!line.empty ())
			logStream (MESSAGE_CRITICAL) << "GeminiUDP incident: " << line << sendLog;
	}
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
	// an incident recovery owns the mount while it runs - the self-test
	// has no business issuing gotos into the middle of a stop/park/cold
	// start sequence
	if (safetyState != SAFETY_OK)
		return;

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
		// after applyStatus(), which is where an incident gets opened -
		// so the first step of a recovery runs on the same tick that
		// detected the problem, not a second later
		runSafetyRecovery (st);
	}

	runSelfTest ();

	return Telescope::idle ();
}

int main (int argc, char **argv)
{
	GeminiUDP device = GeminiUDP (argc, argv);
	return device.run ();
}
