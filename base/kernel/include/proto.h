/*
 * Wire protocol command codes.
 * Copyright (C) 2003-2010 Petr Kubanek <petr@kubanek.net>
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

// base note: these single-letter command codes were #defines inside
// block.h in the classic tree. Extracted to their own header since they are
// pure wire-protocol constants - message.cpp, value.cpp and friends need
// them without needing the rest of Block.

#define PROTO_VALUE            "V"
#define PROTO_SET_VALUE        "X"
#define PROTO_AUTH             "A"
#define PROTO_STATUS           "S"
#define PROTO_PROGRESS         "P"
#define PROTO_STATUS_PROGRESS  "R"
#define PROTO_BOP_STATE        "B"
#define PROTO_TECHNICAL        "T"
#define PROTO_MESSAGE          "M"
#define PROTO_METAINFO         "E"
#define PROTO_SELMETAINFO      "F"
#define PROTO_DELETE           "Z"

#define PROTO_BINARY           "C"
#define PROTO_DATA             "D"
#define PROTO_BINARY_KILLED    "H"
#define PROTO_SHARED           "I"
#define PROTO_SHARED_FULL      "J"
#define PROTO_SHARED_KILLED    "K"
