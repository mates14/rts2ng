/*
 * Rts2 formatting functions.
 * Copyright (C) 2008 Petr Kubanek <petr@kubanek.net>
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

#include <ostream>

/**
 * Stream output will separated hours and minutes with space.
 */
std::ostream & spaceDegSep (std::ostream & _os);

/**
 * Returns state of space formating flag.
 *
 * @return True if degrees, minutes and seconds should be separated by space.
 */
bool formatSpaceDegSep (std::ostream & _os);

/**
 * Stream output will always contains pure numbers. Degreess
 * pretty print will be disabled.
 */
std::ostream & pureNumbers (std::ostream & _os);

/**
 * Returns state of pure number formating flag.
 *
 * @return True if numbers shall be always printed as numbers, e.g. not pretty printed as degrees, minutes etc.
 */
bool formatPureNumbers (std::ostream & _os);

std::ostream & localTime (std::ostream & _os);

/**
 * If print times in local time.
 */
bool formatLocalTime (std::ostream & _os);

/**
 * How a time - a double carrying either UNIX ctime seconds or a Julian
 * Date - is rendered when it is streamed out.
 *
 * base note: new in this tree, not in classic. Classic had exactly two
 * renderings, the ISO-ish calendar string and, under pureNumbers(), a
 * bare ctime number - and every other place that wanted a number simply
 * streamed the double itself, at ostream's default precision of six
 * *significant* digits. For a ctime that is catastrophic and silent:
 * 1788180123.456 prints as 1.78818e+09, i.e. rounded to the nearest ~1000
 * seconds, so a whole list of messages comes out stamped at the same
 * instant (found exactly that way in rts2-httpd's JSON output). A time is
 * not just a double that happens to be large - it has a display mode, and
 * that mode belongs here in the formatting layer next to degree/local-time
 * formatting, not re-decided at every call site.
 *
 * TIME_ISO is the default and is what Timestamp has always printed
 * (2026-08-31T12:40:00.000 UT). TIME_CTIME and TIME_JD print a bare
 * number, in fixed notation, with the precision constants below - never
 * in significant-digit notation, which is what loses the resolution.
 */
typedef enum { TIME_ISO, TIME_CTIME, TIME_JD } timeDisplay_t;

/**
 * Decimal places for a bare ctime number: microseconds, which is the
 * resolution struct timeval carries in the first place.
 */
#define CTIME_PRECISION   6

/**
 * Decimal places for a bare Julian Date: ~0.9 ms, and 7 + 8 = 15 digits
 * stays inside a double's ~15-16 significant digits. Same precision
 * Expander's %J expansion already uses.
 */
#define JD_PRECISION      8

/**
 * Stream times as the ISO-ish calendar string (the default).
 */
std::ostream & isoTime (std::ostream & _os);

/**
 * Stream times as bare UNIX ctime numbers.
 */
std::ostream & ctimeNumbers (std::ostream & _os);

/**
 * Stream times as bare Julian Dates.
 */
std::ostream & jdNumbers (std::ostream & _os);

/**
 * Effective time display mode for a stream: an explicit manipulator on
 * the stream wins, then the process-wide default, then TIME_ISO.
 *
 * pureNumbers() implies TIME_CTIME when nothing else was asked for -
 * that is what it has always meant for timestamps, and callers relying
 * on it keep working unchanged.
 */
timeDisplay_t formatTimeDisplay (std::ostream & _os);

/**
 * Sets the process-wide default time display mode - App does this for
 * --jd/--ctime, the same way it does for --UT.
 */
void setTimeDisplayDefault (timeDisplay_t display);

/**
 * Sets the process-wide default for local-time formatting.
 *
 * base note: the classic tree read this straight off a global App
 * singleton (getMasterApp()->usesLocalTime()), which meant this pure
 * ostream-formatting module depended on the whole App/CLI framework. Here
 * App (once ported) calls this setter instead of formatLocalTime() reaching
 * upward - keeps the dependency one-directional.
 */
void setLocalTimeDefault (bool useLocalTime);
