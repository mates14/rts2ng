#include "userauth.h"

#include "error.h"
#include "utilsfunc.h"

#include <crypt.h>
#include <fstream>
#include <sstream>

using namespace rts2web;

void UserPermissions::parsePermissions (const char *permissionString)
{
	allowedDevices.clear ();
	if (permissionString == nullptr)
		return;
	allowedDevices = SplitStr (std::string (permissionString), " ");
}

bool UserPermissions::canWriteDevice (const std::string &deviceName) const
{
	for (std::vector <std::string>::const_iterator iter = allowedDevices.begin (); iter != allowedDevices.end (); iter++)
	{
		size_t star = iter->find ('*');
		if (star == 0)
			return true;
		else if (star != std::string::npos)
		{
			if (iter->substr (0, star) == deviceName.substr (0, star))
				return true;
		}
		else if (*iter == deviceName)
		{
			return true;
		}
	}
	return false;
}

void UserLogins::load (const std::string &filename)
{
	logins.clear ();

	std::ifstream ifs (filename);
	if (!ifs.good ())
		return;						 // missing/unopenable file - see header comment

	int ln = 0;
	std::string line;
	while (std::getline (ifs, line))
	{
		ln++;
		if (line.empty ())
			continue;

		std::vector <std::string> fields = SplitStr (line, ":");
		if (fields.size () != 2 && fields.size () != 3)
		{
			std::ostringstream os;
			os << "invalid line in auth file " << filename << " on line " << ln << ": expected 2 or 3 fields separated by ':', got " << fields.size ();
			throw rts2core::Error (os.str ());
		}

		logins[fields[0]] = std::pair <std::string, std::string> (fields[1], fields.size () == 3 ? fields[2] : "");
	}
}

bool UserLogins::verifyUser (const std::string &username, const std::string &pass, UserPermissions *permissions) const
{
	std::map <std::string, std::pair <std::string, std::string>>::const_iterator iter = logins.find (username);
	if (iter == logins.end ())
		return false;

	const std::string &cryptedPass = iter->second.first;

	// crypt() re-derives the salt from the stored hash itself (the salt
	// is embedded in cryptedPass, e.g. "$6$<salt>$<hash>"), so this
	// compares against whatever the stored hash actually used. This
	// system's crypt.h is libxcrypt's, whose struct crypt_data has no
	// "initialized" flag to reset (unlike old glibc's) - zero-
	// initializing the whole struct is simpler and still safe.
	struct crypt_data data = {};
	char *result = crypt_r (pass.c_str (), cryptedPass.c_str (), &data);
	bool ok = result != nullptr && cryptedPass == std::string (result);

	if (ok && permissions)
		permissions->parsePermissions (iter->second.second.c_str ());

	return ok;
}
