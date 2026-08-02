# Bugs found in the classic RTS2 tree during the base port

Working notes only cover the base side of things in `STATUS.md`. This file is
the one meant for actually reporting upstream - each entry names the affected
classic-tree file(s), the maintainer(s) per the file's copyright header, what's
wrong, how to reproduce it, and how base fixed it (which should translate
directly to a patch against the classic tree).

None of these were found by inspection alone - each was hit while actually
compiling, linking, and running the ported code (or, in a couple of cases,
while porting `unique_ptr`-style ownership and re-deriving what the original
copy/assignment semantics needed to be).

---

## ConnREMOTES::init() / D50::init() - segfault on raw-socket failure

**Files:** `lib/rts2/connremotes.cpp` (`ConnREMOTES::init()`),
`src/teld/d50.cpp` (`D50::init()`)
**Maintainers:** Petr Kubanek, Jan Strobl (both credited on `connremotes.cpp`
and `d50.cpp`)
**Severity:** crash (SIGSEGV), but narrow trigger window

**Bug:** `ConnREMOTES::init()` calls `ConnEthernet::init()` but discards its
return value, unconditionally returning `0`:

```cpp
int ConnREMOTES::init ()
{
	serialNum = (unsigned char) 0;
	ConnEthernet::init ();
	return 0;
}
```

`ConnEthernet::init()` opens a raw `AF_PACKET`/`SOCK_RAW` socket and, only on
success, allocates `ethOutBuffer`/`ethInBuffer` and sets up `ethOutData`/
`ethInData`/`eh`. If `socket()` fails (`EPERM` without `CAP_NET_RAW`, or any
other reason the interface named in the constructor can't be opened for raw
I/O), `init()` returns `-1` early, before those buffers are ever allocated.

`D50::init()` then makes the failure fatal by never checking the result of
`ConnREMOTES::init()` either - unlike the `raDrive`/`decDrive` (`TGDrive`)
connections a few lines below, which *do* check:

```cpp
remotesRA = new rts2core::ConnREMOTES (this, remotesEthRA, remotesMacRA);
remotesRA->setDebug (getDebug ());
remotesRA->init ();                              // return value ignored
remotesMCRegisterRAState = ...;
ret = remotesSetMCRegister (remotesRA, remotesMCRegisterRAState);  // segfaults
```

`remotesSetMCRegister()` immediately calls `ConnREMOTES::write()` ->
`ConnEthernet::writeRead()`, which dereferences `ethOutData`/`ethInBuffer` -
never allocated, since `init()` returned early. Result: SIGSEGV on the very
first register write, immediately after startup.

**Reproduction:** run `rts2-teld-d50`-equivalent code without `CAP_NET_RAW`
(i.e. not as root, and without `setcap cap_net_raw+ep` on the binary). In
production at D50 this path is presumably never exercised, since the daemon
always runs with root/raw-socket privilege there and a real `eth1` interface -
this is why it's gone unnoticed. It would also trigger if the named interface
doesn't exist, is renamed, or `SO_BINDTODEVICE`/`SIOCGIFHWADDR` fails for any
other reason - i.e. this is a real robustness gap, not purely a permissions
edge case.

**Fix applied in base** (`base/teld/src/connremotes.cpp`,
`base/teld/d50/d50.cpp`): `ConnREMOTES::init()` now returns
`ConnEthernet::init()`'s actual result instead of discarding it; `D50::init()`
now checks `remotesRA->init()`/`remotesDec->init()`'s return value and bails
out (same pattern already used for `raDrive`/`decDrive`) instead of continuing
to use a connection that may not have finished initializing.

```diff
 int ConnREMOTES::init ()
 {
 	serialNum = (unsigned char) 0;
-	ConnEthernet::init ();
-
-        return 0;
+	return ConnEthernet::init ();
 }
```

```diff
 	remotesRA = new rts2core::ConnREMOTES (this, remotesEthRA, remotesMacRA);
 	remotesRA->setDebug (getDebug ());
-	remotesRA->init ();
+	ret = remotesRA->init ();
+	if (ret)
+		return ret;
 	remotesMCRegisterRAState = ...;
```
(same pattern for `remotesDec`)

---

## LibnovaRaDec / LibnovaHrz - double-free on copy-assignment

**File:** `include/libnova_cpp.h`
**Maintainer:** Petr Kubanek
**Severity:** crash / memory corruption (double-free), on a very commonly
copied value type

**Bug:** both classes define a copy constructor and a destructor that `delete`
owned raw pointers, but no copy-*assignment* operator. The compiler therefore
generates a default member-wise assignment operator, which shallow-copies the
raw pointers. Any `a = b;` (not construction, actual assignment) leaves both
objects owning the same pointer, and the second destructor to run double-frees
it.

**Fix applied in base** (`base/kernel/include/libnova_cpp.h`): pointers now
owned via `unique_ptr`, plus an explicit deep-copying `operator=`. Covered by
`base/kernel/tests/test_libnova_cpp.cpp`.

---

## IniSection::createBlockedBy - leaks last parsed device name

**File:** `lib/rts2/iniparser.cpp` (`IniSection::createBlockedBy`)
**Maintainer:** Petr Kubanek
**Severity:** memory leak, one allocation per config section parsed without a
trailing space in its `blocked_by` list

**Bug:** the device-name parsing loop allocates a buffer per name and only
frees/stores it when a separating space is found; the last name in the list
(no trailing space before end-of-string) is parsed but never freed or added to
the list.

**Fix applied in base** (`base/kernel/src/iniparser.cpp`): the final
name is now stored the same way as every other one.

---

## LibnovaDegDist operator>> - leaks on the default exit path

**File:** `include/libnova_cpp.h` (`operator>>` for `LibnovaDegDist`)
**Maintainer:** Petr Kubanek
**Severity:** memory leak, stream-parse-failure path only

**Bug:** the `default:` branch of the parse switch returns/exits without
releasing `os`/`is`, which are allocated earlier in the function on every
other path.

**Fix applied in base** (`base/kernel/include/libnova_cpp.h`): the
`default:` path now releases both before returning.

---

## ValueDoubleStat::getDisplayValue - buffer over-read

**File:** `lib/rts2/value.cpp` (`ValueDoubleStat::getDisplayValue`)
**Maintainer:** Petr Kubanek
**Severity:** reads past the end of a `std::string`'s backing buffer; harmless
in practice on most implementations (reads into the string's own capacity
slack or adjacent heap) but is undefined behavior and a real bug under
hardening/ASan

**Bug:** `memcpy (buf, os.str ().c_str (), sizeof (buf))` always copies the
full 200-byte destination size, regardless of how long the actual formatted
string is - reading past the end of `os.str()`'s C-string once the real
content is shorter than 200 bytes.

**Fix applied in base** (`base/kernel/src/value.cpp`): replaced with a
bounded `strncpy` plus explicit null-termination sized to the real string
length.

---

## App::askForBoolean - returns -1 from a bool-returning function

**File:** `lib/rts2/app.cpp` (`App::askForBoolean`)
**Maintainer:** Petr Kubanek
**Severity:** logic bug - on EOF, the function returns `-1`, which the
compiler converts to `true` when it's implicitly narrowed to `bool` at any
call site expecting a yes/no answer. A user hitting EOF (e.g. piping empty
input, or a closed stdin) gets treated as if they answered "yes".

**Fix applied in base** (`base/kernel/src/app.cpp`): EOF now explicitly
`return false;`.

---

## block.cpp - dead `type_names[]` array, and it doesn't even line up

**File:** `lib/rts2/block.cpp`
**Maintainer:** Petr Kubanek
**Severity:** none currently (the array is unused - grep-verified nothing in
the classic tree reads it), but worth knowing about: if anyone ever wires it
up, it's wrong. A missing comma between `"UNKNOWN"` and `"EXEC"` string
literals silently concatenates them into `"UNKNOWNEXEC"`, which shifts every
subsequent entry out of alignment with the `DEVICE_TYPE_*` constants the array
is indexed by.

**Fix applied in base**: array dropped entirely (dead code), not carried
forward. Flagging here mainly so nobody resurrects it as-is if a future
classic-tree change starts reading it.

---

## nut.cpp - uncaught exception crashes the daemon when the NUT UPS is unreachable

**File:** `src/sensord/nut.cpp` (`NUT::initHardware()`)
**Maintainer:** Petr Kubanek
**Severity:** crash (`std::terminate`) on a very ordinary failure mode - the
configured NUT UPS daemon not being reachable (down, wrong host/port,
firewalled, network hiccup)

**Bug:** `initHardware()` calls `connNUT->init()` and only checks its return
value:

```cpp
connNUT = new ConnNUT (this, host->getHostname (), host->getPort (), upsName);
ret = connNUT->init ();
if (ret)
{
	logStream (MESSAGE_ERROR) << "cannot connect to UPS " ... << sendLog;
	return ret;
}
```

But `ConnTCP::init()` (which `ConnNUT` inherits) throws `ConnCreateError` for
most connect failures - including the ordinary "connection refused" case -
rather than returning a nonzero value; it only returns early without throwing
for `ENETUNREACH` after retrying. Since nothing upstream of `initHardware()`
catches `rts2core::ConnError`, an unreachable UPS daemon crashes the whole
process with an unhandled exception (`terminate called after throwing an
instance of 'rts2core::ConnCreateError'`) instead of the clean error log the
code was clearly written to produce.

**Reproduction:** run `rts2-sensord-nut -n someups@127.0.0.1:1 ...` with
nothing listening on that port - immediate crash instead of a logged
connection error.

**Fix applied in base** (`base/sensord/nut/nut.cpp`): wrapped
`connNUT->init()` in a `try`/`catch (rts2core::ConnError &er)` that logs and
returns `-1`, same as every other failure path in this function already does.

```diff
 connNUT = new ConnNUT (this, host->getHostname (), host->getPort (), upsName);
-ret = connNUT->init ();
+try
+{
+	ret = connNUT->init ();
+}
+catch (rts2core::ConnError &er)
+{
+	logStream (MESSAGE_ERROR) << "cannot connect to UPS " ... << ": " << er << sendLog;
+	return -1;
+}
 if (ret)
 {
 	logStream (MESSAGE_ERROR) << "cannot connect to UPS " ... << sendLog;
 	return ret;
 }
```

---

## d50-wfunit.cpp - fan-power readback stores the wrong variable, plus a stack-smashing sscanf

**File:** `src/sensord/d50-wfunit.cpp` (`D50WFUnit::wfunitCommand()`)
**Maintainers:** Petr Kubanek, Martin Jelinek, Jan Strobl (all credited on the
file)
**Severity:** two independent bugs in the same function - one a stack buffer
overflow, one a logic error that makes the `OTAfanPower` readback nonsensical

**Bug 1 (logic):** the parsed reply's shutter-state character is assigned to
the fan-power selection value instead of the parsed fan-state integer:

```cpp
char shutState;
int fanState;
...
sscanf (buf, "%s %u %f %f %f %f %f %f //*", &shutState, &fanState, ...);
...
wfFanPower->setValueInteger(shutState);   // should be fanState
```

`wfFanPower` is a 10-entry selection ("off", "1".."8", "full"); `shutState` is
`'C'` (67) or `'O'` (79), both wildly out of range for that selection, so the
`OTAfanPower` value reported to clients never reflects the unit's actual fan
setting - `fanState` (correctly parsed a few tokens earlier) is silently
discarded.

**Bug 2 (memory safety):** `shutState` is declared as a bare `char`, but the
first `sscanf` conversion is `%s`, which writes the matched token *plus a NUL
terminator* (at least 2 bytes, since the token itself is one character like
"C") into that 1-byte lvalue's address - a stack buffer overflow on every
single call. It happens to not visibly crash on typical stack layouts, but is
undefined behavior and a real bug under hardening/ASan/stack-protector
variance.

**Fix applied in base** (`base/sensord/d50-wfunit/d50-wfunit.cpp`): added a
real `char shutStateBuf[8]` destination for the `%7s` conversion (bounded,
leaves room for the NUL), then `shutState = shutStateBuf[0]`; and changed the
fan-power readback to use `fanState` instead of `shutState`.

```diff
+char shutStateBuf[8];
 char shutState;
 int fanState;
 ...
-int ret = sscanf (buf, "%s %u %f %f %f %f %f %f //*", &shutState, &fanState, ...);
+int ret = sscanf (buf, "%7s %u %f %f %f %f %f %f //*", shutStateBuf, &fanState, ...);
+shutState = shutStateBuf[0];
 ...
-wfFanPower->setValueInteger(shutState);
+wfFanPower->setValueInteger(fanState);
```

---

## Ford::processOption - swallows Dome::processOption()'s return value

**File:** `lib/rts2/domeford.cpp` (`Ford::processOption()`)
**Maintainers:** Petr Kubanek, Martin Nekola (both credited on the file)
**Severity:** logic bug - option-parsing errors from the base class chain
(`Dome`/`Device`/`Daemon`/`App`) are silently discarded instead of aborting
startup

**Bug:** the `default:` branch of the switch calls `Dome::processOption()`
but doesn't return its result, so the function always returns `0` (success)
regardless of what the base class chain actually reported:

```cpp
int
Ford::processOption (int in_opt)
{
	switch (in_opt)
	{
		case 'f':
			dome_file = optarg;
			break;
		default:
			Dome::processOption (in_opt);   // return value discarded
	}
	return 0;
}
```

Any option recognized by `getopt_long` (i.e. registered via `addOption()`
somewhere in the `Dome`/`Device`/`Daemon`/`App` chain) that isn't `-f` gets
routed through this default case; if the base class chain's parsing logic
for that option ever returns `-1` (malformed argument, or any future
options added upstream that signal an error this way), `Ford::processOption`
silently converts it to success instead of letting the app abort with a
clear error before starting the daemon with bad configuration. This is
distinct from `getopt_long` itself rejecting a completely unrecognized
flag (which still works correctly, since that's caught before
`processOption` is ever called) - it's specifically about *recognized*
options whose value parsing fails downstream.

**Fix applied in base** (`base/dome/src/domeford.cpp`): the `default:`
case now returns `Dome::processOption(in_opt)`'s actual result.

```diff
 		default:
-			Dome::processOption (in_opt);
+			return Dome::processOption (in_opt);
 	}
 	return 0;
```

---

## hmstod() - unconditional memory leak on every call

**File:** `lib/rts2lx200/hms.c` (`hmstod()`)
**Maintainers:** Petr Kubanek
**Severity:** resource leak - small, but called on every LX200 telescope
axis/time read, so it accumulates continuously on a running daemon

**Bug:** the function `strdup()`s its input into `locptr`, then
reassigns `locptr` while walking the string on each loop iteration
(`locptr = endptr + 1`). Every one of the function's five `return`
statements exits without ever calling `free()` - and by the time any of
them executes, the pointer originally returned by `strdup()` has already
been overwritten by the reassignment, so there is no way to recover it
to free even if a `free()` call were added at the existing return
points without restructuring:

```c
double
hmstod (const char *hptr)
{
	char *locptr;
	char *endptr;
	...
	if (!(locptr = strdup (hptr)))   // locptr now owns the allocation
		return nan ("f");
	...
	while (*endptr)
	{
		...
		if (!*endptr)
		{
			errno = 0;
			return ret;               // leak: original strdup() pointer never freed
		}
		...
		locptr = endptr + 1;        // original allocation pointer is now gone
	}
	...
}
```

This function is called from `TelLX200::tel_read_hms()` for every RA,
Dec, altitude, azimuth, latitude, longitude, and local/sidereal time
read from an LX200-protocol mount (and from `Gemini::tel_gemini_get_deg()`
directly) - on a running telescope daemon polling position at typical
intervals, this leaks one small allocation per axis read, continuously,
for the lifetime of the process.

**Fix applied in base** (`base/teld/src/hms.cpp`): keep the original
`strdup()` pointer in a separate `buf` variable that is never
reassigned, and `free(buf)` on every return path; `locptr` is still used
for the sliding parse position exactly as before.

```diff
 double hmstod (const char *hptr)
 {
+	char *buf;
 	char *locptr;
 	char *endptr;
 	double ret;
 	double mul;

-	if (!(locptr = strdup (hptr)))
+	if (!(buf = strdup (hptr)))
 		return NAN;

+	locptr = buf;
 	if (*locptr == '-')
 	...
 	while (*endptr)
 	{
 		...
 		if (!*endptr)
 		{
 			errno = 0;
+			free (buf);
 			return ret;
 		}
 		...
 	}
 	errno = 0;
+	free (buf);
 	return ret;
 }
```

---

## Gemini::startResync() - uninitialized reads if neither mount flip is reachable

**File:** `src/teld/gemini.cpp` (`Gemini::startResync()`)
**Maintainers:** Petr Kubanek
**Severity:** undefined behavior (uninitialized read), cosmetic in
practice - only affects a debug log line, not mount movement

**Bug:** `ra_diff`/`ra_diff_flip`/`dec_diff`/`dec_diff_flip` are declared
uninitialized at the top of the function. They are only assigned inside
the `if (flip_possible[i] == true)` branch of the two-iteration flip
-checking loop. If neither flip is reachable (`flip_possible[0]` and
`flip_possible[1]` both false - a real, if unusual, HA-limit
configuration), none of the four ever get assigned before an
unconditional `logStream (MESSAGE_DEBUG)` line a few lines later prints
all four - reading uninitialized stack memory. Caught by
`-Wmaybe-uninitialized` while building the base port.

**Fix applied in base** (`base/teld/gemini/gemini.cpp`): zero-initialize
at declaration. Behavior is unchanged in the normal case (at least one
flip reachable, which is what these variables are for); the
pathological all-flips-blocked case now logs `0` instead of garbage.

```diff
-	double ra_diff, ra_diff_flip;
-	double dec_diff, dec_diff_flip;
+	double ra_diff = 0, ra_diff_flip = 0;
+	double dec_diff = 0, dec_diff_flip = 0;
```

---

## WindRS::openConnection() - leaks the serial connection on every failed connect

**File:** `src/sensord/windrs.cpp` (`WindRS::openConnection()`)
**Maintainers:** Ronan Cunniffe, Petr Kubanek
**Severity:** resource leak - one `ConnSerial` object leaked every 10
seconds while the configured serial device is unavailable, for the
life of the daemon

**Bug:** `openConnection()` allocates a new `ConnSerial` into
`WindRSConn` before calling `init()`. If `init()` fails, the code sets
`WindRSConn = NULL` directly, discarding the only pointer to the
just-allocated object without freeing it:

```cpp
int WindRS::openConnection()
{
	WindRSConn = new rts2core::ConnSerial (devicename, this, rts2core::BS9600, rts2core::C8, rts2core::NONE, 30);
	int ret = WindRSConn->init ();
	if (ret) {
		logStream (MESSAGE_ERROR) << "Failed to open " << devicename << sendLog;
		WindRSConn = NULL;   // leak: the ConnSerial just allocated above is now unreachable
		return ret;
	}
	...
}
```

`getData()` is driven by a 10-second repeating timer
(`EVENT_LOOP`/`addTimer(10, ...)`) and calls `openConnection()` again
every time it finds `WindRSConn == NULL` - so with the sensor's serial
port missing or misconfigured, this leaks one `ConnSerial` object
every 10 seconds indefinitely.

**Fix applied in base** (`base/sensord/windrs/windrs.cpp`): free the
object before discarding the pointer.

```diff
 	int ret = WindRSConn->init ();
 	if (ret) {
 		logStream (MESSAGE_ERROR) << "Failed to open " << devicename << sendLog;
+		delete WindRSConn;
 		WindRSConn = NULL;
 		return ret;
 	}
```

---

## Paramount::saveFlash() - open() with O_CREAT and no mode argument

**File:** `src/teld/paramount.cpp` (`Paramount::saveFlash()`)
**Maintainers:** Petr Kubanek, Martin Jelinek
**Severity:** undefined file permissions on classic glibc; hard compile
error on any glibc built with `_FORTIFY_SOURCE` (which is the default
on most current distributions)

**Bug:** `open()` is called with `O_CREAT` but only two arguments - the
third `mode_t` argument, which controls the permission bits of a newly
created file, is missing entirely:

```cpp
int file = open ("/etc/rts2/flash", O_CREAT | O_TRUNC);
```

On classic (non-fortified) glibc this compiles but reads whatever
garbage happens to be on the stack/in a register as the permission
bits of the newly created `/etc/rts2/flash` file - a real, if minor,
security/correctness bug (the file could end up world-writable or
completely inaccessible depending on what garbage value gets
interpreted as the mode). On a `_FORTIFY_SOURCE`-enabled glibc (the
default on this system and most current distributions), `open()`'s
fortified wrapper detects the missing mode argument at compile time and
refuses to build at all (`__open_missing_mode` declared with
`attribute error`), which is how this was caught while porting.

**Fix applied in base** (`base/teld/paramount/paramount.cpp`): added
an explicit `0666` mode, matching the convention already used for
equivalent `open(..., O_CREAT, ...)` calls elsewhere in this port
(`daemon.cpp`'s lock file, `gxccd.cpp`'s init lock).

```diff
-	int file = open ("/etc/rts2/flash", O_CREAT | O_TRUNC);
+	int file = open ("/etc/rts2/flash", O_CREAT | O_TRUNC, 0666);
```

---

## ObservationSetDate::load() - suspected copy-paste bug comparing d_value2 to itself

**Files:** `lib/rts2db/observationset.ec` (`ObservationSetDate::load()`)
**Maintainer:** Petr Kubanek
**Severity:** low (only affects date-grouped browsing statistics, e.g. web
month/day drill-down image counts - not on the executor's critical path)
**Status:** suspected, not confirmed with a live DB - ported faithfully,
unchanged, pending upstream confirmation

**Bug:** the function runs three parallel cursors (`obsdate_cur` - observation
counts per date bucket, `obsdateimg_cur` - image counts, `obsdateimggood_cur`
- "good" image counts) and merges rows across them by matching bucket value.
After consuming a matching row from `obsdateimg_cur` and resetting `d_value2`
to `-1`, it does:

```cpp
if (d_value1 == d_value2)
{
	(*this)[d_value1] = DateStatistics (d_c, d_i, d_gi, d_tt);
	d_value2 = -1;
	if (d_value2 == d_value2g)   // <-- always comparing -1 to d_value2g
		d_value2g = -1;
}
```

`d_value2` is reassigned to `-1` on the line immediately above, so
`d_value2 == d_value2g` can only be true when `d_value2g` also happens to be
`-1` - it can never meaningfully detect "the good-image-count cursor's row
also matched this bucket". Given the surrounding logic (three cursors being
walked in lock-step, matched by bucket value), the almost-certainly-intended
comparison is `d_value1 == d_value2g`, mirroring the `d_value1 == d_value2`
check just above it. As written, `d_value2g` is only ever cleared when it was
already `-1`, which looks harmless in most cases (the next loop iteration's
`if (d_value2g < 0)` guard re-fetches anyway) but means the "already
consumed" bookkeeping for the good-image-count cursor never actually fires
off the intended condition - worth a closer look with a real multi-bucket
dataset before drawing firm conclusions about user-visible impact.

**Not fixed in db**: ported faithfully as-is in
`db/db/src/observationset.ec` (see the `base note:` comment at the call
site) - this is speculative without a live DB to verify against, and the
affected code path (`ObservationSetDate`, hierarchical date browsing) isn't
used by `rts2-executor`. Flagging here for whoever picks up the
httpd/reporting side of db later.

---

## sortByAltitude / sortWestEast - inverted NULL check, likely crash on default use

**Files:** `lib/rts2db/targetset.ec` (`sortByAltitude::sortByAltitude()`,
`sortWestEast::sortWestEast()`)
**Maintainer:** Petr Kubanek
**Severity:** likely crash (SIGSEGV) on the common no-arg call path

**Bug:** both constructors take an optional observer pointer, defaulting to
`NULL` in the header (`sortByAltitude (struct ln_lnlat_posn *_observer =
NULL, double _jd = NAN)`), and are meant to fall back to the globally
configured observer when none is given - exactly the pattern used three
other times in the same file (`TargetSet`'s own constructors: `obs = in_obs;
if (!obs) obs = Configuration::instance()->getObserver();`). Here the
condition is inverted:

```cpp
sortByAltitude::sortByAltitude (struct ln_lnlat_posn *_obs, double _jd)
{
	if (!_obs)
		observer = _obs;                 // sets observer = NULL
	else
		observer = rts2core::Configuration::instance ()->getObserver ();
	...
```

When `_obs` is `NULL` (the default, and the common case - e.g. `std::sort
(..., sortByAltitude ())`), the condition takes the `!_obs` branch and sets
`observer = _obs`, i.e. `NULL`. When a real observer *is* passed, it's
discarded in favor of the global configuration instead. `doSort()` then
calls `tar1->getAltAz (&hr1, JD, observer)`, which dereferences `observer`
unconditionally - a near-guaranteed segfault whenever either sort functor is
constructed via its default argument, the most common way to invoke it.

**Fix applied in db** (`db/db/src/targetset.ec`): swapped the
condition to `if (_obs) observer = _obs; else observer = ...getObserver();`
in both constructors, matching the pattern used elsewhere in the same file.

---

## TargetPlan::startSlew() - plan_id passed where update_position bool is expected

**Files:** `lib/rts2db/sub_targets.ec` (`TargetPlan::startSlew()`)
**Maintainer:** Petr Kubanek
**Severity:** low/unclear - legacy plan-based scheduling path, likely superseded
by the Python queuer in current deployments (see the executor/queuer split
noted in the db design doc); not confirmed to have observable impact
**Status:** suspected, ported faithfully unchanged

**Bug:** `TargetPlan::startSlew()`'s own signature ends `(..., bool
update_position, int plan_id = -1)`, matching `Target::startSlew()`. When it
has a `selectedPlan`, it calls:

```cpp
moveType TargetPlan::startSlew (struct ln_equ_posn *pos, std::string &p1, std::string &p2, bool update_position, int plan_id)
{
	moveType ret;
	if (selectedPlan)
	{
		ret = selectedPlan->startSlew (pos, p1, p2, plan_id);
		...
```

`Plan::startSlew()`'s 4th parameter is `bool update_position` - not a
`plan_id`. This call passes `TargetPlan::startSlew`'s own `plan_id` (an
`int`) into that slot, implicitly converted to `bool` (nonzero → `true`).
The `update_position` value actually received by this function is never
forwarded anywhere. Given the very similar pattern used correctly one branch
below (`return Target::startSlew (pos, p1, p2, update_position, plan_id);`),
this reads like a variable mix-up rather than intentional behavior - but
`Plan::startSlew()` itself just passes its own `update_position` straight
through to `Target::startSlew()`, so the practical effect is narrower than
it looks (still means `update_position` is effectively always "true" unless
`plan_id` happens to be exactly `0`, rather than reflecting the caller's
actual intent).

**Not fixed in db**: ported faithfully as-is in
`db/db/src/sub_targets.ec` (see the `base note:` comment at the call
site). This is the legacy plan-based scheduling mechanism (`Plan`/`plan`
table), which per the user's own description is superseded in current
deployments by the Python queuer (`rts2-queue`) handling target-level
selection - low confidence this path is even exercised anymore, so not
worth guessing at intent without a live test case.

---

## ExecutorQueue::addFirst() - rep_n/rep_separation silently dropped, plan_id/hard misrouted

**Files:** `lib/rts2script/executorque.cpp` (`ExecutorQueue::addFirst()`)
**Maintainer:** Petr Kubanek
**Severity:** medium - affects the "insert at first observable position" queue
operation when repeat count/separation are requested together with a
first-possible insert

**Bug:** `addFirst()`'s own signature takes `rep_n`/`rep_separation` as
parameters (positions 6/7) - received from its caller, e.g.
`queueFromConn()` forwards its own `nrep`/`separation`. Inside the loop,
when a valid insertion point is found, it constructs the new queue entry
as:

```cpp
insert (iter, QueuedTarget (queue_id, nt, t_start, t_end, plan_id, hard));
```

`QueuedTarget`'s constructor is `(queue_id, target, t_start, t_end, rep_n,
rep_separation, plan_id, hard, persistent)`. Passing only 6 arguments here
means `addFirst`'s own `plan_id` parameter lands in the `rep_n` slot, and
`hard` (bool) lands in `rep_separation` (float) - implicitly converted.
`addFirst`'s actual `rep_n`/`rep_separation` parameters are never
referenced anywhere else in the function, and the real `plan_id`/`hard` are
silently dropped (always default to `-1`/`false` for entries inserted this
way, regardless of what the caller actually requested).

**Fix applied in db** (`db/executor/src/executorque.cpp`): forwards
all of `addFirst`'s own parameters by name:

```diff
-			insert (iter, QueuedTarget (queue_id, nt, t_start, t_end, plan_id, hard));
+			insert (iter, QueuedTarget (queue_id, nt, t_start, t_end, rep_n, rep_separation, plan_id, hard));
```

---

## rts2-mon (NComWin/NWindowEdit/AbstractBoxSelection) - three format-string vulnerabilities

Three separate spots in `rts2-mon`, all the same shape: a data string
passed directly as a `wprintw`/`mvwprintw` (a `printf`-family function)
format argument, with no format string of its own and no variadic
arguments. If the string ever contains `%s`/`%n`/etc. - plausible, since
all three display arbitrary device/value/command-history string content
- this reads (or, via `%n`, writes) arbitrary stack memory through the
missing varargs. Identical bugs exist in the classic tree, unfixed there.
Caught by Debian's default hardening build flags (`-Wformat
-Werror=format-security`, part of `dpkg-buildflags`), which a plain
manual `cmake`/`make` build doesn't enable - invisible until packaging
`rts2-base` for real via `dpkg-buildpackage`.

1. `monitor/include/nwindowedit.h` (`NWindowEdit::setValue`, classic
   `include/nwindowedit.h`, same line):
   ```cpp
   void setValue (std::string _val) { wprintw (getWriteWindow (), _val.c_str ()); }
   ```
2. `monitor/src/nvaluebox.cpp` (`AbstractBoxSelection::drawRow`, classic
   `src/monitor/nvaluebox.cpp`, same line):
   ```cpp
   mvwprintw (getWriteWindow (), maxrow++, 1, _text);
   ```
3. `monitor/src/ncomwin.cpp` (`NComWin::historyMove` or similar, classic
   `src/monitor/ncomwin.cpp`, same line):
   ```cpp
   mvwprintw (comwin, 0, 0, history.at (historyPos - 1).c_str ());
   ```

**Fix applied in base**: pass the string as data via a real `"%s"`
format in all three, e.g. `wprintw (getWriteWindow (), "%s", _val.c_str ());`.

---

## DevClientCameraImage::allImageDataReceived() - windowed readout position never recorded (LTV1/LTV2/CRPIX)

**Files:** `lib/rts2fits/devcliimg.cpp` (`DevClientCameraImage::allImageDataReceived()`)
**Maintainer:** Petr Kubanek
**Severity:** high - every windowed (non-full-frame) exposure from any
camera that doesn't use the optional multi-channel `CHAN1_OFFSETS`/
`CHAN2_OFFSETS` config (i.e. every camera actually deployed at any real
site checked) writes `LTV1`/`LTV2` as if the window were never offset
from the detector origin, regardless of where it actually was. Found
live: a user maintaining a separate post-processing pipeline
(`asarina/pipeline/patch_window.py`) had to *guess* the window's
position from `NAXIS1`/`NAXIS2` alone, assuming it was centered on the
chip - because RTS2 itself never wrote the real position anywhere.

**Bug:** each received data channel carries the actual per-exposure
readout window's origin (`imgh->x`/`imgh->y`, in unbinned detector
pixels - see `camd.cpp`'s `fhd->x = htons (chipUsedReadout->getXInt ())`
for where the device side puts it on the wire). `allImageDataReceived()`
decodes these into local `x`/`y`, and correctly uses them a few lines
later for `DATASEC`/`DETSEC`/`TRIMSEC` (e.g. `(datasec->getXInt () - x)
/ bin1`). But the `mods[2]`/`mods[3]` array - which becomes both the
literal `LTV1`/`LTV2` FITS header values a few lines below, and the
`CRPIX1`/`CRPIX2` sky-WCS adjustment inside `writeWCS()` - is built
*only* from the optional per-channel `CHAN1_OFFSETS`/`CHAN2_OFFSETS`
config (for multi-amplifier geometry) and never incorporates `x`/`y` at
all:

```cpp
double mods[NUM_WCS_VALUES] = {0, 0, 0, 0, 1, 1, 0};

if (chan1_offsets && chan < chan1_offsets->size ())
    mods[2] += (*chan1_offsets)[chan];
...
if (bin1 != 0)
    mods[2] /= bin1;
```

For any camera not using `CHAN1_OFFSETS` (none of the ones checked do),
`mods[2]`/`mods[3]` stay exactly `0` regardless of the window's actual
position, so `LTV1`/`LTV2`/`CRPIX1`/`CRPIX2` never reflect it.

**Fix applied in base** (`kernel/src/devcliimg.cpp`): fold the window
offset into the same `mods[2]`/`mods[3]` computation, using the exact
same `(detector_pixel - x) / bin` convention the neighboring `DATASEC`
computation already uses (so a windowed image's `LTV1`/`LTV2` now agree
with its own `DATASEC`):

```diff
-			if (bin1 != 0)
-			{
-				mods[2] /= bin1;
-			}
-
-			if (bin2 != 0)
-			{
-				mods[3] /= bin2;
-			}
+			if (bin1 != 0)
+			{
+				mods[2] = mods[2] / bin1 - ((double) x) / bin1;
+			}
+
+			if (bin2 != 0)
+			{
+				mods[3] = mods[3] / bin2 - ((double) y) / bin2;
+			}
```

Verified against two real exposures from a dummy camera (detsize
0:0:1000:1000, datasec 20:20:960:960): a window at `x=0` (matching the
detector origin) produced `LTV1=LTV2=-0`, `DATASEC=[21:100,21:100]`; a
window at `x=500,y=500` produced `LTV1=LTV2=-500`, with `CRPIX1`/`CRPIX2`
shifting by the same `-500` through the untouched `writeWCS()` code -
both self-consistent with each window's own `DATASEC`. `LTM1_1`/`LTM2_2`
are left as-is - they have a separate, pre-existing gap (don't reflect
`bin1`/`bin2` either), out of scope for this fix, which targets the
window-position gap specifically.

---

## Fli::init() - nflush defaults to -1, silently leaving the CCD unflushed before every exposure

**Files:** `src/camd/fli.cpp` (`Fli` constructor, `Fli::init()`)
**Maintainer:** Petr Kubanek
**Severity:** medium/high (image-quality) - affects every FLI camera
that isn't explicitly started with `-l <N>`

**Bug:** the `nflush` value ("number of flushes before exposure") is
created and defaulted to `-1`:

```cpp
createValue (nflush, "nflush", "number of flushes before exposure", true, RTS2_VALUE_WRITABLE, CAM_WORKING);
nflush->setValueInteger (-1);
...
if (nflush->getValueInteger () >= 0)
{
    ret = FLISetNFlushes (dev, nflush->getValueInteger ());
    ...
}
```

`FLISetNFlushes()` is only called when `nflush >= 0`, so at `-1` (the
default, unless `-l` is passed on the command line) it's never called
at all - the camera is left at whatever its own firmware/EEPROM default
happens to be, which in practice (confirmed by the user, who runs real
FLI hardware) is 0 flushes. A CCD that isn't flushed before exposure
accumulates dark current/residual charge from however long it sat idle,
contaminating the frame - exactly the kind of thing a "number of
flushes before exposure" setting exists to prevent, silently disabled
by default.

**User's domain guidance** (informed the base fix, reproduced here for
whoever picks this up upstream): for a camera in regular use with no
long gaps, 1 flush is enough; after a longer idle period, 2 or
(exceptionally) 3 may be warranted for the *first* exposure - but the
right way to handle that is a periodic idle-time CCD dump, not raising
`nflush` (which would slow down the charge-up before every subsequent
normal exposure, not just the first one after a gap). More broadly:
classic `camd`'s architecture assumes the camera driver/hardware takes
care of CCD reset autonomously between exposures - true for some
cameras, not for FLI, and nothing in `camd` itself compensates.

**Fix applied in base** (`camd/fli/fli.cpp`): defaults `nflush` to `1`
instead of `-1` (still overridable via `-l`, including explicitly back
to `-1` for the old "don't touch it" behavior). Also adds a new,
FLI-specific periodic idle-flush timer (`idle_flush_period`, default
300s, 0 disables): while the camera is idle (not exposing, not reading -
`Camera::isIdle()`), calls the same `FLIFlushRow()` already used
elsewhere in this file (`stopExposure()`) once per period, so the CCD
never actually sits unflushed for the length of a "long gap" in the
first place, and `nflush` can stay at its fast, normal-case value
regardless of how long the camera was idle before the next exposure.
This is new functionality with no equivalent in classic `camd` at all -
worth considering as a generic `camd`-level mechanism upstream (per the
user's own framing above) if other camera families turn out to have the
same gap, rather than reimplementing it per-driver each time.

---

## focusclient.cpp/xfocusc.cpp - Ctrl-C never stops an exposure the client itself started

**Files:** `src/focusc/focusclient.cpp` (`FocusClient`/`FocusCameraClient`), same gap in `src/focusc/xfocusc.cpp`
**Maintainer:** Petr Kubanek
**Severity:** medium - wastes a real exposure and, per the user's
concern below, is the kind of impoliteness the RTS2 wire protocol has
no protocol-level defense against, only client-side discipline

**Bug:** neither `FocusClient` nor `FocusCameraClient` override any
shutdown hook. On SIGINT/SIGTERM, `App`'s signal handler calls
`Block::endRunLoop()`, which just sets the `end_loop` flag - the
`while (!getEndLoop())` loop in `Client::run()` exits, destructors run,
and `Connection::~Connection()` does nothing but `close(sock)`. No
"stop exposure"/logout message of any kind is sent. There is no
`CommandStopExposure`-equivalent client-side class anywhere in the
classic tree at all - the only stop mechanism that exists (`stopexpo`,
handled server-side in `camd.cpp`; `killAll`/`CommandKillAll`
client-side) is used by the script-execution machinery
(`rts2script/devscript.cpp`), never by this client.

Net effect: Ctrl-C during an exposure this tool itself commanded leaves
the camera exposing with nobody left to read the result out. Verified
empirically (base's dummy camd): a 20s exposure interrupted with SIGINT
at ~40% ran to full completion regardless, logging `end exposure
without exposure connection, state 1` - the dummy driver tolerates this
gracefully, but nothing ever asked it to stop, and there's no guarantee
every real driver copes as cleanly (this is exactly the "modules need
to be polite to each other" concern the user raised - the protocol
doesn't yet enforce this, so well-behaved clients have to).

**Fix applied in base**: added a generic `rts2core::CommandStopExposure`
class (`kernel/include/command.h`/`command.cpp`, mirrors the existing
`CommandBox`/`CommandCenter` pattern - just sends `stopexpo`). `focusc`
now overrides `FocusClient::run()` to call a new `stopOwnedExposures()`
after `Client::run()`'s loop exits (for any reason, not just Ctrl-C):
it scans live connections for cameras this specific process itself
armed (tracked in a new `armedCameras` list, populated only by
`armCamera()` - see the next entry), and for any of those still
`CAM_WORKING`, sends `CommandStopExposure` and pumps `oneRunLoop()` for
up to 2 seconds to give it a chance to actually reach the device and be
acknowledged before the process's sockets close for good. Only ever
touches cameras this process itself commanded to expose - never a
camera it's just passively watching (autoSave watches and saves images
from *any* connected camera, started by anyone).

Verified against real hardware (this sandbox's live `rts2-camd-v4l`
device): a 20s exposure sent SIGINT-equivalent (`timeout -s INT`) after
~3s aborted immediately instead of running to completion, and the
live `rts2-executor` also attached to that camera logged `detected
exposure failure. Continuing with the script` - it noticed and
recovered gracefully, exactly as a well-behaved client should.

Same gap exists in `xfocusc.cpp`/`XFocusClient` (not yet ported - see
the separate `rts2x`/`rts2-viewer` plan) - worth carrying this same fix
over when that tool is built.

---

## focusclient.cpp - omitting -d silently mutates every connected camera's settings without ever exposing one

**Files:** `src/focusc/focusclient.cpp` (`FocusClient::initFocCamera`)
**Maintainer:** Petr Kubanek
**Severity:** medium - a genuine footgun: produces no visible output
and no error, but silently changes shared device state

**Bug:** `FocusClient::initFocCamera()` applies the window (`-X/-Y/-W/-H`),
exposure-time (`-e`), binning (`-b`), and dark-shutter (`--dark`)
settings to **every currently connected camera unconditionally** - none
of that code is gated on `-d`/`cameraNames` at all. Only the final
`CommandExposure` trigger, at the very end of the function, is gated by
a name match. Consequences, both confirmed empirically against a real
two-camera dummy setup:

- Omit `-d` entirely and the tool looks like a no-op (no output, no
  FITS file) - but any settings passed on the command line get written
  to *every* connected camera anyway. Reproduced: ran `rts2-focusc -X
  10 -Y 10 -W 50 -H 50` (no `-d`) - produced nothing visible; a
  subsequent, separate `rts2-focusc -d C0 -e 0.3` (no window options at
  all) then came back with a **50x50 windowed frame**, because the
  earlier "no-op" run had quietly left that window on the camera.
- Even *with* `-d C0` given explicitly, if an unrelated `C1` also
  happens to be connected, `C1`'s exposure time/binning/window/shutter
  get silently changed too - it just never gets an actual exposure
  command, so nothing ever surfaces the change to the user.

**Fix applied in base**: split `initFocCamera` into `armCamera()` (the
actual settings + exposure-trigger logic, applied only to a camera the
process has decided it owns) and made the "which camera(s) do we own"
decision explicit and gated in all cases - see the "no default camera"
design note below for what "-d not given" now means. `setSaveImage()`
(passive image-watching, non-mutating) still applies to every connected
camera as before - only the state-*mutating* commands are now gated.

### Design note: what should "-d not given" mean? (no default camera in RTS2)

Raised by the user: RTS2 has no notion of a "default camera" - the
closest thing, `rts2.ini`'s `imgproc.astrometry_devices`, is a
special-purpose astrometry-processing hint, not a general default, and
would be a weird thing to repurpose here. Adopted policy, matching the
user's own suggested shape for this: after a short settle delay (1s,
`CAMERA_CHOICE_DELAY`) to let centrald report every currently connected
device -
- **exactly one** camera connected -> use it, printing a visible note
  that this happened (so it's never a silent guess);
- **zero** cameras connected -> stay passive (matches the tool's
  existing "just watch and save whatever comes in" behavior when idle);
- **more than one** -> refuse to guess. Print every connected camera's
  name and ask for `-d <name>`; touch nothing. Verified empirically:
  produced `no -d given and more than one camera connected (C0, C1) -
  pass -d <name> to pick one; nothing was touched`, exited cleanly, no
  FITS file, no settings changed on either camera.

The 1-second settle delay is a pragmatic choice, not a protocol
guarantee - there's no explicit "device discovery complete" signal in
the client protocol to wait on instead (devices are reported to a
freshly-logged-in client incrementally as `addAddress()` messages, with
no terminating marker); a fixed short grace period is the same kind of
approach already used elsewhere in this codebase (e.g. the 0.1s
`CHECK_TIMER` progress-poll).

---

## focusclient.cpp - -e/-b/-X/-Y/-W/-H are not scoped per -d camera, despite reading that way

**Files:** `src/focusc/focusclient.cpp` (`FocusClient::processOption`, `FocusClient::initFocCamera`)
**Maintainer:** Petr Kubanek
**Severity:** medium - no data loss, but a command line that visually
implies per-camera settings silently does something else

**Bug:** raised by the user, who correctly guessed the mechanism before
it was confirmed: `-e`/`-b`/`-X`/`-Y`/`-W`/`-H` are each a single plain
scalar member (`defExposure`/`defBin`/`xOffset`/`yOffset`/`imageWidth`/
`imageHeight`), overwritten every time `getopt_long` parses that flag,
and read only once each, later, whenever a named camera actually
connects. There is no association between an option and the `-d` it
visually follows - `getopt_long` has no concept of "groups" at all, it
just processes flags left to right and the *last* value for each one
wins, applied identically to *every* camera named anywhere in
`cameraNames`.

Reproduced against two real dummy cameras: `rts2-focusc -d C0 -e 1 -b 0
-d C1 -e 5 -b 1` visually reads as "C0 gets 1s/1x1, C1 gets 5s/2x2" -
both `EXPOSURE` and `BINNING` FITS headers came back identical
(`5.0`/`2x2`) for **both** C0 and C1. An `-e` given before any `-d` at
all fares no better - it doesn't become "the default for cameras that
don't specify their own" (a reasonable guess); it's simply overwritten
by any later `-e`, with zero per-camera memory.

Genuinely fixing this (real per-`-d`-group settings scoping) would mean
reworking the option parser to snapshot settings into a map keyed by
camera name as `-d` is encountered, plus settling new semantics for
settings given before any `-d` - a materially bigger change than
anything else in this file. Given the user's own stated preference
(warn, don't silently redesign the parser), **not attempted** here.

**Fix applied in base**: added per-flag occurrence counters
(`optExposureCount`/`optBinCount`/`optXCount`/`optYCount`/
`optWidthCount`/`optHeightCount`, incremented in `processOption`) and a
`warnIfSettingsAmbiguous()` check, run once from `init()`: if more than
one camera is named (`cameraNames.size() > 1`) *and* any of those flags
was given more than once, prints a clear warning explaining the actual
(shared, last-value-wins) semantics and suggesting one `rts2-focusc`
invocation per camera instead. Deliberately narrow trigger condition -
`-d C0 -d C1 -e 5` (one `-e`, two cameras, meaning "expose both
identically") is completely unambiguous and does **not** warn; neither
does repeating `-e` with only one camera named (ordinary CLI
last-value-wins, not a footgun). Verified all three cases empirically.

---

## Connection::isName() - null pointer crashes DevClientCameraFoc's focus-hook path on any camera without a focuser

**Files:** `include/connection.h` (`Connection::isName()`), reached via
`lib/rts2fits/devclifoc.cpp` (`DevClientCameraFoc::postEvent()`,
`EVENT_CHANGE_FOCUS`)
**Maintainer:** Petr Kubanek
**Severity:** high - deterministic crash (SIGSEGV), triggered by entirely
ordinary use of a long-standing, documented feature

**Bug:** found while testing whether `rts2-focusc`'s `-F <script>`
option (runs a script per completed image, via `ConnFocus`) could be
used to ship images into `ds9` via XPA - a real, wanted use case, not a
synthetic test. `ConnFocus::~ConnFocus()` posts `EVENT_CHANGE_FOCUS`
whenever the script never printed a `change <id> <val>` line (the
normal case for any script that isn't itself doing focus analysis - a
`ds9`-display hook has no reason to). `DevClientCameraFoc::postEvent()`
handles that event unconditionally:

```cpp
case EVENT_CHANGE_FOCUS:
	eventConn = (ConnFocus *) event->getArg ();
	focus = connection->getMaster ()->getOpenConnection (getConnection ()->getValueChar ("focuser"));
	if (eventConn && eventConn == focConn)
	{
		focusChange (focus);
		focConn = NULL;
	}
	break;
```

`getValueChar ("focuser")` returns `nullptr` whenever the camera has no
`"focuser"` value at all - true for essentially any camera not
explicitly paired with a focuser device (the dummy camera, `v4l`, and
by extension most real single-camera setups). That `nullptr` is passed
straight into `Block::getOpenConnection()` -> `Connection::isName()`:

```cpp
int isName (const char *in_name) { return (!strcmp (getName (), in_name)); }
```

`strcmp()` with a null second argument is undefined behavior -
observed as a deterministic SIGSEGV inside `__strcmp_avx2` on this
system, on every single completed exposure once any `-F`/`ConnFocus`
script is in use on a camera without a focuser. Confirmed
byte-for-byte identical in classic (`include/connection.h`,
`lib/rts2fits/devclifoc.cpp`) - this is not a porting-introduced bug,
it's a landmine in a genuinely useful, documented feature that appears
to have simply never been exercised against a focuser-less camera
before.

**Reproduction:** `rts2-focusc -d C0 -e 0.5 -F <any script>` against a
dummy or v4l camera (no `focdev`/paired focuser configured) -
segfaults immediately after the first completed exposure, caught live
under `gdb`:
```
Program received signal SIGSEGV, Segmentation fault.
__strcmp_avx2 () at ../sysdeps/x86_64/multiarch/strcmp-avx2.S:287
#1  rts2core::Block::getOpenConnection(char const*) (block.cpp:837)
#2  rts2image::DevClientCameraFoc::postEvent (devclifoc.cpp:48)
...
#5  rts2image::ConnFocus::~ConnFocus (devclifoc.cpp:144)
```

**Fix applied in base** (`kernel/include/connection.h`): `isName()`
now short-circuits on a null `in_name` instead of dereferencing it:

```diff
-int isName (const char *in_name) { return (!strcmp (getName (), in_name)); }
+int isName (const char *in_name) { return in_name && !strcmp (getName (), in_name); }
```

Fixed at this layer (not just the `devclifoc.cpp` call site) because
`getOpenConnection(nullptr)` has no sensible meaning for *any* caller -
"no device is named `(null)`" should safely return "not found," not
crash, no matter how a caller ends up passing it. Verified: the same
`-d C0 -e 0.5 -F <ds9-hook>` reproduction now runs cleanly, exposure
after exposure, no crash - and the hook script itself successfully
shipped the image into a real running `ds9` via `xpaset -p ds9 fits
<path>` once pointed at an absolute path (see the "hooking rts2-focusc
to ds9" note in `STATUS.md` for the two working recipes and the
relative-path gotcha that's a `ds9`/XPA quirk, not an RTS2 bug).

---

## Block::~Block() - never frees still-pending timer Events at shutdown

**Files:** `lib/rts2/block.cpp` (`Block::~Block()`)
**Maintainer:** Petr Kubanek
**Severity:** low (bounded, one-time, reclaimed by the OS at process
exit anyway) but real, and universal - every daemon and client built on
`Block` that uses any timer is affected, not just `rts2-focusc`

**Bug:** found running `rts2-focusc` under `valgrind --leak-check=full`
(the user asked for this check after the port/review work above).
`Block::idle()` dispatches a due timer's `Event*` through the
`postEvent()` chain, which is what actually `delete`s it (see
`Object::postEvent()`'s own doc comment: "every descendant... should
call ancestor postEvent method at the end, so the event object will be
deleted"). That only happens for timers that actually *fire*. Any timer
still pending when the process shuts down - which for a
self-rescheduling timer (`EVENT_EXP_CHECK` in `focusclient.cpp`,
`EVENT_TE_RAMP` in `fli.cpp`, and presumably others following the same
established pattern) is *always* exactly one, since the next firing is
always queued before the current one's handler returns - never goes
through that chain, and `~Block()` never sweeps the `timers` map
itself:

```cpp
Block::~Block (void)
{
	// ...connections, blockAddress, blockUsers all cleaned up...
	delete[] fds;
}
```

`timers` (a `std::map<double, Event*>`) is simply never mentioned.
Confirmed byte-for-byte identical in classic (`lib/rts2/block.cpp`).

**Reproduction:** `valgrind --leak-check=full ... rts2-focusc -d C0 -e
0.5` (or any `Block`-derived program with a running timer), interrupted
or left to exit normally - always exactly one small "definitely lost"
block, backtrace through whichever timer was still pending
(`FocusClient::postEvent()`'s `EVENT_EXP_CHECK` re-arm, in this case).

**Fix applied in base** (`kernel/src/block.cpp`): `~Block()` now deletes
every remaining `Event*` in `timers` before clearing it. Safe to do
unconditionally - `Block::idle()`'s two-phase dispatch-then-erase (via
the `toDelete` batching in `pushToDelete()`) always completes fully
within a single call, so by the time the destructor runs, anything
still in `timers` is guaranteed to be a genuinely never-fired,
never-deleted event, never one already handed off through the
`postEvent` chain.

```diff
 	for (std::list <ConnUser *>::iterator iu = blockUsers.begin (); iu != blockUsers.end (); iu++)
 		delete *iu;
+	for (std::map <double, Event *>::iterator it = timers.begin (); it != timers.end (); it++)
+		delete it->second;
+	timers.clear ();
 	delete[] fds;
```

Verified with valgrind before and after: `rts2-focusc` exercised across
four scenarios (plain exposure loop with an `-F` hook, a `SIGINT`
mid-exposure specifically to exercise the Ctrl-C stop-exposure fix
above, the ambiguous multi-camera path, and the single-camera auto-pick
path) all showed the identical one-block "definitely lost" before this
fix and `definitely lost: 0 bytes in 0 blocks` / `ERROR SUMMARY: 0
errors` after it, with the full test suite (`ctest`, including
`block_connection`) still passing.

## teld.cpp/dummy.cpp - parked dummy telescope floods the log forever with "below horizon"

**Files:** `lib/rts2tel/teld.cpp` (`Telescope::infoUTCLST()`,
`Telescope::abortMoveTracking()`), `src/teld/dummy.cpp`
(`Dummy::startPark()`)

`Telescope::infoUTCLST()` checks the current alt/az against the hard
horizon on *every* `info()` call, and if it's bad:

```cpp
int ret = abortMoveTracking ();
if (ret <= 0)
	logStream (MESSAGE_ERROR) << "info retrieved below horizon position, stop move. alt az: " << hrpos.alt << " " << hrpos.az << sendLog;
```

The base `Telescope::abortMoveTracking()` (used by the dummy driver,
which doesn't override it) unconditionally `return 0`s, so the `ret <=
0` guard is not a guard at all - the ERROR fires on literally every
poll for as long as the position stays bad, with no "log once when
first detected" debounce. It also only calls `stopTracking()`, never
actually aborting an in-progress *move* - a real slew that happens to
pass below horizon keeps ticking through `isMoving()` regardless. (The
header comment on `abortMoveTracking()` documents an escape hatch -
subclasses can `return 1` for "temporarily allowed violation of below
horizon" to suppress the message - `D50`/`Sitech-gem` override it to do
something smarter; the base/dummy implementation never does.)

`Dummy::startPark()` compounds this specifically for the dummy driver:
it hardcodes the stow position to a fixed RA=2h/Dec=2deg with no
horizon check and no `ignoreHorizon` bypass:

```cpp
virtual int startPark ()
{
	dummyPos.ra = 2;
	dummyPos.dec = 2;
	return 0;
}
```

If that fixed point sits below the configured hard horizon for the
site/time (common - it's an arbitrary constant, not derived from
anything), the dummy sits parked there indefinitely, and the info-poll
above logs the ERROR forever, at whatever rate clients happen to be
polling `info()` (observed climbing from ~1.4k lines/hour right after
a start-of-day park to ~5k/hour by mid-afternoon on one running
instance, purely because more pollers/monitors had connected through
the day - nothing about the mount itself was escalating).

**Reproduction:** start `rts2-teld-dummy`, send it `park`, leave it
sitting parked - `grep "below horizon"` on the log grows without bound.

**Fix applied in base** (`teld/dummy/dummy.cpp`): park to local zenith
(alt=90) instead of a fixed RA/Dec - zenith is always above the hard
horizon regardless of site latitude, date, or time of day, so a parked
dummy can never end up stuck below it. Since zenith's RA constantly
changes with sidereal time while its alt stays fixed at 90, `info()`
now re-locks `dummyPos` onto the *current* zenith every poll while the
telescope is in the `TEL_PARKED` state (reusing the existing protected
`Telescope::getEquFromHrz()` helper), so it never drifts back below
horizon just from sitting idle:

```diff
 		virtual int info ()
 		{
+			if ((getState () & TEL_MASK_MOVING) == TEL_PARKED)
+				dummyPos = getZenithEquPosn ();
 			setTelRaDec (dummyPos.ra, dummyPos.dec);
 			julian_day->setValueDouble (ln_get_julian_from_sys ());
 			return Telescope::info ();
 		}
 ...
 		virtual int startPark ()
 		{
-			dummyPos.ra = 2;
-			dummyPos.dec = 2;
+			struct ln_equ_posn pos = getZenithEquPosn ();
+			dummyPos = pos;
+			setTelTarget (pos.ra, pos.dec);
 			return 0;
 		}
```

`setTelTarget()` is also set at park time so `isMoving()`'s
already-there fast-teleport path (triggered whenever `move_fast` is set
or the estimated slew time has already elapsed) lands on zenith too,
rather than on whatever the previous real target happened to be.

This only fixes the dummy driver's specific way of getting stuck below
horizon forever while parked/idle - it does not address the underlying
missing debounce in `Telescope::infoUTCLST()`/`abortMoveTracking()`
itself, which would still flood the log once-per-poll for any driver
(dummy or real) that's deliberately commanded to track or slew through
a below-horizon path - that case is expected/intended to be noisy per
poll during an active move, just not while sitting still.

**Verified**: rebuilt, ran an isolated centrald + `rts2-teld-dummy`
pair, sent `park` via `rts2-sendcmd`, confirmed the log stops growing
new "below horizon" lines immediately after park (previously grew
without bound), and stayed silent across a further 45s of sitting
parked.

## DevClientCameraImage::processCameraImage() - saveImage=0 never actually skipped saving

**Affected files**: `base/kernel/src/devcliimg.cpp` (and, confirmed identical,
classic `lib/rts2fits/devcliimg.cpp`), plus `base/kernel/src/image.cpp`.

Found while chasing a `gui`/`rts2-viewer` report: the viewer's "Save to
disk" toggle sets client-side `DevClientCameraImage::saveImage` to 0/1
(`setSaveImage()`), and `processCameraImage()` guards the actual write:

```cpp
if (saveImage)
{
	// set filter..
	// save us to the disk..
	ci->image->saveImage ();
}
```

This guard is a no-op. `Image::saveImage()` is just `closeFile()` - by the
time `processCameraImage()` runs, the FITS file has *already* been created
on disk and had real pixel data streamed into it: `DevClientCameraImage::
createImage()` constructs the `Image` via the constructor that calls
`createImage(filename, overwrite)` -> `FitsFile::createFile()` ->
`fits_create_file()` immediately (not deferred, and not the in-memory
`memFile` path - a real filename is given), and `CameraImage::writeData()`
streams the received pixel data into that same open file as it arrives.
Skipping the `if (saveImage)` branch above only skips one final explicit
`closeFile()` call - it does not stop the file from existing with real
data in it. Worse, `Image::~Image()` (`image.cpp`) unconditionally calls
`saveImage()` again:

```cpp
Image::~Image (void)
{
	saveImage ();
	...
```

so even the class's own destructor re-triggers the "save" regardless of
the flag. Net effect: toggling `saveImage` to 0 has never actually
prevented a FITS file from being written and kept, in this project or in
classic - the flag only ever changed *when* the final close happened, not
*whether* the file survived.

**Fix applied** (`base/kernel/src/devcliimg.cpp`, `processCameraImage()`):
added the missing `else` branch, calling `ci->image->deleteImage()` -
which closes and unlinks the file - mirroring what `~CameraImage()`
(`cameraimage.cpp`) already does for the unrelated case of an image that
never received any pixel data at all:

```cpp
if (saveImage)
{
	ci->image->saveImage ();
}
else
{
	ci->image->deleteImage ();
}
```

Also changed `gui/viewer`'s own default from off to on (`ViewerClient::
createOtherType()`'s `setSaveImage(1)`, `MainWindow`'s `CameraState::
saveEnabled = true` and the Saving button starting checked) per the
user's explicit request - previously the viewer started with saving off
by policy choice, which had been masking this bug since the "off" default
matched the buggy "always saves anyway" behavior closely enough that
nobody noticed images were being kept regardless of the toggle.

**Verified**: standalone test linked directly against `libbase_kernel.a`,
using the real `Image` constructor/destructor (not a mock) - confirmed a
file created then merely left to go out of scope without any explicit
`saveImage()`/`deleteImage()` call (the old code's actual behavior when
`saveImage` was 0) is still present on disk after the destructor runs
("BUG: saved anyway"), and that adding the explicit `deleteImage()` call
(the fix) removes it and it stays removed. Full `gui`/`base`/`db`
rebuild clean. Not yet re-verified through the live GUI toggle itself
(no display in this environment) - needs the user's own check that
flipping "Saving: ON" to "OFF" in `rts2-viewer` now actually stops new
FITS files from appearing on disk.

## How this list is maintained

Add an entry here (not just to `STATUS.md`) whenever a genuine classic-tree
bug turns up during porting - something that's wrong regardless of base
existing, not a porting mistake or a stylistic modernization. `STATUS.md`'s
"Bugs found and fixed during the port" section keeps the same list inline
with the rest of the port's working notes; this file is the subset meant to
actually go back to upstream, with enough reproduction detail to act on
without needing the base context.
