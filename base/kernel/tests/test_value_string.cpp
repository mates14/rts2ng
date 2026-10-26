// A string value is sent as "..." with no escaping. A value that itself
// contains quotes (a script with exe "/path/x.py -n 8") must still arrive
// whole: SBT's cameras refused it with -2, and SCRIPT was never shown.

#include "block.h"
#include "value.h"

#include <cassert>
#include <cstring>

class TestBlock : public rts2core::Block
{
	public:
		TestBlock (int argc, char **argv) : Block (argc, argv) {}
		virtual int run () { return 0; }

	protected:
		virtual rts2core::Connection *createClientConnection (rts2core::NetworkAddress *) { return nullptr; }
};

class LineConnection : public rts2core::Connection
{
	public:
		LineConnection (rts2core::Block *_master) : Connection (_master) {}

		// what is left on the line after "X SCRIPT =" or "V SCRIPT"
		int set (rts2core::ValueString *v, const char *params)
		{
			strncpy (line, params, sizeof (line) - 1);
			line[sizeof (line) - 1] = '\0';
			command_buf_top = line;
			return v->setValue (this);
		}

	private:
		char line[200];
};

int main ()
{
	char prog[] = "test_value_string";
	char *argv[] = { prog };
	TestBlock block (1, argv);
	LineConnection conn (&block);
	rts2core::ValueString v ("SCRIPT");

	assert (conn.set (&v, "\"filter=r E 10\"") == 0);
	assert (v.getValueString () == "filter=r E 10");

	assert (conn.set (&v, "\"exe \"/home/mates/filtros.py -n 8 -e 60\"\"") == 0);
	assert (v.getValueString () == "exe \"/home/mates/filtros.py -n 8 -e 60\"");

	assert (conn.set (&v, "\"E 1 exe \"/bin/x\" E 1\"  ") == 0);
	assert (v.getValueString () == "E 1 exe \"/bin/x\" E 1");

	assert (conn.set (&v, "\"\"") == 0);
	assert (v.getValueString () == "");

	// unquoted single word, as before
	assert (conn.set (&v, "word") == 0);
	assert (v.getValueString () == "word");

	// malformed lines are still refused, and leave the value alone
	assert (conn.set (&v, "\"unterminated") == -2);
	assert (conn.set (&v, "\"") == -2);
	assert (conn.set (&v, "two words") == -2);
	assert (v.getValueString () == "word");

	return 0;
}
