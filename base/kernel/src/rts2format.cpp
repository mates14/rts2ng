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

#include "rts2format.h"

#include <cstdlib>

static int flagSpace = -1;

std::ostream & spaceDegSep (std::ostream & _os)
{
	if (flagSpace == -1)
		flagSpace = _os.xalloc ();
	_os.iword (flagSpace) = 1;
	return _os;
}

bool formatSpaceDegSep (std::ostream & _os)
{
	return flagSpace != -1 && _os.iword (flagSpace) == 1;
}

static int flagPureNumbers = -1;

std::ostream & pureNumbers (std::ostream & _os)
{
	if (flagPureNumbers == -1)
		flagPureNumbers = _os.xalloc ();
	_os.iword (flagPureNumbers) = 1;
	return _os;
}

bool formatPureNumbers (std::ostream & _os)
{
	return flagPureNumbers != -1 && _os.iword (flagPureNumbers) == 1;
}

static int flagLocalTime = -1;
static bool defaultLocalTime = false;

std::ostream & localTime (std::ostream & _os)
{
	if (flagLocalTime == -1)
		flagLocalTime = _os.xalloc ();
	_os.iword (flagLocalTime) = 1;
	tzset ();
	return _os;
}

bool formatLocalTime (std::ostream & _os)
{
	return defaultLocalTime || (flagLocalTime != -1 && _os.iword (flagLocalTime) == 1);
}

void setLocalTimeDefault (bool useLocalTime)
{
	defaultLocalTime = useLocalTime;
}

// Stored as iword+1 so that 0 - the value every stream starts with, and
// the value a stream that never saw one of these manipulators keeps -
// means "nothing asked for here", not TIME_ISO.
static int flagTimeDisplay = -1;
static timeDisplay_t defaultTimeDisplay = TIME_ISO;

static std::ostream & setTimeDisplay (std::ostream & _os, timeDisplay_t display)
{
	if (flagTimeDisplay == -1)
		flagTimeDisplay = _os.xalloc ();
	_os.iword (flagTimeDisplay) = display + 1;
	return _os;
}

std::ostream & isoTime (std::ostream & _os)
{
	return setTimeDisplay (_os, TIME_ISO);
}

std::ostream & ctimeNumbers (std::ostream & _os)
{
	return setTimeDisplay (_os, TIME_CTIME);
}

std::ostream & jdNumbers (std::ostream & _os)
{
	return setTimeDisplay (_os, TIME_JD);
}

timeDisplay_t formatTimeDisplay (std::ostream & _os)
{
	if (flagTimeDisplay != -1 && _os.iword (flagTimeDisplay) != 0)
		return (timeDisplay_t) (_os.iword (flagTimeDisplay) - 1);
	if (defaultTimeDisplay != TIME_ISO)
		return defaultTimeDisplay;
	// pureNumbers() has always meant "a timestamp prints as a ctime
	// number" - see Timestamp's operator << in classic and here.
	if (formatPureNumbers (_os))
		return TIME_CTIME;
	return TIME_ISO;
}

void setTimeDisplayDefault (timeDisplay_t display)
{
	defaultTimeDisplay = display;
}
