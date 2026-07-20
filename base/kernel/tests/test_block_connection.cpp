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

int main ()
{
	char prog[] = "test_block_connection";
	char *argv[] = { prog };
	TestBlock block (1, argv);

	// Connection needs a real Block* - exercise the constructor and basic
	// accessors end to end.
	rts2core::Connection conn (&block);
	assert (conn.getMaster () == &block);
	assert (conn.getConnState () == CONN_UNKNOW);
	conn.setName (0, "testdev");
	assert (strcmp (conn.getName (), "testdev") == 0);

	block.addConnection (&conn);
	assert (block.connectionSize () == 0); // added connections move over on idle()
	block.callIdle ();
	assert (block.connectionSize () == 1);
	assert (block.findName ("testdev") == &conn);

	// Value + ValueVector, exercised through Connection's value list.
	rts2core::ValueInteger *v = new rts2core::ValueInteger ("counter");
	v->setValueInteger (42);
	assert (v->getValueInteger () == 42);
	assert (strcmp (v->getValue (), "42") == 0);

	rts2core::ValueVector::iterator end = conn.valueEnd ();
	conn.addValue (v, end);
	assert (conn.valueSize () == 1);
	assert (conn.getValue ("counter") == v);
	assert (conn.getValueInteger ("counter") == 42);

	// remove before TestBlock's destructor also tries to delete it via
	// connections_added/connections cleanup
	block.removeConnection (&conn);

	return 0;
}
