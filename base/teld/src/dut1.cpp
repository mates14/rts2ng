/*
 * Parse DUT1 file and output DUT1 for given date.
 * Copyright (C) 2017 Petr Kubanek <petr@kubanek.net>
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
 */

// base note: retrieveDUT1() deferred - see dut1.h. The classic
// implementation used xmlrpc++ to POST to the USNO DUT1 service; ported
// here as a stub that logs and returns failure instead.

#include <stdio.h>
#include <errno.h>
#include <cstring>
#include <cmath>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "dut1.h"
#include "app.h"

int retrieveDUT1 (const char *fn, const char *url)
{
	logStream (MESSAGE_ERROR) << "DUT1 network retrieval not ported in base (requires xmlrpc++); provide a pre-downloaded DUT1 file instead" << sendLog;
	return -1;
}

double getDUT1 (const char *fn, struct tm *gmdate)
{
	FILE *f = fopen (fn, "r");
	if (f == NULL)
	{
		logStream (MESSAGE_ERROR) << "cannot open DUT file " << fn << ", error:" << strerror (errno) << sendLog;
		return NAN;
	}

	char *line = NULL;
	size_t n = 0;

	char datepart[48];
	snprintf (datepart, sizeof (datepart), "%2d%2d%2d", gmdate->tm_year - 100, gmdate->tm_mon + 1, gmdate->tm_mday);

	while (getline (&line, &n, f) >= 0)
	{
		if (n > 60 && strncmp (line, datepart, 6) == 0)
		{
			double dut1 = atof (line + 59);
			free (line);
			fclose (f);
			return dut1;
		}
	}
	free (line);
	fclose (f);

	return NAN;
}
