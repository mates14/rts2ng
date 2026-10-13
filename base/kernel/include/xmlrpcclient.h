/*
 * Minimal blocking XML-RPC client.
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

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace rts2core
{

/**
 * One XML-RPC value: scalar, array or struct.
 *
 * Classic RTS2 used xmlrpc++ for this; base has no XML-RPC library, and
 * the servers it talks to (the Perek 2m daemons) need only the basic
 * types, so this is a small self-contained implementation.
 */
class XmlRpcValue
{
	public:
		enum Type { NIL, BOOL, INT, DOUBLE, STRING, ARRAY, STRUCT };

		XmlRpcValue (): type (NIL), i (0), d (0) {}
		XmlRpcValue (bool v): type (BOOL), i (v ? 1 : 0), d (0) {}
		XmlRpcValue (int v): type (INT), i (v), d (0) {}
		XmlRpcValue (double v): type (DOUBLE), i (0), d (v) {}
		XmlRpcValue (const char *v): type (STRING), i (0), d (0), s (v) {}
		XmlRpcValue (const std::string &v): type (STRING), i (0), d (0), s (v) {}

		static XmlRpcValue array () { XmlRpcValue v; v.type = ARRAY; return v; }
		static XmlRpcValue structure () { XmlRpcValue v; v.type = STRUCT; return v; }

		Type getType () const { return type; }

		bool asBool () const;
		long asInt () const;
		double asDouble () const;
		// strings as they are, numbers formatted
		std::string asString () const;

		// array access
		size_t size () const { return type == STRUCT ? members.size () : items.size (); }
		const XmlRpcValue & operator[] (size_t idx) const;
		void push (const XmlRpcValue &v) { items.push_back (v); }

		// struct access; a missing member throws XmlRpcError
		bool has (const std::string &name) const { return members.find (name) != members.end (); }
		const XmlRpcValue & operator[] (const std::string &name) const;
		void set (const std::string &name, const XmlRpcValue &v) { members[name] = v; }
		const std::map <std::string, XmlRpcValue> & getMembers () const { return members; }

		void toXml (std::string &out) const;

	private:
		Type type;
		long i;
		double d;
		std::string s;
		std::vector <XmlRpcValue> items;
		std::map <std::string, XmlRpcValue> members;

		friend class XmlRpcParser;
};

/**
 * Raised for any failed call: network, HTTP, parse error or a server fault.
 * isFault () tells a fault (the server answered, the call failed) from the rest.
 */
class XmlRpcError:public std::runtime_error
{
	public:
		XmlRpcError (const std::string &msg, bool _fault = false): std::runtime_error (msg), fault (_fault) {}
		bool isFault () const { return fault; }
	private:
		bool fault;
};

/**
 * Blocking XML-RPC client over plain HTTP/1.0.
 *
 * Every call opens its own TCP connection, so calls from different threads
 * never share a socket; there is no state but the address. Every step
 * (connect, send, receive) is bounded by the timeout.
 */
class XmlRpcClient
{
	public:
		XmlRpcClient (const std::string &_host, int _port, double _timeout = 5.0, const std::string &_path = "/RPC2"):
			host (_host), port (_port), timeout (_timeout), path (_path) {}

		/**
		 * Call a method and return its result. Throws XmlRpcError.
		 */
		XmlRpcValue call (const std::string &method, const std::vector <XmlRpcValue> &params = std::vector <XmlRpcValue> ());

		std::string getUrl () const;

		void setTimeout (double _timeout) { timeout = _timeout; }

	private:
		std::string host;
		int port;
		double timeout;
		std::string path;

		std::string httpPost (const std::string &body);
};

}
