// Compile-only: value.h must parse standalone (Connection is only
// forward-declared). value.cpp itself isn't linkable until task 5 ports
// Connection - see the note at the top of value.h.
#include "value.h"

int main ()
{
	return 0;
}
