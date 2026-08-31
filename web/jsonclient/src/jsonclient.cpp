/*
 * rts2-jsonclient: a command line client for rts2-httpd's JSON API.
 *
 * The scriptable counterpart of the web frontend, and the replacement for
 * classic's rts2-xmlrpcclient (dropped along with XML-RPC itself - see
 * web/STATUS.md's "Dropped outright"). Everything it does goes through the
 * same HTTP endpoints the browser uses, so it works from any machine that
 * can reach the daemon's port, without an RTS2 bus connection of its own -
 * that is the whole point of having it, next to rts2-sendcmd (which speaks
 * the bus protocol and must run where the bus is reachable).
 *
 * Writes (set/inc/dec, switchstate) are fire-and-forget on the daemon side:
 * /api/set queues the value change and answers immediately, before the
 * device acknowledges it (see the fire-and-forget comment in httpd.cpp).
 * A successful exit therefore means "the daemon accepted and queued the
 * change", not "the device has applied it" - hence no value echo on
 * success, which would only ever print the pre-change state.
 */

#include "cliapp.h"
#include "error.h"
#include "message.h"
#include "option.h"
#include "timestamp.h"

#include "httpfetch.h"
#include "json.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>

#define OPT_TIMEOUT     OPT_LOCAL + 1

namespace rts2web
{

class JsonClient: public rts2core::CliApp
{
	public:
		JsonClient (int in_argc, char **in_argv);
		virtual ~JsonClient ();

	protected:
		virtual int processOption (int in_opt);
		virtual int processArgs (const char *arg);
		virtual int init ();
		virtual int doProcessing ();
		virtual void usage ();

	private:
		std::string serverUrl;
		std::string userPass;
		bool rawJson;
		long timeout;

		std::vector <std::string> args;
		HttpFetch *fetch;

		int runCommand ();

		/**
		 * GET one endpoint and parse the response.
		 *
		 * The daemon reports every API-level problem the same way - a
		 * non-200 status with a {"error":"..."} body (see handleRequest()'s
		 * ApiError catch) - so unwrapping that into an exception here means
		 * no command handler has to check for it.
		 */
		JsonPtr request (const std::string &path, const HttpParams &params = HttpParams ());

		/** With --json, print the response as indented JSON and tell the
		 * caller its own formatting is not wanted. */
		bool dumpJson (JsonPtr response);

		/** Print `name = value` for every member of a value object,
		 * with the '=' aligned. prefix is the device name (getall) or
		 * empty (get of a single device). */
		void printValues (const JsonValue *values, const std::string &prefix);

		/** Endpoint path and parameters for the "api"/"db" escape-hatch
		 * commands: remaining args are taken as key=value pairs. */
		HttpParams argParams (size_t from);
};

}

using namespace rts2web;

JsonClient::JsonClient (int in_argc, char **in_argv): rts2core::CliApp (in_argc, in_argv)
{
	// Same default port rts2-httpd itself uses when started without -p.
	// $RTS2_HTTPD overrides it, so a site with a non-default port (or a
	// reverse proxy) can set it once in the environment rather than on
	// every invocation.
	const char *env = getenv ("RTS2_HTTPD");
	serverUrl = env ? env : "http://localhost:8889";
	rawJson = false;
	timeout = 10;
	fetch = nullptr;

	addOption ('s', "server", 1, "rts2-httpd base URL (default http://localhost:8889, or $RTS2_HTTPD)");
	addOption ('u', "user", 1, "HTTP basic credentials as user[:password]; the password is asked for if not given");
	addOption ('j', "json", 0, "print the raw JSON response instead of formatted output");
	addOption (OPT_TIMEOUT, "timeout", 1, "HTTP timeout in seconds (default 10)");
}

JsonClient::~JsonClient ()
{
	delete fetch;
}

int JsonClient::processOption (int in_opt)
{
	switch (in_opt)
	{
		case 's':
			serverUrl = optarg;
			break;
		case 'u':
			userPass = optarg;
			break;
		case 'j':
			rawJson = true;
			break;
		case OPT_TIMEOUT:
			timeout = atol (optarg);
			if (timeout < 1)
			{
				std::cerr << "timeout must be a positive number of seconds" << std::endl;
				return -1;
			}
			break;
		default:
			return rts2core::CliApp::processOption (in_opt);
	}
	return 0;
}

int JsonClient::processArgs (const char *arg)
{
	args.push_back (std::string (arg));
	return 0;
}

int JsonClient::init ()
{
	int ret = rts2core::CliApp::init ();
	if (ret)
		return ret;

	if (args.empty ())
	{
		std::cerr << "no command given" << std::endl;
		help ();
		return -1;
	}

	// A password on the command line ends up in the shell history and in
	// every ps listing on the machine, so -u alone (no colon) asks for it
	// on the terminal instead - App::askForPassword() turns echoing off.
	if (!userPass.empty () && userPass.find (':') == std::string::npos)
	{
		std::string password;
		if (askForPassword ("password", password))
			return -1;
		userPass += ":" + password;
	}

	try
	{
		fetch = new HttpFetch (serverUrl);
	}
	catch (rts2core::Error &er)
	{
		std::cerr << getAppName () << ": " << er.what () << std::endl;
		return -1;
	}
	fetch->setAuth (userPass);
	fetch->setTimeout (timeout);

	return 0;
}

void JsonClient::usage ()
{
	std::cout << "  " << getAppName () << " [options] <command> [arguments]" << std::endl
		<< std::endl
		<< "Commands:" << std::endl
		<< "  devices                       list connected devices" << std::endl
		<< "  getall                        every value of every device" << std::endl
		<< "  get <device> [value]          all values of a device, or one named value" << std::endl
		<< "  set <device> <value> <new>    set a value" << std::endl
		<< "  inc <device> <value> <by>     add to a value" << std::endl
		<< "  dec <device> <value> <by>     subtract from a value" << std::endl
		<< "  selval <device> <value>       option names of a selection value" << std::endl
		<< "  switchstate on|standby|off    switch the whole system state" << std::endl
		<< "  messages                      recent messages held by the daemon" << std::endl
		<< "  horizon                       observatory position and horizon" << std::endl
		<< "  db <endpoint> [key=value..]   call an /api/db/ endpoint (targets, night, images, ...)" << std::endl
		<< "  api <path> [key=value..]      call any endpoint by path" << std::endl
		<< std::endl
		<< "Writing commands print nothing on success - the daemon queues the change and" << std::endl
		<< "answers before the device acknowledges it, so there is no new value to report." << std::endl
		<< std::endl
		<< "Examples:" << std::endl
		<< "  " << getAppName () << " devices" << std::endl
		<< "  " << getAppName () << " get C0 EXPOSURE" << std::endl
		<< "  " << getAppName () << " -u petr set C0 EXPOSURE 10" << std::endl
		<< "  " << getAppName () << " -s http://observatory:8889 getall" << std::endl
		<< "  " << getAppName () << " db targets name=M31" << std::endl;
}

JsonPtr JsonClient::request (const std::string &path, const HttpParams &params)
{
	long httpCode = 0;
	if (getDebug ())
		std::cerr << "GET " << fetch->buildUrl (path, params) << std::endl;

	std::string body = fetch->get (path, params, httpCode);

	JsonPtr response;
	try
	{
		response = JsonValue::parse (body);
	}
	catch (rts2core::Error &er)
	{
		// Anything that isn't JSON came from something other than the
		// API - a reverse proxy's error page, or a plain web server on
		// the port. Say what was actually received rather than just
		// "invalid JSON".
		std::ostringstream os;
		os << "response from " << serverUrl << path << " is not JSON (HTTP " << httpCode << "): " << er.what ();
		throw rts2core::Error (os.str ());
	}

	// 401 first: its body is a {"error":"authentication required"} like
	// any other, but "pass -u" is the useful thing to say about it.
	if (httpCode == 401)
		throw rts2core::Error ("authentication required - pass credentials with -u user[:password]");

	const JsonValue *error = response->get ("error");
	if (error != nullptr)
		throw rts2core::Error (error->text ());
	if (httpCode != 200)
	{
		std::ostringstream os;
		os << "HTTP " << httpCode << " from " << serverUrl << path;
		throw rts2core::Error (os.str ());
	}

	return response;
}

bool JsonClient::dumpJson (JsonPtr response)
{
	if (!rawJson)
		return false;
	response->print (std::cout);
	std::cout << std::endl;
	return true;
}

void JsonClient::printValues (const JsonValue *values, const std::string &prefix)
{
	if (!values->isObject ())
	{
		std::cout << values->text () << std::endl;
		return;
	}

	size_t width = 0;
	for (size_t i = 0; i < values->members ().size (); i++)
	{
		size_t len = prefix.length () + values->members ()[i].first.length ();
		if (len > width)
			width = len;
	}

	for (size_t i = 0; i < values->members ().size (); i++)
	{
		std::string name = prefix + values->members ()[i].first;
		std::cout << std::left << std::setw ((int) width) << name << " = " << values->members ()[i].second->text () << std::endl;
	}
}

HttpParams JsonClient::argParams (size_t from)
{
	HttpParams params;
	for (size_t i = from; i < args.size (); i++)
	{
		size_t eq = args[i].find ('=');
		if (eq == std::string::npos)
			throw rts2core::Error ("endpoint arguments must be key=value pairs, got: " + args[i]);
		params.push_back (std::pair <std::string, std::string> (args[i].substr (0, eq), args[i].substr (eq + 1)));
	}
	return params;
}

int JsonClient::runCommand ()
{
	const std::string &command = args[0];

	if (command == "devices")
	{
		JsonPtr response = request ("/api/devices");
		if (dumpJson (response))
			return 0;
		for (size_t i = 0; i < response->size (); i++)
		{
			// A connection that hasn't sent its name yet has an empty
			// one, and /api/devices (unlike /api/getall, which skips
			// them) reports it as "" - printing a blank line for it is
			// noise in a device listing, and the connection isn't
			// addressable by any other command until it is named
			// anyway.
			std::string name = response->at (i)->text ();
			if (name.empty ())
				continue;
			std::cout << name << std::endl;
		}
		return 0;
	}
	else if (command == "getall")
	{
		JsonPtr response = request ("/api/getall");
		if (dumpJson (response))
			return 0;
		for (size_t i = 0; i < response->members ().size (); i++)
			printValues (response->members ()[i].second.get (), response->members ()[i].first + ".");
		return 0;
	}
	else if (command == "get")
	{
		if (args.size () < 2 || args.size () > 3)
			throw rts2core::Error ("get takes a device name and an optional value name");
		HttpParams params;
		params.push_back (std::pair <std::string, std::string> ("d", args[1]));
		if (args.size () == 3)
			params.push_back (std::pair <std::string, std::string> ("n", args[2]));

		JsonPtr response = request ("/api/get", params);
		if (dumpJson (response))
			return 0;
		// A single named value prints bare, with no name and no padding -
		// this is the form that ends up inside `$(...)` in a shell script.
		if (args.size () == 3)
		{
			const JsonValue *value = response->get (args[2].c_str ());
			if (value == nullptr)
				throw rts2core::Error ("device did not report value " + args[2]);
			std::cout << value->text () << std::endl;
		}
		else
		{
			printValues (response.get (), "");
		}
		return 0;
	}
	else if (command == "set" || command == "inc" || command == "dec")
	{
		if (args.size () != 4)
			throw rts2core::Error (command + " takes a device name, a value name and a value");
		HttpParams params;
		params.push_back (std::pair <std::string, std::string> ("d", args[1]));
		params.push_back (std::pair <std::string, std::string> ("n", args[2]));
		params.push_back (std::pair <std::string, std::string> ("v", args[3]));

		JsonPtr response = request ("/api/" + command, params);
		if (dumpJson (response))
			return 0;
		return 0;
	}
	else if (command == "selval")
	{
		if (args.size () != 3)
			throw rts2core::Error ("selval takes a device name and a value name");
		HttpParams params;
		params.push_back (std::pair <std::string, std::string> ("d", args[1]));
		params.push_back (std::pair <std::string, std::string> ("n", args[2]));

		JsonPtr response = request ("/api/selval", params);
		if (dumpJson (response))
			return 0;
		// Printed with the index the value itself holds (jsonValue()
		// sends a selection as its raw numeric index), so the listing can
		// be read against a `get` of the same value.
		for (size_t i = 0; i < response->size (); i++)
			std::cout << std::setw (3) << i << "  " << response->at (i)->text () << std::endl;
		return 0;
	}
	else if (command == "switchstate")
	{
		if (args.size () != 2)
			throw rts2core::Error ("switchstate takes one of on, standby, off");
		HttpParams params;
		params.push_back (std::pair <std::string, std::string> ("state", args[1]));

		JsonPtr response = request ("/api/switchstate", params);
		if (dumpJson (response))
			return 0;
		return 0;
	}
	else if (command == "messages")
	{
		JsonPtr response = request ("/api/messages");
		if (dumpJson (response))
			return 0;
		for (size_t i = 0; i < response->size (); i++)
		{
			const JsonValue *message = response->at (i);
			const JsonValue *time = message->get ("time");
			const JsonValue *device = message->get ("device");
			const JsonValue *type = message->get ("type");
			const JsonValue *text = message->get ("text");
			if (time == nullptr || device == nullptr || type == nullptr || text == nullptr)
				continue;

			// The daemon sends the raw messageType_t bitmask; only its
			// severity bits are meaningful for a listing (the ID bits
			// carry which observation/mount event it was - see
			// message.h's INFO_OBSERVATION_* / INFO_MOUNT_*).
			const char *severity = "unknown";
			switch (((unsigned int) type->asDouble ()) & MESSAGE_LEVEL_MASK)
			{
				case MESSAGE_ERROR:
					severity = "error";
					break;
				case MESSAGE_WARNING:
					severity = "warning";
					break;
				case MESSAGE_INFO:
					severity = "info";
					break;
				case MESSAGE_DEBUG:
					severity = "debug";
					break;
			}

			// std::right before the timestamp is not cosmetic: the
			// std::left set for the device column below stays set on
			// the stream, and Timestamp's operator << fills with '0'
			// and honours the current adjustfield - left-aligned, it
			// pads each field on the *right*, printing August as
			// "2026-80-31" on every line after the first.
			std::cout << std::right << Timestamp (time->asDouble ()) << " "
				<< std::left << std::setw (10) << device->text ()
				<< " " << std::setw (7) << severity << " " << text->text () << std::endl;
		}
		return 0;
	}
	else if (command == "horizon")
	{
		JsonPtr response = request ("/api/horizon");
		if (dumpJson (response))
			return 0;
		const JsonValue *lat = response->get ("lat");
		const JsonValue *lon = response->get ("lon");
		const JsonValue *alt = response->get ("alt");
		if (lat && lon && alt)
			std::cout << "observer  lat " << lat->text () << "  lon " << lon->text () << "  altitude " << alt->text () << " m" << std::endl;
		const JsonValue *horizon = response->get ("horizon");
		if (horizon)
		{
			for (size_t i = 0; i < horizon->size (); i++)
			{
				const JsonValue *point = horizon->at (i);
				if (point->size () == 2)
					std::cout << "horizon   az " << std::setw (10) << point->at (0)->text () << "  alt " << point->at (1)->text () << std::endl;
			}
		}
		return 0;
	}
	else if (command == "db" || command == "api")
	{
		if (args.size () < 2)
			throw rts2core::Error (command + " takes an endpoint name, then key=value arguments");
		std::string path;
		if (command == "db")
		{
			path = "/api/db/" + args[1];
		}
		else
		{
			path = args[1];
			if (path.empty () || path[0] != '/')
				path = "/" + path;
		}

		// No dedicated formatter: these two are the escape hatch for
		// every endpoint this client doesn't know about by name (and the
		// DB endpoints' responses are nested target/observation records,
		// which indented JSON shows better than any flat key = value
		// rendering could).
		JsonPtr response = request (path, argParams (2));
		response->print (std::cout);
		std::cout << std::endl;
		return 0;
	}

	throw rts2core::Error ("unknown command: " + command);
}

int JsonClient::doProcessing ()
{
	try
	{
		return runCommand ();
	}
	catch (rts2core::Error &er)
	{
		std::cerr << getAppName () << ": " << er.what () << std::endl;
		return -1;
	}
}

int main (int argc, char **argv)
{
	// Explicit global init (rather than relying on curl_easy_init() to do
	// it implicitly) - the implicit path is documented as not thread-safe,
	// and doing it here costs one line.
	curl_global_init (CURL_GLOBAL_DEFAULT);
	JsonClient app (argc, argv);
	int ret = app.run ();
	curl_global_cleanup ();
	return ret == 0 ? 0 : 1;
}
