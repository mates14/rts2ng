#pragma once

#include <list>
#include <string>

namespace rts2db
{

class CamList:public std::list < std::string >
{
	public:
		CamList ();
		virtual ~CamList ();

		int load ();
};

}
