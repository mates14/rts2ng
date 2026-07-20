#include "gui/viewerclient.h"
#include "gui/viewercamera.h"

#include <app.h>
#include <command.h>
#include <configuration.h>
#include <connection.h>
#include <status.h>

using namespace gui;

#define OPT_DEVICE   OPT_LOCAL + 900

ViewerClient::ViewerClient (int argc, char **argv):
	rts2core::Client (argc, argv, "rts2-viewer")
{
	configFile = NULL;
	addOption (OPT_CONFIG, "config", 1, "configuration file");
	addOption (OPT_DEVICE, "device", 1, "name of the camera device to select initially (optional - every camera device is watched and can be picked from the camera selector regardless)");
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
	return 0;
}

rts2core::DevClient *ViewerClient::createOtherType (rts2core::Connection *conn, int other_device_type)
{
	if (other_device_type == DEVICE_TYPE_CCD)
	{
		std::string devName = conn->getName ();
		ViewerCamera *cam = new ViewerCamera (conn);

		// DevClientCameraImage defaults to saveImage=1 (kernel/src/
		// devcliimg.cpp) - a viewer used for casual framing/focusing
		// shouldn't silently start writing FITS files until the user
		// explicitly turns saving on (see MainWindow's Saving button).
		cam->setSaveImage (0);

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

void ViewerClient::setActiveCamera (const std::string &camName)
{
	std::lock_guard<std::mutex> lock (camerasMutex);
	activeCamera = camName;
}

void ViewerClient::requestExposure (double exptime)
{
	requestedExptime.store (exptime);
	exposeRequested.store (true);
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

		if (exposeRequested.exchange (false))
		{
			// Set "exposure" (the device's own exposure-time Value) before
			// triggering it - CommandExposure itself takes no exposure-time
			// argument, it just starts a CCD_WORKING exposure using whatever
			// "exposure" is currently set to on the device. Commands queued
			// on the same connection are processed in order, so this always
			// applies before the exposure it's meant for.
			active->getConnection ()->queCommand (new rts2core::CommandChangeValue (this, "exposure", '=', requestedExptime.load ()));
			active->getConnection ()->queCommand (new rts2core::CommandExposure (this, active, 0));
		}

		if (stopRequested.exchange (false))
		{
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

void ClientThread::requestExposure (double exptime)
{
	ViewerClient *c = client.load ();
	if (c)
		c->requestExposure (exptime);
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
