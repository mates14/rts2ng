/*
 * Send a single raw command to a named RTS2 device and exit.
 *
 * New (not a classic port): there is no Python queuer or rts2-httpd in this
 * tree yet, so there was no scriptable way to drive rts2-executor's wire
 * commands ("now", "queue", "next", ...) for a live smoke test. rts2-mon
 * can send them interactively, but nothing non-interactive existed. This
 * is a minimal rts2core::Client subclass (modeled on how rts2-scriptexec
 * and rts2-mon already use Client/ConnClient/Command) that connects,
 * waits for the named device to become ready, sends one command, prints
 * the result, and exits.
 */

#include "client.h"
#include "command.h"
#include "connection.h"

#include <iostream>
#include <sstream>
#include <ctime>

namespace
{

class SendCommand : public rts2core::Command
{
	public:
		SendCommand (rts2core::Block *_owner, const char *_text, bool *_done, bool *_ok):
			rts2core::Command (_owner, _text)
		{
			done = _done;
			ok = _ok;
		}

		virtual int commandReturnOK (rts2core::Connection *conn)
		{
			*ok = true;
			*done = true;
			return -1;
		}

		virtual int commandReturnFailed (int status, rts2core::Connection *conn)
		{
			std::cerr << "command failed, status " << status << std::endl;
			*ok = false;
			*done = true;
			return -1;
		}

	private:
		bool *done;
		bool *ok;
};

}

class SendCmd : public rts2core::Client
{
	public:
		SendCmd (int in_argc, char **in_argv):rts2core::Client (in_argc, in_argv, "sendcmd")
		{
			targetDevice = NULL;
			sent = false;
			done = false;
			ok = false;
			startTime = time (NULL);
			doneTime = 0;
		}

		virtual int processArgs (const char *arg)
		{
			if (!targetDevice)
			{
				targetDevice = arg;
			}
			else
			{
				if (!cmdText.empty ())
					cmdText += " ";
				cmdText += arg;
			}
			return 0;
		}

		virtual void deviceReady (rts2core::Connection *conn)
		{
			if (sent || !targetDevice)
				return;
			if (conn->isName (targetDevice))
			{
				std::cerr << "device '" << targetDevice << "' ready, sending: " << cmdText << std::endl;
				conn->queCommand (new SendCommand (this, cmdText.c_str (), &done, &ok));
				sent = true;
			}
		}

		virtual int idle ()
		{
			if (done && doneTime == 0)
				doneTime = time (NULL);
			if (doneTime != 0 && time (NULL) > doneTime)
				setEndLoop (true);
			if (!sent && time (NULL) - startTime > 20)
			{
				std::cerr << "timeout waiting for device '" << (targetDevice ? targetDevice : "?") << "' to become ready" << std::endl;
				ok = false;
				setEndLoop (true);
			}
			return rts2core::Client::idle ();
		}

		bool wasOk () { return ok; }

	private:
		const char *targetDevice;
		std::string cmdText;
		bool sent;
		bool done;
		bool ok;
		time_t startTime;
		time_t doneTime;
};

int main (int argc, char **argv)
{
	SendCmd app = SendCmd (argc, argv);
	int ret = app.run ();
	if (ret)
		return ret;
	return app.wasOk () ? 0 : 1;
}
