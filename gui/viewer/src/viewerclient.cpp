#include "gui/viewerclient.h"
#include "gui/viewercamera.h"

#include <app.h>
#include <command.h>
#include <configuration.h>
#include <connection.h>
#include <status.h>

#include <sstream>

using namespace gui;

#define OPT_DEVICE   OPT_LOCAL + 900
#define OPT_IMAGES   OPT_LOCAL + 901
#define OPT_MODES    OPT_LOCAL + 902

ViewerClient::ViewerClient (int argc, char **argv):
	rts2core::Client (argc, argv, "rts2-viewer")
{
	configFile = NULL;
	addOption (OPT_CONFIG, "config", 1, "configuration file");
	addOption (OPT_DEVICE, "device", 1, "name of the camera device to select initially (optional - every camera device is watched and can be picked from the camera selector regardless)");
	addOption (OPT_IMAGES, "images", 1, "expand-path expression for saved images (see image.h), overrides rts2.ini's [viewer] expand_path; default: %y%m%d%H%M%S-%s.fits, in the current directory");
	addOption (OPT_MODES, "modes", 1, "exposure-modes file (see gui/modes.h), overrides rts2.ini's [viewer] modes; default: built-in Light and Dark");

	// GUI requests are only picked up in idle(), which Block runs when a
	// network message wakes the poll loop or, failing that, every 10 s -
	// so a click took anything from under a second to 10 s to act,
	// depending on how chatty the devices happened to be, and two quick
	// Expose clicks within one wait merged into one exposure.
	setTimeout (USEC_SEC / 10);
}

int ViewerClient::processOption (int in_opt)
{
	switch (in_opt)
	{
		case OPT_CONFIG:
			configFile = optarg;
			break;
		case OPT_DEVICE:
			initialDevice = optarg;
			break;
		case OPT_IMAGES:
			imageExpandPath = optarg;
			break;
		case OPT_MODES:
			modesFile = optarg;
			break;
		default:
			return rts2core::Client::processOption (in_opt);
	}
	return 0;
}

int ViewerClient::init ()
{
	int ret = rts2core::Client::init ();
	if (ret)
		return ret;

	// Every RTS2 client loads rts2.ini itself (see e.g. scriptexec.cpp,
	// focusclient.cpp) - rts2core::Client::init() does not do this
	// generically. Without it, Configuration::instance() stays a
	// never-loaded empty singleton, and every per-device config lookup
	// (DevClientCameraImage's instrume/telescop/origin/template, kernel/src/
	// devcliimg.cpp) silently "cannot find section" for every camera,
	// forever - found via a real ORM deployment where a camera device
	// (WF0) triggered exactly that.
	rts2core::Configuration *config = rts2core::Configuration::instance ();
	ret = config->loadFile (configFile);
	if (ret)
	{
		std::cerr << "Cannot load configuration file '"
			<< (configFile ? configFile : "/etc/rts2/rts2.ini")
			<< "'" << std::endl;
		return ret;
	}

	// --images already set it; otherwise fall back to rts2.ini's own
	// [viewer] expand_path, same convention as [scriptexec] expand_path
	// (scriptexec.cpp) - and if neither is set, just a filename in the
	// directory the viewer was started from.
	//
	// Lowercase %y, not %Y: the expander has two families, actual date
	// (%y %m %d) and night date (%Y %O %D, or %N for all three). A night
	// belongs to the evening it started on, so %Y/%N/ is right for filing
	// the archive - but inside a timestamp %Y would pair the night's year
	// with the actual month and day, and a frame taken at 01:00 on Jan 1
	// would be named for Jan 1 of the year before.
	//
	// Deliberately not %b (observatoryBasePath, "/images/"): that is the
	// site archive, typically root-owned, so the default used to fail with
	// "cannot create directory ... Permission denied" for anyone running
	// the viewer as themselves - and when it did succeed it dropped ad-hoc
	// frames into the archive. Somewhere like FLORES, where the archive
	// really is where viewer frames belong, says so explicitly with
	// --images or [viewer] expand_path.
	if (imageExpandPath.empty ())
		imageExpandPath = config->getStringDefault ("viewer", "expand_path", "%y%m%d%H%M%S-%s.fits");

	if (modesFile.empty ())
		modesFile = config->getStringDefault ("viewer", "modes", "");
	emit modesFileResolved (QString::fromStdString (modesFile));

	scriptTarget = new rts2script::ScriptTarget (this);

	return 0;
}

ViewerClient::~ViewerClient ()
{
	delete scriptTarget;
}

int ViewerClient::findScript (std::string in_deviceName, std::string &buf)
{
	// The camera client asks for its script with the device name; returning
	// an empty one (not -1, which ScriptTarget turns into an error) leaves
	// any other camera idle.
	buf = (in_deviceName == runningScriptDevice) ? runningScript : std::string ();
	return 0;
}

rts2core::DevClient *ViewerClient::createOtherType (rts2core::Connection *conn, int other_device_type)
{
	if (other_device_type == DEVICE_TYPE_CCD)
	{
		std::string devName = conn->getName ();
		ViewerCamera *cam = new ViewerCamera (conn);

		// DevClientCameraImage already defaults to saveImage=1
		// (kernel/src/devcliimg.cpp) - explicit here to match MainWindow's
		// own default (Saving button starts ON), not to override it.
		cam->setSaveImage (1);
		cam->setArchivePath (imageExpandPath);

		bool selected = false;
		{
			std::lock_guard<std::mutex> lock (camerasMutex);
			cameras[devName] = cam;
			cameraNames[conn] = devName;
			if (activeCamera.empty () && (initialDevice.empty () || initialDevice == devName))
			{
				activeCamera = devName;
				selected = true;
			}
		}

		emit cameraCreated (QString::fromStdString (devName), cam, selected);
		return cam;
	}
	// The other devices' values go into the FITS headers, as with
	// rts2-scriptexec (scriptexec.cpp's createOtherType()) - a spectrum is
	// only usable with the lamp, fiber and wheel positions it was taken
	// with, and the modes' scripts are what move them.
	switch (other_device_type)
	{
		case DEVICE_TYPE_MOUNT:
			return new rts2image::DevClientTelescopeImage (conn);
		case DEVICE_TYPE_FOCUS:
			return new rts2image::DevClientFocusImage (conn);
		case DEVICE_TYPE_DOME:
		case DEVICE_TYPE_SENSOR:
		case DEVICE_TYPE_ROTATOR:
		case DEVICE_TYPE_FW:
			return new rts2image::DevClientWriteImage (conn);
	}
	return rts2core::Client::createOtherType (conn, other_device_type);
}

std::string ViewerClient::checkScriptValues (ViewerCamera *camera, const std::string &script)
{
	// Value changes look like [DEVICE.]NAME=VALUE (or +=, -=), see
	// Script::parseBuf(). The script engine skips one whose device is not
	// connected with only a log line, and the device refuses an unknown
	// value - either way the frames are taken anyway, so a lambda calib
	// without its lamp would look like a good one.
	std::istringstream words (script);
	std::string word;
	std::string missing;
	while (words >> word)
	{
		size_t eq = word.find ('=');
		if (eq == std::string::npos || eq == 0)
			continue;
		std::string lhs = word.substr (0, eq);
		if (lhs.back () == '+' || lhs.back () == '-')
			lhs.pop_back ();
		std::string dev = camera->getName ();
		std::string valueName = lhs;
		size_t dot = lhs.find ('.');
		if (dot != std::string::npos)
		{
			dev = lhs.substr (0, dot);
			valueName = lhs.substr (dot + 1);
			// CX, TX, ... - every device of a type (execcli.cpp)
			if (dev.size () == 2 && dev[1] == 'X')
				continue;
		}
		rts2core::Connection *conn = (dev == camera->getName ()) ? camera->getConnection () : getOpenConnection (dev.c_str ());
		std::string problem;
		if (!conn)
			problem = "device " + dev + " is not connected";
		else if (!conn->getValue (valueName.c_str ()))
			problem = dev + " has no value " + valueName;
		else
			continue;
		if (missing.find (problem) == std::string::npos)
			missing += (missing.empty () ? "" : ", ") + problem;
	}
	if (!missing.empty ())
		missing = "not started: " + missing;
	return missing;
}

void ViewerClient::setActiveCamera (const std::string &camName)
{
	std::lock_guard<std::mutex> lock (camerasMutex);
	activeCamera = camName;
}

void ViewerClient::requestScript (const std::string &script)
{
	std::lock_guard<std::mutex> lock (scriptMutex);
	pendingScript = script;
	scriptPending = true;
}

void ViewerClient::requestStop ()
{
	stopRequested.store (true);
}

void ViewerClient::requestValueChange (const std::string &valueName, char op, std::variant<int, double, bool> value)
{
	std::lock_guard<std::mutex> lock (pendingMutex);
	pendingChanges.push_back ({valueName, op, value});
}

void ViewerClient::requestSaveToggle (bool enabled)
{
	requestedSaveEnabled.store (enabled);
	saveToggleRequested.store (true);
}

void ViewerClient::requestWindowChange (int x, int y, int w, int h)
{
	std::lock_guard<std::mutex> lock (windowMutex);
	pendingWindow = { x, y, w, h };
	windowPending = true;
}

void ViewerClient::requestRefit ()
{
	refitRequested.store (true);
}

void ViewerClient::requestQuit ()
{
	quitRequested.store (true);
}

int ViewerClient::idle ()
{
	if (quitRequested.exchange (false))
	{
		setEndLoop (true);
		return rts2core::Client::idle ();
	}

	ViewerCamera *active = nullptr;
	{
		std::lock_guard<std::mutex> lock (camerasMutex);
		auto it = cameras.find (activeCamera);
		if (it != cameras.end ())
			active = it->second;
	}

	if (active)
	{
		{
			bool doWindow = false;
			PendingWindow win {};
			{
				std::lock_guard<std::mutex> lock (windowMutex);
				if (windowPending)
				{
					doWindow = true;
					win = pendingWindow;
					windowPending = false;
				}
			}
			if (doWindow)
			{
				// Queued ahead of the exposure block below (textually and
				// therefore also in queCommand order) - see
				// requestWindowChange()'s doc comment.
				active->getConnection ()->queCommand (new rts2core::CommandChangeValue (this, "WINDOW", '=', win.x, win.y, win.w, win.h));
			}
		}

		bool startScript = false;
		{
			std::lock_guard<std::mutex> lock (scriptMutex);
			if (scriptPending)
			{
				startScript = true;
				runningScript = pendingScript;
				scriptPending = false;
			}
		}
		std::string missing;
		if (startScript)
			missing = checkScriptValues (active, runningScript);
		if (!missing.empty ())
		{
			emit active->scriptEnded (QString::fromStdString (missing));
		}
		else if (startScript)
		{
			// After the window change above, so a script's first exposure
			// reads the window asked for with it. The script sets exposure
			// time and SHUTTER itself for every frame (E = light, D = dark -
			// script/src/element.cpp's ElementImage), so a dark cannot be
			// left switched on by a previous frame.
			runningScriptDevice = active->getName ();
			scriptTarget->setObsId (++scriptRuns);
			active->runScript (scriptTarget);
		}

		if (stopRequested.exchange (false))
		{
			// the script first, or it queues its next command as soon as
			// the exposure stops
			active->stopScript ();
			// "stopexpo" is a plain text command (camd.cpp), same class of
			// raw command rts2-sendcmd already sends elsewhere in this
			// tree - no dedicated Command subclass exists for it.
			active->getConnection ()->queCommand (new rts2core::Command (this, "stopexpo"));
		}

		if (saveToggleRequested.exchange (false))
		{
			// Purely local bookkeeping (DevClientCameraImage::saveImage) -
			// no wire command, no device Value. Safe to call directly:
			// this object is only ever touched from this same thread.
			active->setSaveImage (requestedSaveEnabled.load () ? 1 : 0);
		}

		if (refitRequested.exchange (false))
		{
			// Also purely local (re-runs the fit against the already-cached
			// image) - no wire command either.
			active->refit ();
		}

		std::vector<PendingChange> changes;
		{
			std::lock_guard<std::mutex> lock (pendingMutex);
			changes.swap (pendingChanges);
		}
		for (const auto &change : changes)
		{
			std::visit ([this, &active, &change] (auto &&v)
			{
				active->getConnection ()->queCommand (new rts2core::CommandChangeValue (this, change.valueName, change.op, v));
			}, change.value);
		}
	}

	return rts2core::Client::idle ();
}

int ViewerClient::progress (rts2core::Connection *conn, double start, double end)
{
	std::string devName;
	{
		std::lock_guard<std::mutex> lock (camerasMutex);
		auto it = cameraNames.find (conn);
		if (it != cameraNames.end ())
			devName = it->second;
	}
	if (!devName.empty ())
		emit progressUpdated (QString::fromStdString (devName), start, end);
	return rts2core::Client::progress (conn, start, end);
}

ClientThread::ClientThread (int _argc, char **_argv, QObject *parent):
	QThread (parent), argc (_argc), argv (_argv)
{
}

void ClientThread::run ()
{
	ViewerClient c (argc, argv);
	connect (&c, &ViewerClient::cameraCreated, this, &ClientThread::cameraCreated, Qt::DirectConnection);
	connect (&c, &ViewerClient::modesFileResolved, this, &ClientThread::modesFileResolved, Qt::DirectConnection);
	connect (&c, &ViewerClient::progressUpdated, this, &ClientThread::progressUpdated, Qt::DirectConnection);

	client.store (&c);
	c.run ();
	client.store (nullptr);
}

void ClientThread::setActiveCamera (const std::string &name)
{
	ViewerClient *c = client.load ();
	if (c)
		c->setActiveCamera (name);
}

void ClientThread::requestScript (const std::string &script)
{
	ViewerClient *c = client.load ();
	if (c)
		c->requestScript (script);
}

void ClientThread::requestStop ()
{
	ViewerClient *c = client.load ();
	if (c)
		c->requestStop ();
}

void ClientThread::requestValueChange (const std::string &valueName, char op, std::variant<int, double, bool> value)
{
	ViewerClient *c = client.load ();
	if (c)
		c->requestValueChange (valueName, op, value);
}

void ClientThread::requestSaveToggle (bool enabled)
{
	ViewerClient *c = client.load ();
	if (c)
		c->requestSaveToggle (enabled);
}

void ClientThread::requestWindowChange (int x, int y, int w, int h)
{
	ViewerClient *c = client.load ();
	if (c)
		c->requestWindowChange (x, y, w, h);
}

void ClientThread::requestRefit ()
{
	ViewerClient *c = client.load ();
	if (c)
		c->requestRefit ();
}

void ClientThread::requestQuit ()
{
	ViewerClient *c = client.load ();
	if (c)
		c->requestQuit ();
}
