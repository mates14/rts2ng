/*
 * Caring-loop transport for the Losmandy Gemini-2 UDP protocol.
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

// base note: wholly new (see UPSTREAM_BUGS.md for why this replaced the
// first attempt, rts2core::ConnUDP-based rts2teld::ConnGeminiUDP). That
// version tried to give the framework's synchronous startResync() contract
// a fake blocking wait by manually pumping Block::oneRunLoop() from inside
// idle() - which calls back into idle() itself before its own state had
// advanced, recursing without bound. This version takes a completely
// different shape: a dedicated thread ("caring loop", the user's own term
// from an earlier Python RTS2 mixin) owns the raw UDP socket and speaks the
// wire protocol with ordinary blocking recv()-with-timeout calls, exactly
// like the classic rts2core::ConnUDP::sendReceive() or libmks3's termios
// VTIME-based serial reads (~/paracl/libmks3.c) - genuinely blocking is
// fine and simple as long as it's not the RTS2 thread doing it, since a
// real OS-level wait can't recurse into anything (nothing else is running
// while the kernel has the thread parked). The RTS2-facing side of this
// class never touches the network and never blocks more than a mutex
// lock/unlock: it only ever reads a plain-data snapshot (GeminiStatus) or
// queues a request. The one place the framework's own contract requires a
// synchronous answer (Telescope::startResyncMove() -> startResync() has to
// return accept/reject immediately) is satisfied by gotoRaDec()'s
// std::condition_variable::wait_for() - a real, bounded OS wait, not a
// pumped loop, so it's safe for the same reason libmks3's read() is safe.
//
// Discipline that must never be violated: the caring-loop thread must
// NEVER call into rts2core::Value/logStream/maskState/any Telescope method
// directly - none of that is designed for concurrent access. It only ever
// reads/writes plain data (GeminiStatus, the command queue) under mutex_.
// All translation from that plain data into RTS2 Values happens on the
// RTS2 thread, from GeminiUDP::idle(), by copying out a GeminiStatus
// snapshot and never touching the mutex again once copied.

#pragma once

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <netinet/in.h>

namespace rts2teld
{

/**
 * Shortest angular distance between two right ascensions, in degrees,
 * always 0..180. NaN in, NaN out.
 *
 * Exists because `fabs (ln_range_degrees (a - b))` reads as if it did this
 * and does not: ln_range_degrees() normalizes into 0..360, so two RAs a
 * hair apart come back as ~0 or ~360 depending purely on which way round
 * the subtraction went, and fabs() cannot undo it. That mistake was in
 * four places here and it cost a real incident on the mount at SBT
 * (2026-09-14): a slew that arrived dead on target - true angular
 * separation 2e-5 deg - was reported as "move stopped 360.000 deg from
 * target", which parked the mount and marked its position LOST. Use this,
 * or compare true angular separations; never fabs() a wrapped difference.
 */
inline double raDistanceDeg (double ra1, double ra2)
{
	double d = fmod (ra1 - ra2, 360.0);
	if (d < 0)
		d += 360.0;
	return d > 180.0 ? 360.0 - d : d;
}


/**
 * The mount geometry Gemini's own goto side decision works with, read once
 * per startup. All ticks are RA/Dec motor encoder ticks as native 239
 * reports them; see ~/tmp/gemini/FLIP_LOGIC.md (firmware HGM_Gem2.bin,
 * FUN_00048ed0 and FUN_0000a3bc) for where every number here comes from.
 */
struct GeminiAxisGeometry
{
	bool valid = false;
	int32_t raHalf = 0, decHalf = 0;	// native 238: half a circle, i.e. the CWD position of each axis
	int32_t eastLimit = 0;			// native 231 first value: CWD + eastern safety limit
	int32_t westLimit = 0;			// native 231 second value: CWD - western safety limit
	double westGotoDeg = 0;			// native 223, degrees INSIDE the western safety limit (not from the meridian)
	int flipPoints = -1;			// native 229: flip points in use, 0 none; -1 unread

	double ticksPerDeg () const { return raHalf / 180.0; }
	int32_t westGotoTicks () const { return (int32_t) (westGotoDeg * ticksPerDeg ()); }

	// the RA axis window a goto target has to land strictly inside
	int32_t windowLow () const { return westLimit + westGotoTicks (); }
	int32_t windowHigh () const { return eastLimit; }
};

/**
 * What Gemini's :MS# / :MM# will do with a target, worked out the same way
 * the firmware does it (FUN_00048ed0): put the target on the side of the
 * pier the Dec axis is on now, check its RA axis position against
 * [westLimit + westGoto, eastLimit], and only if that fails try the other
 * side - :MM# tries the two in the opposite order. Done relative to the
 * current axis position, so the only thing not modelled is how much
 * Gemini's own pointing model shifts the RA axis between here and the
 * target: hence the margins, and GeminiUDP's flip_ambiguity_margin.
 */
struct GeminiSidePrediction
{
	enum Outcome { UNKNOWN, STAY, FLIP, REFUSE };
	Outcome outcome = UNKNOWN;
	char sideBefore = '?';		// Dec axis side, 'E' (Dec ticks >= half) or 'W'
	char sideAfter = '?';

	// RA axis distance of each candidate inside the window, in degrees,
	// negative when outside - in the order the firmware tries them
	double firstMarginDeg = NAN;
	double secondMarginDeg = NAN;
	std::string reason;		// why UNKNOWN

	/** the margin of the candidate the mount ends up on */
	double marginDeg () const;

	/** could the pointing-model slack the prediction doesn't see change the answer? */
	bool ambiguous (double threshold) const;

	std::string describe () const;
};

/**
 * @param raTicks, decTicks  current axis position, native 239
 * @param curRaDeg           current RA the mount reports (ENQ), same frame as targetRaDeg
 * @param preferOther        true for :MM# (other side first), false for :MS#
 */
GeminiSidePrediction predictGotoSide (const GeminiAxisGeometry &geo, int32_t raTicks, int32_t decTicks, double curRaDeg, double targetRaDeg, bool preferOther);

/**
 * Tracking towards the western safety limit (native 226 counting down): is
 * this the moment for the flip (an :MM# to the same target), too early, or
 * will a flip never work before the mount stops at the limit?
 *
 * The other side of the pier only accepts the target once it has moved east
 * of CWD + east limit - 90 deg in hour angle; the tracking side stops at CWD -
 * west limit + 90 deg. Whether those overlap, and by how much, is the mount's
 * configuration: with SBT's 91/91 deg limits there are only 2 deg (8 min)
 * between them, so a fixed "flip at 660 s" lands before the flip is possible.
 *
 * @return FLIP when secToLimit < earliestSec and an :MM# is predicted to flip
 *         with marginDeg to spare; PARK when the limits leave no such moment
 *         (or it has not come by the last 30 s); WAIT otherwise
 */
enum GeminiLimitDecision { LIMIT_WAIT, LIMIT_FLIP, LIMIT_PARK };

GeminiLimitDecision decideTrackingLimit (const GeminiAxisGeometry &geo, int32_t raTicks, int32_t decTicks, double curRaDeg,
	double secToLimit, double marginDeg, double earliestSec = 660.0);

/**
 * Where Gemini's axis counters should read, at the telescope's current
 * physical position, for it to be pointing at (mountRaDeg, mountDecDeg) - a
 * mount-frame coordinate, i.e. what a goto to the true sky position would
 * send - by the relation the firmware sets up at a cold start (counters at
 * CWD, RA reference at sidereal time + 6h, no Gemini model; FUN_0000bd78 and
 * FUN_00016b7c): RA axis (270 - HA) on the E side of the pier, (90 - HA) on
 * the W side; Dec axis (270 - Dec) on E, (Dec + 90) on W. The difference from
 * the counters read now is their zero error; stepping the axes to CWD minus
 * that error and cold-starting there removes it.
 */
struct GeminiCounterError
{
	double raTicks = 0, decTicks = 0;	// counters wanted minus counters read, RA wrapped to +-half circle
	double raDeg = 0, decDeg = 0;
	int32_t rezeroRa = 0, rezeroDec = 0;	// counters to step to before the cold start (CWD minus the error)
};

GeminiCounterError computeCounterError (const GeminiAxisGeometry &geo, int32_t raTicks, int32_t decTicks, char decSide,
	double lstDeg, double mountRaDeg, double mountDecDeg);

/**
 * Plain-data snapshot of everything the caring loop knows about the mount.
 * Copied out under mutex_, then read freely - no locking needed once
 * copied. See base note above for why it has to stay plain data.
 */
struct GeminiStatus
{
	bool connected = false;
	bool valid = false;		// at least one successful poll has landed
	double timestamp = 0;		// getNow()-style seconds, when this snapshot was taken

	// verified against live samples: LST = HA + RA exactly, pier side
	// matches the documented :Gm# W/E format - see STATUS.md for the
	// worked-out field mapping of the 0x05 ENQ macro this is filled from
	double ra = 0, dec = 0;	// degrees
	double ha = 0;			// degrees
	double az = 0, alt = 0;	// degrees
	double lst = 0;			// degrees
	char pierSide = '?';		// 'W' / 'E', see :Gm#
	char moveRate = '?';		// N/T/G/C/S, see :Gv#
	int32_t praRaw = 0, pdecRaw = 0;

	// native register 226, read fresh every poll cycle alongside the ENQ
	// macro - seconds of tracking left before Gemini's own firmware hits
	// the western safety limit and just stops (no flip, no warning of its
	// own - see gemini2ser.cpp's info(): tel_gemini_get(226, ...), and its
	// use of this exact value, compared against 660s/11min, to proactively
	// trigger a flip while there's still comfortable margin). NAN until
	// the first successful read.
	double trackingSecToWestLimit = NAN;

	// ENQ macro fields whose exact semantics aren't confirmed yet (see
	// base/teld/gemini/gemini.cpp's tel_gemini_get(99,...) / <99: status
	// bits for what these probably are) - kept raw rather than guessing
	// wrong labels on a live mount.
	std::string rawExtended;

	bool moveInProgress = false;

	// set when a move stops (position no longer changing, or the
	// deadline was hit) without ever getting near the requested target -
	// see pollStatus()'s arrival check. Distinct from moveInProgress:
	// "stopped changing" and "arrived" are NOT the same thing (found the
	// hard way - see UPSTREAM_BUGS.md/STATUS.md - a move that stalled
	// partway, e.g. against a mount-side limit or another client's
	// conflicting command, was previously reported as a successful
	// completion because nothing ever compared the final position
	// against the target).
	bool moveFailed = false;
	std::string moveFailReason;

	// Set when a move in progress is cut short by :Q# - an operator
	// pressing stop, or a safety recovery. Distinct from moveFailed on
	// purpose: an aborted move is NOT an arrival (the framework must not
	// log it as one, nor resume tracking on a target the mount never
	// reached), but it is also not evidence of a misbehaving mount, so it
	// deliberately does not feed the safety watchdog. Cleared by the next
	// accepted goto. See GeminiUDP::isMoving().
	bool moveAborted = false;

	// how the last move ended, for the log: "arrived / stopped moving / timed
	// out / stopped by :Q# after N s, X deg from target, mount rate 'R'" -
	// and which goto (gotoSerial) it belongs to
	std::string moveEndReason;
	unsigned moveEndSerial = 0;

	// set by requestPark(); cleared once :h?# reports '1' (done) or '0'
	// (production driver's gemini2ser.cpp logs this as "isParking called
	// without park command" - treated the same way here: parkFailed).
	// See GeminiCaringLoop::pollParkStatus().
	bool parking = false;
	bool parkFailed = false;
	char parkStatus = '?';

	// ---- startup / boot-menu handshake (the 0x06 ACK command) ----
	// Gemini answers 0x06 with a single character describing where it is in
	// its own startup: 'B' initial startup message on screen, 'b' waiting
	// for the operator (or us) to pick a startup mode, 'S' cold start
	// running, 'G' startup finished with a German equatorial mount selected,
	// 'A' finished with an Alt/Az mount. Until it reaches 'G'/'A' the mount
	// answers status queries but silently ignores every motion command -
	// see GeminiCaringLoop::pollStartupState() for what drives this.
	char startupState = '?';	// '?' until the first handshake lands
	bool startupComplete = false;	// 'G' or 'A' seen

	// what the last completed startup looked like, for GeminiUDP to judge
	// whether the axis counters can still be trusted: bootObserved is false
	// when the mount was already up the first time we asked (a driver
	// restart), bootSelection is the boot menu answer we sent during it -
	// 'R' restart, 'W' warm start, 'C' cold start, '?' none (the mount
	// picked a mode itself, or 65533 skipped the menu)
	bool bootObserved = false;
	char bootSelection = '?';

	// mount UTC minus system UTC as read after startup, before any clock
	// setting; NAN if it could not be read
	double clockOffsetSec = NAN;

	// bumped once per completed startup. GeminiUDP::idle() watches it to
	// notice both the initial connect and any later reboot of the mount
	// (ours or somebody else's), and re-runs its own post-startup work.
	unsigned startupCount = 0;
	unsigned startupSelections = 0;	// bR#/bW#/bC# selections sent during the current boot, capped - see MAX_STARTUP_SELECTIONS
	bool clockMatched = false;	// the last post-startup sequence got the mount's clock set to system UTC

	// native 130, refreshed with the slow poll group: 131 sidereal, 132
	// King, 133 lunar, 134 solar, 135 terrestrial (= tracking effectively
	// off), 136 closed loop, 137 comet/user. 0 until first read.
	int trackingRate = 0;

	// native 220-223, read once per startup by runPostStartupSequence() -
	// raw and unparsed, same reasoning as readNativeRaw()'s doc comment
	bool limitsValid = false;
	std::string limitBothRaw, limitEastRaw, limitWestRaw, limitWestGotoRaw;

	// native 238/231/223/229, read after startup and retried on the slow
	// poll until they land - see GeminiAxisGeometry
	GeminiAxisGeometry geometry;

	// native 239, polled every cycle. The Dec axis side is the mount's real
	// flip state: pierSide above comes from the RA axis (the ENQ macro and
	// :Gm# say W when the RA axis is short of CWD), and the two disagree
	// whenever the telescope points more than 6h from the meridian.
	bool axisValid = false;
	int32_t raAxisTicks = 0, decAxisTicks = 0;
	double axisTimestamp = 0;	// nowSeconds() of the last 239 read
	char decSide () const { return !axisValid || !geometry.valid ? '?' : (decAxisTicks >= geometry.decHalf ? 'E' : 'W'); }

	// the side prediction made for the last accepted goto, and its serial
	// number so the RTS2 side can check it against where the mount ended up
	unsigned gotoSerial = 0;
	GeminiSidePrediction lastPrediction;

	// ---- in-flight move sanity, see pollStatus() ----
	double moveSeparation = NAN;	// angular distance from the current position to the active move's target
	double lastMoveSeparation = NAN;	// the same, as last seen during the most recent move - kept after it ends

	// true once the distance to the target has grown WRONG_WAY_MARGIN_DEG
	// past the smallest distance seen so far in this move, for several
	// consecutive polls - i.e. the mount is confidently travelling away
	// from where it was told to go. Sticky until the next accepted goto,
	// like moveFailed.
	bool moveWrongWay = false;

	// pier side changed during the current move: a real meridian flip, in
	// which the reported RA/Dec legitimately swings far away from both ends
	// of the move. Suspends moveWrongWay detection for the rest of it.
	// Also set up front when the goto was predicted to flip (or too close
	// to call): the distance to the target grows long before either axis
	// crosses its half circle, so noticing the flip after the fact is too
	// late to keep the wrong-way check from firing on it.
	bool movePierChanged = false;
};

/**
 * Dedicated-thread ("caring loop") transport for the Gemini UDP protocol
 * (spec v1.2). Owns a raw, plain blocking UDP socket - see base note
 * above for why blocking is the right call here, unlike on the RTS2
 * thread. Implements the DatagramNumber/LastDatagramNumber framing,
 * command batching, and NACK/resync recovery from the spec directly,
 * with ordinary sequential blocking code (no Timer/Event machinery
 * needed at all now - that complexity was only ever needed to make this
 * safe to run on the RTS2 thread, which this design no longer attempts).
 */
class GeminiCaringLoop
{
	public:
		GeminiCaringLoop (const char *hostname, int port);
		~GeminiCaringLoop ();

		/** opens the socket and starts the thread; false on socket setup failure */
		bool start ();
		void stop ();

		/** cheap: mutex lock + struct copy + unlock */
		GeminiStatus getStatus ();

		/** takes effect on the caring loop's next poll cycle check, no locking needed (std::atomic) */
		void setPollInterval (double sec) { pollIntervalSec = sec; }

		/**
		 * What to answer the mount's boot menu with (handshake 'b'). Restart
		 * keeps the stored axis counters; warm and cold start both set them
		 * to CWD without looking (firmware FUN_0000bd78), so they are right
		 * only if the telescope really is at CWD. STARTUP_NONE leaves the
		 * mount waiting for a human, which is the default: nothing but a
		 * person at the telescope knows which answer is true.
		 */
		enum StartupMode { STARTUP_NONE = 0, STARTUP_RESTART = 1, STARTUP_WARM = 2, STARTUP_COLD = 3 };

		/**
		 * The standing answer. Also re-arms the per-boot selection budget
		 * (see MAX_STARTUP_SELECTIONS).
		 */
		void setStartupMode (StartupMode mode);

		/**
		 * One-shot answer for the current (or next) boot, whatever the
		 * standing mode is - how a human's "position unmoved" / "position
		 * cwd" reaches a mount waiting in its menu.
		 */
		void selectStartup (StartupMode mode);

		/**
		 * Best-effort, asynchronous: reboot the Gemini controller. cold ==
		 * true sends native 65533 ("reboot enforcing a Cold Start"), false
		 * sends 65535 ("reboot"), exactly as base/teld/gemini/gemini.cpp's
		 * resetMount() picks between them. Either way the snapshot's
		 * startupComplete goes false immediately, so the caring loop drops
		 * back into handshake mode and drives the mount back up through
		 * pollStatus()'s boot menu on its own.
		 *
		 * Note the mount is unreachable for a while afterwards (a cold
		 * start takes ~20s on serial, and Gemini's UDP listener has to come
		 * back too) - that shows up as a normal disconnected stretch.
		 *
		 * selection is the boot menu answer for this boot, if the menu
		 * shows; STARTUP_NONE leaves it to the standing startup mode.
		 */
		void requestReboot (bool cold, StartupMode selection = STARTUP_NONE);

		/** margin, in degrees, for GeminiStatus::moveWrongWay - see there */
		void setWrongWayMargin (double deg) { wrongWayMarginDeg = deg; }

		/**
		 * How a goto may treat the pier side. The prediction is made on the
		 * caring thread from a fresh ENQ + native 239 read right before the
		 * slew command goes out, so the refusals below act on the same data
		 * the mount is about to decide with.
		 */
		enum GotoSideMode
		{
			GOTO_ANY_SIDE,		// :MS#, whatever Gemini decides
			GOTO_KEEP_SIDE,		// :MS#, but not sent unless it is predicted to stay on this side with margin to spare
			GOTO_FLIP		// :MM#, but not sent unless it is predicted to flip with margin to spare
		};

		/**
		 * Send a goto and wait (bounded, real OS wait) for the mount to
		 * accept or reject it - satisfies Telescope::startResync()'s
		 * synchronous contract. See base note at top of file for why this
		 * is safe where the previous design's fake-blocking wasn't.
		 */
		bool gotoRaDec (double raDeg, double decDeg, std::string &errorMessage, double waitTimeoutSec = 3.0,
			GotoSideMode sideMode = GOTO_ANY_SIDE, GeminiSidePrediction *prediction = nullptr);

		/** degrees; below this a side prediction counts as too close to call - see GeminiSidePrediction::ambiguous() */
		void setFlipAmbiguityMargin (double deg) { flipAmbiguityMarginDeg = deg; }

		/**
		 * What to send ahead of every goto. On SBT two meridian flips sent
		 * to a tracking mount moved the RA axis at ~21x sidereal for the
		 * whole move, while gotos from a stopped mount slewed normally; the
		 * production driver always stops first. Which of these the mount
		 * actually needs is still to be established on the sky.
		 */
		enum GotoPrestop { PRESTOP_NONE = 0, PRESTOP_STOP = 1, PRESTOP_STOP_TRACKING = 2 };
		void setGotoPrestop (GotoPrestop mode) { gotoPrestop = (int) mode; }

		/** best-effort, asynchronous: caring loop sends :Q# at its next opportunity, ahead of routine polling */
		void requestAbort ();

		/**
		 * Best-effort, asynchronous: caring loop sends :hP# (park) at its
		 * next opportunity, then polls :h?# (park status) instead of the
		 * routine ENQ status poll until it reports done - see
		 * base/teld/gemini/gemini.cpp's startPark()/isParking() (this
		 * exact command pair, ported from the live production driver at
		 * ~/gemini2ser.cpp, not the classic tree copy).
		 *
		 * atStartupPosition sends :hC# instead: park at CWD. That is the
		 * park to use ahead of a cold or warm start, both of which set the
		 * axes to CWD without looking (firmware FUN_0000bd78) - :hP# goes to
		 * the configured home position, which is CWD only until somebody
		 * sets another one (:hH#, native 250).
		 */
		void requestPark (bool atStartupPosition = false);

		/**
		 * Fire-and-forget native Gemini command (checksummed >ID:VAL#
		 * register write - see tel_gemini_set in base/teld/gemini/gemini.cpp
		 * for the classic RS232 driver's equivalent). No confirmation is
		 * modeled - matches that driver's own precedent of writing local
		 * defaults without reading them back (see e.g. its guidingSpeed/
		 * centeringSpeed Values, never read from hardware either).
		 */
		void queueNativeSet (int id, int32_t value);
		void queueNativeSet (int id, double value);

		/**
		 * Fire-and-forget pulse-guide command: ":Mi" + direction
		 * ('e'/'w'/'n'/'s') + magnitude, e.g. ":Mie40#" - NOT the
		 * documented LX200 ":Mgn DDDD#" command (that was this driver's
		 * first guess, based on the UDP protocol spec's Appendix 3 list;
		 * wrong - see UPSTREAM_BUGS.md). ":Mi..." is what the live
		 * production driver (~/gemini2ser.cpp's Gemini::performGuide(),
		 * actively guiding real observations at lascaux) actually sends,
		 * and its magnitude is capped at 255 there (values above are
		 * rejected outright, not clamped) - same cap enforced here, since
		 * we have no evidence for what a larger value does on this
		 * firmware and no reason to be the first to find out live.
		 */
		void queuePulseGuide (char direction, unsigned int magnitude);

		/**
		 * Read a native Gemini register synchronously (bounded real OS
		 * wait, same shape/safety reasoning as gotoRaDec() - see its doc
		 * comment). Checksum-verified (see tel_gemini_get's checksum2 in
		 * base/teld/gemini/gemini.cpp) but returned as the RAW string,
		 * deliberately not parsed into a number here: at least one of
		 * these registers (223) is documented as a "DDDdMM" degrees
		 * format, not a plain integer, and guessing at the format for a
		 * safety-relevant value is worse than just showing it raw.
		 *
		 * @return false on timeout, no response, or checksum mismatch
		 */
		bool readNativeRaw (int id, std::string &value, double waitTimeoutSec = 3.0);

		/**
		 * Send an arbitrary raw Gemini command and wait (bounded, real OS
		 * wait) for its response - same shape/safety reasoning as
		 * gotoRaDec()/readNativeRaw() (which is built on this). For rare,
		 * one-off synchronous operations only - never for routine/high-
		 * frequency use, which stays on the async queueCommand()-style
		 * paths driven from idle().
		 */
		bool sendRawSync (const std::string &geminiData, std::string &response, double waitTimeoutSec = 3.0);

		/**
		 * Sets the mount's clock to the current system UTC time, with its
		 * UTC-offset register zeroed so "local time" as Gemini understands
		 * it is UTC - same three plain LX200 commands as
		 * base/teld/src/tellx200.cpp's TelLX200::matchTime() (:SG+00.0#,
		 * :SL#, :SC#), sent sequentially rather than batched: :SC#'s reply
		 * is a second, differently-shaped hash-terminated segment (the
		 * classic "Updating Planetary Data#    #" trailer) and guessing at
		 * how that interacts with datagram batching isn't worth the risk
		 * for a command this rare (called once at startup/park-exit, not
		 * routinely).
		 */
		bool matchTimeUtc (std::string &errorMessage, double waitTimeoutSec = 3.0);

		/**
		 * Sync (not slew): tells Gemini the mount is currently, actually,
		 * pointed at (raDeg, decDeg) - same :Sr/:Sd target-set as
		 * gotoRaDec(), but followed by ":CI#" (index-only sync) instead of
		 * ":MS#" (go there). Deliberately ":CI#", not ":Cm#" - the latter
		 * appends to Gemini's own onboard alignment model, which would
		 * double-count against the T-Point model this driver already
		 * applies in software (see GeminiUDP::computeModelCorrection()) -
		 * matches gemini2ser.cpp's setTo()'s non-append path, its default.
		 * raDeg/decDeg are sent as-is, with no model correction applied -
		 * matches that same production setTo() (its applyModel() call for
		 * this path is commented out there too): a sync tells Gemini what
		 * it's actually looking at, so there's nothing to correct for.
		 *
		 * :CI#'s own reply is not strictly parsed (sent as a separate
		 * command from :Sr/:Sd, not batched) - same open-ended-reply
		 * caution as matchTimeUtc()'s handling of ":SC#".
		 */
		bool syncTo (double raDeg, double decDeg, std::string &errorMessage, double waitTimeoutSec = 3.0);

	private:
		void threadMain ();
		// all of these run on the caring-loop thread only
		bool openSocket ();
		bool sendAndReceive (const std::string &payload, std::string &response, double timeoutSec, int maxResyncAttempts);
		void pollStatus ();
		void pollTrackingLimit ();
		void pollTrackingRate ();
		void pollParkStatus ();
		void pollAxisPosition ();

		/** native 238/231/223/229 into status.geometry; false if any of the required ones failed */
		bool readGeometryInternal ();

		/** fresh ENQ + native 239, then predictGotoSide() - nothing is written to status */
		GeminiSidePrediction predictInternal (double targetRaDeg, bool preferOther);

		/**
		 * Sends the 0x06 handshake, records where the mount is in its
		 * startup, and - when it answers 'b' (boot menu) - picks the
		 * configured startup mode for it. Without this the mount answers
		 * every status query perfectly happily and ignores every motion
		 * command, which is exactly what it looks like from the outside
		 * when a driver "doesn't work" after a power cycle.
		 */
		void pollStartupState ();

		/** run once each time the mount finishes starting up, on the caring thread */
		void runPostStartupSequence ();

		void handleGoto ();
		void handleAbort ();
		void handlePark ();
		void handleReboot ();
		void handleQueuedCommand ();
		void handleQueuedRawCommand ();
		void handleSyncQuery ();

		// caring-thread-side twins of the public, RTS2-thread-facing
		// matchTimeUtc()/readNativeRaw(): same wire commands, but issued
		// with sendAndReceive() directly instead of going through
		// sendRawSync() - which would deadlock, since the thread that has
		// to service a sync query is this one.
		bool matchTimeUtcInternal ();
		bool readNativeInternal (int id, std::string &value);

		/**
		 * pollStatus() builds a whole fresh GeminiStatus out of one ENQ
		 * reply and assigns it wholesale, so every snapshot field that does
		 * NOT come from that reply has to be copied across explicitly or it
		 * silently resets to its default once a second. Keep this in sync
		 * with GeminiStatus whenever a field is added there.
		 */
		static void carryPersistentFields (const GeminiStatus &from, GeminiStatus &to);

		std::string hostname;
		int port;
		int sock;
		struct sockaddr_in destAddr;

		std::thread worker;
		std::atomic<bool> stopFlag;

		// move-completion-by-stability tracking: touched only by the
		// caring-loop thread itself (handleGoto()/pollStatus() never run
		// concurrently with each other - one thread, one loop), so these
		// need no locking despite living alongside the mutex-protected
		// members below.
		double lastPollRa, lastPollDec;
		int stableCount;
		double moveStartedAt, moveDeadline;
		double activeMoveTargetRa, activeMoveTargetDec;

		std::mutex mutex_;
		std::condition_variable cv_;

		GeminiStatus status;

		// synchronous goto request/result, protected by mutex_
		bool gotoRequested = false;
		double gotoTargetRa = 0, gotoTargetDec = 0;
		GotoSideMode gotoSideMode = GOTO_ANY_SIDE;
		GeminiSidePrediction gotoPrediction;
		bool gotoCancelled = false;	// the requester timed out waiting - see gotoRaDec()
		bool gotoDone = false;
		bool gotoAccepted = false;
		std::string gotoMessage;

		// synchronous raw-command request/result (readNativeRaw()),
		// protected by mutex_ - same shape as the goto fields above
		bool syncQueryRequested = false;
		std::string syncQueryCommand;
		bool syncQueryDone = false;
		bool syncQueryOk = false;
		std::string syncQueryResponse;

		std::atomic<bool> abortRequested;
		std::atomic<bool> parkRequested;
		std::atomic<bool> parkAtStartupPosition;
		std::atomic<bool> rebootRequested;
		std::atomic<bool> rebootCold;
		std::atomic<int> startupMode;

		// one-shot boot menu answer (a StartupMode, STARTUP_NONE for none),
		// set by selectStartup()/requestReboot(), cleared by
		// runPostStartupSequence()
		std::atomic<int> forcedSelection;

		/** reads :GG#/:GL#/:GC#; false if any of them could not be parsed */
		bool readClockOffsetInternal (double &offsetSec);
		std::atomic<double> pollIntervalSec;
		std::atomic<double> wrongWayMarginDeg;
		std::atomic<double> flipAmbiguityMarginDeg;
		std::atomic<int> gotoPrestop;

		// caring-thread-only, like the move-tracking members above
		double moveMinSeparation;
		double crawlSince;	// timestamp the mount started reporting centering rate far from its target, NAN otherwise
		int wrongWayCount;
		char moveStartPierSide;
		char moveStartDecSide;
		bool movePierChangedFlag;
		int slowPollCounter;

		struct NativeSetCommand
		{
			int id;
			std::string valueStr;
		};
		std::deque<NativeSetCommand> commandQueue;

		// fire-and-forget raw wire commands (currently just :Mg pulse-guide
		// - see queuePulseGuide()), queued as pre-built strings rather than
		// reusing commandQueue/NativeSetCommand since these aren't native
		// >ID:VAL# register writes
		std::deque<std::string> rawCommandQueue;

		uint32_t nextDatagramNumber;
};

}
