#include "app.h"

#include <cassert>
#include <cstring>

class TestApp : public rts2core::App
{
	public:
		TestApp (int argc, char **argv) : App (argc, argv) {}
		virtual int run () { return init (); }
};

int main ()
{
	char prog[] = "test_app";
	char debugFlag[] = "--debug";
	char *argv[] = { prog, debugFlag };

	TestApp app (2, argv);

	// a real App now exists - getMasterApp() must return it, not nullptr
	assert (getMasterApp () == &app);

	assert (app.run () == 0);
	assert (app.getDebug () == 1);

	// logStream()/sendLog must not crash with a real App registered
	logStream (MESSAGE_DEBUG) << "hello from test_app" << sendLog;

	assert (app.usesLocalTime () == true);

	return 0;
}
