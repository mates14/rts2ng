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

#include "xmlrpcclient.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace rts2core;

static const XmlRpcValue nilValue;

bool XmlRpcValue::asBool () const
{
	switch (type)
	{
		case BOOL:
		case INT:
			return i != 0;
		case DOUBLE:
			return d != 0;
		case STRING:
			return s == "1" || s == "true" || s == "True";
		default:
			throw XmlRpcError ("value is not a boolean");
	}
}

long XmlRpcValue::asInt () const
{
	switch (type)
	{
		case BOOL:
		case INT:
			return i;
		case DOUBLE:
			return lround (d);
		case STRING:
		{
			char *end;
			long r = strtol (s.c_str (), &end, 10);
			if (end == s.c_str ())
				throw XmlRpcError ("not an integer: '" + s + "'");
			return r;
		}
		default:
			throw XmlRpcError ("value is not an integer");
	}
}

double XmlRpcValue::asDouble () const
{
	switch (type)
	{
		case BOOL:
		case INT:
			return i;
		case DOUBLE:
			return d;
		case STRING:
		{
			char *end;
			double r = strtod (s.c_str (), &end);
			if (end == s.c_str ())
				throw XmlRpcError ("not a number: '" + s + "'");
			return r;
		}
		default:
			throw XmlRpcError ("value is not a number");
	}
}

std::string XmlRpcValue::asString () const
{
	switch (type)
	{
		case STRING:
			return s;
		case BOOL:
		case INT:
			return std::to_string (i);
		case DOUBLE:
		{
			char buf[50];
			snprintf (buf, sizeof (buf), "%.17g", d);
			return buf;
		}
		case NIL:
			return "";
		default:
			throw XmlRpcError ("value is not a scalar");
	}
}

const XmlRpcValue & XmlRpcValue::operator[] (size_t idx) const
{
	if (type != ARRAY || idx >= items.size ())
		throw XmlRpcError ("array index out of range");
	return items[idx];
}

const XmlRpcValue & XmlRpcValue::operator[] (const std::string &name) const
{
	if (type != STRUCT)
		throw XmlRpcError ("value is not a struct");
	auto it = members.find (name);
	if (it == members.end ())
		throw XmlRpcError ("missing member '" + name + "'");
	return it->second;
}

static void xmlEscape (const std::string &in, std::string &out)
{
	for (char c : in)
	{
		switch (c)
		{
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			case '&': out += "&amp;"; break;
			default: out += c;
		}
	}
}

void XmlRpcValue::toXml (std::string &out) const
{
	out += "<value>";
	switch (type)
	{
		case NIL:
			out += "<nil/>";
			break;
		case BOOL:
			out += i ? "<boolean>1</boolean>" : "<boolean>0</boolean>";
			break;
		case INT:
			out += "<int>" + std::to_string (i) + "</int>";
			break;
		case DOUBLE:
			out += "<double>" + asString () + "</double>";
			break;
		case STRING:
			out += "<string>";
			xmlEscape (s, out);
			out += "</string>";
			break;
		case ARRAY:
			out += "<array><data>";
			for (auto &v : items)
				v.toXml (out);
			out += "</data></array>";
			break;
		case STRUCT:
			out += "<struct>";
			for (auto &m : members)
			{
				out += "<member><name>";
				xmlEscape (m.first, out);
				out += "</name>";
				m.second.toXml (out);
				out += "</member>";
			}
			out += "</struct>";
			break;
	}
	out += "</value>";
}

namespace rts2core
{

/**
 * Just enough XML to read a methodResponse: elements, text, entities,
 * comments and the XML declaration. No attributes are used by XML-RPC,
 * so they are skipped.
 */
class XmlRpcParser
{
	public:
		XmlRpcParser (const std::string &_doc): doc (_doc), pos (0) {}

		XmlRpcValue parseResponse ()
		{
			expectOpen ("methodResponse");
			std::string tag = nextOpen ();
			if (tag == "fault")
			{
				XmlRpcValue f = parseValue ();
				expectClose ("fault");
				std::string msg = "server fault";
				if (f.type == XmlRpcValue::STRUCT)
				{
					if (f.has ("faultString"))
						msg = f["faultString"].asString ();
					if (f.has ("faultCode"))
						msg = "fault " + f["faultCode"].asString () + ": " + msg;
				}
				throw XmlRpcError (msg, true);
			}
			if (tag != "params")
				throw XmlRpcError ("unexpected <" + tag + "> in response");
			XmlRpcValue ret;
			if (peekOpen ("param"))
			{
				expectOpen ("param");
				ret = parseValue ();
				expectClose ("param");
			}
			return ret;
		}

	private:
		const std::string &doc;
		size_t pos;

		void skipMisc ()
		{
			while (true)
			{
				while (pos < doc.size () && isspace ((unsigned char) doc[pos]))
					pos++;
				if (doc.compare (pos, 2, "<?") == 0)
				{
					size_t e = doc.find ("?>", pos);
					if (e == std::string::npos)
						throw XmlRpcError ("unterminated XML declaration");
					pos = e + 2;
				}
				else if (doc.compare (pos, 4, "<!--") == 0)
				{
					size_t e = doc.find ("-->", pos);
					if (e == std::string::npos)
						throw XmlRpcError ("unterminated XML comment");
					pos = e + 3;
				}
				else
					return;
			}
		}

		// reads a tag at pos: returns its name, sets closing / selfClosing
		std::string readTag (bool &closing, bool &selfClosing)
		{
			skipMisc ();
			if (pos >= doc.size () || doc[pos] != '<')
				throw XmlRpcError ("expected a tag in XML-RPC response");
			size_t e = doc.find ('>', pos);
			if (e == std::string::npos)
				throw XmlRpcError ("unterminated tag in XML-RPC response");
			std::string body = doc.substr (pos + 1, e - pos - 1);
			pos = e + 1;
			closing = !body.empty () && body[0] == '/';
			if (closing)
				body.erase (0, 1);
			selfClosing = !body.empty () && body.back () == '/';
			if (selfClosing)
				body.pop_back ();
			size_t sp = body.find_first_of (" \t\r\n");
			if (sp != std::string::npos)
				body.erase (sp);
			return body;
		}

		std::string nextOpen ()
		{
			bool closing, selfClosing;
			std::string t = readTag (closing, selfClosing);
			if (closing || selfClosing)
				throw XmlRpcError ("unexpected </" + t + "> in XML-RPC response");
			return t;
		}

		bool peekOpen (const char *name)
		{
			size_t saved = pos;
			bool closing, selfClosing;
			std::string t;
			try
			{
				t = readTag (closing, selfClosing);
			}
			catch (XmlRpcError &)
			{
				pos = saved;
				return false;
			}
			pos = saved;
			return !closing && t == name;
		}

		void expectOpen (const char *name)
		{
			std::string t = nextOpen ();
			if (t != name)
				throw XmlRpcError (std::string ("expected <") + name + ">, got <" + t + ">");
		}

		void expectClose (const char *name)
		{
			bool closing, selfClosing;
			std::string t = readTag (closing, selfClosing);
			if (!closing || t != name)
				throw XmlRpcError (std::string ("expected </") + name + ">, got <" + t + ">");
		}

		// text up to the next '<', entities decoded
		std::string readText ()
		{
			size_t e = doc.find ('<', pos);
			if (e == std::string::npos)
				throw XmlRpcError ("unterminated text in XML-RPC response");
			std::string raw = doc.substr (pos, e - pos);
			pos = e;
			std::string out;
			for (size_t k = 0; k < raw.size (); k++)
			{
				if (raw[k] != '&')
				{
					out += raw[k];
					continue;
				}
				size_t semi = raw.find (';', k);
				if (semi == std::string::npos)
				{
					out += raw[k];
					continue;
				}
				std::string ent = raw.substr (k + 1, semi - k - 1);
				if (ent == "lt") out += '<';
				else if (ent == "gt") out += '>';
				else if (ent == "amp") out += '&';
				else if (ent == "quot") out += '"';
				else if (ent == "apos") out += '\'';
				else if (!ent.empty () && ent[0] == '#')
				{
					long cp = (ent.size () > 1 && (ent[1] == 'x' || ent[1] == 'X')) ? strtol (ent.c_str () + 2, nullptr, 16) : strtol (ent.c_str () + 1, nullptr, 10);
					// UTF-8 encode
					if (cp < 0x80)
						out += (char) cp;
					else if (cp < 0x800)
					{
						out += (char) (0xC0 | (cp >> 6));
						out += (char) (0x80 | (cp & 0x3F));
					}
					else if (cp < 0x10000)
					{
						out += (char) (0xE0 | (cp >> 12));
						out += (char) (0x80 | ((cp >> 6) & 0x3F));
						out += (char) (0x80 | (cp & 0x3F));
					}
					else
					{
						out += (char) (0xF0 | (cp >> 18));
						out += (char) (0x80 | ((cp >> 12) & 0x3F));
						out += (char) (0x80 | ((cp >> 6) & 0x3F));
						out += (char) (0x80 | (cp & 0x3F));
					}
				}
				else
					out += "&" + ent + ";";
				k = semi;
			}
			return out;
		}

		// at <value>
		XmlRpcValue parseValue ()
		{
			bool closing, selfClosing;
			std::string t = readTag (closing, selfClosing);
			if (t != "value" || closing)
				throw XmlRpcError ("expected <value>, got <" + t + ">");
			XmlRpcValue v;
			if (selfClosing)
			{
				v.type = XmlRpcValue::STRING;
				return v;
			}
			// an untyped value is a string: its text runs to </value>;
			// before a type tag, the text is only whitespace
			std::string text = readText ();
			t = readTag (closing, selfClosing);
			if (closing)
			{
				if (t != "value")
					throw XmlRpcError ("unexpected </" + t + "> in <value>");
				v.type = XmlRpcValue::STRING;
				v.s = text;
				return v;
			}
			if (selfClosing)
			{
				if (t == "nil")
					v.type = XmlRpcValue::NIL;
				else if (t == "string")
					v.type = XmlRpcValue::STRING;
				else
					throw XmlRpcError ("empty <" + t + "/> in XML-RPC response");
				expectClose ("value");
				return v;
			}
			if (t == "string")
			{
				v.type = XmlRpcValue::STRING;
				v.s = readText ();
			}
			else if (t == "int" || t == "i4" || t == "i8")
			{
				v.type = XmlRpcValue::INT;
				v.i = strtol (readText ().c_str (), nullptr, 10);
			}
			else if (t == "boolean")
			{
				v.type = XmlRpcValue::BOOL;
				v.i = strtol (readText ().c_str (), nullptr, 10) ? 1 : 0;
			}
			else if (t == "double")
			{
				v.type = XmlRpcValue::DOUBLE;
				v.d = strtod (readText ().c_str (), nullptr);
			}
			else if (t == "dateTime.iso8601" || t == "base64")
			{
				// not decoded; kept as the raw text
				v.type = XmlRpcValue::STRING;
				v.s = readText ();
			}
			else if (t == "array")
			{
				v.type = XmlRpcValue::ARRAY;
				t = readTag (closing, selfClosing);
				if (t != "data" || closing)
					throw XmlRpcError ("expected <data> in <array>");
				if (!selfClosing)
				{
					while (peekOpen ("value"))
						v.items.push_back (parseValue ());
					expectClose ("data");
				}
			}
			else if (t == "struct")
			{
				v.type = XmlRpcValue::STRUCT;
				while (peekOpen ("member"))
				{
					expectOpen ("member");
					expectOpen ("name");
					std::string name = readText ();
					expectClose ("name");
					v.members[name] = parseValue ();
					expectClose ("member");
				}
			}
			else
			{
				throw XmlRpcError ("unknown XML-RPC type <" + t + ">");
			}
			expectClose (t.c_str ());
			expectClose ("value");
			return v;
		}
};

}

std::string XmlRpcClient::getUrl () const
{
	return "http://" + host + ":" + std::to_string (port) + path;
}

static double nowSec ()
{
	struct timeval tv;
	gettimeofday (&tv, nullptr);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

// waits for events on fd until the deadline; false on timeout
static bool waitFd (int fd, short events, double deadline)
{
	while (true)
	{
		double left = deadline - nowSec ();
		if (left <= 0)
			return false;
		struct pollfd pfd;
		pfd.fd = fd;
		pfd.events = events;
		pfd.revents = 0;
		int ret = poll (&pfd, 1, (int) ceil (left * 1000));
		if (ret > 0)
			return true;
		if (ret == 0)
			return false;
		if (errno != EINTR)
			return false;
	}
}

std::string XmlRpcClient::httpPost (const std::string &body)
{
	double deadline = nowSec () + timeout;

	struct addrinfo hints, *res;
	memset (&hints, 0, sizeof (hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	int gai = getaddrinfo (host.c_str (), std::to_string (port).c_str (), &hints, &res);
	if (gai)
		throw XmlRpcError ("cannot resolve " + host + ": " + gai_strerror (gai));

	int fd = -1;
	std::string lastErr = "no address";
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
	{
		fd = socket (ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
		if (fd < 0)
		{
			lastErr = strerror (errno);
			continue;
		}
		int ret = connect (fd, ai->ai_addr, ai->ai_addrlen);
		if (ret < 0 && errno == EINPROGRESS)
		{
			if (waitFd (fd, POLLOUT, deadline))
			{
				int err = 0;
				socklen_t len = sizeof (err);
				getsockopt (fd, SOL_SOCKET, SO_ERROR, &err, &len);
				ret = err ? -1 : 0;
				errno = err;
			}
			else
			{
				errno = ETIMEDOUT;
			}
		}
		if (ret == 0)
			break;
		lastErr = strerror (errno);
		close (fd);
		fd = -1;
	}
	freeaddrinfo (res);
	if (fd < 0)
		throw XmlRpcError ("cannot connect to " + getUrl () + ": " + lastErr);

	std::ostringstream req;
	req << "POST " << path << " HTTP/1.0\r\n"
		<< "Host: " << host << ":" << port << "\r\n"
		<< "User-Agent: rts2\r\n"
		<< "Content-Type: text/xml\r\n"
		<< "Content-Length: " << body.size () << "\r\n\r\n"
		<< body;
	std::string out = req.str ();

	size_t sent = 0;
	while (sent < out.size ())
	{
		if (!waitFd (fd, POLLOUT, deadline))
		{
			close (fd);
			throw XmlRpcError ("timeout sending to " + getUrl ());
		}
		ssize_t r = send (fd, out.data () + sent, out.size () - sent, MSG_NOSIGNAL);
		if (r < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			std::string e = strerror (errno);
			close (fd);
			throw XmlRpcError ("send to " + getUrl () + " failed: " + e);
		}
		sent += r;
	}

	// HTTP/1.0: the server closes the connection after the reply
	std::string reply;
	char buf[8192];
	while (true)
	{
		if (!waitFd (fd, POLLIN, deadline))
		{
			close (fd);
			throw XmlRpcError ("timeout waiting for " + getUrl ());
		}
		ssize_t r = recv (fd, buf, sizeof (buf), 0);
		if (r < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			std::string e = strerror (errno);
			close (fd);
			throw XmlRpcError ("receive from " + getUrl () + " failed: " + e);
		}
		if (r == 0)
			break;
		reply.append (buf, r);
	}
	close (fd);

	size_t hdrEnd = reply.find ("\r\n\r\n");
	if (hdrEnd == std::string::npos)
		throw XmlRpcError ("malformed HTTP reply from " + getUrl ());
	// status line: HTTP/1.x 200 OK
	size_t sp = reply.find (' ');
	if (sp == std::string::npos || sp > hdrEnd)
		throw XmlRpcError ("malformed HTTP status from " + getUrl ());
	int status = atoi (reply.c_str () + sp + 1);
	if (status != 200)
	{
		size_t eol = reply.find ("\r\n");
		throw XmlRpcError ("HTTP error from " + getUrl () + ": " + reply.substr (0, eol));
	}
	return reply.substr (hdrEnd + 4);
}

XmlRpcValue XmlRpcClient::call (const std::string &method, const std::vector <XmlRpcValue> &params)
{
	std::string body = "<?xml version=\"1.0\"?>\n<methodCall><methodName>";
	xmlEscape (method, body);
	body += "</methodName><params>";
	for (auto &p : params)
	{
		body += "<param>";
		p.toXml (body);
		body += "</param>";
	}
	body += "</params></methodCall>\n";

	std::string reply = httpPost (body);
	XmlRpcParser parser (reply);
	return parser.parseResponse ();
}
