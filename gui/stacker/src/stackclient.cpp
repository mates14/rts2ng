#include "stacker/stackclient.h"
#include "stacker/stackcamera.h"
#include "stacker/calibration.h"

#include <app.h>
#include <command.h>
#include <configuration.h>
#include <connection.h>
#include <status.h>

#include <QVariantMap>

using namespace stacker;

#define OPT_DEVICE     OPT_LOCAL + 900
#define OPT_IMAGES     OPT_LOCAL + 901
#define OPT_CALIB      OPT_LOCAL + 902
#define OPT_STACK_DIR  OPT_LOCAL + 903

StackClient::StackClient (int argc, char **argv):
	rts2core::Client (argc, argv, "rts2-stacker")
{
	configFile = NULL;
	addOption (OPT_CONFIG, "config", 1, "configuration file");
	addOption (OPT_DEVICE, "device", 1, "name of the camera device to select initially");
	addOption (OPT_CALIB, "calib", 1, "directory with master darks (dark-*.fits) and flats (flat-<filter>-*.fits); exposure times are limited to those with a dark. Without it frames are stacked raw");
	addOption (OPT_STACK_DIR, "stack-dir", 1, "directory stacks are saved to on reset (default: current directory)");
	addOption (OPT_IMAGES, "images", 1, "expand-path expression for individual frames, when saving them is switched on (see image.h), overrides rts2.ini's [viewer] expand_path; default: %y%m%d%H%M%S-%s.fits");

	// GUI requests are only picked up in idle(), which Block runs every
	// 10 s when nothing arrives from the network - so an Expose/Reset
	// click on a quiet camera could take that long to happen, and two
	// quick Expose clicks merged into one exposure.
	setTimeout (USEC_SEC / 10);
}

StackClient::~StackClient ()
{
}

int StackClient::processOption (int in_opt)
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
		case OPT_CALIB:
			calibDir = optarg;
			break;
		case OPT_STACK_DIR:
			stackDir = optarg;
			break;
		default:
			return rts2core::Client::processOption (in_opt);
	}
	return 0;
}

int StackClient::init ()
{
	int ret = rts2core::Client::init ();
	if (ret)
		return ret;

	// See ViewerClient::init() for why a client has to load rts2.ini itself.
	rts2core::Configuration *config = rts2core::Configuration::instance ();
	ret = config->loadFile (configFile);
	if (ret)
	{
		std::cerr << "Cannot load configuration file '"
			<< (configFile ? configFile : "/etc/rts2/rts2.ini")
			<< "'" << std::endl;
		return ret;
	}

	if (imageExpandPath.empty ())
		imageExpandPath = config->getStringDefault ("viewer", "expand_path", "%y%m%d%H%M%S-%s.fits");

	QVariantList darks;
	QStringList flats;
	if (!calibDir.empty ())
	{
		calib.reset (new CalibrationLibrary ());
		std::string err;
		if (!calib->scan (calibDir, err))
		{
			std::cerr << err << std::endl;
			emit calibrationReady (QString::fromStdString (calibDir), darks, flats, QString::fromStdString (err));
			return -1;
		}
		for (const auto &d : calib->darks ())
		{
			QVariantMap m;
			m["exptime"] = d.exptime;
			m["width"] = (qlonglong) d.width;
			m["height"] = (qlonglong) d.height;
			m["name"] = QString::fromStdString (d.name);
			darks << m;
		}
		for (const auto &f : calib->flats ())
			flats << QString::fromStdString (f.filter);
	}
	emit calibrationReady (QString::fromStdString (calibDir), darks, flats, QString ());

	return 0;
}

rts2core::DevClient *StackClient::createOtherType (rts2core::Connection *conn, int other_device_type)
{
	if (other_device_type == DEVICE_TYPE_CCD)
	{
		std::string devName = conn->getName ();
		StackCamera *cam = new StackCamera (conn, calib.get ());

		// Unlike the viewer, individual frames are not kept by default -
		// what a stacking session produces is the stack.
		cam->setSaveImage (0);
		cam->setArchivePath (imageExpandPath);
		cam->setStackDir (stackDir);

		{
			std::lock_guard<std::mutex> lock (camerasMutex);
			cameras[devName] = cam;
			cameraNames[conn] = devName;
			if (activeCamera.empty () && (initialDevice.empty () || initialDevice == devName))
				activeCamera = devName;
		}

		emit cameraCreated (QString::fromStdString (devName), cam);
		return cam;
	}
	return rts2core::Client::createOtherType (conn, other_device_type);
}

void StackClient::setActiveCamera (const std::string &camName)
{
	std::lock_guard<std::mutex> lock (camerasMutex);
	activeCamera = camName;
}

void StackClient::requestExposure (double exptime)
{
	requestedExptime.store (exptime);
	exposeRequested.store (true);
}

void StackClient::requestStop ()
{
	stopRequested.store (true);
}

void StackClient::requestValueChange (const std::string &valueName, char op, std::variant<int, double, bool> value)
{
	std::lock_guard<std::mutex> lock (pendingMutex);
	pendingChanges.push_back ({valueName, op, value});
}

void StackClient::requestSaveToggle (bool enabled)
{
	requestedSaveEnabled.store (enabled);
	saveToggleRequested.store (true);
}

void StackClient::requestWindowChange (int x, int y, int w, int h)
{
	std::lock_guard<std::mutex> lock (windowMutex);
	pendingWindow = { x, y, w, h };
	windowPending = true;
}

void StackClient::requestRefit ()
{
	refitRequested.store (true);
}

void StackClient::requestShowStack (bool show)
{
	requestedShowStack.store (show);
	showStackRequested.store (true);
}

void StackClient::requestResetStack ()
{
	resetRequested.store (true);
}

void StackClient::requestQuit ()
{
	quitRequested.store (true);
}

int StackClient::idle ()
{
	if (quitRequested.exchange (false))
	{
		// Closing the window must not lose a stack still being built.
		std::lock_guard<std::mutex> lock (camerasMutex);
		for (auto &c : cameras)
			c.second->resetStack ("program exit");
		setEndLoop (true);
		return rts2core::Client::idle ();
	}

	StackCamera *active = nullptr;
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
				active->getConnection ()->queCommand (new rts2core::CommandChangeValue (this, "WINDOW", '=', win.x, win.y, win.w, win.h));
		}

		// Value changes (binning, filter, ...) go out before a pending
		// exposure, so "change it, then Expose" exposes with the change.
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

		if (exposeRequested.exchange (false))
		{
			active->getConnection ()->queCommand (new rts2core::CommandChangeValue (this, "exposure", '=', requestedExptime.load ()));
			active->getConnection ()->queCommand (new rts2core::CommandExposure (this, active, 0));
		}

		if (stopRequested.exchange (false))
			active->getConnection ()->queCommand (new rts2core::Command (this, "stopexpo"));

		if (saveToggleRequested.exchange (false))
			active->setSaveImage (requestedSaveEnabled.load () ? 1 : 0);

		if (resetRequested.exchange (false))
			active->resetStack ("reset");

		if (showStackRequested.exchange (false))
		{
			active->setShowStack (requestedShowStack.load ());
			active->redisplay ();
		}

		if (refitRequested.exchange (false))
			active->refit ();
	}

	return rts2core::Client::idle ();
}

int StackClient::progress (rts2core::Connection *conn, double start, double end)
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
	StackClient c (argc, argv);
	connect (&c, &StackClient::cameraCreated, this, &ClientThread::cameraCreated, Qt::DirectConnection);
	connect (&c, &StackClient::progressUpdated, this, &ClientThread::progressUpdated, Qt::DirectConnection);
	connect (&c, &StackClient::calibrationReady, this, &ClientThread::calibrationReady, Qt::DirectConnection);

	client.store (&c);
	c.run ();
	client.store (nullptr);
}

void ClientThread::setActiveCamera (const std::string &name)
{
	if (StackClient *c = client.load ())
		c->setActiveCamera (name);
}

void ClientThread::requestExposure (double exptime)
{
	if (StackClient *c = client.load ())
		c->requestExposure (exptime);
}

void ClientThread::requestStop ()
{
	if (StackClient *c = client.load ())
		c->requestStop ();
}

void ClientThread::requestValueChange (const std::string &valueName, char op, std::variant<int, double, bool> value)
{
	if (StackClient *c = client.load ())
		c->requestValueChange (valueName, op, value);
}

void ClientThread::requestSaveToggle (bool enabled)
{
	if (StackClient *c = client.load ())
		c->requestSaveToggle (enabled);
}

void ClientThread::requestWindowChange (int x, int y, int w, int h)
{
	if (StackClient *c = client.load ())
		c->requestWindowChange (x, y, w, h);
}

void ClientThread::requestRefit ()
{
	if (StackClient *c = client.load ())
		c->requestRefit ();
}

void ClientThread::requestShowStack (bool show)
{
	if (StackClient *c = client.load ())
		c->requestShowStack (show);
}

void ClientThread::requestResetStack ()
{
	if (StackClient *c = client.load ())
		c->requestResetStack ();
}

void ClientThread::requestQuit ()
{
	if (StackClient *c = client.load ())
		c->requestQuit ();
}
