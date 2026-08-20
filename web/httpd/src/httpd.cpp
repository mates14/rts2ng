/*
 * rts2-httpd (STATUS.md tasks 1-7): a real RTS2 Device that also runs
 * libmicrohttpd, driven from Block's own poll() loop instead of a second
 * event loop. libmicrohttpd is run in external-polling mode
 * (MHD_USE_EPOLL, no internal thread); its whole connection set is
 * represented by one epoll fd (MHD_DAEMON_INFO_EPOLL_FD), which is
 * registered as one more source in Block's own pollfd array via
 * addPollFD()/isForRead() - see STATUS.md's "Networking" design decision
 * for why this was chosen over a second reactor.
 *
 * DB-bound endpoints (STATUS.md task 7) are entirely opt-in at build time
 * via the WEB_HAVE_DB preprocessor define (set by CMake iff WEB_WITH_DB is
 * ON - see web/CMakeLists.txt): HttpD derives from rts2db::DeviceDb
 * instead of the bare rts2core::Device when it's set, mirroring classic
 * src/httpd/httpd.h's own #ifdef RTS2_HAVE_PGSQL split. Everything from
 * tasks 1-6 (bus-only JSON/REST, previews, WebSocket push, worker pool,
 * local auth) works identically either way - DB presence only adds
 * endpoints, it was never a prerequisite for the rest of the daemon.
 */

#include "device.h"
#include "status.h"
#include "command.h"
#include "option.h"

#include "jsonvalue.h"
#include "preview.h"
#include "userauth.h"
#include "websocket.h"
#include "workerpool.h"

#ifdef WEB_HAVE_DB
#include "rts2db/devicedb.h"
#include "dbendpoints.h"
#endif

#include <microhttpd.h>

#define OPT_IMAGES_DIR       OPT_LOCAL + 1
#define OPT_CACHE_DIR        OPT_LOCAL + 2
#define OPT_PREVIEW_WORKERS  OPT_LOCAL + 3
#define OPT_AUTH_FILE        OPT_LOCAL + 4
#define OPT_STATIC_DIR       OPT_LOCAL + 5
#define OPT_BIND_ADDRESS     OPT_LOCAL + 6

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <vector>
#include <list>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace rts2core;
using namespace rts2web;

namespace
{

/** Thrown for a request that reached a real endpoint but had bad/missing
 * parameters - reported to the client as 400, not a 5xx or a crash. */
class ApiError:public std::runtime_error
{
	public:
		ApiError (const std::string &msg):std::runtime_error (msg) {}
};

const char *getParam (struct MHD_Connection *connection, const char *key, const char *def)
{
	const char *v = MHD_lookup_connection_value (connection, MHD_GET_ARGUMENT_KIND, key);
	return v ? v : def;
}

// STATUS.md task 8: minimal extension-based Content-Type guess for
// files under --static-dir - just enough for the shipped web/static/
// tree (html/css/js), not a general MIME database.
const char *staticContentType (const std::string &path)
{
	if (path.size () >= 5 && path.compare (path.size () - 5, 5, ".html") == 0)
		return "text/html; charset=utf-8";
	if (path.size () >= 4 && path.compare (path.size () - 4, 4, ".css") == 0)
		return "text/css; charset=utf-8";
	if (path.size () >= 3 && path.compare (path.size () - 3, 3, ".js") == 0)
		return "application/javascript; charset=utf-8";
	if (path.size () >= 5 && path.compare (path.size () - 5, 5, ".json") == 0)
		return "application/json; charset=utf-8";
	if (path.size () >= 4 && path.compare (path.size () - 4, 4, ".svg") == 0)
		return "image/svg+xml";
	if (path.size () >= 4 && path.compare (path.size () - 4, 4, ".png") == 0)
		return "image/png";
	if (path.size () >= 4 && path.compare (path.size () - 4, 4, ".ico") == 0)
		return "image/x-icon";
	return "application/octet-stream";
}

}

namespace rts2web
{

class HttpD;

/**
 * State for one upgraded WebSocket connection (STATUS.md task 4). Owns
 * the raw socket libmicrohttpd hands off after MHD_UPGRADE - once
 * upgraded, MHD steps aside entirely and this daemon is responsible for
 * all further I/O on it, same as any other Block-managed fd.
 */
struct WsClient
{
	MHD_socket sock;
	struct MHD_UpgradeResponseHandle *urh;
	std::string recvBuffer;
	bool closed;

	WsClient (MHD_socket _sock, struct MHD_UpgradeResponseHandle *_urh):sock (_sock), urh (_urh), closed (false) {}

	void sendFrame (const std::string &frame)
	{
		if (closed)
			return;
		// Best-effort: a short/failed send just drops this one pushed
		// update for this client rather than buffering/retrying - a
		// real per-client write queue (for slow readers under load) is
		// a later refinement, not needed to prove push works at all.
		ssize_t ret = ::send (sock, frame.data (), frame.size (), MSG_NOSIGNAL);
		if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			closed = true;
	}
};

/**
 * Result of a worker-thread preview generation (STATUS.md task 5),
 * queued up for the main thread to pick up and finally answer the
 * (suspended) HTTP request that asked for it.
 */
struct PreviewResult
{
	struct MHD_Connection *connection;
	bool ok;
	std::string jpegData;
	std::string errorMsg;
};

#ifdef WEB_HAVE_DB
/**
 * Result of a worker-thread DB query (STATUS.md task 7, added after
 * lascaux.asu.cas.cz testing showed why this can't stay synchronous -
 * see the long comment by handleDb() below). The job itself decides the
 * final JSON body and HTTP status (success or error), so - unlike
 * PreviewResult - there's nothing left for the main thread to branch on
 * beyond "send exactly this".
 */
struct DbResult
{
	struct MHD_Connection *connection;
	std::string body;
	unsigned int httpStatus;
};
#endif

/**
 * Generic DevClient used for every connected device (see
 * HttpD::createOtherType()). Unlike classic's per-device-type
 * XmlDevClient/XmlDevTelescopeClient/XmlDevFocusClient split, web has no
 * camera-specific or telescope-specific behavior to add here (no image
 * writing, no script execution - that's execcli/scriptexec's job, not a
 * monitoring daemon's), so one class overriding the base DevClient's
 * valueChanged()/stateChanged() hooks covers every device type. Method
 * bodies are defined after HttpD below - they call back into it
 * (broadcastValue/broadcastState), so they need its complete type.
 */
class HttpDevClient:public DevClient
{
	public:
		HttpDevClient (Connection *_conn, HttpD *_master):DevClient (_conn), master (_master) {}

		virtual void valueChanged (Value *value);
		virtual void stateChanged (ServerState *state);

	private:
		HttpD *master;
};

// STATUS.md task 7: which class HttpD actually extends is a compile-time
// choice, matching classic src/httpd/httpd.h's own #ifdef RTS2_HAVE_PGSQL
// split - DeviceDb layers DB connection handling (--database/--config
// CLI options, initDB() during init()) on top of the exact same Device
// base used in the no-DB build.
#ifdef WEB_HAVE_DB
typedef rts2db::DeviceDb HttpDBase;
#else
typedef Device HttpDBase;
#endif

class HttpD:public HttpDBase
{
	public:
		HttpD (int argc, char **argv);
		virtual ~HttpD ();

		virtual void addPollSocks ();
		virtual void pollSuccess ();

		// Every connected device gets the generic HttpDevClient above,
		// regardless of type - see its class comment for why that's
		// enough here (no camera/telescope-specific behavior needed).
		virtual DevClient *createOtherType (Connection *conn, int)
		{
			return new HttpDevClient (conn, this);
		}

		// Called from HttpDevClient - push a value/state change to every
		// open WebSocket client. No-op (and cheap to check) if nothing
		// is currently connected.
		void broadcastValue (const char *device, Value *value);
		void broadcastState (const char *device, Connection *conn);
		void broadcastMessage (Message &msg);

	protected:
		virtual int processOption (int in_opt);
		virtual int init ();
		virtual int info () { return 0; }

		// Bounded ring buffer of recent messages, exposed at
		// /api/messages - same idea as classic's src/httpd/httpd.h
		// std::deque<Message> messages (there, the bound was a runtime-
		// configurable messageBufferSize Value; here it's a fixed
		// constant for now, simpler and enough until something needs it
		// tunable).
		virtual void message (Message &msg);

		// Block::willConnect() defaults to "don't connect" - a device
		// only opens direct peer connections it actually needs. HttpD's
		// whole job is observing every device, so it opts into all of
		// them (ported from classic src/httpd/httpd.cpp's non-PGSQL
		// HttpD::willConnect(): lower-type, or same-type-lower-name,
		// initiates - keeps two devices from both dialing each other).
		virtual int willConnect (NetworkAddress *_addr)
		{
			if (_addr->getType () < getDeviceType ()
				|| (_addr->getType () == getDeviceType () && strcmp (_addr->getName (), getDeviceName ()) < 0))
				return 1;
			return 0;
		}

	private:
		int httpPort;
		// Empty (default) binds all interfaces, same as before this
		// option existed. Set via --bind-address to restrict to e.g.
		// 127.0.0.1 - useful both for the "fronted by a proxy, never
		// reachable directly" deployment model, and for testing this
		// daemon on a shared/production host without exposing an
		// unauthenticated HTTP port on every interface in the meantime
		// (found genuinely necessary, not just theoretical, the first
		// time this was tried against a real production machine -
		// lascaux.asu.cas.cz - which has no per-daemon firewalling to
		// fall back on).
		std::string bindAddress;
		struct MHD_Daemon *mhd;
		int mhdEpollFd;

		// STATUS.md task 3: on-disk thumbnail/preview cache. No
		// production default yet (nothing in rts2ng has a settled
		// archive-root convention to inherit) - both must be set
		// explicitly for /preview/ to work; left empty, it 404s rather
		// than guessing a path.
		std::string imagesDir;
		std::string cacheDir;

		// STATUS.md task 6: local (non-DB) auth, gating /api/set,inc,dec.
		// Deliberately open (no auth check at all) when authFile is
		// empty - matches the images-dir/cache-dir "empty = feature not
		// enabled" convention above, and matches how every earlier task
		// in this session was actually tested. Not a silent default:
		// flagged explicitly in STATUS.md as a site-operator decision to
		// confirm before real deployment, not an oversight.
		std::string authFile;
		UserLogins userLogins;

		// STATUS.md task 8: the static frontend (web/static/) - served
		// by the daemon itself directly (MHD_create_response_from_fd,
		// zero-copy), matching the "daemon is fully self-sufficient,
		// proxy is opportunistic, not required" deployment decision. "/"
		// maps to "<staticDir>/index.html". Empty (unset) disables this
		// route entirely, same convention as images-dir/cache-dir/
		// auth-file.
		std::string staticDir;

		static const size_t maxMessages = 200;
		std::deque <Message> recentMessages;

		// STATUS.md task 4: WebSocket push. wsClients isn't touched by
		// addPollSocks()/pollSuccess() incrementally - like every other
		// Block source, it's re-added into the shared pollfd array from
		// scratch every poll cycle (Block::addPollSocks() resets
		// npolls=0 first), so both methods must walk the whole list
		// every time, not just at connect time.
		std::list <WsClient *> wsClients;

		// STATUS.md task 5: worker thread pool for cache-miss preview
		// generation, so it can't stall bus traffic on the main thread.
		// wakeupFd is an eventfd, registered in Block's poll set exactly
		// like mhdEpollFd/WS client sockets - a worker thread writes to
		// it (via wakeup(), thread-safe: eventfd writes are atomic at
		// the kernel level) after pushing a result, so the main thread's
		// pollSuccess() notices and drains previewResults.
		int previewWorkers;
		int wakeupFd;
		WorkerPool *workerPool;
		std::mutex previewResultsMutex;
		std::queue <PreviewResult> previewResults;

		void wakeup ();
		void drainPreviewResults ();

		// Builds and queues the actual MHD_Response for a preview result
		// (JPEG on success, JSON error otherwise) - shared between the
		// synchronous cache-hit path and the async worker-completion
		// path in drainPreviewResults().
		MHD_Result sendPreviewResponse (struct MHD_Connection *connection, bool ok, const std::string &jpegData, const std::string &errorMsg);

#ifdef WEB_HAVE_DB
		// STATUS.md task 7: DB queries also run on workerPool now, not
		// inline - see handleDb()'s comment for why this stopped being
		// optional. Shares the same pool/wakeupFd as preview generation
		// (STATUS.md task 5) rather than a second pool - both are
		// occasional, bursty background work, not worth a dedicated pool
		// each until real contention between the two shows up.
		std::mutex dbResultsMutex;
		std::queue <DbResult> dbResults;

		void drainDbResults ();
		MHD_Result handleDb (struct MHD_Connection *connection, const char *url);
#endif

		// STATUS.md task 6: HTTP Basic Auth (RFC 7617) against userLogins,
		// gating write access to a specific device. Returns true if the
		// request may proceed; false (with errorMsg filled) if it
		// doesn't - the caller is expected to respond via
		// sendUnauthorized() in that case, same pattern as ApiError/
		// sendPreviewResponse above.
		bool checkWriteAuth (struct MHD_Connection *connection, const char *device, std::string &errorMsg);
		MHD_Result sendUnauthorized (struct MHD_Connection *connection, const char *errorMsg);

		static MHD_Result answer (void *cls, struct MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **req_cls);

		MHD_Result handleRequest (struct MHD_Connection *connection, const char *url);
		MHD_Result handlePreview (struct MHD_Connection *connection, const char *relPath);

		// STATUS.md task 8: serves a file under staticDir if one exists
		// for url, filling result and returning true; returns false
		// (result untouched) if staticDir is unset or nothing matches,
		// so the caller falls through to the normal 404 path - this is
		// deliberately a "did I handle it" bool, not an MHD_Result,
		// since "no static file here" isn't itself an error worth a
		// response of its own.
		bool handleStatic (struct MHD_Connection *connection, const char *url, MHD_Result &result);
		MHD_Result handleWsUpgrade (struct MHD_Connection *connection);

		static void wsUpgradeCallback (void *cls, struct MHD_Connection *connection, void *req_cls, const char *extra_in, size_t extra_in_size, MHD_socket sock, struct MHD_UpgradeResponseHandle *urh);
		void wsClientConnected (const char *extra_in, size_t extra_in_size, MHD_socket sock, struct MHD_UpgradeResponseHandle *urh);
		void wsClientReadable (WsClient *c);

		// Shared "d" (device name) parameter lookup used by get/set/inc/
		// dec - ported from the isCentraldName()/getOpenConnection()
		// pairing classic's api.cpp repeats at every one of those call
		// sites without a shared helper; worth having one here since
		// web will keep needing it.
		Connection *findDeviceConnection (const char *device)
		{
			if (isCentraldName (device))
				return getSingleCentralConn ();
			return getOpenConnection (device);
		}
};

}

using namespace rts2web;

HttpD::HttpD (int argc, char **argv):HttpDBase (argc, argv, DEVICE_TYPE_HTTPD, "HTTPD")
{
	httpPort = 8889;
	mhd = nullptr;
	mhdEpollFd = -1;
	previewWorkers = 2;
	wakeupFd = -1;
	workerPool = nullptr;

	addOption ('p', "port", 1, "HTTP port to listen on (default 8889)");
	addOption (OPT_IMAGES_DIR, "images-dir", 1, "root of the image archive on disk (enables /preview/)");
	addOption (OPT_CACHE_DIR, "cache-dir", 1, "root of the on-disk preview cache (defaults to <images-dir>/.cache)");
	addOption (OPT_PREVIEW_WORKERS, "preview-workers", 1, "worker threads for preview generation (default 2)");
	addOption (OPT_AUTH_FILE, "auth-file", 1, "credentials file gating /api/set,inc,dec (user:cryptedpass:perms lines, crypt(3) hashes) - writes are unrestricted if not set");
	addOption (OPT_STATIC_DIR, "static-dir", 1, "root of the static web frontend (web/static/) - disabled if not set");
	addOption (OPT_BIND_ADDRESS, "bind-address", 1, "restrict the HTTP port to this address (e.g. 127.0.0.1) - binds all interfaces if not set");
}

HttpD::~HttpD ()
{
	// Destroy the pool first - its destructor blocks until every worker
	// thread has exited, so nothing can still be running (and possibly
	// calling wakeup()) once wakeupFd/wsClients/mhd start being torn
	// down below.
	delete workerPool;
	if (wakeupFd >= 0)
		close (wakeupFd);

	for (std::list <WsClient *>::iterator iter = wsClients.begin (); iter != wsClients.end (); iter++)
	{
		MHD_upgrade_action ((*iter)->urh, MHD_UPGRADE_ACTION_CLOSE);
		delete *iter;
	}
	if (mhd)
		MHD_stop_daemon (mhd);
}

void HttpDevClient::valueChanged (Value *value)
{
	master->broadcastValue (getConnection ()->getName (), value);
	DevClient::valueChanged (value);
}

void HttpDevClient::stateChanged (ServerState *state)
{
	master->broadcastState (getConnection ()->getName (), getConnection ());
	DevClient::stateChanged (state);
}

int HttpD::processOption (int in_opt)
{
	switch (in_opt)
	{
		case 'p':
			httpPort = atoi (optarg);
			break;
		case OPT_IMAGES_DIR:
			imagesDir = optarg;
			break;
		case OPT_CACHE_DIR:
			cacheDir = optarg;
			break;
		case OPT_PREVIEW_WORKERS:
			previewWorkers = atoi (optarg);
			if (previewWorkers < 1)
				previewWorkers = 1;
			break;
		case OPT_AUTH_FILE:
			authFile = optarg;
			break;
		case OPT_STATIC_DIR:
			staticDir = optarg;
			break;
		case OPT_BIND_ADDRESS:
			bindAddress = optarg;
			break;
		default:
			return HttpDBase::processOption (in_opt);
	}
	return 0;
}

int HttpD::init ()
{
	// When WEB_HAVE_DB, this is DeviceDb::init(): calls Device::init()
	// itself, then connects to the database (--database, default
	// "stars") - one call does both, no separate step needed here.
	int ret = HttpDBase::init ();
	if (ret)
		return ret;

	if (!imagesDir.empty ())
	{
		if (cacheDir.empty ())
			cacheDir = imagesDir + "/.cache";
		logStream (MESSAGE_INFO) << "/preview/ enabled, images-dir " << imagesDir << ", cache-dir " << cacheDir << sendLog;
	}

	if (authFile.empty ())
	{
		logStream (MESSAGE_WARNING) << "no --auth-file set - /api/set,inc,dec are UNRESTRICTED (any client may write to any device)" << sendLog;
	}
	else
	{
		// A missing file loads as "no users" silently (see UserLogins::
		// load()'s doc comment) - genuinely malformed content in an
		// existing file is different: it's a real, operator-fixable
		// startup error for a security-relevant file, so it refuses to
		// start rather than silently running with a truncated/empty
		// credential set the operator didn't intend.
		try
		{
			userLogins.load (authFile);
			logStream (MESSAGE_INFO) << "loaded auth file " << authFile << sendLog;
		}
		catch (rts2core::Error &er)
		{
			logStream (MESSAGE_ERROR) << "cannot load --auth-file " << authFile << ": " << er << sendLog;
			return -1;
		}
	}

	if (!staticDir.empty ())
		logStream (MESSAGE_INFO) << "serving static frontend from " << staticDir << sendLog;

	// MHD_ALLOW_UPGRADE is required for MHD_create_response_for_upgrade()/
	// MHD_UPGRADE_ACTION to work at all (STATUS.md task 4, WebSocket) -
	// without it MHD silently can't process a 101 Switching Protocols
	// response; found by testing a real handshake and getting a
	// connection that accepted bytes but sent nothing back at all, not
	// even an error.
	//
	// MHD_OPTION_SOCK_ADDR (not the newer MHD_OPTION_SOCK_ADDR_LEN) for
	// the same portability reason as the basic-auth function below:
	// _LEN needs libmicrohttpd >= 0.9.77.06, not present on lascaux's
	// 0.9.75.
	struct sockaddr_in bindAddr;
	if (!bindAddress.empty ())
	{
		memset (&bindAddr, 0, sizeof (bindAddr));
		bindAddr.sin_family = AF_INET;
		bindAddr.sin_port = htons (httpPort);
		if (inet_pton (AF_INET, bindAddress.c_str (), &bindAddr.sin_addr) != 1)
		{
			logStream (MESSAGE_ERROR) << "invalid --bind-address " << bindAddress << " (must be a plain IPv4 address)" << sendLog;
			return -1;
		}
		mhd = MHD_start_daemon (MHD_USE_EPOLL | MHD_ALLOW_UPGRADE, httpPort, nullptr, nullptr, &HttpD::answer, this, MHD_OPTION_SOCK_ADDR, (struct sockaddr *) &bindAddr, MHD_OPTION_END);
	}
	else
	{
		mhd = MHD_start_daemon (MHD_USE_EPOLL | MHD_ALLOW_UPGRADE, httpPort, nullptr, nullptr, &HttpD::answer, this, MHD_OPTION_END);
	}
	if (mhd == nullptr)
	{
		logStream (MESSAGE_ERROR) << "cannot start HTTP server on port " << httpPort << (bindAddress.empty () ? "" : (" bound to " + bindAddress)) << sendLog;
		return -1;
	}

	const union MHD_DaemonInfo *dinfo = MHD_get_daemon_info (mhd, MHD_DAEMON_INFO_EPOLL_FD);
	if (dinfo == nullptr)
	{
		logStream (MESSAGE_ERROR) << "cannot retrieve libmicrohttpd epoll fd" << sendLog;
		return -1;
	}
	mhdEpollFd = dinfo->epoll_fd;

	// Without this, message() is never called - centrald only forwards
	// its log broadcast to connections that opted in. Ported from
	// classic src/httpd/httpd.cpp's HttpD::init(); found by testing
	// /api/messages and noticing it stayed empty even while centrald's
	// own log kept producing real MESSAGE_INFO/MESSAGE_ERROR lines.
	setMessageMask (MESSAGE_MASK_ALL);

	wakeupFd = eventfd (0, EFD_NONBLOCK);
	if (wakeupFd < 0)
	{
		logStream (MESSAGE_ERROR) << "cannot create wakeup eventfd" << sendLog;
		return -1;
	}
	workerPool = new WorkerPool (previewWorkers, [this] () { wakeup (); });

	logStream (MESSAGE_INFO) << "HTTP server listening on port " << httpPort << ", " << previewWorkers << " preview worker thread(s)" << sendLog;

	return 0;
}

void HttpD::wakeup ()
{
	// Called from a worker thread. eventfd writes are atomic at the
	// kernel level, so this needs no locking of its own - the actual
	// cross-thread handoff is previewResultsMutex, guarding
	// previewResults itself.
	uint64_t one = 1;
	ssize_t ret = write (wakeupFd, &one, sizeof (one));
	(void) ret;						 // best-effort wakeup; a missed write just means the
										 // next unrelated poll wakeup will still drain the queue
}

void HttpD::message (Message &msg)
{
	Device::message (msg);
	recentMessages.push_back (msg);
	while (recentMessages.size () > maxMessages)
		recentMessages.pop_front ();
	broadcastMessage (msg);
}

void HttpD::addPollSocks ()
{
	Device::addPollSocks ();
	if (mhdEpollFd >= 0)
		addPollFD (mhdEpollFd, POLLIN);
	if (wakeupFd >= 0)
		addPollFD (wakeupFd, POLLIN);
	for (std::list <WsClient *>::iterator iter = wsClients.begin (); iter != wsClients.end (); iter++)
		if (!(*iter)->closed)
			addPollFD ((*iter)->sock, POLLIN);
}

void HttpD::pollSuccess ()
{
	Device::pollSuccess ();
	// MHD_run() drains and dispatches everything ready on its epoll set;
	// it's safe (a no-op) to call even if nothing beyond the registered
	// fd actually fired.
	if (mhdEpollFd >= 0 && isForRead (mhdEpollFd))
		MHD_run (mhd);

	if (wakeupFd >= 0 && isForRead (wakeupFd))
	{
		uint64_t val;
		while (read (wakeupFd, &val, sizeof (val)) > 0)
			;							 // drain the eventfd counter (non-blocking fd - the loop
										 // ends on EAGAIN once it's empty)
		drainPreviewResults ();
#ifdef WEB_HAVE_DB
		drainDbResults ();
#endif
	}

	for (std::list <WsClient *>::iterator iter = wsClients.begin (); iter != wsClients.end (); )
	{
		WsClient *c = *iter;
		if (!c->closed && isForRead (c->sock))
			wsClientReadable (c);

		if (c->closed)
		{
			// Ends this daemon's ownership of the raw socket - MHD
			// takes over closing it from here, the application must not
			// close(sock) itself.
			MHD_upgrade_action (c->urh, MHD_UPGRADE_ACTION_CLOSE);
			delete c;
			iter = wsClients.erase (iter);
		}
		else
		{
			iter++;
		}
	}
}

void HttpD::broadcastMessage (Message &msg)
{
	if (wsClients.empty ())
		return;
	std::ostringstream os;
	os << "{\"event\":\"message\",\"time\":" << msg.getMessageTime () << ",\"device\":";
	jsonString (msg.getMessageOName (), os);
	os << ",\"type\":" << msg.getType () << ",\"text\":";
	jsonString (msg.getMessageString ().c_str (), os);
	os << "}";
	std::string frame = wsEncodeTextFrame (os.str ());
	for (std::list <WsClient *>::iterator iter = wsClients.begin (); iter != wsClients.end (); iter++)
		(*iter)->sendFrame (frame);
}

void HttpD::broadcastValue (const char *device, Value *value)
{
	if (wsClients.empty ())
		return;
	std::ostringstream os;
	os << "{\"event\":\"value\",\"device\":";
	jsonString (device, os);
	os << ",\"v\":{";
	jsonValue (value, os);
	os << "}}";
	std::string frame = wsEncodeTextFrame (os.str ());
	for (std::list <WsClient *>::iterator iter = wsClients.begin (); iter != wsClients.end (); iter++)
		(*iter)->sendFrame (frame);
}

void HttpD::broadcastState (const char *device, Connection *conn)
{
	if (wsClients.empty ())
		return;
	std::ostringstream os;
	os << "{\"event\":\"state\",\"device\":";
	jsonString (device, os);
	os << ",\"value\":" << conn->getState () << ",\"statestring\":";
	jsonString (conn->getStateString (true).c_str (), os);
	os << "}";
	std::string frame = wsEncodeTextFrame (os.str ());
	for (std::list <WsClient *>::iterator iter = wsClients.begin (); iter != wsClients.end (); iter++)
		(*iter)->sendFrame (frame);
}

MHD_Result HttpD::answer (void *cls, struct MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **req_cls)
{
	return ((HttpD *) cls)->handleRequest (connection, url);
}

MHD_Result HttpD::handleWsUpgrade (struct MHD_Connection *connection)
{
	const char *key = MHD_lookup_connection_value (connection, MHD_HEADER_KIND, "Sec-WebSocket-Key");
	const char *upgradeHdr = MHD_lookup_connection_value (connection, MHD_HEADER_KIND, "Upgrade");
	const char *version = MHD_lookup_connection_value (connection, MHD_HEADER_KIND, "Sec-WebSocket-Version");

	if (!key || !upgradeHdr || strcasecmp (upgradeHdr, "websocket") || !version || strcmp (version, "13"))
	{
		static const char *msg = "{\"error\":\"not a websocket upgrade request\"}";
		struct MHD_Response *response = MHD_create_response_from_buffer (strlen (msg), (void *) msg, MHD_RESPMEM_PERSISTENT);
		MHD_add_response_header (response, "Content-Type", "application/json");
		MHD_Result ret = MHD_queue_response (connection, MHD_HTTP_BAD_REQUEST, response);
		MHD_destroy_response (response);
		return ret;
	}

	std::string accept = wsAcceptKey (key);

	// MHD adds its own "Connection: Upgrade" for a 101 response - adding
	// one here too just duplicates the header value, found by checking
	// the raw response bytes of a real handshake.
	struct MHD_Response *response = MHD_create_response_for_upgrade (&HttpD::wsUpgradeCallback, this);
	MHD_add_response_header (response, "Upgrade", "websocket");
	MHD_add_response_header (response, "Sec-WebSocket-Accept", accept.c_str ());
	MHD_Result ret = MHD_queue_response (connection, MHD_HTTP_SWITCHING_PROTOCOLS, response);
	MHD_destroy_response (response);
	return ret;
}

void HttpD::wsUpgradeCallback (void *cls, struct MHD_Connection *connection, void *req_cls, const char *extra_in, size_t extra_in_size, MHD_socket sock, struct MHD_UpgradeResponseHandle *urh)
{
	((HttpD *) cls)->wsClientConnected (extra_in, extra_in_size, sock, urh);
}

void HttpD::wsClientConnected (const char *extra_in, size_t extra_in_size, MHD_socket sock, struct MHD_UpgradeResponseHandle *urh)
{
	WsClient *c = new WsClient (sock, urh);
	if (extra_in_size > 0)
		c->recvBuffer.append (extra_in, extra_in_size);
	wsClients.push_back (c);
	logStream (MESSAGE_DEBUG) << "WebSocket client connected, fd " << sock << sendLog;
}

void HttpD::wsClientReadable (WsClient *c)
{
	unsigned char buf[4096];
	ssize_t ret = recv (c->sock, buf, sizeof (buf), 0);
	if (ret < 0)
	{
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			c->closed = true;
		return;
	}
	if (ret == 0)
	{
		c->closed = true;
		return;
	}

	c->recvBuffer.append ((const char *) buf, ret);

	while (true)
	{
		int opcode = 0;
		std::string payload;
		size_t consumed = wsConsumeClientFrame ((const unsigned char *) c->recvBuffer.data (), c->recvBuffer.size (), opcode, payload);
		if (consumed == 0)
			break;

		c->recvBuffer.erase (0, consumed);

		if (opcode == 0x8)
		{
			// close
			c->closed = true;
			break;
		}
		else if (opcode == 0x9)
		{
			// ping -> pong (small control-frame payloads only, per spec)
			std::string pong;
			pong.push_back ((char) 0x8A);
			pong.push_back ((char) payload.size ());
			pong.append (payload);
			c->sendFrame (pong);
		}
		// text/binary/pong/continuation from the client: this is a
		// push-only channel, there's no application use for client-sent
		// data - the frame is already consumed above, just move on.
	}
}

bool HttpD::checkWriteAuth (struct MHD_Connection *connection, const char *device, std::string &errorMsg)
{
	if (authFile.empty ())
		return true;					 // no auth configured - see the authFile member's doc comment

	// Deliberately the older MHD_basic_auth_get_username_password() (a
	// plain char* out-param pair), not the newer, struct-based
	// MHD_basic_auth_get_username_password3() - found while deploying to
	// lascaux.asu.cas.cz (real production libmicrohttpd 0.9.75, vs.
	// 1.0.1 on the dev machine this was first built against): the "3"
	// variant was only added in 0.9.77 and doesn't exist at all on
	// 0.9.75, while this older one is still present (just deprecated,
	// not removed) even in 1.0.1 - the more portable choice across the
	// range of libmicrohttpd versions real sites actually run.
	char *password_c = nullptr;
	char *username_c = MHD_basic_auth_get_username_password (connection, &password_c);
	if (username_c == nullptr)
	{
		if (password_c)
			MHD_free (password_c);
		errorMsg = "authentication required";
		return false;
	}

	std::string username (username_c);
	std::string password (password_c ? password_c : "");
	MHD_free (username_c);
	if (password_c)
		MHD_free (password_c);

	UserPermissions perms;
	if (!userLogins.verifyUser (username, password, &perms))
	{
		errorMsg = "invalid credentials";
		return false;
	}
	if (!perms.canWriteDevice (device))
	{
		errorMsg = "not authorized to write to this device";
		return false;
	}
	return true;
}

MHD_Result HttpD::sendUnauthorized (struct MHD_Connection *connection, const char *errorMsg)
{
	std::ostringstream os;
	os << "{\"error\":";
	jsonString (errorMsg, os);
	os << "}";
	std::string body = os.str ();
	struct MHD_Response *response = MHD_create_response_from_buffer (body.length (), (void *) body.c_str (), MHD_RESPMEM_MUST_COPY);
	MHD_add_response_header (response, "Content-Type", "application/json");
	// Prompts a browser's native login dialog; curl/API clients just see
	// a 401 with this header and know to retry with -u user:pass.
	MHD_add_response_header (response, "WWW-Authenticate", "Basic realm=\"rts2-httpd\"");
	MHD_Result ret = MHD_queue_response (connection, MHD_HTTP_UNAUTHORIZED, response);
	MHD_destroy_response (response);
	return ret;
}

MHD_Result HttpD::sendPreviewResponse (struct MHD_Connection *connection, bool ok, const std::string &jpegData, const std::string &errorMsg)
{
	struct MHD_Response *response;
	unsigned int httpStatus;

	if (ok)
	{
		response = MHD_create_response_from_buffer (jpegData.length (), (void *) jpegData.data (), MHD_RESPMEM_MUST_COPY);
		MHD_add_response_header (response, "Content-Type", "image/jpeg");
		httpStatus = MHD_HTTP_OK;
	}
	else
	{
		std::ostringstream os;
		os << "{\"error\":";
		jsonString (errorMsg.c_str (), os);
		os << "}";
		std::string body = os.str ();
		response = MHD_create_response_from_buffer (body.length (), (void *) body.c_str (), MHD_RESPMEM_MUST_COPY);
		MHD_add_response_header (response, "Content-Type", "application/json");
		httpStatus = MHD_HTTP_NOT_FOUND;
	}

	MHD_Result ret = MHD_queue_response (connection, httpStatus, response);
	MHD_destroy_response (response);
	return ret;
}

bool HttpD::handleStatic (struct MHD_Connection *connection, const char *url, MHD_Result &result)
{
	if (staticDir.empty ())
		return false;

	std::string relPath = !strcmp (url, "/") ? "index.html" : (url[0] == '/' ? url + 1 : url);

	// Same two-layer defense as preview.cpp's looksSafe()/canonical
	// check: reject ".." segments before touching the filesystem, then
	// independently confirm the resolved path is still inside staticDir.
	if (relPath.empty () || relPath.find ("..") != std::string::npos)
		return false;

	std::string fullPath = staticDir + "/" + relPath;

	std::error_code ec;
	std::filesystem::path canonicalFull = std::filesystem::weakly_canonical (fullPath, ec);
	std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical (staticDir, ec);
	std::string cf = canonicalFull.string ();
	std::string cr = canonicalRoot.string ();
	if (ec || cf.compare (0, cr.size (), cr) != 0)
		return false;

	int fd = open (fullPath.c_str (), O_RDONLY);
	if (fd < 0)
		return false;

	struct stat st;
	if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode))
	{
		close (fd);
		return false;
	}

	struct MHD_Response *response = MHD_create_response_from_fd (st.st_size, fd);
	MHD_add_response_header (response, "Content-Type", staticContentType (relPath));
	result = MHD_queue_response (connection, MHD_HTTP_OK, response);
	MHD_destroy_response (response);
	return true;
}

MHD_Result HttpD::handlePreview (struct MHD_Connection *connection, const char *relPath)
{
	if (imagesDir.empty ())
		return sendPreviewResponse (connection, false, "", "images-dir not configured");

	int previewSize = atoi (getParam (connection, "ps", "128"));
	if (previewSize <= 0)
		previewSize = 128;
	float quantiles = atof (getParam (connection, "q", "0.005"));
	std::string relPathStr (relPath);

	// Cache-hit (or invalid-path/missing-source) is cheap - a stat()
	// plus maybe a file read - and stays inline on the main thread, per
	// STATUS.md task 5's design: only cache-miss regeneration needs
	// offloading.
	std::string jpegDataOrError;
	PreviewStatus status = checkPreviewCache (imagesDir, cacheDir, relPathStr, previewSize, quantiles, jpegDataOrError);

	if (status == PreviewStatus::Hit)
		return sendPreviewResponse (connection, true, jpegDataOrError, "");
	if (status == PreviewStatus::Invalid)
		return sendPreviewResponse (connection, false, "", jpegDataOrError);

	// Miss: suspend this connection and hand the actual FITS decode +
	// JPEG encode off to the worker pool, so a burst of misses can't
	// stall bus traffic on the main thread. The job closure captures its
	// own copies of every string it needs - none of imagesDir/cacheDir/
	// relPathStr are touched again from the main thread while this job
	// is in flight, but copying is simpler to reason about than sharing
	// and costs nothing measurable next to a FITS decode.
	MHD_suspend_connection (connection);

	std::string imagesDirCopy = imagesDir;
	std::string cacheDirCopy = cacheDir;
	workerPool->submit ([this, connection, imagesDirCopy, cacheDirCopy, relPathStr, previewSize, quantiles] ()
	{
		PreviewResult r;
		r.connection = connection;
		r.ok = generatePreview (imagesDirCopy, cacheDirCopy, relPathStr, previewSize, quantiles, r.jpegData, r.errorMsg);
		{
			std::lock_guard <std::mutex> lock (previewResultsMutex);
			previewResults.push (std::move (r));
		}
	});

	return MHD_YES;
}

void HttpD::drainPreviewResults ()
{
	std::vector <PreviewResult> results;
	{
		std::lock_guard <std::mutex> lock (previewResultsMutex);
		while (!previewResults.empty ())
		{
			results.push_back (std::move (previewResults.front ()));
			previewResults.pop ();
		}
	}

	if (results.empty ())
		return;

	for (std::vector <PreviewResult>::iterator iter = results.begin (); iter != results.end (); iter++)
	{
		sendPreviewResponse (iter->connection, iter->ok, iter->jpegData, iter->errorMsg);
		MHD_resume_connection (iter->connection);
	}

	// Required in external-polling mode per MHD_resume_connection()'s
	// own documentation: the resume doesn't take effect - and the
	// queued response doesn't actually go out - until MHD_run() is
	// called again, which won't happen on its own just because
	// mhdEpollFd fired (resuming isn't itself an epoll event).
	MHD_run (mhd);
}

#ifdef WEB_HAVE_DB
void HttpD::drainDbResults ()
{
	std::vector <DbResult> results;
	{
		std::lock_guard <std::mutex> lock (dbResultsMutex);
		while (!dbResults.empty ())
		{
			results.push_back (std::move (dbResults.front ()));
			dbResults.pop ();
		}
	}

	if (results.empty ())
		return;

	for (std::vector <DbResult>::iterator iter = results.begin (); iter != results.end (); iter++)
	{
		struct MHD_Response *response = MHD_create_response_from_buffer (iter->body.length (), (void *) iter->body.c_str (), MHD_RESPMEM_MUST_COPY);
		MHD_add_response_header (response, "Content-Type", "application/json");
		MHD_queue_response (iter->connection, iter->httpStatus, response);
		MHD_destroy_response (response);
		MHD_resume_connection (iter->connection);
	}

	// Same requirement as drainPreviewResults() - see its comment.
	MHD_run (mhd);
}

// STATUS.md task 7: DB queries run on workerPool, not inline on the main
// thread, as of 2026-08-17 - the original first-slice implementation
// answered these synchronously (fine against the sparse local test
// database used to build it), but live-tested against
// lascaux.asu.cas.cz's real production database (15,753 targets, not
// 121), a single /api/db/targets request took ~14.7 seconds - and
// during that entire window, /api/devices (normally sub-millisecond,
// pure in-memory) *also* took ~14 seconds, confirmed by firing it
// concurrently from a second connection. The whole daemon, including
// live bus/device traffic, was completely blocked for the duration of
// one DB query - exactly the risk STATUS.md's task 7 write-up flagged
// as "worth measuring before deciding, not assuming" and left
// unresolved. Now measured, on the real dataset this daemon actually
// has to serve, not assumed.
MHD_Result HttpD::handleDb (struct MHD_Connection *connection, const char *url)
{
	if (!strcmp (url, "/api/db/current-night"))
	{
		// No DB access - answered inline, no worker pool/suspend needed.
		std::ostringstream os;
		dbCurrentNight (os);
		std::string body = os.str ();
		struct MHD_Response *response = MHD_create_response_from_buffer (body.length (), (void *) body.c_str (), MHD_RESPMEM_MUST_COPY);
		MHD_add_response_header (response, "Content-Type", "application/json");
		MHD_Result ret = MHD_queue_response (connection, MHD_HTTP_OK, response);
		MHD_destroy_response (response);
		return ret;
	}

	bool wantTarget = !strcmp (url, "/api/db/target");
	bool wantObservations = !strcmp (url, "/api/db/observations");

	if (!strcmp (url, "/api/db/targets"))
	{
		MHD_suspend_connection (connection);
		workerPool->submit ([this, connection] ()
		{
			DbResult r;
			r.connection = connection;
			std::ostringstream os;
			try
			{
				dbListTargets (os);
				r.body = os.str ();
				r.httpStatus = MHD_HTTP_OK;
			}
			catch (rts2core::Error &er)
			{
				std::ostringstream errText;
				errText << er;
				std::ostringstream errOs;
				errOs << "{\"error\":";
				jsonString (errText.str ().c_str (), errOs);
				errOs << "}";
				r.body = errOs.str ();
				r.httpStatus = MHD_HTTP_BAD_REQUEST;
			}
			{
				std::lock_guard <std::mutex> lock (dbResultsMutex);
				dbResults.push (std::move (r));
			}
			wakeup ();
		});
		return MHD_YES;
	}
	else if (wantTarget || wantObservations)
	{
		const char *idStr = getParam (connection, "id", "");
		if (idStr[0] == '\0')
		{
			static const char *msg = "{\"error\":\"missing id parameter\"}";
			struct MHD_Response *response = MHD_create_response_from_buffer (strlen (msg), (void *) msg, MHD_RESPMEM_PERSISTENT);
			MHD_add_response_header (response, "Content-Type", "application/json");
			MHD_Result ret = MHD_queue_response (connection, MHD_HTTP_BAD_REQUEST, response);
			MHD_destroy_response (response);
			return ret;
		}
		int targetId = atoi (idStr);

		MHD_suspend_connection (connection);
		workerPool->submit ([this, connection, targetId, wantObservations] ()
		{
			DbResult r;
			r.connection = connection;
			std::ostringstream os;
			try
			{
				if (wantObservations)
					dbListObservations (targetId, os);
				else
					dbGetTarget (targetId, os);
				r.body = os.str ();
				r.httpStatus = MHD_HTTP_OK;
			}
			catch (rts2core::Error &er)
			{
				std::ostringstream errText;
				errText << er;
				std::ostringstream errOs;
				errOs << "{\"error\":";
				jsonString (errText.str ().c_str (), errOs);
				errOs << "}";
				r.body = errOs.str ();
				r.httpStatus = MHD_HTTP_BAD_REQUEST;
			}
			{
				std::lock_guard <std::mutex> lock (dbResultsMutex);
				dbResults.push (std::move (r));
			}
			wakeup ();
		});
		return MHD_YES;
	}
	else if (!strcmp (url, "/api/db/nights") || !strcmp (url, "/api/db/night"))
	{
		bool wantDetail = !strcmp (url, "/api/db/night");
		int year = atoi (getParam (connection, "year", "-1"));
		int month = atoi (getParam (connection, "month", "-1"));
		int day = atoi (getParam (connection, "day", "-1"));

		if (wantDetail && (year <= 0 || month <= 0 || day <= 0))
		{
			static const char *msg = "{\"error\":\"year, month and day are all required for /api/db/night\"}";
			struct MHD_Response *response = MHD_create_response_from_buffer (strlen (msg), (void *) msg, MHD_RESPMEM_PERSISTENT);
			MHD_add_response_header (response, "Content-Type", "application/json");
			MHD_Result ret = MHD_queue_response (connection, MHD_HTTP_BAD_REQUEST, response);
			MHD_destroy_response (response);
			return ret;
		}

		MHD_suspend_connection (connection);
		workerPool->submit ([this, connection, year, month, day, wantDetail] ()
		{
			DbResult r;
			r.connection = connection;
			std::ostringstream os;
			try
			{
				if (wantDetail)
					dbNightDetail (year, month, day, os);
				else
					dbNightsSummary (year, month, day, os);
				r.body = os.str ();
				r.httpStatus = MHD_HTTP_OK;
			}
			catch (rts2core::Error &er)
			{
				std::ostringstream errText;
				errText << er;
				std::ostringstream errOs;
				errOs << "{\"error\":";
				jsonString (errText.str ().c_str (), errOs);
				errOs << "}";
				r.body = errOs.str ();
				r.httpStatus = MHD_HTTP_BAD_REQUEST;
			}
			{
				std::lock_guard <std::mutex> lock (dbResultsMutex);
				dbResults.push (std::move (r));
			}
			wakeup ();
		});
		return MHD_YES;
	}
	else if (!strcmp (url, "/api/db/images"))
	{
		const char *targetStr = getParam (connection, "target", "");
		int year = atoi (getParam (connection, "year", "-1"));
		int month = atoi (getParam (connection, "month", "-1"));
		int day = atoi (getParam (connection, "day", "-1"));

		bool byTarget = targetStr[0] != '\0';
		bool byNight = year > 0 && month > 0 && day > 0;

		if (!byTarget && !byNight)
		{
			static const char *msg = "{\"error\":\"pass either target=<id> or year=&month=&day= to search images\"}";
			struct MHD_Response *response = MHD_create_response_from_buffer (strlen (msg), (void *) msg, MHD_RESPMEM_PERSISTENT);
			MHD_add_response_header (response, "Content-Type", "application/json");
			MHD_Result ret = MHD_queue_response (connection, MHD_HTTP_BAD_REQUEST, response);
			MHD_destroy_response (response);
			return ret;
		}

		int targetId = byTarget ? atoi (targetStr) : 0;
		std::string imagesDirCopy = imagesDir;

		MHD_suspend_connection (connection);
		workerPool->submit ([this, connection, imagesDirCopy, byTarget, targetId, year, month, day] ()
		{
			DbResult r;
			r.connection = connection;
			std::ostringstream os;
			try
			{
				if (byTarget)
					dbSearchImagesByTarget (imagesDirCopy, targetId, os);
				else
					dbSearchImagesByNight (imagesDirCopy, year, month, day, os);
				r.body = os.str ();
				r.httpStatus = MHD_HTTP_OK;
			}
			catch (rts2core::Error &er)
			{
				std::ostringstream errText;
				errText << er;
				std::ostringstream errOs;
				errOs << "{\"error\":";
				jsonString (errText.str ().c_str (), errOs);
				errOs << "}";
				r.body = errOs.str ();
				r.httpStatus = MHD_HTTP_BAD_REQUEST;
			}
			{
				std::lock_guard <std::mutex> lock (dbResultsMutex);
				dbResults.push (std::move (r));
			}
			wakeup ();
		});
		return MHD_YES;
	}

	static const char *notFound = "{\"error\":\"not found\"}";
	struct MHD_Response *response = MHD_create_response_from_buffer (strlen (notFound), (void *) notFound, MHD_RESPMEM_PERSISTENT);
	MHD_add_response_header (response, "Content-Type", "application/json");
	MHD_Result ret = MHD_queue_response (connection, MHD_HTTP_NOT_FOUND, response);
	MHD_destroy_response (response);
	return ret;
}
#endif

MHD_Result HttpD::handleRequest (struct MHD_Connection *connection, const char *url)
{
	static const char *previewPrefix = "/preview/";
	if (!strncmp (url, previewPrefix, strlen (previewPrefix)))
		return handlePreview (connection, url + strlen (previewPrefix));

	if (!strcmp (url, "/ws"))
		return handleWsUpgrade (connection);

#ifdef WEB_HAVE_DB
	static const char *dbPrefix = "/api/db/";
	if (!strncmp (url, dbPrefix, strlen (dbPrefix)))
		return handleDb (connection, url);
#endif

	// STATUS.md task 8: static frontend files. Checked before the JSON
	// API dispatch below but harmless either way - handleStatic() only
	// returns true (taking over the response) when a real file actually
	// exists under staticDir for this url.
	MHD_Result staticResult;
	if (handleStatic (connection, url, staticResult))
		return staticResult;

	std::ostringstream os;
	unsigned int httpStatus = MHD_HTTP_OK;

	// STATUS.md task 2 endpoint set, ported from classic src/httpd/
	// api.cpp: device/value listing (devices, getall, get), set/inc/dec,
	// messages. Deliberately not ported yet: devbytype, selval,
	// deviceinfo and everything DB-bound (task 6/7 territory) - this is
	// the cheap in-memory subset the plan called out, not the full
	// classic surface.
	try
	{
		if (!strcmp (url, "/api/devices"))
		{
			os << "[";
			connections_t *conns = getConnections ();
			bool first = true;
			for (connections_t::iterator iter = conns->begin (); iter != conns->end (); iter++)
			{
				if (!first)
					os << ",";
				first = false;
				jsonString ((*iter)->getName (), os);
			}
			os << "]";
		}
		else if (!strcmp (url, "/api/getall"))
		{
			os << "{";
			bool first = true;

			Connection *cc = getSingleCentralConn ();
			if (cc)
			{
				jsonString ("centrald", os);
				os << ":";
				sendConnectionValues (cc, os);
				first = false;
			}

			for (connections_t::iterator iter = getConnections ()->begin (); iter != getConnections ()->end (); iter++)
			{
				if ((*iter)->getName ()[0] == '\0')
					continue;
				if (!first)
					os << ",";
				first = false;
				jsonString ((*iter)->getName (), os);
				os << ":";
				sendConnectionValues (*iter, os);
			}
			os << "}";
		}
		else if (!strcmp (url, "/api/get"))
		{
			const char *device = getParam (connection, "d", "");
			const char *variable = getParam (connection, "n", "");
			if (device[0] == '\0')
				throw ApiError ("missing d parameter");

			Connection *conn = findDeviceConnection (device);
			if (conn == nullptr)
				throw ApiError ("cannot find device with given name");

			if (variable[0] == '\0')
			{
				sendConnectionValues (conn, os);
			}
			else
			{
				Value *v = conn->getValue (variable);
				if (v == nullptr)
					throw ApiError ("cannot find variable");
				os << "{";
				jsonValue (v, os);
				os << "}";
			}
		}
		else if (!strcmp (url, "/api/set") || !strcmp (url, "/api/inc") || !strcmp (url, "/api/dec"))
		{
			const char *device = getParam (connection, "d", "");
			const char *variable = getParam (connection, "n", "");
			const char *value = getParam (connection, "v", "");
			if (device[0] == '\0' || variable[0] == '\0' || value[0] == '\0')
				throw ApiError ("missing d, n or v parameter");

			// STATUS.md task 6: only the write endpoints are gated - see
			// the authFile member's doc comment for why reads stay open.
			std::string authError;
			if (!checkWriteAuth (connection, device, authError))
				return sendUnauthorized (connection, authError.c_str ());

			char op = '=';
			if (!strcmp (url, "/api/inc"))
				op = '+';
			else if (!strcmp (url, "/api/dec"))
				op = '-';

			Connection *conn = findDeviceConnection (device);
			if (conn == nullptr)
				throw ApiError ("cannot find device with given name");
			if (conn->getValue (variable) == nullptr)
				throw ApiError ("cannot find variable");

			// Fire-and-forget: queue the command and report back the
			// (still old, pre-ack) value state immediately, rather than
			// holding the HTTP response open until the device
			// acknowledges. Classic's api.cpp defaults to the opposite
			// (an AsyncAPI holds the response until the ack arrives,
			// with fire-and-forget only via an explicit async=1 param) -
			// deferred here until the async-response infrastructure
			// those pushed-update calls need lands (STATUS.md tasks 4/5,
			// WebSocket push + the worker/wakeup plumbing).
			conn->queCommand (new CommandChangeValue (this, std::string (variable), op, std::string (value)));

			sendConnectionValues (conn, os);
		}
		else if (!strcmp (url, "/api/switchstate"))
		{
			// System-wide on/standby/off, the same "on"/"standby"/"off"
			// raw text commands classic's nmonitor.cpp sends to every
			// centrald connection (see Block::queAllCentralds()) - this
			// is not a per-device value write, it flips the whole
			// system's state, so gate it on "centrald" rather than a
			// specific device name.
			const char *newState = getParam (connection, "state", "");
			if (strcmp (newState, "on") && strcmp (newState, "standby") && strcmp (newState, "off"))
				throw ApiError ("state must be one of on, standby, off");

			std::string authError;
			if (!checkWriteAuth (connection, "centrald", authError))
				return sendUnauthorized (connection, authError.c_str ());

			queAllCentralds (newState);

			os << "{\"state\":";
			jsonString (newState, os);
			os << "}";
		}
		else if (!strcmp (url, "/api/messages"))
		{
			os << "[";
			bool first = true;
			for (std::deque <Message>::iterator iter = recentMessages.begin (); iter != recentMessages.end (); iter++)
			{
				if (!first)
					os << ",";
				first = false;
				os << "{\"time\":" << iter->getMessageTime () << ",\"device\":";
				jsonString (iter->getMessageOName (), os);
				os << ",\"type\":" << iter->getType () << ",\"text\":";
				jsonString (iter->getMessageString ().c_str (), os);
				os << "}";
			}
			os << "]";
		}
		else
		{
			httpStatus = MHD_HTTP_NOT_FOUND;
			os << "{\"error\":\"not found\"}";
		}
	}
	catch (ApiError &er)
	{
		httpStatus = MHD_HTTP_BAD_REQUEST;
		os.str ("");
		os << "{\"error\":";
		jsonString (er.what (), os);
		os << "}";
	}
#ifdef WEB_HAVE_DB
	// rts2db::createTarget()/TargetSet::load() report a nonexistent
	// target or a query-level DB problem by throwing rts2core::Error
	// (rts2db::SqlError derives from it) - same 400-with-JSON-body
	// treatment as ApiError above, not an uncaught exception.
	catch (rts2core::Error &er)
	{
		httpStatus = MHD_HTTP_BAD_REQUEST;
		os.str ("");
		std::ostringstream errText;
		errText << er;
		os << "{\"error\":";
		jsonString (errText.str ().c_str (), os);
		os << "}";
	}
#endif

	std::string body = os.str ();
	struct MHD_Response *response = MHD_create_response_from_buffer (body.length (), (void *) body.c_str (), MHD_RESPMEM_MUST_COPY);
	MHD_add_response_header (response, "Content-Type", "application/json");
	MHD_Result ret = MHD_queue_response (connection, httpStatus, response);
	MHD_destroy_response (response);
	return ret;
}

int main (int argc, char **argv)
{
	HttpD device (argc, argv);
	return device.run ();
}
