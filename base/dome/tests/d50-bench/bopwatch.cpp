// D50 dome bench helper: print the state and full BOP state of every device
// whenever it changes (no rts2ng tool shows BOP bits)
#include "client.h"
#include "connection.h"
#include "status.h"

#include <iomanip>
#include <iostream>
#include <map>
#include <sys/time.h>

class BopWatch:public rts2core::Client
{
	public:
		BopWatch (int argc, char **argv):rts2core::Client (argc, argv, "bopwatch") {}

		virtual int idle ()
		{
			for (rts2core::connections_t::iterator iter = getConnections ()->begin (); iter != getConnections ()->end (); iter++)
			{
				rts2core::Connection *c = *iter;
				if (c->getName ()[0] == '\0' || c->getOtherType () <= 1)
					continue;
				std::pair <rts2_status_t, rts2_status_t> now (c->getState (), c->getFullBopState ());
				std::map <std::string, std::pair <rts2_status_t, rts2_status_t> >::iterator l = last.find (c->getName ());
				if (l == last.end () || l->second != now)
				{
					struct timeval tv;
					gettimeofday (&tv, nullptr);
					rts2_status_t bop = now.first | now.second;
					std::cout << tv.tv_sec << "." << std::setw (3) << std::setfill ('0') << tv.tv_usec / 1000 << std::setfill (' ') << " " << c->getName () << " state 0x" << std::hex << now.first << " bop 0x" << now.second << std::dec
						<< ((bop & BOP_EXPOSURE) ? " BLOCK_EXPOSURE" : "")
						<< ((bop & BOP_TEL_MOVE) ? " BLOCK_TEL_MOVE" : "") << std::endl;
					last[c->getName ()] = now;
				}
			}
			return rts2core::Client::idle ();
		}

	private:
		std::map <std::string, std::pair <rts2_status_t, rts2_status_t> > last;
};

int main (int argc, char **argv)
{
	BopWatch app (argc, argv);
	return app.run ();
}
