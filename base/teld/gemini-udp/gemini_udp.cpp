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
#include <sys/stat.h>

#include "base-config.h"	// BASE_GIT_DESCRIBE
#include <strings.h>
#include <vector>
#include <algorithm>
#include <iomanip>

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
#define OPT_POSITION_STATE       OPT_LOCAL + 7

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

		// the "position" command - see positionCommand()
		virtual int commandAuthorized (rts2core::Connection *conn);

	private:
		// ---- position trust ----
		// Whether the mount's axis counters - which its safety limits, its
		// pier side decisions and every goto are measured against - can be
		// believed. The motor encoders are relative: nothing in the mount
		// can tell a slipped clutch, or a cold start done away from CWD,
		// from the truth. So the driver keeps its own verdict:
		//  CONFIRMED  a sky position (astrometry) agreed with the mount
		//  ASSUMED    nothing suggests a problem, nothing proved it either
		//  LOST       evidence the counters are wrong, or a startup no one
		//             vouched for - motion, parking and tracking refused
		//             until a human says otherwise or a re-zero from the
		//             sky fixes it (see positionCommand())
		// The verdict survives driver restarts (positionStatePath): a
		// restart must neither forget a lost position nor reset a healthy
		// mount.
		enum PositionTrust { TRUST_UNKNOWN, TRUST_CONFIRMED, TRUST_ASSUMED, TRUST_LOST };
		PositionTrust positionTrust;
		rts2core::ValueSelection *positionTrustValue;
		rts2core::ValueString *positionReasonValue;
		const char *positionStatePath;
		double positionStateSavedAt;
		bool positionStateFailed;

		// what the state file said when the driver started
		PositionTrust savedTrust;
		std::string savedReason;
		bool savedHaveAxis;
		int32_t savedDecTicks;

		// the last axis position seen while the mount was up, for telling a
		// counter reset (a cold or warm start sets Dec to exactly half a
		// circle) from ordinary movement
		bool haveLastAxis;
		int32_t lastDecTicks;
		double lastAxisSampleTimestamp;
		double lastMotionAt;		// getNow() when a move, park or re-zero was last in flight

		// set by "position unmoved/cwd" while the mount (re)boots on a
		// human's word: 'R' restart, 'W'/'C' warm/cold start at CWD
		char pendingHumanStartup;
		bool lostOnlyForBootMenu;	// LOST only because the boot menu is waiting, not for anything that happened before

		void setPositionTrust (PositionTrust trust, const std::string &reason);
		bool positionLost () const { return positionTrust == TRUST_LOST; }
		void checkPositionEvidence (const GeminiStatus &st);
		void judgeStartup (const GeminiStatus &st);
		void loadPositionState ();
		void savePositionState (const GeminiStatus *st);
		int positionCommand (rts2core::Connection *conn);

		// ---- re-zero from the sky ----
		// No command in this firmware sets the axis counters; only a cold
		// (or warm) start does, to CWD. So: learn from astrometry how far the
		// counters are off, step the axes to where the counters read CWD
		// minus that error - physically true CWD - and cold-start there.
		// The evidence comes from the ordinary closed loop: every "correct"
		// the framework would act on (same move, same correction state) says
		// where the telescope truly points, see recordSkyEvidence(). One
		// sample per target is kept; when the last rezero_samples of them
		// agree, come from pointings far enough apart, and show an error
		// above rezero_min, a re-zero is armed and runs just before the next
		// move to a new target (rezero_auto), or on "position rezero".
		struct SkyEvidence
		{
			double at;		// getNow()
			int moveNum;
			double raErrDeg, decErrDeg;	// counter error, degrees of axis
			double ha, dec;			// mount frame pointing, deg
			char side;
			std::string summary;
		};
		std::vector<SkyEvidence> skyEvidence;
		bool rezeroArmed;
		bool rezeroThenMove;		// a framework move is waiting for the re-zero to finish
		bool rezeroMoveFailed;		// ... and it will not happen - isMoving() reports the failure once
		int lastResyncMoveNum;
		double lastRezeroAt;
		rts2core::ValueBool *rezeroAutoValue;
		rts2core::ValueInteger *rezeroSamplesValue;
		rts2core::ValueDouble *rezeroAgreeValue;
		rts2core::ValueDouble *rezeroSpreadValue;
		rts2core::ValueDouble *rezeroIntervalValue;
		rts2core::ValueString *skyEvidenceValue;
		rts2core::ValueBool *rezeroArmedValue;

		void recordSkyEvidence (rts2core::Connection *conn);
		bool measureCounterError (double raJ2000, double decJ2000, const GeminiStatus &st, SkyEvidence &sample, std::string &err);
		void evaluateSkyEvidence ();
		void clearSkyEvidence (const char *why);
		int beginRezero (double raErrDeg, double decErrDeg, const std::string &summary, std::string &err);
		enum RezeroState { REZERO_IDLE, REZERO_STOPPING, REZERO_MOVING, REZERO_SETTLING, REZERO_REBOOTING };
		RezeroState rezeroState;
		double rezeroSince;
		int32_t rezeroTargetRa, rezeroTargetDec;
		int rezeroStableCount;
		int32_t rezeroLastRa, rezeroLastDec;
		double rezeroLastSample;
		unsigned rezeroStartupBaseline;
		std::string rezeroSummary;
		std::vector<std::pair<int, int>> rezeroModelTerms;	// Gemini model terms to put back after the cold start

		rts2core::ValueString *rezeroStateValue;
		rts2core::ValueDouble *rezeroMinValue;
		rts2core::ValueDouble *rezeroMaxValue;

		void runRezero (const GeminiStatus &st);
		void setRezeroState (RezeroState newState);
		void abortRezero (const std::string &why);

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

		// ---- pier side prediction (see GeminiSidePrediction) ----
		rts2core::ValueString *decSideValue;		// E/W from the Dec axis (native 239 vs 238) - the real flip state
		rts2core::ValueString *sideWindowValue;		// the RA axis window gotos have to fit, from native 231/223
		rts2core::ValueString *gotoPredictionValue;	// what the last goto was predicted to do with the pier side
		rts2core::ValueDouble *flipAmbiguityMarginValue;
		rts2core::ValueSelection *gotoPrestopValue;
		rts2core::ValueInteger *predictionMissesValue;
		unsigned verifiedGotoSerial;
		unsigned loggedMoveEndSerial;
		bool geometryLogged;

		void checkSidePrediction (const GeminiStatus &st);

		// ---- move recovery (execution failures) ----
		// When the mount fails to *execute* a move - the RA axis crawls, an
		// axis stalls, it ends nowhere near the target - but its counters are
		// still sound, the driver recovers on its own: stop, park to CWD (the
		// division of the two sky halves, the pose parks reach reliably and
		// the best place to start any move from), and re-send the target,
		// through goto_prestop so the worm is off first. Up to move_retries
		// times per target; then it leaves the mount parked at CWD and fails
		// the move to the framework (no lock, no lost position - the counters
		// are fine, the scheduler simply moves on). Position-integrity faults
		// (a cold start behind our back, the boot menu) are NOT this: they
		// keep going through setPositionTrust(LOST). All of it is invisible to
		// the framework, which sees one move that stays in flight until it
		// arrives or the retries run out (isMoving()).
		enum MoveRecoveryState { RECOVER_IDLE, RECOVER_STOPPING, RECOVER_PARKING };
		MoveRecoveryState recoverState;
		double recoverSince;
		double recoverTargetRa, recoverTargetDec;
		int moveRetries;			// used so far for the framework's current target
		bool recoverGiveUp;			// budget spent - isMoving() fails the move once
		rts2core::ValueString *moveRecoveryValue;
		rts2core::ValueInteger *moveRetriesValue;

		bool tryStartMoveRecovery (const std::string &reason);
		void runMoveRecovery (const GeminiStatus &st);

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

		// one line at startup saying what this binary is - see its definition
		void logBuildIdentity ();

		// ---- safety watchdog ----
		// The driver polls the mount once a second anyway, so it is in a
		// position to notice the mount misbehaving; when it does, it stops,
		// parks - the one operation that has proven reliable on this mount
		// - and holds the mount locked with a report behind. It used to go
		// on to cold-start the controller; it no longer does: a cold start
		// sets the axes to CWD wherever the telescope happens to be, and
		// after an incident that is exactly what nobody knows. Incidents
		// that cast doubt on the position mark it LOST instead.
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
		enum SafetyState { SAFETY_OK, SAFETY_STOPPING, SAFETY_PARKING, SAFETY_LOCKED };
		SafetyState safetyState;
		double safetyStateSince;
		std::string safetyReason;
		bool safetyLosesPosition;	// the incident casts doubt on the axis counters - LOST once the recovery is done

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
		// (above horizon) once the tracking limit is close, flip to it (a
		// same-target :MM#, sent only when predicted to flip - a same-target
		// :MS# at 660s does nothing, the firmware keeps the side until the
		// target is inside the 223 goto limit, ~600s for its default 2.5
		// deg); if it's set, or the flip isn't predicted to work, park.
		// Either way, this MUST NOT interrupt an in-progress exposure -
		// block new exposures (BOP_EXPOSURE) the moment the decision is
		// armed, then wait for BOP_TEL_MOVE to clear (no camera currently
		// demanding the telescope hold still) before actually moving -
		// same coordination as gemini2ser.cpp's flippingRequested/
		// setFullBopState(), just generalized to cover park too.
		enum LimitAction { LIMIT_ACTION_NONE, LIMIT_ACTION_FLIP, LIMIT_ACTION_PARK } pendingLimitAction;
		bool limitActionInFlight;	// true from the moment the goto/park actually starts until endMove()/endPark() sees it through - keeps the exposure block up for the whole move, not just until it's accepted

		void armLimitAction (LimitAction wanted);
		bool trackingLimitWaitLogged;
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
		bool doGoto (double raDeg, double decDeg, const char *label, GeminiCaringLoop::GotoSideMode sideMode = GeminiCaringLoop::GOTO_ANY_SIDE);

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

	createValue (decSideValue, "dec_side", "pier side from the Dec axis (native 239 vs half circle): E Dec >= half, W below - what Gemini's flip decision uses", false);
	createValue (sideWindowValue, "side_window", "RA axis window a goto target must fit on the side it ends up on: west safety limit + 223 goto limit ... east safety limit, degrees from CWD", false);
	createValue (gotoPredictionValue, "goto_prediction", "pier side outcome predicted for the last goto from Gemini's own decision rule", false);
	createValue (flipAmbiguityMarginValue, "flip_ambiguity_margin", "[deg] side predictions closer than this to a window edge count as too close to call (Gemini's pointing model is not predicted)", false, RTS2_VALUE_WRITABLE);
	flipAmbiguityMarginValue->setValueDouble (0.5);
	createValue (gotoPrestopValue, "goto_prestop", "sent ahead of every goto: NONE, STOP (:Q#, as gemini2ser.cpp - default), STOP_TRACKING (worm off, :Q#, wait until the RA axis is still - loses a little sky, only if the mount is shown to need it)", false, RTS2_VALUE_WRITABLE);
	gotoPrestopValue->addSelVal ("NONE");
	gotoPrestopValue->addSelVal ("STOP");
	gotoPrestopValue->addSelVal ("STOP_TRACKING");
	gotoPrestopValue->setValueInteger (GeminiCaringLoop::PRESTOP_STOP);
	createValue (predictionMissesValue, "side_prediction_misses", "gotos that ended on a different pier side than predicted", false);
	predictionMissesValue->setValueInteger (0);
	verifiedGotoSerial = 0;
	loggedMoveEndSerial = 0;
	geometryLogged = false;

	createValue (startupModeValue, "startup_mode", "startup mode picked for the mount's own boot menu, and used by the reset command", false, RTS2_VALUE_WRITABLE);
	startupModeValue->addSelVal ("NONE");		// wait for a human ("position unmoved" / "position cwd")
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
	safetyLosesPosition = false;
	incidentLogPath = "/var/log/rts2/gemini-udp-incidents.log";
	incidentLogFailed = false;
	maxRecoveries = 3;

	createValue (positionTrustValue, "position_trust", "can the mount's axis counters be believed: CONFIRMED by the sky, ASSUMED, or LOST (moves refused - see the position command)", false);
	positionTrustValue->addSelVal ("UNKNOWN");
	positionTrustValue->addSelVal ("CONFIRMED");
	positionTrustValue->addSelVal ("ASSUMED");
	positionTrustValue->addSelVal ("LOST");
	positionTrustValue->setValueInteger (TRUST_UNKNOWN);
	createValue (positionReasonValue, "position_reason", "why position_trust is what it is", false);
	positionTrust = TRUST_UNKNOWN;
	positionStatePath = "/var/log/rts2/gemini-udp-position.state";
	positionStateSavedAt = 0;
	positionStateFailed = false;
	savedTrust = TRUST_UNKNOWN;
	savedHaveAxis = false;
	savedDecTicks = 0;
	haveLastAxis = false;
	lastDecTicks = 0;
	lastAxisSampleTimestamp = 0;
	lastMotionAt = 0;
	pendingHumanStartup = 0;
	lostOnlyForBootMenu = false;

	recoverState = RECOVER_IDLE;
	recoverSince = 0;
	recoverTargetRa = recoverTargetDec = NAN;
	recoverGiveUp = false;
	createValue (moveRecoveryValue, "move_recovery", "auto-recovery of a move the mount failed to execute: IDLE, STOPPING, PARKING", false);
	moveRecoveryValue->setValueCharArr ("IDLE");
	createValue (moveRetriesValue, "move_retries", "how many times a move the mount fails to execute is retried from CWD before the mount is left parked for a look", false, RTS2_VALUE_WRITABLE);
	moveRetriesValue->setValueInteger (3);
	moveRetries = 0;

	rezeroState = REZERO_IDLE;
	rezeroSince = 0;
	rezeroTargetRa = rezeroTargetDec = 0;
	rezeroStableCount = 0;
	rezeroLastRa = rezeroLastDec = 0;
	rezeroLastSample = 0;
	rezeroStartupBaseline = 0;
	createValue (rezeroStateValue, "rezero_state", "re-zero from a sky position in progress: IDLE, STOPPING, MOVING, SETTLING, REBOOTING", false);
	rezeroStateValue->setValueCharArr ("IDLE");
	createValue (rezeroMinValue, "rezero_min", "[deg] counter errors from astrometry below this only confirm the position, without a re-zero", false, RTS2_VALUE_WRITABLE);
	rezeroMinValue->setValueDouble (0.25);
	createValue (rezeroMaxValue, "rezero_max", "[deg] counter errors from astrometry above this are not re-zeroed but make the position LOST", false, RTS2_VALUE_WRITABLE);
	rezeroMaxValue->setValueDouble (30.0);
	rezeroArmed = false;
	rezeroThenMove = false;
	rezeroMoveFailed = false;
	lastResyncMoveNum = -1;
	lastRezeroAt = 0;
	createValue (rezeroAutoValue, "rezero_auto", "re-zero on its own, before the next move to a new target, once the astrometric evidence qualifies", false, RTS2_VALUE_WRITABLE);
	rezeroAutoValue->setValueBool (false);
	createValue (rezeroSamplesValue, "rezero_samples", "how many targets' astrometry have to agree on the counter error before a re-zero is armed", false, RTS2_VALUE_WRITABLE);
	rezeroSamplesValue->setValueInteger (3);
	createValue (rezeroAgreeValue, "rezero_agree", "[deg] how closely those samples have to agree", false, RTS2_VALUE_WRITABLE);
	rezeroAgreeValue->setValueDouble (0.1);
	createValue (rezeroSpreadValue, "rezero_spread", "[deg] at least two of those samples have to come from pointings this far apart - a zero error is the same everywhere, a model error is not", false, RTS2_VALUE_WRITABLE);
	rezeroSpreadValue->setValueDouble (15.0);
	createValue (rezeroIntervalValue, "rezero_interval", "[h] minimum time between automatic re-zeros", false, RTS2_VALUE_WRITABLE);
	rezeroIntervalValue->setValueDouble (4.0);
	createValue (skyEvidenceValue, "sky_evidence", "astrometric counter error samples, newest last: RA axis/Dec axis deg @HA,Dec side", false);
	createValue (rezeroArmedValue, "rezero_armed", "the evidence qualifies - a re-zero will run before the next move to a new target (with rezero_auto)", false);
	rezeroArmedValue->setValueBool (false);
	lastSafetyPollTimestamp = NAN;
	unexpectedMoveCount = 0;
	belowHorizonCount = 0;
	parkedTrackingCount = 0;
	parkedTrackingCorrections = 0;
	moveFailReported = false;
	wrongWayReported = false;
	createValue (trackingSecToLimitValue, "tracking_sec_to_limit", "native 226: seconds of tracking left before Gemini's own firmware hits the western limit and stops - triggers armLimitAction() below 660s", false);
	trackingLimitWarned = false;
	trackingLimitWaitLogged = false;
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
	addOption (OPT_STARTUP_MODE, "startup-mode", 1, "what to answer the mount's boot menu with: none (default - wait for a human's position command), restart, warm or cold");
	addOption (OPT_INCIDENT_LOG, "incident-log", 1, "file to append safety incident reports to (default /var/log/rts2/gemini-udp-incidents.log; incidents are always logged to RTS2 as well)");
	addOption (OPT_NO_SAFETY, "no-safety-watchdog", 0, "do not watch for unexpected mount movement / wrong-way slews / below-horizon pointing (the watchdog is on by default, and can also be toggled at runtime via safety_enabled)");
	addOption (OPT_NO_COLDSTART, "no-safety-coldstart", 0, "no longer does anything - safety incidents never cold-start the mount");
	addOption (OPT_MAX_RECOVERIES, "max-recoveries", 1, "no longer does anything - every incident leaves the mount locked for a human");
	addOption (OPT_POSITION_STATE, "position-state", 1, "file keeping position_trust across driver restarts (default /var/log/rts2/gemini-udp-position.state)");
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
			if (!strcasecmp (optarg, "none"))
				startupModeValue->setValueInteger (GeminiCaringLoop::STARTUP_NONE);
			else if (!strcasecmp (optarg, "restart"))
				startupModeValue->setValueInteger (GeminiCaringLoop::STARTUP_RESTART);
			else if (!strcasecmp (optarg, "warm"))
				startupModeValue->setValueInteger (GeminiCaringLoop::STARTUP_WARM);
			else if (!strcasecmp (optarg, "cold"))
				startupModeValue->setValueInteger (GeminiCaringLoop::STARTUP_COLD);
			else
			{
				logStream (MESSAGE_ERROR) << "unknown --startup-mode \"" << optarg << "\" - expected none, restart, warm or cold" << sendLog;
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
			break;
		case OPT_MAX_RECOVERIES:
			maxRecoveries = atoi (optarg);
			break;
		case OPT_POSITION_STATE:
			positionStatePath = optarg;
			break;
		default:
			return Telescope::processOption (in_opt);
	}
	return 0;
}

// What is actually running. BASE_GIT_DESCRIBE is captured by CMake at
// configure time and the executable's mtime is read here at startup: the two
// answer different halves of the question, because a `cmake --build` after a
// pull relinks the binary without re-running configure, and a reconfigure
// without a build does the opposite. If they disagree, believe the mtime and
// reconfigure. Reading /proc/self/exe fails on anything but Linux, and on a
// binary that has been replaced underneath a running process - the line is
// still worth printing without it.
void GeminiUDP::logBuildIdentity ()
{
	struct stat st;
	char built[64] = "";
	if (stat ("/proc/self/exe", &st) == 0)
	{
		struct tm tm;
		gmtime_r (&st.st_mtime, &tm);
		strftime (built, sizeof (built), "%Y-%m-%dT%H:%M:%SZ", &tm);
	}

	logStream (MESSAGE_INFO) << "GeminiUDP: build " << BASE_GIT_DESCRIBE
		<< (built[0] ? ", executable linked " : "") << built << sendLog;
}

int GeminiUDP::initHardware ()
{
	logBuildIdentity ();

	if (host == nullptr)
	{
		logStream (MESSAGE_ERROR) << "You must specify IP:port of the Gemini-2 mount's UDP interface (-e option)." << sendLog;
		return -1;
	}

	setIdleInfoInterval (1);

	caring = new GeminiCaringLoop (host->getHostname (), host->getPort ());
	caring->setStartupMode ((GeminiCaringLoop::StartupMode) startupModeValue->getValueInteger ());
	caring->setWrongWayMargin (wrongWayMarginValue->getValueDouble ());
	caring->setFlipAmbiguityMargin (flipAmbiguityMarginValue->getValueDouble ());
	caring->setGotoPrestop ((GeminiCaringLoop::GotoPrestop) gotoPrestopValue->getValueInteger ());
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

	loadPositionState ();

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
		{
			caring->queueNativeSet (GEMINI_CMD_RATE_CENTER, (int32_t) newValue->getValueInteger ());
			caring->setCenteringSpeed (newValue->getValueInteger ());
		}
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
	if (oldValue == gotoPrestopValue)
	{
		if (caring)
			caring->setGotoPrestop ((GeminiCaringLoop::GotoPrestop) newValue->getValueInteger ());
		return 0;
	}
	if (oldValue == flipAmbiguityMarginValue)
	{
		if (caring)
			caring->setFlipAmbiguityMargin (newValue->getValueDouble ());
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
	// tracking walks the RA axis towards a western limit that is only where
	// the counters say it is
	if (track && (positionLost () || rezeroState != REZERO_IDLE))
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: tracking refused, " << (positionLost () ? "the mount position is LOST" : "a re-zero is in progress") << sendLog;
		return -1;
	}
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
	// from Dec encoder ticks vs. a per-mount decFlipLimit, 'E'/Dec past
	// half -> 1. Same here, with the half circle read from the mount
	// (native 238/239). The ENQ pier side is only the fallback: it comes
	// from the RA axis relative to CWD, and changes on its own, without any
	// flip, when the telescope tracks through 6h from the meridian - which
	// circumpolar targets do.
	char flipSide = st.decSide () != '?' ? st.decSide () : st.pierSide;
	if (flipSide == 'E' || flipSide == 'W')
		telFlip->setValueInteger (flipSide == 'E' ? 1 : 0);
	pierSideValue->setValueCharArr (std::string (1, st.pierSide).c_str ());
	decSideValue->setValueCharArr (std::string (1, st.decSide ()).c_str ());
	checkSidePrediction (st);
	moveRateValue->setValueCharArr (std::string (1, st.moveRate).c_str ());
	praRawValue->setValueLong (st.praRaw);
	pdecRawValue->setValueLong (st.pdecRaw);
	extendedStatusValue->setValueCharArr (st.rawExtended.c_str ());
	lastMoveErrorValue->setValueCharArr (st.moveFailed ? st.moveFailReason.c_str () : "");
	parkStatusValue->setValueCharArr (std::string (1, st.parkStatus).c_str ());
	trackingSecToLimitValue->setValueDouble (st.trackingSecToWestLimit);
	trackingRateValue->setValueInteger (st.trackingRate);

	// The tracking-limit flip: at most 660 s (11 min, gemini2ser.cpp's
	// threshold) before the western limit, and not before an :MM# to the
	// same target is predicted to flip - on a mount whose limits leave only
	// a few minutes in which the other side accepts the target, 660 s is too
	// early and the flip would be refused. See decideTrackingLimit().
	// Hysteresis (only re-arms above 900 s) keeps it to one decision per
	// approach; pendingLimitAction / limitActionInFlight guard against
	// re-arming mid-flight.
	if (!std::isnan (st.trackingSecToWestLimit))
	{
		if (!trackingLimitWarned && pendingLimitAction == LIMIT_ACTION_NONE && !limitActionInFlight
			&& isTracking () && (getState () & TEL_MASK_MOVING) != TEL_MOVING && !positionLost () && rezeroState == REZERO_IDLE)
		{
			GeminiLimitDecision decision = decideTrackingLimit (st.geometry, st.raAxisTicks, st.decAxisTicks, st.ra,
				st.trackingSecToWestLimit, flipAmbiguityMarginValue->getValueDouble ());
			if (decision != LIMIT_WAIT)
			{
				trackingLimitWarned = true;
				trackingLimitWaitLogged = false;
				armLimitAction (decision == LIMIT_FLIP ? LIMIT_ACTION_FLIP : LIMIT_ACTION_PARK);
			}
			else if (st.trackingSecToWestLimit < 660.0 && !trackingLimitWaitLogged)
			{
				trackingLimitWaitLogged = true;
				logStream (MESSAGE_INFO) << "GeminiUDP: tracking limit " << st.trackingSecToWestLimit
					<< " s away, but the other side of the pier does not accept the target yet - flipping once it does" << sendLog;
			}
		}
		else if (st.trackingSecToWestLimit > 900.0)
		{
			trackingLimitWarned = false;
			trackingLimitWaitLogged = false;
		}
	}

	checkSafety (st);

	logStream (MESSAGE_DEBUG) << "GeminiUDP: caring loop poll OK, RA=" << st.ra << " Dec=" << st.dec
		<< " HA=" << st.ha << " AZ=" << st.az << " ALT=" << st.alt << " LST=" << st.lst
		<< " pier=" << st.pierSide << " rate=" << st.moveRate
		<< " trackingSecToLimit=" << st.trackingSecToWestLimit << sendLog;
}

// Keeps the prediction honest: once a predicted goto has finished, compare
// the Dec axis side the mount ended up on with the one predicted. A miss
// means either Gemini's pointing model moved the target across a window
// edge by more than flip_ambiguity_margin, or a setting the prediction does
// not see (native 229 flip points, "Disable Flip" in Gemini.cfg, a mount
// design other than 0) - either way something to look at, not to act on.
void GeminiUDP::checkSidePrediction (const GeminiStatus &st)
{
	if (st.geometry.valid && !geometryLogged)
	{
		geometryLogged = true;
		const GeminiAxisGeometry &g = st.geometry;
		double k = g.ticksPerDeg ();
		double eastReach = -((g.eastLimit - g.raHalf) / k - 90.0);	// E side accepts gotos east of meridian down to this HA
		double westGoto = (g.raHalf - g.windowLow ()) / k - 90.0;	// W side accepts gotos up to this HA
		double westStop = (g.raHalf - g.westLimit) / k - 90.0;		// W side tracking stops here
		std::ostringstream w;
		w.precision (2);
		w << std::fixed << "E side HA > " << eastReach << ", W side HA < " << westGoto << " (tracking to " << westStop << ")";
		sideWindowValue->setValueCharArr (w.str ().c_str ());
		logStream (MESSAGE_INFO) << "GeminiUDP: goto windows (hour angle, deg): E side of the pier accepts HA > " << eastReach
			<< ", W side HA < " << westGoto << " (west safety limit " << (g.raHalf - g.westLimit) / k << " minus 223 goto limit "
			<< g.westGotoDeg << "), W side tracking stops at HA " << westStop << "; east safety limit " << (g.eastLimit - g.raHalf) / k << sendLog;
		if (westGoto > eastReach)
			logStream (MESSAGE_INFO) << "GeminiUDP: :MS# keeps whichever pier side the mount is on for targets between HA "
				<< eastReach << " and " << westGoto << sendLog;
		else
			logStream (MESSAGE_WARNING) << "GeminiUDP: the limits leave a dead zone: gotos to HA between " << westGoto << " and " << eastReach
				<< " are refused from both sides of the pier" << sendLog;
		if (westStop - eastReach > flipAmbiguityMarginValue->getValueDouble ())
			logStream (MESSAGE_INFO) << "GeminiUDP: a tracking-limit flip is possible from HA " << eastReach << " until the stop at "
				<< westStop << " (" << (westStop - eastReach) * 239.345 << " s)" << sendLog;
		else
			logStream (MESSAGE_WARNING) << "GeminiUDP: the limits leave no time for a tracking-limit flip - targets reaching the western limit get parked" << sendLog;
		if (g.flipPoints > 0)
			logStream (MESSAGE_WARNING) << "GeminiUDP: meridian flip points are enabled (native 229 = " << g.flipPoints
				<< ") - they can force flips the side prediction does not model; set 229 to 0 to rely on it" << sendLog;
	}
	else if (!st.geometry.valid)
	{
		geometryLogged = false;
	}

	if (st.moveEndSerial != loggedMoveEndSerial && !st.moveEndReason.empty ())
	{
		loggedMoveEndSerial = st.moveEndSerial;
		logStream (st.moveFailed || st.moveAborted ? MESSAGE_WARNING : MESSAGE_INFO) << "GeminiUDP: move ended: " << st.moveEndReason << sendLog;
	}

	if (st.gotoSerial == verifiedGotoSerial)
		return;

	gotoPredictionValue->setValueCharArr (st.lastPrediction.describe ().c_str ());

	if (st.moveInProgress)
		return;
	verifiedGotoSerial = st.gotoSerial;

	const GeminiSidePrediction &p = st.lastPrediction;
	if (st.decSide () == '?' || (p.outcome != GeminiSidePrediction::STAY && p.outcome != GeminiSidePrediction::FLIP))
		return;

	// Checked on failed moves too - a move that ended somewhere unexpected is
	// exactly where a wrong prediction matters. It is not counted as a miss,
	// though: a move stopped part way can legitimately sit on either side.
	const char *failedNote = st.moveAborted ? " (the move was stopped before arriving)"
		: (st.moveFailed ? " (the move was reported failed - it may have stopped part way)" : "");
	if (st.decSide () == p.sideAfter)
	{
		logStream (MESSAGE_INFO) << "GeminiUDP: goto ended on pier side " << st.decSide () << " as predicted (" << p.describe () << ")" << failedNote << sendLog;
		return;
	}

	if (!st.moveFailed && !st.moveAborted)
	{
		predictionMissesValue->inc ();
		sendValueAll (predictionMissesValue);
	}
	logStream (MESSAGE_WARNING) << "GeminiUDP: goto ended on pier side " << st.decSide () << ", predicted " << p.describe ()
		<< (p.ambiguous (flipAmbiguityMarginValue->getValueDouble ()) ? " - was within flip_ambiguity_margin" : " - NOT within flip_ambiguity_margin, the rule or its inputs are off")
		<< " (axis ticks RA=" << st.raAxisTicks << " Dec=" << st.decAxisTicks << ")" << failedNote << sendLog;
}

// Reflects the caring loop's startup handshake, and does the RTS2-thread
// half of "the mount just (re)started": everything here has to be able to
// run more than once, because the mount can be power-cycled, rebooted by
// somebody at the hand controller or over the Gemini web interface, or
// cold-started by our own incident recovery, all while this driver keeps
// running.
void GeminiUDP::checkStartup (const GeminiStatus &st)
{
	// not "state": Daemon has a member of that name, and -Wshadow is on
	char startupChar[2] = { st.startupState, 0 };
	startupStateValue->setValueCharArr (startupChar);

	bool wasReady = mountReadyValue->getValueBool ();
	mountReadyValue->setValueBool (st.startupComplete);

	if (wasReady && !st.startupComplete)
	{
		logStream (MESSAGE_WARNING) << "GeminiUDP: mount is no longer started up (handshake now '" << st.startupState
			<< "') - it has rebooted, or is being rebooted. Motion commands are refused until it is back up." << sendLog;
	}

	// a mount waiting in its boot menu with nobody deciding the answer
	if (st.connected && st.startupState == 'b' && pendingHumanStartup == 0 && rezeroState == REZERO_IDLE
		&& startupModeValue->getValueInteger () == GeminiCaringLoop::STARTUP_NONE && !positionLost ())
	{
		setPositionTrust (TRUST_LOST, "the mount is waiting in its boot menu - \"position unmoved\" if nothing moved while it was off, \"position cwd\" if the telescope is at CWD");
		lostOnlyForBootMenu = true;
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

	// the mount's own centering speed, not the driver's default: it sets the
	// speed of a goto whose RA axis does not slew (see pollAxisPosition())
	if (st.centeringSpeed > 0)
	{
		centeringSpeedValue->setValueInteger (st.centeringSpeed);
		sendValueAll (centeringSpeedValue);
		logStream (MESSAGE_INFO) << "GeminiUDP: centering speed " << st.centeringSpeed << "x sidereal (native 170) - an RA axis moving at "
			<< (st.centeringSpeed + 1) * 15.04106858 / 3600.0 << " deg/s during a goto is taken for one that does not slew" << sendLog;
	}

	if (!std::isnan (st.clockOffsetSec) && fabs (st.clockOffsetSec) > 2.0)
		logStream (MESSAGE_WARNING) << "GeminiUDP: the mount clock was " << st.clockOffsetSec << " s off system UTC - reset it" << sendLog;

	judgeStartup (st);

	// the parked flag is battery-backed, so a mount parked before a driver
	// restart or a power cut still says so
	if (st.parkStatus == '1' && (getState () & TEL_MASK_MOVING) != TEL_PARKED && (getState () & TEL_MASK_MOVING) != TEL_PARKING)
	{
		logStream (MESSAGE_INFO) << "GeminiUDP: the mount reports itself parked" << sendLog;
		maskState (TEL_MASK_MOVING, TEL_PARKED, "mount reports parked");
	}
	else if (isTracking () && !positionLost ())
	{
		// a mount that has just come up is not tracking anything, whatever
		// RTS2 still believes - re-assert it rather than leaving the two
		// out of step
		logStream (MESSAGE_INFO) << "GeminiUDP: re-asserting sidereal tracking after mount startup" << sendLog;
		if (caring)
			caring->queueNativeSet (GEMINI_CMD_TRACK_SIDEREAL, (int32_t) 1);
	}
}

// ---- position trust ----

void GeminiUDP::setPositionTrust (PositionTrust trust, const std::string &reason)
{
	if (trust == positionTrust && reason == positionReasonValue->getValue ())
		return;

	PositionTrust previous = positionTrust;
	positionTrust = trust;
	if (trust == TRUST_LOST)
		clearSkyEvidence ("position LOST");
	lostOnlyForBootMenu = false;
	positionTrustValue->setValueInteger (trust);
	positionReasonValue->setValueCharArr (reason.c_str ());
	sendValueAll (positionTrustValue);
	sendValueAll (positionReasonValue);

	logStream (trust == TRUST_LOST ? MESSAGE_CRITICAL : MESSAGE_INFO) << "GeminiUDP: position " << positionTrustValue->getSelName () << " - " << reason << sendLog;
	if (trust == TRUST_LOST && previous != TRUST_LOST)
	{
		logStream (MESSAGE_CRITICAL) << "GeminiUDP: moves, parking and tracking are refused until \"position ok\" (it is fine after all), "
			<< "\"position unmoved\" / \"position cwd\" (from the boot menu), or \"position rezero\" (from collected astrometry)" << sendLog;
		// stop whatever is moving, but leave a parked mount alone: :Q#
		// clears the firmware's park flag
		if (rezeroState == REZERO_IDLE && (getState () & TEL_MASK_MOVING) != TEL_PARKED)
			stopTracking ("position lost");
		appendIncidentLine (std::string ("position LOST: ") + reason);
	}

	GeminiStatus st;
	if (caring)
		st = caring->getStatus ();
	savePositionState (caring ? &st : nullptr);
}

// Evidence that can turn up on any poll, not only at a startup: the Dec
// axis counter landing on exactly half a circle while nothing of ours was
// moving is the signature of a cold or warm start (both set it there without
// looking) that happened behind our back - from the hand controller, the web
// interface, or a power cycle hidden inside a communication gap.
void GeminiUDP::checkPositionEvidence (const GeminiStatus &st)
{
	if (st.moveInProgress || st.parking || rezeroState != REZERO_IDLE)
		lastMotionAt = getNow ();

	if (!st.axisValid || !st.geometry.valid || st.axisTimestamp == lastAxisSampleTimestamp)
		return;
	lastAxisSampleTimestamp = st.axisTimestamp;

	bool atCwdNow = st.decAxisTicks == st.geometry.decHalf;
	if (haveLastAxis && atCwdNow && lastDecTicks != st.geometry.decHalf && getNow () - lastMotionAt > 10.0
		&& pendingHumanStartup == 0 && rezeroState == REZERO_IDLE)
	{
		setPositionTrust (TRUST_LOST, "the Dec axis counter jumped to exactly CWD while no move was in flight - the mount was cold or warm started somewhere nobody confirmed");
	}
	haveLastAxis = true;
	lastDecTicks = st.decAxisTicks;

	if (getNow () - positionStateSavedAt > 60.0)
		savePositionState (&st);
}

// Called once per completed mount startup (see checkStartup()). The rules,
// in the order they are tried:
//  - a human's "position unmoved" / "position cwd" is what this boot was: take
//    their word, but check a warm/cold start really put the counters at CWD
//  - the mount was already up when we connected (a driver restart): nothing
//    happened to it that we know of - carry over what the state file said,
//    unless its counters have since been reset to CWD
//  - it booted, and we answered warm/cold from startup_mode: nobody vouched
//    for it being at CWD - LOST
//  - it booted into a restart (ours, or its own choice): the counters were
//    kept, so the position is as good as before the boot - unless they came
//    back at exactly CWD, which means a cold/warm start after all
void GeminiUDP::judgeStartup (const GeminiStatus &st)
{
	if (rezeroState == REZERO_REBOOTING)
		return;	// runRezero() judges its own cold start

	// whatever the verdict, samples measured against the counters before a
	// boot say nothing reliable about the counters after it
	clearSkyEvidence ("the mount started up");

	// the counters a startup leaves behind are the new baseline for
	// checkPositionEvidence() - a cold start we just judged must not be
	// taken for one behind our back on the next poll
	if (st.axisValid)
	{
		haveLastAxis = true;
		lastDecTicks = st.decAxisTicks;
		lastAxisSampleTimestamp = st.axisTimestamp;
	}

	bool atCwd = st.axisValid && st.geometry.valid && st.decAxisTicks == st.geometry.decHalf;

	bool carriedLostAtBoot = lastStartupCount == 1 && savedTrust == TRUST_LOST;
	if (pendingHumanStartup != 0)
	{
		char said = pendingHumanStartup;
		pendingHumanStartup = 0;
		if (said == 'R' && (carriedLostAtBoot || (positionLost () && !lostOnlyForBootMenu)))
			setPositionTrust (TRUST_LOST, "restarted on the operator's word, but the stored counters were already lost before ("
				+ std::string (positionReasonValue->getValue ()) + ") - \"position ok\" or \"position cwd\"");
		else if (said == 'R')
			setPositionTrust (TRUST_ASSUMED, "operator: nothing moved while the mount was off - restarted with its stored position");
		else if (atCwd)
			setPositionTrust (TRUST_ASSUMED, std::string ("operator: telescope at CWD - mount ") + (said == 'W' ? "warm" : "cold") + " started there");
		else
			setPositionTrust (TRUST_LOST, "operator confirmed CWD, but the mount did not cold/warm start (Dec counter not at CWD) - its boot menu was skipped, check DefaultBootMode");
		return;
	}

	// the first startup this driver sees inherits a LOST from the state file
	bool carriedLost = carriedLostAtBoot;

	if (!st.bootObserved)
	{
		if (lastStartupCount > 1)
			return;	// cannot happen without a boot, but never demote on it
		if (carriedLost)
			setPositionTrust (TRUST_LOST, "still lost from before the driver restart: " + savedReason);
		else if (atCwd && savedHaveAxis && savedDecTicks != st.geometry.decHalf)
			setPositionTrust (TRUST_LOST, "since the driver last ran, the mount's counters were reset to CWD - a cold or warm start nobody confirmed");
		else
			setPositionTrust (TRUST_ASSUMED, std::string ("driver connected to an already running mount")
				+ (savedTrust == TRUST_CONFIRMED ? " (confirmed by the sky before the restart)" : ""));
		return;
	}

	if (st.bootSelection == 'W' || st.bootSelection == 'C')
	{
		setPositionTrust (TRUST_LOST, std::string ("the mount was ") + (st.bootSelection == 'W' ? "warm" : "cold")
			+ " started from startup_mode, which sets the counters to CWD without anybody confirming the telescope is there");
		return;
	}

	if (atCwd && !(haveLastAxis && lastDecTicks == st.geometry.decHalf))
	{
		setPositionTrust (TRUST_LOST, "the mount rebooted and came back with its counters at CWD - a cold or warm start nobody confirmed");
		return;
	}

	if (carriedLost)
	{
		setPositionTrust (TRUST_LOST, "still lost from before the driver restart: " + savedReason);
		return;
	}
	if (positionLost ())
		return;	// a restart keeps the same counters, and so the same doubt

	setPositionTrust (TRUST_ASSUMED, st.bootSelection == 'R' ? "the mount rebooted and restarted from startup_mode with its stored counters"
		: "the mount rebooted by itself, counters kept");
}

// "trust <n>", "reason <text>", "dec <ticks>", "saved <unix time>" - one
// per line, rewritten whole every time
void GeminiUDP::loadPositionState ()
{
	if (positionStatePath == nullptr)
		return;
	std::ifstream f (positionStatePath);
	if (!f.good ())
		return;
	std::string line;
	while (std::getline (f, line))
	{
		size_t sp = line.find (' ');
		if (sp == std::string::npos)
			continue;
		std::string key = line.substr (0, sp), val = line.substr (sp + 1);
		if (key == "trust")
			savedTrust = (PositionTrust) atoi (val.c_str ());
		else if (key == "reason")
			savedReason = val;
		else if (key == "dec")
		{
			savedDecTicks = atol (val.c_str ());
			savedHaveAxis = true;
		}
	}
	if (savedTrust < TRUST_UNKNOWN || savedTrust > TRUST_LOST)
		savedTrust = TRUST_UNKNOWN;
	logStream (MESSAGE_INFO) << "GeminiUDP: position state from " << positionStatePath << ": " << positionTrustValue->getSelName (savedTrust)
		<< " (" << savedReason << ")" << sendLog;
}

void GeminiUDP::savePositionState (const GeminiStatus *st)
{
	positionStateSavedAt = getNow ();
	if (positionStatePath == nullptr || positionTrust == TRUST_UNKNOWN)
		return;
	std::string tmp = std::string (positionStatePath) + ".tmp";
	{
		std::ofstream f (tmp.c_str (), std::ios::trunc);
		if (!f.good ())
		{
			if (!positionStateFailed)
			{
				positionStateFailed = true;
				logStream (MESSAGE_ERROR) << "GeminiUDP: cannot write the position state file " << positionStatePath
					<< " - position_trust will not survive a driver restart; create the directory, make it writable, or use --position-state" << sendLog;
			}
			return;
		}
		f << "trust " << (int) positionTrust << "\n";
		f << "reason " << positionReasonValue->getValue () << "\n";
		if (st && st->axisValid)
			f << "dec " << st->decAxisTicks << "\n";
		f << "saved " << (long) time (nullptr) << "\n";
	}
	rename (tmp.c_str (), positionStatePath);
}

int GeminiUDP::commandAuthorized (rts2core::Connection *conn)
{
	if (conn->isCommand ("position"))
		return positionCommand (conn);
	if (conn->isCommand ("correct"))
		recordSkyEvidence (conn);	// looks only; the framework handles the correction as always
	return Telescope::commandAuthorized (conn);
}

// One command, the first word says what the human is telling the driver:
//
//   position                      report
//   position ok                   the mount's own position is fine - ASSUMED; also releases a safety lock
//   position lost                 it is not - LOST
//   position unmoved              (boot menu) nothing moved while it was off: restart with stored counters
//   position cwd [warm]           the telescope is physically at CWD: cold start there (warm keeps Gemini's model);
//                                 from the boot menu, or by rebooting a running mount
//   position rezero               re-zero now from the astrometric evidence collected so far
//                                 (see recordSkyEvidence()), without waiting for it to qualify
//   position abort                abort a re-zero that has not reached its cold start yet
int GeminiUDP::positionCommand (rts2core::Connection *conn)
{
	if (caring == nullptr)
	{
		conn->sendCommandEnd (DEVDEM_E_HW, "no mount connection");
		return -1;
	}
	GeminiStatus st = caring->getStatus ();

	if (conn->paramEnd ())
	{
		logStream (MESSAGE_INFO) << "GeminiUDP: position " << positionTrustValue->getSelName () << " - " << positionReasonValue->getValue ()
			<< "; handshake '" << st.startupState << "', rezero " << rezeroStateValue->getValue ()
			<< ", axis ticks RA=" << st.raAxisTicks << " Dec=" << st.decAxisTicks << sendLog;
		return 0;
	}

	char *what;
	if (conn->paramNextString (&what))
		return DEVDEM_E_PARAMSNUM;

	bool inBootMenu = st.connected && st.startupState == 'b';
	auto refuse = [conn] (const char *why) { conn->sendCommandEnd (DEVDEM_E_PARAMSVAL, why); return -1; };

	if (rezeroState != REZERO_IDLE && strcasecmp (what, "abort"))
		return refuse ("a re-zero is in progress - \"position abort\" first");

	if (!strcasecmp (what, "rezero"))
	{
		if (!conn->paramEnd ())
			return DEVDEM_E_PARAMSNUM;
		if (skyEvidence.empty ())
			return refuse ("no astrometric evidence collected yet");
		// the median of everything collected - the operator decides it is
		// good enough, the agreement/spread rules are not applied
		std::vector<double> ra, dec;
		for (const auto &ev : skyEvidence)
		{
			ra.push_back (ev.raErrDeg);
			dec.push_back (ev.decErrDeg);
		}
		std::sort (ra.begin (), ra.end ());
		std::sort (dec.begin (), dec.end ());
		double mRa = ra[ra.size () / 2], mDec = dec[dec.size () / 2];
		char buf[160];
		snprintf (buf, sizeof (buf), "operator, median of %d samples: RA axis %+.3f, Dec axis %+.3f deg", (int) skyEvidence.size (), mRa, mDec);
		std::string err;
		if (beginRezero (mRa, mDec, buf, err))
		{
			logStream (MESSAGE_ERROR) << "GeminiUDP: position rezero refused: " << err << sendLog;
			conn->sendCommandEnd (DEVDEM_E_PARAMSVAL, err.c_str ());
			return -1;
		}
		return 0;
	}

	if (!strcasecmp (what, "abort"))
	{
		if (!conn->paramEnd ())
			return DEVDEM_E_PARAMSNUM;
		if (rezeroState == REZERO_IDLE)
			return refuse ("no re-zero in progress");
		if (rezeroState == REZERO_REBOOTING)
			return refuse ("the re-zero cold start has already been sent");
		abortRezero ("aborted by operator");
		return 0;
	}

	if (!strcasecmp (what, "ok"))
	{
		if (!conn->paramEnd ())
			return DEVDEM_E_PARAMSNUM;
		if (!st.startupComplete)
			return refuse ("the mount is not started up - \"position unmoved\" or \"position cwd\"");
		if (safetyState == SAFETY_STOPPING || safetyState == SAFETY_PARKING)
			return refuse ("a safety recovery is still running");
		if (safetyState == SAFETY_LOCKED)
		{
			logStream (MESSAGE_WARNING) << "GeminiUDP: safety lock released by \"position ok\"" << sendLog;
			unBlockMove ();
			setSafetyState (SAFETY_OK);
		}
		setPositionTrust (TRUST_ASSUMED, "operator: the mount's position is fine");
		return 0;
	}

	if (!strcasecmp (what, "lost"))
	{
		if (!conn->paramEnd ())
			return DEVDEM_E_PARAMSNUM;
		setPositionTrust (TRUST_LOST, "operator: the mount's position is not to be trusted");
		return 0;
	}

	if (!strcasecmp (what, "unmoved"))
	{
		if (!conn->paramEnd ())
			return DEVDEM_E_PARAMSNUM;
		if (!inBootMenu)
			return refuse ("the mount is not waiting in its boot menu - \"position ok\" if it is up and fine");
		pendingHumanStartup = 'R';
		caring->selectStartup (GeminiCaringLoop::STARTUP_RESTART);
		logStream (MESSAGE_INFO) << "GeminiUDP: answering the boot menu with restart (bR#) on the operator's word" << sendLog;
		return 0;
	}

	if (!strcasecmp (what, "cwd"))
	{
		bool warm = false;
		if (!conn->paramEnd ())
		{
			char *mode;
			if (conn->paramNextString (&mode) || !conn->paramEnd () || strcasecmp (mode, "warm"))
				return DEVDEM_E_PARAMSNUM;
			warm = true;
		}
		GeminiCaringLoop::StartupMode mode = warm ? GeminiCaringLoop::STARTUP_WARM : GeminiCaringLoop::STARTUP_COLD;
		pendingHumanStartup = warm ? 'W' : 'C';
		if (inBootMenu)
		{
			caring->selectStartup (mode);
			logStream (MESSAGE_INFO) << "GeminiUDP: answering the boot menu with " << (warm ? "warm (bW#)" : "cold (bC#)") << " start - telescope at CWD on the operator's word" << sendLog;
		}
		else
		{
			if (st.moveInProgress || st.parking || st.moveRate == 'S' || st.moveRate == 'C')
			{
				pendingHumanStartup = 0;
				return refuse ("the mount is moving");
			}
			stopTracking ("position cwd");
			caring->requestReboot (!warm, mode);
			logStream (MESSAGE_WARNING) << "GeminiUDP: rebooting the mount into a " << (warm ? "warm" : "cold") << " start at CWD on the operator's word" << sendLog;
		}
		return 0;
	}

	return refuse ("expected: position [ok | lost | unmoved | cwd [warm] | rezero | abort]");
}

// ---- re-zero from the sky ----
//
// The evidence. The framework's closed loop: astrometry solves an image and
// reports ra_err/dec_err = header position (CRVAL, i.e. OBJ of the move) minus
// the solved centre; the executor forwards that as "correct", and the
// framework adds it to the correction the next move of the same target gets.
// So whenever the framework would take a "correct" at face value - the same
// move (MOVE_NUM) and the same correction state (CORR_IMG/CORR_OBS) the image
// was taken with - the telescope truly points at OBJ - (ra_err, dec_err),
// J2000, for as long as it keeps tracking that target. That is all a re-zero
// needs. Nothing is changed here; the framework then handles the correction
// exactly as before.
void GeminiUDP::recordSkyEvidence (rts2core::Connection *conn)
{
	int corMark, corrImg, corrObs, imgId, obsId;
	double raErr, decErr, posErrDeg;
	if (sscanf (conn->getCommandFull ().c_str (), "correct %d %d %d %d %d %lf %lf %lf", &corMark, &corrImg, &corrObs, &imgId, &obsId, &raErr, &decErr, &posErrDeg) != 8)
		return;
	if (caring == nullptr || rezeroState != REZERO_IDLE || positionLost ())
		return;

	rts2core::Value *moveNumV = getOwnValue ("MOVE_NUM");
	rts2core::Value *corrImgV = getOwnValue ("CORR_IMG");
	rts2core::Value *corrObsV = getOwnValue ("CORR_OBS");
	rts2core::ValueRaDec *obj = (rts2core::ValueRaDec *) getOwnValue ("OBJ");
	rts2core::ValueRaDec *offs = (rts2core::ValueRaDec *) getOwnValue ("OFFS");
	if (!moveNumV || !corrImgV || !corrObsV || !obj || !offs)
		return;

	auto skip = [this] (const std::string &why)
	{
		logStream (MESSAGE_DEBUG) << "GeminiUDP: astrometry not used as re-zero evidence: " << why << sendLog;
	};
	if (corMark != moveNumV->getValueInteger () || corrImg != corrImgV->getValueInteger () || corrObs != corrObsV->getValueInteger ())
		return skip ("taken during another move or correction state (MOVE_NUM " + std::to_string (corMark) + " vs " + std::to_string (moveNumV->getValueInteger ()) + ")");
	// an offset applied after the exposure would shift OBJ under the image
	if (offs->getRa () != 0 || offs->getDec () != 0)
		return skip ("target offsets are in use");

	GeminiStatus st = caring->getStatus ();
	if (!isTracking () || st.moveInProgress || st.parking || st.moveRate == 'S' || st.moveRate == 'C')
		return skip ("the mount is not simply tracking its target");

	SkyEvidence sample;
	std::string err;
	if (!measureCounterError (ln_range_degrees (obj->getRa () - raErr), obj->getDec () - decErr, st, sample, err))
		return skip (err);
	sample.moveNum = corMark;

	// one per target: the newest replaces an older one of the same move
	for (auto it = skyEvidence.begin (); it != skyEvidence.end (); )
		it = it->moveNum == corMark ? skyEvidence.erase (it) : it + 1;
	skyEvidence.push_back (sample);
	while (skyEvidence.size () > 10)
		skyEvidence.erase (skyEvidence.begin ());

	logStream (MESSAGE_INFO) << "GeminiUDP: re-zero evidence from image " << imgId << " (move " << corMark << ", pos_err " << posErrDeg << "): " << sample.summary << sendLog;
	evaluateSkyEvidence ();
}

// Given where the telescope truly points (J2000), work out where Gemini's
// axis counters should read at this physical position:
//  1. the true position goes through the same pipeline as a goto target -
//     precession, nutation, aberration, refraction as configured, then this
//     driver's pointing model for the pier side the mount is on - giving the
//     mount frame coordinate that "points here"
//  2. computeCounterError(): the firmware's post-cold-start relation maps it
//     to counters; the difference from the counters now is the zero error
// The hour angle comes from the mount's own LST; nothing depends on Gemini's
// index terms or sync offsets.
bool GeminiUDP::measureCounterError (double raJ2000, double decJ2000, const GeminiStatus &st, SkyEvidence &sample, std::string &err)
{
	if (!st.valid || !st.startupComplete || !st.axisValid || !st.geometry.valid)
	{
		err = "the axis position and geometry (native 239/238/231) have not been read";
		return false;
	}

	double jd = ln_get_julian_from_sys ();
	struct ln_equ_posn pos;
	pos.ra = raJ2000;
	pos.dec = decJ2000;
	struct ln_hrz_posn hrz;
	applyCorrections (&pos, jd, 0, &hrz, false);
	if (hrz.alt < 20.0)
	{
		err = "below 20 deg altitude, refraction and flexure make it too uncertain";
		return false;
	}

	char side = st.decSide ();
	double mountRa, mountDec;
	computeModelCorrection (pos.ra, pos.dec, st.lst, side, mountRa, mountDec);

	const GeminiAxisGeometry &g = st.geometry;
	GeminiCounterError e = computeCounterError (g, st.raAxisTicks, st.decAxisTicks, side, st.lst, mountRa, mountDec);

	// The relation is read from the firmware, not from a manual. The mount's
	// own idea of where it points goes through the same relation and can
	// only be off by what Gemini's index terms and syncs hold - never by a
	// large fraction of a turn. If it is, the relation does not fit this
	// mount, and moving the axes on its say-so could be dangerous.
	GeminiCounterError belief = computeCounterError (g, st.raAxisTicks, st.decAxisTicks, side, st.lst, st.ra, st.dec);

	char buf[200];
	snprintf (buf, sizeof (buf), "RA axis %+.3f, Dec axis %+.3f deg at HA %+.1f Dec %+.1f side %c (mount's own offsets %+.3f, %+.3f)",
		e.raDeg, e.decDeg, ln_range_degrees (st.lst - mountRa + 180.0) - 180.0, mountDec, side, belief.raDeg, belief.decDeg);
	if (fabs (belief.raDeg) > 60.0 || fabs (belief.decDeg) > 60.0)
	{
		err = std::string ("the cold-start relation does not fit the mount's own reported position (") + buf + ") - needs checking against the firmware analysis";
		logStream (MESSAGE_ERROR) << "GeminiUDP: " << err << sendLog;
		return false;
	}

	sample.at = getNow ();
	sample.raErrDeg = e.raDeg;
	sample.decErrDeg = e.decDeg;
	sample.ha = ln_range_degrees (st.lst - mountRa + 180.0) - 180.0;
	sample.dec = mountDec;
	sample.side = side;
	sample.summary = buf;
	return true;
}

// When does the evidence justify spending the observing time?
//  - a sample below rezero_min alone confirms the position
//  - a re-zero needs the last rezero_samples samples (distinct targets) to
//    agree within rezero_agree of their median, two of them to be at least
//    rezero_spread apart on the sky (a zero error is the same everywhere; a
//    pointing model error or a bad solve is not), the median error above
//    rezero_min, and rezero_interval since the last re-zero
//  - an agreed error above rezero_max is not something to fix automatically:
//    LOST
void GeminiUDP::evaluateSkyEvidence ()
{
	std::ostringstream all;
	all.precision (3);
	for (const auto &ev : skyEvidence)
		all << std::fixed << ev.raErrDeg << "/" << ev.decErrDeg << "@" << std::setprecision (0) << ev.ha << "," << ev.dec << ev.side << std::setprecision (3) << " ";
	skyEvidenceValue->setValueCharArr (all.str ().c_str ());
	sendValueAll (skyEvidenceValue);

	const SkyEvidence &newest = skyEvidence.back ();
	double minErr = rezeroMinValue->getValueDouble ();
	if (std::max (fabs (newest.raErrDeg), fabs (newest.decErrDeg)) < minErr)
	{
		if (rezeroArmed)
		{
			rezeroArmed = false;
			rezeroArmedValue->setValueBool (false);
			sendValueAll (rezeroArmedValue);
			logStream (MESSAGE_INFO) << "GeminiUDP: re-zero disarmed - the newest astrometry agrees with the counters" << sendLog;
		}
		if (positionTrust != TRUST_CONFIRMED)
			setPositionTrust (TRUST_CONFIRMED, "astrometry agrees with the counters: " + newest.summary);
		return;
	}

	size_t n = (size_t) std::max (1, rezeroSamplesValue->getValueInteger ());
	if (skyEvidence.size () < n)
		return;
	std::vector<SkyEvidence> last (skyEvidence.end () - n, skyEvidence.end ());

	std::vector<double> ra, dec;
	for (const auto &ev : last)
	{
		ra.push_back (ev.raErrDeg);
		dec.push_back (ev.decErrDeg);
	}
	std::sort (ra.begin (), ra.end ());
	std::sort (dec.begin (), dec.end ());
	double mRa = ra[n / 2], mDec = dec[n / 2];

	double agree = rezeroAgreeValue->getValueDouble ();
	double spread = 0;
	for (size_t i = 0; i < n; i++)
	{
		if (fabs (last[i].raErrDeg - mRa) > agree || fabs (last[i].decErrDeg - mDec) > agree)
		{
			logStream (MESSAGE_INFO) << "GeminiUDP: re-zero evidence does not agree yet (sample " << last[i].summary << " vs median "
				<< mRa << ", " << mDec << ")" << sendLog;
			return;
		}
		for (size_t j = i + 1; j < n; j++)
		{
			struct ln_equ_posn a, b;
			a.ra = last[i].ha;
			a.dec = last[i].dec;
			b.ra = last[j].ha;
			b.dec = last[j].dec;
			spread = std::max (spread, ln_get_angular_separation (&a, &b));
		}
	}
	if (n > 1 && spread < rezeroSpreadValue->getValueDouble ())
	{
		logStream (MESSAGE_INFO) << "GeminiUDP: re-zero evidence agrees, but its pointings are only " << spread << " deg apart" << sendLog;
		return;
	}

	char buf[200];
	snprintf (buf, sizeof (buf), "%d targets agree: RA axis %+.3f, Dec axis %+.3f deg (spread %.0f deg)", (int) n, mRa, mDec, spread);
	if (std::max (fabs (mRa), fabs (mDec)) > rezeroMaxValue->getValueDouble ())
	{
		setPositionTrust (TRUST_LOST, std::string ("astrometry: ") + buf + ", beyond rezero_max");
		return;
	}
	if (lastRezeroAt > 0 && getNow () - lastRezeroAt < rezeroIntervalValue->getValueDouble () * 3600.0)
	{
		logStream (MESSAGE_WARNING) << "GeminiUDP: " << buf << " - but the last re-zero was less than rezero_interval ago" << sendLog;
		return;
	}
	if (!rezeroArmed)
		logStream (MESSAGE_WARNING) << "GeminiUDP: re-zero ARMED - " << buf
			<< (rezeroAutoValue->getValueBool () ? "; it runs before the next move to a new target" : "; rezero_auto is off, \"position rezero\" to run it") << sendLog;
	rezeroArmed = true;
	rezeroArmedValue->setValueBool (true);
	sendValueAll (rezeroArmedValue);
}

void GeminiUDP::clearSkyEvidence (const char *why)
{
	if (skyEvidence.empty () && !rezeroArmed)
		return;
	logStream (MESSAGE_INFO) << "GeminiUDP: re-zero evidence cleared: " << why << sendLog;
	skyEvidence.clear ();
	rezeroArmed = false;
	rezeroArmedValue->setValueBool (false);
	sendValueAll (rezeroArmedValue);
	skyEvidenceValue->setValueCharArr ("");
	sendValueAll (skyEvidenceValue);
}

// The execution: step both axes (:MP, absolute counters) to CWD minus the
// error - which is where true CWD is - and cold-start there. Gemini's own
// model terms other than the index terms (which only ever held the old zero
// error) are read before and put back after.
int GeminiUDP::beginRezero (double raErrDeg, double decErrDeg, const std::string &summary, std::string &err)
{
	if (caring == nullptr)
	{
		err = "no mount connection";
		return -1;
	}
	GeminiStatus st = caring->getStatus ();
	if (rezeroState != REZERO_IDLE)
	{
		err = "a re-zero is already in progress";
		return -1;
	}
	if (!st.valid || !st.startupComplete || !st.axisValid || !st.geometry.valid)
	{
		err = "the mount is not up, or its axis position and geometry have not been read";
		return -1;
	}
	if (st.parking || safetyState == SAFETY_STOPPING || safetyState == SAFETY_PARKING)
	{
		err = "the mount is parking or a safety recovery is running";
		return -1;
	}
	if (std::max (fabs (raErrDeg), fabs (decErrDeg)) > rezeroMaxValue->getValueDouble ())
	{
		err = "the error exceeds rezero_max";
		return -1;
	}

	const GeminiAxisGeometry &g = st.geometry;
	int32_t targetRa = (int32_t) lround (g.raHalf - raErrDeg * g.ticksPerDeg ());
	int32_t targetDec = (int32_t) lround (g.decHalf - decErrDeg * g.decHalf / 180.0);
	if (!(g.westLimit < targetRa && targetRa < g.eastLimit) || targetDec <= 0 || targetDec >= 2 * g.decHalf)
	{
		err = "CWD corrected by that error lies outside the mount's safety limits";
		return -1;
	}

	rezeroModelTerms.clear ();
	static const int terms[] = { 201, 202, 203, 204, 207, 208, 209, 211 };
	for (int id : terms)
	{
		std::string value;
		if (!caring->readNativeRaw (id, value, 2.0))
		{
			err = "could not read Gemini model term " + std::to_string (id) + " to restore it after the cold start";
			return -1;
		}
		int v = atoi (value.c_str ());
		if (v != 0)
			rezeroModelTerms.push_back ({ id, v });
	}

	rezeroSummary = summary;
	logStream (MESSAGE_WARNING) << "GeminiUDP: RE-ZERO (" << summary << "): stopping, stepping the axes to counters RA=" << targetRa << " Dec=" << targetDec
		<< " (true CWD), then cold-starting the mount there; " << rezeroModelTerms.size () << " Gemini model terms to restore" << sendLog;
	appendIncidentLine ("re-zero started: " + summary);

	stopTracking ("re-zero");	// before the state changes - stopMove() aborts a running re-zero
	caring->requestAbort ();
	rezeroTargetRa = targetRa;
	rezeroTargetDec = targetDec;
	setRezeroState (REZERO_STOPPING);
	return 0;
}

void GeminiUDP::setRezeroState (RezeroState newState)
{
	static const char *names[] = { "IDLE", "STOPPING", "MOVING", "SETTLING", "REBOOTING" };
	rezeroState = newState;
	rezeroSince = getNow ();
	rezeroStableCount = 0;
	rezeroStateValue->setValueCharArr (names[newState]);
	sendValueAll (rezeroStateValue);
}

void GeminiUDP::abortRezero (const std::string &why)
{
	if (rezeroThenMove)
	{
		rezeroThenMove = false;
		rezeroMoveFailed = true;
	}
	if (caring && rezeroState != REZERO_REBOOTING)
		caring->requestAbort ();
	setRezeroState (REZERO_IDLE);
	setPositionTrust (TRUST_LOST, "re-zero did not finish (" + why + ") - the axes are somewhere between the old position and CWD");
}

void GeminiUDP::runRezero (const GeminiStatus &st)
{
	if (rezeroState == REZERO_IDLE)
		return;

	double elapsed = getNow () - rezeroSince;
	bool newSample = st.axisValid && st.axisTimestamp != rezeroLastSample;

	switch (rezeroState)
	{
		case REZERO_STOPPING:
		{
			if (elapsed < 3.0 || st.moveRate == 'S' || st.moveRate == 'C')
			{
				if (elapsed > 30.0)
					abortRezero ("the mount did not stop");
				return;
			}
			char cmd[64];
			snprintf (cmd, sizeof (cmd), ":MP%d;%d#", rezeroTargetRa, rezeroTargetDec);
			std::string reply;
			if (!caring->sendRawSync (cmd, reply, 3.0) || reply != "1")
			{
				abortRezero (std::string ("the mount refused ") + cmd + ", reply \"" + reply + "\"");
				return;
			}
			logStream (MESSAGE_INFO) << "GeminiUDP: RE-ZERO: " << cmd << " accepted" << sendLog;
			setRezeroState (REZERO_MOVING);
			return;
		}

		case REZERO_MOVING:
		{
			if (elapsed > 300.0)
			{
				abortRezero ("the axes did not reach the target counters within 300 s");
				return;
			}
			if (!newSample)
				return;
			rezeroLastSample = st.axisTimestamp;
			double tolerance = std::max (4.0, 5.0 / 3600.0 * st.geometry.ticksPerDeg ());
			bool there = fabs ((double) st.raAxisTicks - rezeroTargetRa) <= tolerance && fabs ((double) st.decAxisTicks - rezeroTargetDec) <= tolerance
				&& st.moveRate != 'S' && st.moveRate != 'C';
			rezeroStableCount = there ? rezeroStableCount + 1 : 0;
			if (rezeroStableCount >= 2)
			{
				// a positional move may leave the worm running
				caring->queueNativeSet (GEMINI_CMD_TRACK_TERRESTRIAL, (int32_t) 1);
				rezeroLastRa = st.raAxisTicks;
				rezeroLastDec = st.decAxisTicks;
				setRezeroState (REZERO_SETTLING);
			}
			return;
		}

		case REZERO_SETTLING:
		{
			if (elapsed > 60.0)
			{
				abortRezero ("the axes did not come to rest at the target counters");
				return;
			}
			if (!newSample)
				return;
			rezeroLastSample = st.axisTimestamp;
			bool still = abs (st.raAxisTicks - rezeroLastRa) <= 1 && abs (st.decAxisTicks - rezeroLastDec) <= 1;
			rezeroLastRa = st.raAxisTicks;
			rezeroLastDec = st.decAxisTicks;
			rezeroStableCount = still ? rezeroStableCount + 1 : 0;
			if (rezeroStableCount < 2)
				return;

			double k = st.geometry.ticksPerDeg ();
			logStream (MESSAGE_WARNING) << "GeminiUDP: RE-ZERO: axes at counters RA=" << st.raAxisTicks << " Dec=" << st.decAxisTicks
				<< " (residual " << (st.raAxisTicks - rezeroTargetRa) / k * 3600.0 << ", " << (st.decAxisTicks - rezeroTargetDec) / k * 3600.0
				<< " arcsec) - cold-starting the mount (native 65533)" << sendLog;
			rezeroStartupBaseline = st.startupCount;
			caring->requestReboot (true, GeminiCaringLoop::STARTUP_COLD);
			setRezeroState (REZERO_REBOOTING);
			return;
		}

		case REZERO_REBOOTING:
		{
			// requestReboot() cleared axisValid and geometry.valid; the
			// post-startup sequence reads both afresh
			if (st.startupCount == rezeroStartupBaseline || !st.axisValid || !st.geometry.valid)
			{
				if (elapsed > 300.0)
				{
					setRezeroState (REZERO_IDLE);
					if (rezeroThenMove)
					{
						rezeroThenMove = false;
						rezeroMoveFailed = true;
					}
					setPositionTrust (TRUST_LOST, "re-zero: the mount did not come back within 300 s of its cold start");
				}
				return;
			}

			const GeminiAxisGeometry &g = st.geometry;
			double allowedRa = 4.0 + (getNow () - rezeroSince) * 15.04106858 / 3600.0 * g.ticksPerDeg ();
			bool atCwd = st.decAxisTicks == g.decHalf && fabs ((double) st.raAxisTicks - g.raHalf) <= allowedRa;
			setRezeroState (REZERO_IDLE);
			caring->queueNativeSet (GEMINI_CMD_TRACK_TERRESTRIAL, (int32_t) 1);
			if (!atCwd)
			{
				if (rezeroThenMove)
				{
					rezeroThenMove = false;
					rezeroMoveFailed = true;
				}
				setPositionTrust (TRUST_LOST, "re-zero: after the cold start the counters are not at CWD (RA=" + std::to_string (st.raAxisTicks)
					+ " Dec=" + std::to_string (st.decAxisTicks) + ") - the boot menu was skipped?");
				return;
			}
			for (const auto &term : rezeroModelTerms)
				caring->queueNativeSet (term.first, (int32_t) term.second);

			// the accumulated astrometric corrections compensated the old
			// zero error; they would now apply it twice
			zeroCorrRaDec ();
			if (safetyState == SAFETY_LOCKED)
			{
				unBlockMove ();
				setSafetyState (SAFETY_OK);
			}
			appendIncidentLine ("re-zero finished: " + rezeroSummary);
			setPositionTrust (TRUST_CONFIRMED, "re-zeroed from the sky: " + rezeroSummary);
			clearSkyEvidence ("re-zero finished");
			lastRezeroAt = getNow ();
			if (rezeroThenMove)
			{
				rezeroThenMove = false;
				struct ln_equ_posn pos;
				getTarget (&pos);
				logStream (MESSAGE_WARNING) << "GeminiUDP: RE-ZERO finished, continuing with the move it was run before" << sendLog;
				if (!doGoto (pos.ra, pos.dec, "move after re-zero"))
					rezeroMoveFailed = true;
			}
			else
			{
				logStream (MESSAGE_WARNING) << "GeminiUDP: RE-ZERO finished, the mount is at CWD and not tracking - re-issue the move" << sendLog;
			}
			return;
		}

		default:
			return;
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

	// a move recovery owns the mount (it is deliberately parking to CWD) -
	// leave every check to it, and start from clean counts afterwards
	if (recoverState != RECOVER_IDLE)
	{
		unexpectedMoveCount = belowHorizonCount = 0;
		return;
	}

	rts2_status_t moving = getState () & TEL_MASK_MOVING;
	bool weCommandedMovement = (moving == TEL_MOVING || moving == TEL_PARKING) || st.moveInProgress || rezeroState != REZERO_IDLE;

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
	// Also while a move of ours is in flight - especially then. On SBT
	// (2026-09-14) W->E meridian flips swung the Dec axis to the other side
	// while the RA axis did not slew; the telescope went to alt 3.8 deg in one
	// test and to alt -49 deg (tube down, counterweight up) in another. That
	// is not a flip path: done with both axes, a flip passes near the pole.
	// (For one build this check was skipped during slews on the mistaken
	// reading that the low pass was normal. It is not.)
	if (moving != TEL_PARKED && st.alt < safetyAltLimitValue->getValueDouble ())
		belowHorizonCount++;
	else
		belowHorizonCount = 0;

	if (belowHorizonCount >= SAFETY_CONFIRM_POLLS)
	{
		std::ostringstream detail;
		detail << "mount reports alt=" << st.alt << " az=" << st.az << " deg, below the " << safetyAltLimitValue->getValueDouble ()
			<< " deg safety limit, for " << belowHorizonCount << " consecutive polls";
		belowHorizonCount = 0;
		// low while a move of ours is in flight is a move that failed to
		// execute (a flip whose RA axis did not slew, so one axis alone drove
		// the tube down), not a controller we distrust: recover it from CWD,
		// don't lock the mount. The counters are still sound.
		if ((moving == TEL_MOVING || st.moveInProgress) && !positionLost () && tryStartMoveRecovery ("pointed below the horizon during a move (" + detail.str () + ")"))
			return;
		bool active = weCommandedMovement || isTracking () || st.moveRate != 'N';
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
	static const char *names[] = { "OK", "STOPPING", "PARKING", "LOCKED" };
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
	safetyLosesPosition = escalate;

	incidentCountValue->inc ();
	sendValueAll (incidentCountValue);
	lastIncidentValue->setValueCharArr (safetyReason.c_str ());
	sendValueAll (lastIncidentValue);

	logStream (MESSAGE_CRITICAL) << "GeminiUDP: SAFETY INCIDENT #" << incidentCountValue->getValueInteger () << " - " << safetyReason
		<< " -> stopping, parking, then holding the mount locked" << (safetyLosesPosition ? " with its position marked LOST" : "") << sendLog;

	if (caring)
		writeIncidentReport (caring->getStatus ());

	if (rezeroState != REZERO_IDLE && rezeroState != REZERO_REBOOTING)
		abortRezero ("safety incident");

	// stopTracking() calls stopMove() for us, which is requestAbort() -
	// but ask for the abort explicitly too: whatever is going on, the one
	// thing worth spending a datagram on immediately is ":Q#".
	stopTracking ("safety incident");
	if (caring)
		caring->requestAbort ();

	setSafetyState (SAFETY_STOPPING);
}

// Drives the stop -> park -> lock sequence. Runs from idle(), one step per
// tick, so nothing here may block. It ends locked, always: what happens next
// is a human's call ("position ok" releases it), and when the incident casts
// doubt on the axis counters the position is LOST as well, which only a
// human's word or a re-zero from the sky lifts.
void GeminiUDP::runSafetyRecovery (const GeminiStatus &st)
{
	constexpr double STOP_SETTLE_SEC = 2.0;
	constexpr double PARK_TIMEOUT_SEC = 180.0;

	if (safetyState == SAFETY_OK || safetyState == SAFETY_LOCKED)
		return;

	double elapsed = getNow () - safetyStateSince;

	auto finish = [this] (const char *how)
	{
		setSafetyState (SAFETY_LOCKED);
		if (safetyLosesPosition)
			setPositionTrust (TRUST_LOST, std::string ("safety incident (") + how + "): " + safetyReason);
		logStream (MESSAGE_CRITICAL) << "GeminiUDP: SAFETY - mount held locked (" << how << "). Look at " << incidentLogPath
			<< ", then \"position ok\" if the position is fine, or \"position cwd\" if it is not." << sendLog;
	};

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
				logStream (MESSAGE_ERROR) << "GeminiUDP: SAFETY - could not start a park" << sendLog;
				appendIncidentLine ("park could NOT be started - locked");
				finish ("park could not be started");
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
				appendIncidentLine ("parked - locked");
				finish ("parked");
			}
			else
			{
				logStream (MESSAGE_ERROR) << "GeminiUDP: SAFETY - park did not complete within " << PARK_TIMEOUT_SEC << "s" << sendLog;
				appendIncidentLine ("park did NOT complete within the timeout - locked");
				// a park that never finished is itself a reason to doubt the counters
				safetyLosesPosition = true;
				finish ("park did not complete");
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
	o << "action:      stop -> park -> locked" << (safetyLosesPosition ? ", position LOST" : "") << "\n";
	o << "position:    " << positionTrustValue->getSelName () << " (" << positionReasonValue->getValue () << ")"
		<< " axis ticks RA=" << st.raAxisTicks << " Dec=" << st.decAxisTicks << (st.axisValid ? "" : " (not read)") << "\n";
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
		<< " move_separation=" << st.moveSeparation << " last_move_separation=" << st.lastMoveSeparation << "\n";
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

// Starts a recovery if this failure is worth one: the position is trusted, a
// re-zero is not already running, and the retry budget is not spent. Captures
// the framework's target to re-send after the CWD park.
bool GeminiUDP::tryStartMoveRecovery (const std::string &reason)
{
	if (caring == nullptr || positionLost () || rezeroState != REZERO_IDLE || recoverState != RECOVER_IDLE)
		return false;
	if (moveRetries >= moveRetriesValue->getValueInteger ())
	{
		logStream (MESSAGE_CRITICAL) << "GeminiUDP: move failed to execute (" << reason << ") - retry budget of "
			<< moveRetriesValue->getValueInteger () << " spent; leaving the mount to park at CWD" << sendLog;
		return false;
	}

	struct ln_equ_posn tar;
	getTarget (&tar);
	recoverTargetRa = tar.ra;
	recoverTargetDec = tar.dec;
	moveRetries++;
	logStream (MESSAGE_WARNING) << "GeminiUDP: move failed to execute (" << reason << ") - recovery " << moveRetries
		<< " of " << moveRetriesValue->getValueInteger () << ": stop, park to CWD, then retry RA=" << tar.ra << " Dec=" << tar.dec << sendLog;
	appendIncidentLine ("move execution failure, recovery " + std::to_string (moveRetries) + ": " + reason);

	caring->requestAbort ();
	recoverState = RECOVER_STOPPING;
	recoverSince = getNow ();
	moveRecoveryValue->setValueCharArr ("STOPPING");
	sendValueAll (moveRecoveryValue);
	return true;
}

// One step per idle() tick. Uses the caring-loop park (:hC#) directly, not
// the framework's park hook, so the framework's move state is untouched and
// the whole stop/park/retry looks like one continuous move to it.
void GeminiUDP::runMoveRecovery (const GeminiStatus &st)
{
	constexpr double STOP_SETTLE_SEC = 2.5;
	constexpr double PARK_TIMEOUT_SEC = 180.0;

	auto giveUp = [this] (const char *why)
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: move recovery gave up (" << why << ") - mount left parked at CWD, position still trusted" << sendLog;
		appendIncidentLine (std::string ("move recovery gave up: ") + why);
		recoverState = RECOVER_IDLE;
		recoverGiveUp = true;
		moveRecoveryValue->setValueCharArr ("IDLE");
		sendValueAll (moveRecoveryValue);
	};

	switch (recoverState)
	{
		case RECOVER_IDLE:
			return;
		case RECOVER_STOPPING:
			if (getNow () - recoverSince < STOP_SETTLE_SEC)
				return;
			caring->requestPark (true);	// :hC#, park at CWD
			recoverState = RECOVER_PARKING;
			recoverSince = getNow ();
			moveRecoveryValue->setValueCharArr ("PARKING");
			sendValueAll (moveRecoveryValue);
			return;
		case RECOVER_PARKING:
			if (st.parkFailed)
			{
				giveUp ("the CWD park failed");
				return;
			}
			if (!st.parking && st.parkStatus == '1')	// parked at CWD
			{
				// Leave the recovery state BEFORE the retry goto, deliberately:
				// the retry is a real slew and must run with checkSafety() and
				// the RA-crawl detector live (a botched retry has to be caught,
				// including going below the horizon). The stand-down only
				// covers STOPPING and PARKING - a decelerating mount and a park
				// climbing to the pole, the two phases where nothing goes down.
				// Do not move this reset after the doGoto().
				recoverState = RECOVER_IDLE;
				moveRecoveryValue->setValueCharArr ("IDLE");
				sendValueAll (moveRecoveryValue);
				logStream (MESSAGE_INFO) << "GeminiUDP: move recovery parked at CWD, re-sending the target" << sendLog;
				if (!doGoto (recoverTargetRa, recoverTargetDec, "move recovery: retry from CWD"))
					giveUp ("the retry goto was refused");
				return;
			}
			if (getNow () - recoverSince > PARK_TIMEOUT_SEC)
				giveUp ("the CWD park did not complete in time");
			return;
	}
}

int GeminiUDP::startResync ()
{
	moveRetries = 0;	// a fresh framework target starts with a full retry budget
	if (positionLost ())
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: move refused, the mount position is LOST (" << positionReasonValue->getValue ()
			<< ") - \"position ok\", \"position unmoved\" or \"position cwd\" first" << sendLog;
		return -1;
	}
	if (rezeroState != REZERO_IDLE)
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: move refused, a re-zero is in progress" << sendLog;
		return -1;
	}

	// a move to a new target (not an offset or correction of the current
	// one) is the natural break for an armed re-zero: nothing is exposing,
	// and the telescope is leaving its field anyway
	rts2core::Value *moveNumV = getOwnValue ("MOVE_NUM");
	int currentMove = moveNumV ? moveNumV->getValueInteger () : -1;
	bool newTarget = currentMove != lastResyncMoveNum;
	lastResyncMoveNum = currentMove;
	if (newTarget && rezeroArmed && rezeroAutoValue->getValueBool ())
	{
		double mRa = 0, mDec = 0;
		size_t n = std::min (skyEvidence.size (), (size_t) std::max (1, rezeroSamplesValue->getValueInteger ()));
		std::vector<double> ra, dec;
		for (size_t i = skyEvidence.size () - n; i < skyEvidence.size (); i++)
		{
			ra.push_back (skyEvidence[i].raErrDeg);
			dec.push_back (skyEvidence[i].decErrDeg);
		}
		std::sort (ra.begin (), ra.end ());
		std::sort (dec.begin (), dec.end ());
		mRa = ra[n / 2];
		mDec = dec[n / 2];
		char buf[160];
		snprintf (buf, sizeof (buf), "automatic, %d targets agree: RA axis %+.3f, Dec axis %+.3f deg", (int) n, mRa, mDec);
		std::string err;
		if (beginRezero (mRa, mDec, buf, err) == 0)
		{
			rezeroThenMove = true;
			rezeroMoveFailed = false;
			return 0;	// the framework's move is under way; isMoving() covers the re-zero, then the goto
		}
		logStream (MESSAGE_ERROR) << "GeminiUDP: armed re-zero could not start (" << err << ") - moving without it" << sendLog;
		clearSkyEvidence ("armed re-zero could not start");
	}

	struct ln_equ_posn pos;
	getTarget (&pos);
	return doGoto (pos.ra, pos.dec, "framework-requested move") ? 0 : -1;
}

bool GeminiUDP::doGoto (double raDeg, double decDeg, const char *label, GeminiCaringLoop::GotoSideMode sideMode)
{
	if (caring == nullptr)
		return false;
	if (positionLost () || rezeroState != REZERO_IDLE)
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: " << label << " refused: " << (positionLost () ? "the mount position is LOST" : "a re-zero is in progress") << sendLog;
		return false;
	}

	std::string err;
	GeminiSidePrediction prediction;
	bool ok = caring->gotoRaDec (raDeg, decDeg, err, 3.0, sideMode, &prediction);
	if (!ok)
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: " << label << " refused: " << err << sendLog;
		return false;
	}
	logStream (MESSAGE_INFO) << "GeminiUDP: " << label << " accepted, RA=" << raDeg << " Dec=" << decDeg
		<< ", pier side " << prediction.describe ()
		<< (prediction.outcome != GeminiSidePrediction::UNKNOWN && prediction.ambiguous (flipAmbiguityMarginValue->getValueDouble ()) ? " - too close to call" : "")
		<< sendLog;

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
// substitutes for "the flip has already happened, if any"). The retarget
// is itself a goto, and Gemini decides the pier side again for it - with
// the target now shifted by the correction and by however long the slew
// took, so a first goto that just fit the window can be followed by a flip
// on arrival. Hence GOTO_KEEP_SIDE: a retarget predicted to flip, or too
// close to call, is not sent at all. Relies on the mid-slew retarget
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

	double dRa = raDistanceDeg (st.ra, pendingMoveNaiveRa);
	double dDec = fabs (st.dec - pendingMoveNaiveDec);
	if (dRa > 1.0 || dDec > 1.0)
		return;

	moveCorrectionApplied = true;	// never retry, even if nothing to correct or the correction below fails

	double corrRa, corrDec;
	computeModelCorrection (pendingMoveNaiveRa, pendingMoveNaiveDec, st.lst, st.decSide () != '?' ? st.decSide () : st.pierSide, corrRa, corrDec);

	// model is private on Telescope, not reachable from here - but
	// computeModel() itself already no-ops to a zero correction when no
	// model is loaded, so check the result instead of the pointer: skip a
	// pointless identical retarget rather than guess at "is one loaded".
	if (raDistanceDeg (corrRa, pendingMoveNaiveRa) < (1.0 / 3600.0) && fabs (corrDec - pendingMoveNaiveDec) < (1.0 / 3600.0))
		return;

	logStream (MESSAGE_INFO) << "GeminiUDP: near arrival (dRA=" << dRa << " dDec=" << dDec << "), applying model correction: naive RA=" << pendingMoveNaiveRa << " Dec=" << pendingMoveNaiveDec
		<< " -> corrected RA=" << corrRa << " Dec=" << corrDec << sendLog;

	std::string err;
	if (!caring->gotoRaDec (corrRa, corrDec, err, 3.0, GeminiCaringLoop::GOTO_KEEP_SIDE))
		logStream (MESSAGE_WARNING) << "GeminiUDP: model-corrected retarget skipped, pointing stays uncorrected: " << err << sendLog;
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
	if (rezeroMoveFailed)
	{
		rezeroMoveFailed = false;
		logStream (MESSAGE_ERROR) << "GeminiUDP: move failed - the re-zero run before it did not finish" << sendLog;
		return -1;
	}
	if (rezeroThenMove)
		return USEC_SEC;
	// a recovery in flight keeps the framework's move "in progress" - it sees
	// one move that ends when the retry arrives or the budget runs out
	if (recoverState != RECOVER_IDLE)
		return USEC_SEC;
	if (recoverGiveUp)
	{
		recoverGiveUp = false;
		logStream (MESSAGE_ERROR) << "GeminiUDP: move failed after " << moveRetries << " recovery attempts - the mount is parked at CWD, its position still trusted; the next move will try again from there" << sendLog;
		return -1;
	}
	GeminiStatus st = caring->getStatus ();
	if (st.moveInProgress)
		return USEC_SEC;
	if (!st.moveFailed && !st.moveAborted)
	{
		moveRetries = 0;	// a clean arrival ends the retry budget for this target
		return -2;
	}
	// The mount failed to execute the move but its counters are sound (the RA
	// axis crawled, an axis stalled, it stopped short): stop, park CWD, retry.
	// An operator's stop sets moveAborted without moveExecutionFault and is
	// never retried.
	if (st.moveExecutionFault && tryStartMoveRecovery (st.moveEndReason.empty () ? st.moveFailReason : st.moveEndReason))
		return USEC_SEC;
	if (st.moveFailed)
		logStream (MESSAGE_ERROR) << "GeminiUDP: " << st.moveFailReason << sendLog;
	else
		logStream (MESSAGE_WARNING) << "GeminiUDP: move was stopped before it reached its target - the mount is wherever it got to, not at the requested position" << sendLog;
	return -1;
}

int GeminiUDP::stopMove ()
{
	// the framework's stop (an operator's "stop", a new move replacing one in
	// flight, stopTracking()) - logged, so a log tells an operator's stop from
	// the mount stopping by itself
	{
		GeminiStatus st = caring ? caring->getStatus () : GeminiStatus ();
		logStream (st.moveInProgress ? MESSAGE_WARNING : MESSAGE_DEBUG) << "GeminiUDP: stop (:Q#) sent"
			<< (st.moveInProgress ? " while a move was in flight" : "") << sendLog;
	}
	if (rezeroState != REZERO_IDLE && rezeroState != REZERO_REBOOTING)
		abortRezero ("stop command");
	if (caring)
		caring->requestAbort ();
	return 0;
}

int GeminiUDP::startPark ()
{
	if (caring == nullptr)
		return -1;
	// a park is a goto to a position measured in axis counters that are
	// not believed - see setPositionTrust()
	if (positionLost ())
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: park refused, the mount position is LOST (" << positionReasonValue->getValue () << ")" << sendLog;
		return -1;
	}
	if (rezeroState != REZERO_IDLE)
	{
		logStream (MESSAGE_ERROR) << "GeminiUDP: park refused, a re-zero is in progress" << sendLog;
		return -1;
	}
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
		logStream (MESSAGE_INFO) << "GeminiUDP: tracking-limit flip finished, exposures unblocked" << sendLog;
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
void GeminiUDP::armLimitAction (LimitAction wanted)
{
	struct ln_equ_posn tar;
	getTelTargetRaDec (&tar);

	bool stillUp = altitudeSafe (tar.ra, tar.dec, 20.0);
	pendingLimitAction = stillUp && wanted == LIMIT_ACTION_FLIP ? LIMIT_ACTION_FLIP : LIMIT_ACTION_PARK;

	logStream (MESSAGE_WARNING) << "GeminiUDP: tracking limit approaching ("
		<< trackingSecToLimitValue->getValueDouble () << "s left) - target RA=" << tar.ra << " Dec=" << tar.dec
		<< (pendingLimitAction == LIMIT_ACTION_FLIP ? " still above horizon, will flip to it (:MM#)"
			: (stillUp ? " - the mount's limits leave no moment the other side accepts it, will park" : " has set, will park"))
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
		logStream (MESSAGE_INFO) << "GeminiUDP: tracking-limit flip, re-sending RA=" << tar.ra << " Dec=" << tar.dec << " with :MM#" << sendLog;
		limitActionInFlight = true;
		if (doGoto (tar.ra, tar.dec, "tracking-limit flip", GeminiCaringLoop::GOTO_FLIP))
		{
			started = true;
		}
		else
		{
			logStream (MESSAGE_ERROR) << "GeminiUDP: tracking-limit flip was refused or not predicted to flip, falling back to park" << sendLog;
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
			double dRa = raDistanceDeg (getTelRa (), testTargetRa);
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
			double dRa = raDistanceDeg (getTelRa (), testStartRa);
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
		checkPositionEvidence (st);
		runRezero (st);
		runMoveRecovery (st);
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
