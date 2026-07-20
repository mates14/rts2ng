#include "hoststring.h"

#include <cassert>
#include <cstring>

int main ()
{
	HostString withPort ("camera.example.org:1234");
	assert (strcmp (withPort.getHostname (), "camera.example.org") == 0);
	assert (withPort.getPort () == 1234);

	HostString defaultPort ("camera.example.org");
	assert (strcmp (defaultPort.getHostname (), "camera.example.org") == 0);
	assert (defaultPort.getPort () == 617);

	HostString explicitDefault ("camera.example.org", "8080");
	assert (explicitDefault.getPort () == 8080);

	return 0;
}
