/*
 * Various utility functions.
 * Copyright (C) 2003-2009 Petr Kubanek <petr@kubanek.net>
 * Copyright (C) 2011-2013 Petr Kubanek, Institute of Physics <kubanek@fzu.cz>
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

// base note: dropped the Solaris/Cygwin/ancient-GCC compat shims that lived
// here in the classic tree (isinf/strcasestr/getline fallbacks, HUGE_VALF and
// INFINITY macro ladders, nan.h). base targets a modern POSIX.1-2008 + C++17
// host, where libc and <cmath> already provide all of that.

#include <cinttypes>
#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/time.h>
#include <sstream>
#include <libnova/libnova.h>

#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iostream>

#ifndef JD_TO_MJD_OFFSET
#define JD_TO_MJD_OFFSET  2400000.5
#endif

#ifndef D2R
const double D2R  = M_PI / 180.0;
#endif

#ifndef AS2R
const double AS2R = D2R / 3600.0;
#endif

/**
 * Return random number in 0-1 range.
 */
double random_num ();

/**
 * Random salt for crypt function
 */
void random_salt (char *buf, int len);

/**
 * Create directory recursively.
 *
 * @param path Path that will be created.
 * @param mode Create mask for path creation.
 *
 * @return 0 on success, otherwise error code.
 */
int mkpath (const char *path, mode_t mode);

/**
 * Remove recursively directory.
 *
 * @param dir directory to remove.
 *
 * @return 0 on success, -1 and sets errno on error.
 */
int rmdir_r (const char *dir);

/**
 * Parses and initialize tm structure from char.
 *
 * String can contain either date, in that case it will be converted to night
 * starting on that date, or full date with time (hour, hour:min, or hour:min:sec).
 *
 * @return -1 on error, 0 on succes
 */
int parseDate (const char *in_date, struct ln_date *out_time, bool forceUT = false, bool *only_date = nullptr);

int parseDate (const char *in_date, double &JD, bool forceUT = false, bool *only_date = nullptr);

int parseDate (const char *in_date, time_t *out_time, bool forceUT = false, bool *only_date = nullptr);

/**
 * Return date in FITS format.
 */
void getDateObs (const time_t t, const suseconds_t usec, char buf[25]);

std::string getDateObs (const time_t t, const suseconds_t usec);

/**
 * Split std::string to vector of strings.
 *
 * @param text        String which will be splitted.
 * @param delimeter   String shich separates entries in vector.
 *
 * @return Vector of std::string.
 */
std::vector<std::string> SplitStr (const std::string& text, const std::string& delimeter);

/**
 * Splits string to vector of chars
 *
 * @param text   Text which will be splited.
 */
std::vector<char> Str2CharVector (std::string text);

/**
 * Parse range range string which uses : and , to specify range. Syntax is similar
 * to Python slices. Example of valid ranges:
 *
 *   - 1:10
 *   - 1:
 *   - 1:2,4:16
 *
 * Index of first member is assumed to be 1. The function returns C-style
 * indexes, where index of the first member is 0.
 *
 * @param range_str   string to parse
 * @param array_size  size of resulting array
 * @return vector of integer values with array indices
 */
std::vector <int> parseRange (const char *range_str, int array_size, const char *& endp);

int charToBool (const char *in_value, bool &ret);

/**
 * Fill value to const char**.
 *
 * @param p    Pointer to char which will be filled,
 * @param val  Value which will be copied to character.
 */
template < typename T > void fillIn (char **p, T val)
{
	std::ostringstream _os;
	_os << val;
	*p = new char[_os.str ().length () + 1];
	strcpy (*p, _os.str (). c_str ());
}

/**
 * Case ingore string traits operations.
 */
struct ci_char_traits : public std::char_traits<char>
{
	static bool eq( char c1, char c2 ) { return toupper(c1) == toupper(c2); }

	static bool ne( char c1, char c2 ) { return toupper(c1) != toupper(c2); }

	static bool lt( char c1, char c2 ) { return toupper(c1) <  toupper(c2); }

	static int compare( const char* s1, const char* s2, size_t n )
	{
		return strncasecmp ( s1, s2, n );
		// if available on your compiler,
		//  otherwise you can roll your own
	}

	static const char* find( const char* s, int n, char a )
	{
		while( n-- > 0 && toupper(*s) != toupper(a) ) {
			++s;
		}
		return s;
	}
};

typedef std::basic_string<char, ci_char_traits> ci_string;

/**
 * Converts string int some type.
 *
 * @param t     returned value
 * @param s     string to convert.
 *
 * @return false on failure, true on success
 */
template <class T> bool from_string (T& t, const std::string& s, std::ios_base& (*f)(std::ios_base&))
{
	std::istringstream iss(s);
	return !(iss >> f >> t).fail();
}

// number of microseconds in sec
#define USEC_SEC    1000000

// number of nanoseconds in sec
#define NSEC_SEC    1000000000

/**
 * Return current time as double.
 */
double getNow ();

/**
 * Creates multiple WCS name from value name and suffix.
 */
const char * multiWCS (const char *name, char multi_wcs);

/**
 * Put -1 to indicator if value is NAN.
 */
int db_nan_indicator (double value);

/**
 * Return NAN if indicator signaled NULL value.
 */
double db_nan_double (double value, int ind);

float db_nan_float (float value, int ind);

/**
 * Return next night start/stop times.
 */
void getNight (time_t curr_time, struct ln_lnlat_posn *observer, double nightHorizon, time_t &nstart, time_t &nstop);

/**
 * Normalize over-the-pole RaDec. On GEM and all other models mounts, which can cross the pole, the usuall
 * notation which should be used for crossed pole is to keep counting dec. This leads to absolute dec values
 * > 90. This functions normalized such over-the-pole values.
 */
void normalizeRaDec (double &ra, double &dec);

/**
 * Converts degrees Celsius to degrees Fahrenheit.
 */
float celsiusToFahrenheit (float c);

/**
 * Converts degrees Fahrenheit to Celsius.
 */
float fahrenheitToCelsius (float f);

/**
 * Converts Kelvins to Celsius.
 */
float kelvinToCelsius (float k);

/**
 * Converts Celisus to Kelvins.
 */
float celsiusToKelvin (float c);

std::string string_format (const char* format, ...);

/**
 * Converts speed in miles/hour to meters/second.
 */
float mphToMs (float mph);

/**
 * Calculates CRC-16.
 */
uint16_t getMsgBufCRC16 (char *msgBuf, int msgLen);

/**
 * Split string into device.variable parts. Parts must be freed.
 */
int parseVariableName (const char *name, char **device, char **variable);

/**
 * Calculate paralactic angle.
 *
 * @param ha object hour angle (in degrees)
 * @param dec object declination (in degrees)
 * @param sin_lat sinus of latitude
 * @param cos_lat cosinus of latitude
 * @param tan_lat tan of latitude
 * @param pa calculated object paralactic angle (in degrees)
 * @param parate calculated object paralactic rate (in degrees/hours)
 */
void parallacticAngle (double ha, double dec, double sin_lat, double cos_lat, double tan_lat, double &pa, double &parate);

/**
 * Converts spherical coordinates to vector.
 */
void sph2cart (double a, double b, double *xyz);

/**
 * Converts vector coordinates to spherical.
 */
void cart2sph (double *xyz, double &a, double &b);

/**
 * Calculates position angle between two points.
 */
double posangle (double *xyz0, double *xyz1);

/**
 * Send progress to ostream.
 *
 * base note: the classic header declared the spinner position/glyphs as
 * file-scope `static` globals, so each translation unit that included the
 * header got its own independent counter. Moved to function-local statics
 * inside the (already-header-defined, hence inline/one-definition-rule)
 * friend operator - one shared counter program-wide instead, which is what
 * a rotating "busy" spinner is presumably meant to be. Purely cosmetic.
 *
 * @author Petr Kubanek <petr@kubanek.net>
 */
class ProgressIndicator
{
	public:
		ProgressIndicator (double progress, int width = 20) { pr = progress; w = width; };
		friend std::ostream & operator << (std::ostream &os, ProgressIndicator p)
		{
			static int curp = 0;
			curp++;
			curp %= 4;
			static const char screenSymbols[4] = {'-','\\','|','/'};
			for (int cp = 0; cp < p.w; cp ++)
			{
				if (100.0 * cp / p.w < p.pr)
					os << "#";
				else
					os << screenSymbols[curp];
			}
			return os;
		}
	private:
		double pr;
		int w;
};
