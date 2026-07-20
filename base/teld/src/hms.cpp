/*
 * Two functions to work with HMS format.
 * Copyright (C) 2003-2008 Petr Kubanek <petr@kubanek.net>
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

#include "hms.h"

#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <cstring>

double hmstod (const char *hptr)
{
	char *buf;
	char *locptr;
	char *endptr;
	double ret;					 //to store return value
	double mul;					 //multiplier

	if (!(buf = strdup (hptr)))
		return NAN;

	locptr = buf;
	if (*locptr == '-')
	{
		locptr++;
		mul = -1;
	}
	else
	{
		mul = 1;
	}

	endptr = locptr;
	ret = 0;

	while (*endptr)
	{
		errno = 0;
		// convert test
		ret += strtod (locptr, &endptr) * mul;

		if (errno == ERANGE)
		{
			free (buf);
			return NAN;
		}
		// we get sucessfuly to end
		if (!*endptr)
		{
			errno = 0;
			free (buf);
			return ret;
		}

		// if we have error in translating first string..
		if (locptr == endptr)
		{
			errno = EINVAL;
			free (buf);
			return NAN;
		}

		mul /= 60;
		locptr = endptr + 1;
	}

	errno = 0;
	free (buf);
	return ret;
}

int dtoints (double value, int *h, int *m, int *s)
{
	int sign;
	double m_fraction;
	if (value < 0)
	{
		value = -value;
		sign = -1;
	}
	else
	{
		sign = 1;
	}
	*h = floor (value) * sign;
	m_fraction = (value - floor (value)) * 60;
	*m = floor (m_fraction);
	*s = round ((m_fraction - *m) * 60);
	// unification
	if (*s == 60)
	{
		*s = 0;
		(*m)++;
	}
	if (*m >= 60)
	{
		*m -= 60;
		(*h)++;
	}
	return 0;
}
