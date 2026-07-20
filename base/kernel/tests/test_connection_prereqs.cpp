// Compile-only: these headers must parse standalone ahead of Connection
// itself (task 5). Connection is only forward-declared in data.h/valuelist
// pulls in value.h+app.h which are already linkable.
#include "event.h"
#include "object.h"
#include "serverstate.h"
#include "valuelist.h"
#include "data.h"
#include "centralstate.h"
#include "valuestat.h"
#include "valueminmax.h"
#include "valuerectangle.h"
#include "valuearray.h"

int main ()
{
	rts2core::Event ev (1, nullptr);
	rts2core::ServerState st;
	rts2core::ValueVector vv;
	(void) ev;
	(void) st;
	(void) vv;
	return 0;
}
