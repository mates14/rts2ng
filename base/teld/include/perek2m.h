/*
 * Link to the Perek 2m telescope control system (Jan Fuchs' telescoped).
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

#pragma once

#include "xmlrpcclient.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

/**
 * The Perek 2m (Ondrejov) TCS is driven over ASCOL, which Jan Fuchs'
 * telescoped (alhena:9999) exposes as XML-RPC: telescope_info () returns
 * the whole state, telescope_execute (cmd) sends one ASCOL command and
 * answers "1" when it was accepted (not done), telescope_set_coordinates ()
 * presets an RA/Dec target. Protocol notes: teld/perek2m/PROTOCOL.md.
 *
 * Shared by rts2-teld-perek2m and rts2-dome-perek2m. Both talk to the same
 * telescoped; nothing here keeps state the other daemon would need.
 */
namespace perek2m
{

// GLST fields
enum GlstField
{
	GLST_OIL = 0,
	GLST_TEL,
	GLST_HA,
	GLST_DA,
	GLST_FOCUS,
	GLST_DOME,
	GLST_SLIT,
	GLST_TUBE_FLAP,
	GLST_MIRROR_FLAP,
	GLST_BITS,
	GLST_FIELDS
};

// raw GLST values used by the drivers
#define OIL_OFF          0
#define OIL_ON           4
#define OIL_OFF_DELAY    5

#define TEL_ST_INIT      0
#define TEL_ST_OFF       1
#define TEL_ST_OFF_WAIT  2
#define TEL_ST_STOP      3
#define TEL_ST_TRACK     4
#define TEL_ST_OFF_REQ   5

#define AXIS_STOP        0
#define AXIS_POSITION    1

#define DOME_ST_OFF      0
#define DOME_ST_STOP     1
#define DOME_ST_AUTO_STOP 6

// slit and flaps
#define COVER_UNDEF      0
#define COVER_OPENING    1
#define COVER_CLOSING    2
#define COVER_OPEN       3
#define COVER_CLOSE      4

/**
 * Name of a raw GLST value of the given field, as Fuky's clients show it
 * ("UNKNOWN" for anything outside the table, including -1).
 */
const char *glstName (int field, int raw);

// number of named states of a field (bitfield: 0)
int glstStates (int field);

/**
 * One telescope_info () reply, parsed. Angles in degrees.
 */
struct TcsInfo
{
	TcsInfo ();

	bool valid;              // a reply was parsed
	double fetched;          // when (system time)

	std::string ut;
	int glst[GLST_FIELDS];

	// TRRD: current position in the catalogue frame (J2000), side 0 east 1 west
	double ra, dec;
	int side;
	// TSRA: last preset target
	double tarRa, tarDec;
	int tarSide;
	// TRHD: hour and declination axis positions
	double haAxis, decAxis;
	// TRGV, TRUS: guide corrections and user speeds, as reported
	double guideRa, guideDec;
	double userRa, userDec;

	double domeAz;
	double focus;
	int model;
	std::string object;

	// telescope moving to a target: SS_* / ST_* slew states
	bool slewing () const { return glst[GLST_TEL] >= 6; }
	// telescope powered and standing or tracking
	bool ready () const { return glst[GLST_TEL] == TEL_ST_STOP || glst[GLST_TEL] == TEL_ST_TRACK; }
};

/**
 * "HHMMSS.ss" / "[+-]DDMMSS.ss" to degrees; NAN when it does not parse.
 */
double parseHms (const std::string &s);
double parseDms (const std::string &s);

/**
 * Degrees to "HHMMSS.ss" and "[-]DDMMSS.ss" (no sign when positive, as
 * observe_batch sends them).
 */
std::string formatRa (double ra);
std::string formatDec (double dec);

/**
 * Parses a telescope_info () reply. Throws rts2core::XmlRpcError on a reply
 * without the GLST.
 */
void parseInfo (const rts2core::XmlRpcValue &v, TcsInfo &info);

class Tcs
{
	public:
		Tcs ();
		~Tcs ();

		void setHost (const std::string &host) { tcsHost = host; }
		void setPort (int port) { tcsPort = port; }
		void setPollInterval (double sec) { pollInterval = sec; }
		void setTimeout (double sec) { timeout = sec; }

		std::string getUrl () const { return "http://" + tcsHost + ":" + std::to_string (tcsPort); }

		/**
		 * Starts the polling thread.
		 */
		void start ();
		void stop ();

		/**
		 * Copies the latest snapshot; returns false while the server is not
		 * answering (the snapshot is then the last good one, or invalid).
		 */
		bool getInfo (TcsInfo &out);

		bool isConnected () const { return connected; }

		std::string getLastError ();

		/**
		 * Sends one ASCOL command and returns the raw answer. Blocking,
		 * bounded by the timeout; throws rts2core::XmlRpcError.
		 */
		std::string execute (const std::string &cmd);

		/**
		 * Sends a command that must be accepted. Returns 0 when the answer
		 * was "1"; -1 otherwise, with the reason in err.
		 */
		int command (const std::string &cmd, std::string &err);

		/**
		 * Presets an RA/Dec (J2000) target; side 0 east, 1 west.
		 */
		int setCoordinates (double ra, double dec, int side, const std::string &object, std::string &err);

	private:
		std::string tcsHost;
		int tcsPort;
		double pollInterval;
		double timeout;

		std::thread worker;
		std::atomic <bool> running;
		std::atomic <bool> connected;

		std::mutex lock;
		TcsInfo snapshot;
		std::string lastError;

		void threadMain ();
};

}
