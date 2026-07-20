/*
 * Connection for inotify events.
 * Copyright (C) 2011 Petr Kubanek, Institute of Physics <kubanek@fzu.cz>
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

#include "connnosend.h"

#include <sys/inotify.h>

namespace rts2core
{

/**
 * Class for inotify file descriptors.
 *
 * Read events from descriptor created by inotify, and send them to master.
 * If ConnNotify is added to watched connections by calling addConnection,
 * fileModified will be called every time modification to file is noticed.
 *
 * @author Petr Kubanek <kubanek@fzu.cz>
 */
class ConnNotify:public ConnNoSend
{
	public:
		/**
		 * Create new notify connection.
		 *
		 * @param _master   Reference to master holding this connection.
		 */
		ConnNotify (Block *_master);

		/**
		 * Init inotify file descriptor.
		 *
		 * @return -1 on error, 0 on success.
		 */
		virtual int init ();

		/**
		 * Set debugging flag. When debugging is enabled, all data send
		 * to and from the socket will be logged using standard RTS2
		 * logging with MESSAGE_DEBUG type.
		 */
		void setDebug (bool _debug = true) { debug = _debug; }

		/**
		 * Called when select call indicates that socket holds new
		 * data for reading.
		 *
		 * @param fds	poll structure, connection must test if socket has data to read
		 *
		 * @return -1 on error, 0 on success.
		 */
		virtual int receive (Block *block);

		bool getDebug () { return debug; }

		int addWatch (const char *filename, uint32_t mask = IN_MODIFY);

	private:
		// base note: classic tree had an unused `hostname` field here (never
		// set, never read) - dropped.
		bool debug;
};

}
