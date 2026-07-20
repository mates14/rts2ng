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

#pragma once

// base note: retrieveDUT1() (network fetch from the USNO DUT1 service) is
// declared here for API-shape parity with the classic header, but its
// implementation is stubbed in dut1.cpp - it requires libxml2 + RTS2's own
// xmlrpc++ HTTP client, same optional/vendor-tier dependency as
// SimbadTarget in the monitor. getDUT1() (local-file parse, no network) is
// ported for real and is all Telescope needs when no --dut1-filename is
// actively being refreshed from the internet.

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Retrieves DUT1 from the internet.
 */
int retrieveDUT1 (const char *fn, const char *url = NULL);

/**
 * Calculates DUT from filename stored on HDD for given GM date.
 * File with offsets can be downloaded from:
 * http://maia.usno.navy.mil/ser7/finals2000A.daily
 *
 * @param fn         filename
 * @param gmdate     date for which offset should be retrieved
 * @return DUT1 for given data, NAN if DUT was not found in the provided file
 */
double getDUT1 (const char *fn, struct tm *gmdate);

#ifdef __cplusplus
};
#endif
