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

	// ---- in-flight move sanity, see pollStatus() ----
	double moveSeparation = NAN;	// angular distance from the current position to the active move's target

	// true once the distance to the target has grown WRONG_WAY_MARGIN_DEG
	// past the smallest distance seen so far in this move, for several
	// consecutive polls - i.e. the mount is confidently travelling away
	// from where it was told to go. Sticky until the next accepted goto,
	// like moveFailed.
	bool moveWrongWay = false;

	// pier side changed during the current move: a real meridian flip, in
	// which the reported RA/Dec legitimately swings far away from both ends
	// of the move. Suspends moveWrongWay detection for the rest of it.
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
		 * Which startup mode to pick when the mount is sitting in its boot
		 * menu (handshake answers 'b'). Same three choices, in the same
		 * order, as base/teld/gemini/gemini.cpp's next_reset selection.
		 */
		enum StartupMode { STARTUP_RESTART = 0, STARTUP_WARM = 1, STARTUP_COLD = 2 };

		/**
		 * Also re-arms the per-boot selection budget (see
		 * MAX_STARTUP_SELECTIONS): if the mount is stuck in its boot menu
		 * because the configured mode isn't the one it will accept,
		 * changing the mode is exactly the moment to try again.
		 */
		void setStartupMode (StartupMode mode);

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
		 */
		void requestReboot (bool cold);

		/** margin, in degrees, for GeminiStatus::moveWrongWay - see there */
		void setWrongWayMargin (double deg) { wrongWayMarginDeg = deg; }

		/**
		 * Send a goto and wait (bounded, real OS wait) for the mount to
		 * accept or reject it - satisfies Telescope::startResync()'s
		 * synchronous contract. See base note at top of file for why this
		 * is safe where the previous design's fake-blocking wasn't.
		 */
		bool gotoRaDec (double raDeg, double decDeg, std::string &errorMessage, double waitTimeoutSec = 3.0);

		/** best-effort, asynchronous: caring loop sends :Q# at its next opportunity, ahead of routine polling */
		void requestAbort ();

		/**
		 * Best-effort, asynchronous: caring loop sends :hP# (park) at its
		 * next opportunity, then polls :h?# (park status) instead of the
		 * routine ENQ status poll until it reports done - see
		 * base/teld/gemini/gemini.cpp's startPark()/isParking() (this
		 * exact command pair, ported from the live production driver at
		 * ~/gemini2ser.cpp, not the classic tree copy).
		 */
		void requestPark ();

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
		std::atomic<bool> rebootRequested;
		std::atomic<bool> rebootCold;
		std::atomic<int> startupMode;

		// one-shot: requestReboot(true) sets it so the boot menu gets bC#
		// for that one boot even when startup_mode says otherwise (native
		// 65533 is supposed to enforce a cold start without ever showing
		// the menu, but if it does show it, a recovery's whole point is
		// the cold start). runPostStartupSequence() clears it.
		std::atomic<bool> forceColdSelection;
		std::atomic<double> pollIntervalSec;
		std::atomic<double> wrongWayMarginDeg;

		// caring-thread-only, like the move-tracking members above
		double moveMinSeparation;
		int wrongWayCount;
		char moveStartPierSide;
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
