#pragma once

#include <map>
#include <string>
#include <vector>

namespace rts2web
{

/**
 * Which devices a logged-in user may write to. Ported from classic
 * lib/rts2/userpermissions.cpp (rts2core::UserPermissions) - the
 * permission model is unchanged, just renamed into rts2web and modernized
 * (nullptr, const-correct accessors).
 *
 * Deliberately narrow: this is device-write granularity only ("can this
 * user write to device X"), matching what STATUS.md task 2's /api/set,
 * /api/inc, /api/dec actually need to gate. Classic's DB-bound
 * PERMISSIONS_TARGET_* constants (target enable/slew/next/scriptedit)
 * are a different, DB-bound permission axis entirely - not relevant
 * until task 7's target/observation endpoints exist.
 */
class UserPermissions
{
	public:
		UserPermissions () {}

		/** permissionString is a space-separated device-name list; "*"
		 * (or a "prefix*" wildcard) matches "all"/"all matching prefix". */
		void parsePermissions (const char *permissionString);

		bool canWriteDevice (const std::string &deviceName) const;

	private:
		std::vector <std::string> allowedDevices;
};

/**
 * Local (non-DB) credential store (STATUS.md task 6). Ported from
 * classic lib/rts2/userlogins.cpp (rts2core::UserLogins) - the exact
 * mechanism classic's own non-PGSQL httpd build already used for
 * authentication, proof this approach genuinely works without a
 * database rather than a new invention for this port.
 *
 * File format: one user per line, "username:cryptedpassword:permissions"
 * (permissions parsed by UserPermissions above). Passwords are
 * crypt(3)-hashed (glibc's SHA-512 "$6$" scheme, e.g. as produced by
 * `openssl passwd -6` or `mkpasswd -m sha-512`). Classic's
 * `#ifdef RTS2_HAVE_CRYPT` plaintext fallback for crypt()-less systems
 * is dropped - consistent with base's "modern Linux only" convention,
 * crypt() is always available here.
 *
 * Deliberately independent of WEB_WITH_DB: this is how *every* build
 * variant authenticates, not just the no-DB one. A database has never
 * actually been required for authentication - classic's own non-PGSQL
 * build already proved that - so DB presence only gates DB-bound
 * endpoints (task 7+), not whether the daemon can authenticate anyone at
 * all.
 *
 * User-management tooling (a `rts2-web-passwd`-style CLI, mirroring
 * classic's `rts2-usernondb`) is deliberately not built yet - the file
 * format is simple enough to hand-edit for now (one line per user, a
 * crypt(3) hash from any standard tool), and building a management tool
 * before anyone needs one beyond hand-editing isn't worth it yet.
 */
class UserLogins
{
	public:
		UserLogins () {}

		/**
		 * Load (replacing any previously loaded entries) from filename.
		 * A missing/unopenable file is treated as "no users configured"
		 * (a config problem for the operator to notice via the 401s
		 * everyone gets, not a reason to crash) - matches
		 * checkPreviewCache()'s/generatePreview()'s "don't crash on an
		 * unexpected shape" principle, extended to config parsing.
		 *
		 * @throws rts2core::Error if the file exists and is readable but
		 *         contains a malformed line - unlike a missing file,
		 *         this is treated as a real, operator-fixable startup
		 *         error (see HttpD::init() in httpd.cpp): better to
		 *         refuse to start than silently run with a truncated or
		 *         wrong credential set for a security-relevant file.
		 */
		void load (const std::string &filename);

		bool verifyUser (const std::string &username, const std::string &pass, UserPermissions *permissions = nullptr) const;

	private:
		// username -> (crypted password, permissions string)
		std::map <std::string, std::pair <std::string, std::string>> logins;
};

}
