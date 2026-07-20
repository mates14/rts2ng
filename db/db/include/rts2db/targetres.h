/*
 * Target from any string.
 * Copyright (C) 2005-2016 Petr Kubanek <petr@kubanek.net>
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

#include "rts2db/target.h"

/**
 * Return new target object, created from string. String might contain RA DEC pair or an MPEC one-line.
 *
 * base note: classic createTargetByString() also resolved plain names via SIMBAD
 * (rts2db::SimbadTargetDb), MPEC-catalog lookups (rts2db::MPECTarget) and TLE satellite
 * elements (rts2db::TLETarget). None of those three are ported yet - SIMBAD/MPEC need an
 * XmlRpc client not present in this tree, TLE needs the ~3200-line SGP4/SDP4 propagator
 * (see db/STATUS.md, same deferral as TargetAuger/TLETarget in Target::createTarget()).
 * This port keeps the empty-target, RA/Dec-parse and MPC-ephemeris (EllTarget::orbitFromMPC)
 * branches faithfully and throws a clear rts2core::Error for anything else, rather than
 * silently misbehaving.
 *
 * @param tar_string  String containing RA DEC position, MPC one-line ephemeris, or empty.
 * @param debug Print debug data.
 *
 * @return new target object. Caller must deallocate target object (delete it).
 */
rts2db::Target *createTargetByString (std::string tar_string, bool debug);
