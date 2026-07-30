# Bug report: `getaddrinfo` failures are logged as an uninformative "System error"

**Component:** `librts2` (connection setup), plus three minor issues found nearby
**Found in:** RTS2 tree at `/home/torman/rts2`, running `rts2-executor` (EXEC)
**Severity:** low impact, high diagnostic cost

---

## 1. Main issue — `EAI_SYSTEM` is reported without `errno`

### Symptom

```
2026-07-28T22:24:12.016 UTC EXEC 1 NetworkAddress::getAddress getaddrinfor for host localhost:System error
```

The message states only that *something* failed, with no way to tell what.

### Cause

`gai_strerror(EAI_SYSTEM)` returns the literal string `"System error"`. In glibc,
`EAI_SYSTEM` (-11) is not an error in itself — it means *"an underlying syscall
failed; the real reason is in `errno`"*. RTS2 prints `gai_strerror(ret)` and
discards `errno`, so the only useful information is lost.

Confirmed that this is glibc's `getaddrinfo`/`gai_strerror` and not RTS2's CYGWIN
compat shim:

- `include/rts2-config.h:122` defines `RTS2_HAVE_GETADDRINFO 1`, so
  `include/networkaddress.h:29` does **not** include `getaddrinfo.h` (and its
  redefined positive `EAI_*` constants).
- `lib/rts2/getaddrinfo.c` is not compiled — `NOT_GETADDRINFO` is false, and the
  generated `lib/rts2/Makefile` shows `#am__append_8 = getaddrinfo.c`.

Plausible underlying causes that all collapse into this one message: file
descriptor exhaustion (`EMFILE`), NSS module `dlopen` failure after a glibc or
systemd upgrade under a running process, an unreadable/unmounted
`/etc/hosts` or `/etc/nsswitch.conf`, or a dead `nscd`/`systemd-resolved`
socket. None of these are distinguishable from the current log line.

### Affected sites

- `lib/rts2/device.cpp:98` — `DevConnection::init()`
- `lib/rts2/client.cpp:62` — `ConnClient::init()`
- `src/grb/conngrb.ec:1417`
- `src/grb/forward.cpp:199`

### Suggested fix

Capture `errno` immediately after the call (before anything else can clobber it)
and print it when the return value is `EAI_SYSTEM`:

```cpp
ret = address->getSockaddr (&device_addr);
int saved_errno = errno;
if (ret)
{
    logStream (MESSAGE_ERROR) << "NetworkAddress::getAddress getaddrinfo for host "
        << address->getHost () << ": "
        << (ret == EAI_SYSTEM ? strerror (saved_errno) : gai_strerror (ret))
        << sendLog;
    return -1;
}
```

Note `getaddrinfo` only guarantees `errno` is meaningful when it returns
`EAI_SYSTEM`, hence the conditional.

Two cosmetic points while touching these lines: the word `getaddrinfor` is a
typo for `getaddrinfo` in all four sites, and `device.cpp:98` omits the space
after the colon that `client.cpp:62` has (`":"` vs `": "`), which makes the two
messages inconsistent.

---

## 2. `ConnExecute` leaked and left in a half-initialised state on init failure

`lib/rts2script/elementexe.cpp:730-741`:

```cpp
if (connExecute == NULL)
{
    connExecute = new ConnExecute (this, master, exec);
    int ret = connExecute->init ();
    if (ret)
    {
        logStream (MESSAGE_ERROR) << "Cannot execute script control command, ..." << sendLog;
        return NEXT_COMMAND_STOP_TARGET;
    }
    client = _client;
    client->getMaster ()->addConnection (connExecute);
}
```

On the failure path `connExecute` is neither deleted nor handed to
`addConnection()`, but it is left non-NULL. The object leaks, and any later call
to `defnextCommand()` on the same `Execute` element takes the `connExecute !=
NULL` branch and proceeds with a connection that was never successfully
initialised (`sock == -1`). Suggest `delete connExecute; connExecute = NULL;`
before returning.

Reproducible trivially by pointing a script's `exe` command at a non-existent
path — which is how this was found (a database entry missing the `.py`
extension).

---

## 3. `ConnFork::init()` skips `initFailed()` on the `access()` path

`lib/rts2/connfork.cpp:320`:

```cpp
if (access (exePath, X_OK))
{
    logStream (MESSAGE_ERROR) << "execution of " << exePath
        << " failed with error: " << strerror (errno) << sendLog;
    return -1;
}
```

Every other failure path in this function calls `initFailed()` before returning
`-1`; this one does not. Probably an oversight rather than intentional.

(The message itself is correct and useful — worth noting it comes from `access()`,
so no fork has happened at that point. Minor: `access()` tests the *real* uid and
is TOCTOU with the later `execv`, but neither matters much here.)

---

## 4. Potential stack buffer overflow in `NetworkAddress::getSockaddr()`

`lib/rts2/networkaddress.cpp`, `getSockaddr()`:

```cpp
char s_port[10];
...
sprintf (s_port, "%i", port);
```

`port` is an `int` originating from centrald / the network. `"%i"` of
`-2147483648` is 11 characters plus the terminator — 12 bytes into a 10-byte
buffer. Any value `>= 1000000000` or negative overflows.

Not currently triggered, and it requires a malformed address announcement, but
it is a straightforward stack smash if one ever arrives. Suggest `char
s_port[16]` and `snprintf`, and/or validating `port` into `1..65535` before use.

---

## Note on the original incident

The two log lines below are **unrelated** despite being 1 ms apart:

```
EXEC 1 NetworkAddress::getAddress getaddrinfor for host localhost:System error
EXEC 1 execution of /home/mates/filtros failed with error: No such file or directory
```

The second is `ConnFork::init()`/`access()` reporting a genuinely missing script
(the actual operational fault — a bad database entry). The first is
`DevConnection::init()` failing to resolve a device address, on a completely
separate code path. They landed in the same event loop pass; there is no causal
link. Fixing item 1 would have made that obvious immediately.
