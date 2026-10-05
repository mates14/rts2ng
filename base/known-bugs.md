# Known bugs in rts2ng

Bugs found in rts2ng itself (regressions of the port, or problems seen in
deployment) that are not fixed yet. Bugs inherited from classic RTS2 go to
`UPSTREAM_BUGS.md`.

---

## FIXED: Clients started without a login session cannot log in to centrald

**Fixed:** 2026-10-05 - `Client::Client ()` takes the effective user
(`getpwuid (geteuid ())`) as classic did.

**Found:** 2026-10-05 on makak (morpheus), from centrald log noise
**File:** `kernel/src/client.cpp:144` (`Client::Client ()`)
**Severity:** high - the observing loop at makak cannot command any device

**Symptom.** centrald logs, every time a device (re)registers:

```
rts2[<centrald>]: Connection::command unknow command: getCommand key state: 4 type: 0 name:
rts2[<centrald>]: command end with error -1 description: unknow command key
```

and once, when the client connects:

```
rts2[<centrald>]: command end with error -2 description: invalid parameters/invalid number of parameters - login
```

**Cause.** The client constructor takes the login name from `getlogin ()`:

```cpp
	login = getlogin();
	password = login;
```

`getlogin ()` returns the user of the *login session* (glibc reads
`/proc/self/loginuid`), and returns NULL for processes outside one -
systemd services, cron jobs, anything below `systemd-run`. Then
`CommandLogin` does `_os << "login " << login << " " << name;` with a NULL
`const char *`, which puts the stream into the bad state and drops the rest:
centrald receives `login ` and rejects it with -2. The connection stays
`NOT_DEFINED_SERVER` (type 0, no name), so whenever the client later asks
centrald for the key of a device it wants to talk to (`key <device>`,
`ConnClient::connConnected ()`), centrald's `ConnCentrald::command ()` falls
through to `Connection::command ()` - "unknow command key". The client's
connection to the device stays in `CONN_AUTH_PENDING` and nothing it queues
there is ever sent.

The rts2ng port introduced this: classic `lib/rts2/client.cpp` has
`login = cuserid(NULL);` (with `getlogin ()` commented out above it), and
`cuserid` uses the effective uid, so it always gives a name.

**Impact.** Every client run from systemd. At makak this is
`makak-acquire.service` -> `rts2-scriptexec -c C0 -s 'exe kseq-expose.sh'`
(loginuid 4294967295): it cannot command C0 or SHUTTER, so makak does not
observe. Daemons (devices) are not affected - they register, they do not log
in. Scripts that a `ssh` session runs work, which hides the bug in manual
testing.

**Reproduce** (any host with rts2ng running):

```
cat /proc/$(pidof rts2-scriptexec)/loginuid        # 4294967295 = no session
systemd-run --quiet --pipe --wait rts2-sendcmd SHUTTER info
  -> timeout waiting for device 'SHUTTER' to become ready
rts2-sendcmd SHUTTER info                          # from an ssh shell
  -> device 'SHUTTER' ready, sending: info
```

(verified on morpheus 2026-10-05; centrald logs the empty `login` for the
systemd-run case.)

Not a startup-ordering problem: makak-acquire already has
`After=rts2.service`, and the same failure happens with centrald up long
before the client starts.

**Fix (proposed).** Take the name from the effective uid, as classic did,
without the obsolete `cuserid`:

```cpp
	struct passwd *pw = getpwuid (geteuid ());
	login = pw ? pw->pw_name : "rts2";
```

(`login` is a `const char *`; `pw_name` points into static storage that a
later `getpw*` call overwrites, so copy it into a `std::string` member if
anything else in the process uses `getpw*`.) Possibly also make
`CommandLogin` refuse an empty login loudly instead of sending `login `.

---

## `exec_device` in rts2-scriptexec casts a Client to a Device

**Found:** 2026-10-05, writing the makak observing script in Python
**File:** `script/src/connexe.cpp` (`ConnExe::processCommand ()`, `exec_device`)
**Severity:** undefined behaviour; harmless-looking in practice so far

```cpp
	else if (!strcmp (cmd, "exec_device"))
	{
		writeToProcess (((rts2core::Device *) master)->getDeviceName () ? ((rts2core::Device *) master)->getDeviceName () : "None");
	}
```

`master` is whatever runs the exe script. In rts2-executor that is a
`Device`, but in rts2-scriptexec it is a `Client` (`ScriptExec: public
rts2core::Client`), and the C-style cast reads the `Device` member layout
from an object that does not have it. Locally it answered an empty line;
it could as well crash or print garbage. `rts2.scriptcomm.Rts2Comm`
sends `exec_device` from every `setValue`/`getValue` that names a device,
so any Python script under rts2-scriptexec hits it.

**Fix (proposed):** `dynamic_cast<rts2core::Device *> (master)` and answer
`None` when it is not a device. Until then `python/sites/makak/
kseq-expose.py` overrides `getExecDevice ()`.
