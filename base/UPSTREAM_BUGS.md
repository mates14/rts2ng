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

## How this list is maintained

Add an entry here (not just to `STATUS.md`) whenever a genuine classic-tree
bug turns up during porting - something that's wrong regardless of base
existing, not a porting mistake or a stylistic modernization. `STATUS.md`'s
"Bugs found and fixed during the port" section keeps the same list inline
with the rest of the port's working notes; this file is the subset meant to
actually go back to upstream, with enough reproduction detail to act on
without needing the base context.
