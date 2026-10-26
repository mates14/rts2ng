#pragma once

#include <QObject>
#include <QThread>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "client.h"
#include "scriptinterface.h"
#include "scripttarget.h"

namespace gui
{

class ViewerCamera;

/**
 * Connects to centrald and watches for every camera device that appears
 * (not just one named device - see fiber_pointing_client.py's multi-camera
 * G1/G2 selector, which this mirrors), exactly like every other RTS2
 * client (login/discovery is generic rts2core::Client behaviour - see
 * block.cpp's createOtherType dispatch), handing each one to its own
 * ViewerCamera instead of a focus/monitor client.
 */
class ViewerClient : public QObject, public rts2core::Client, public rts2script::ScriptInterface
{
	Q_OBJECT

	public:
		ViewerClient (int argc, char **argv);
		virtual ~ViewerClient ();

		/**
		 * All of the below are thread-safe: callable from any thread (the
		 * GUI thread's widget handlers call these on the ClientThread
		 * wrapper, which forwards here). Consumed on this Client's own next
		 * idle() tick, since its run() loop is a plain blocking poll() loop
		 * with no Qt event loop of its own to deliver a queued signal to.
		 */
		void setActiveCamera (const std::string &name);

		/**
		 * Run an RTS2 script (an exposure mode's, variables already filled
		 * in - see gui/modes.h) on the active camera. The camera reports
		 * its end with ViewerCamera::scriptEnded().
		 */
		void requestScript (const std::string &script);

		/** Stop the running script and the exposure in progress. */
		void requestStop ();
		void requestValueChange (const std::string &valueName, char op, std::variant<int, double, bool> value);

		/**
		 * rts2script::ScriptInterface: the script requestScript() gave, for
		 * the camera it was started on; nothing for anyone else.
		 */
		virtual int findScript (std::string in_deviceName, std::string &buf) override;

		/**
		 * Whether the active camera's own DevClientCameraImage writes the
		 * FITS files it receives to local disk (Image::saveImage(), gated
		 * by DevClientCameraImage::saveImage - kernel/src/devcliimg.cpp).
		 * Purely client-side bookkeeping, no device Value/wire command
		 * involved (unlike requestValueChange) - dispatched straight onto
		 * the ViewerCamera object from idle(), safe because that object is
		 * only ever touched from this same worker thread.
		 */
		void requestSaveToggle (bool enabled);

		/**
		 * Change the active camera's "WINDOW" ValueRectangle (the chip
		 * subframe read out) - base/camd/include/camd.h's chipUsedReadout.
		 * Queued ahead of the next requestScript()'s commands (idle()
		 * checks this block first), same "set the Value, then trigger"
		 * ordering as exposure time - see MainWindow, which sends this
		 * immediately before every exposure request (the full chip rect
		 * when its windowing checkbox is off, the blue cursor's rect when
		 * on), rather than trying to track "is windowing currently on" in
		 * here too.
		 */
		void requestWindowChange (int x, int y, int w, int h);

		/**
		 * Re-run the focus fit against the active camera's already-received
		 * image (ViewerCamera::refit()) - no exposure/wire command
		 * involved, purely local, but still routed through this same
		 * idle()-drained mechanism so the actual call happens on
		 * ViewerCamera's own worker thread, same as requestSaveToggle().
		 */
		void requestRefit ();

		void requestQuit ();

	signals:
		/**
		 * selected: this is the camera to show first (--device, else the
		 * first one to connect).
		 */
		void cameraCreated (QString name, ViewerCamera *camera, bool selected);

		/**
		 * The exposure-modes file to use (--modes, else rts2.ini's [viewer]
		 * modes), empty for the built-in light/dark. Emitted once from
		 * init(), after rts2.ini is loaded.
		 */
		void modesFileResolved (QString path);

		/**
		 * Fires whenever the server broadcasts a PROTO_PROGRESS message for
		 * a connection (Block::progress(), overridden here) - this is the
		 * same start/expected-end timestamp pair rts2-mon's own progress
		 * bar (Connection::getProgress(), kernel/src/connection.cpp) is
		 * built on. MainWindow interpolates percent/elapsed/remaining
		 * locally against wall-clock time from these two numbers, rather
		 * than polling this thread for a live percentage.
		 */
		void progressUpdated (QString cameraName, double start, double end);

	protected:
		virtual int processOption (int in_opt) override;
		virtual int init () override;
		virtual rts2core::DevClient *createOtherType (rts2core::Connection *conn, int other_device_type) override;
		virtual int idle () override;
		virtual int progress (rts2core::Connection *conn, double start, double end) override;

	private:
		/**
		 * Every device and value the script sets, checked against what is
		 * connected - empty if all there, else what is missing.
		 */
		std::string checkScriptValues (ViewerCamera *camera, const std::string &script);

		const char *configFile;
		std::string initialDevice;

		/**
		 * Expand-path expression for frames that get kept (saveImage on) -
		 * see ViewerCamera::setArchivePath(). Populated in init(): the
		 * --images command-line option if given, else rts2.ini's
		 * [viewer] expand_path, else a built-in default of just
		 * "%y%m%d%H%M%S-%s.fits", relative to wherever the viewer was
		 * started - no target involved, since this viewer has no concept
		 * of one, and no site archive either, since a hand-run viewer
		 * usually cannot write there and its frames rarely belong there.
		 */
		std::string imageExpandPath;

		std::string modesFile;

		// One target for every script the viewer runs; each run gets its
		// own obs id, so a run that failed (DevScript then refuses the
		// target id + obs id it failed with) does not block the next one.
		rts2script::ScriptTarget *scriptTarget = nullptr;
		int scriptRuns = 0;
		// what findScript() hands out - worker thread only
		std::string runningScriptDevice;
		std::string runningScript;

		std::mutex scriptMutex;
		bool scriptPending = false;
		std::string pendingScript;

		std::mutex camerasMutex;
		std::map<std::string, ViewerCamera *> cameras;
		std::map<rts2core::Connection *, std::string> cameraNames;
		std::string activeCamera;

		struct PendingChange
		{
			std::string valueName;
			char op;
			std::variant<int, double, bool> value;
		};
		std::mutex pendingMutex;
		std::vector<PendingChange> pendingChanges;

		struct PendingWindow
		{
			int x, y, w, h;
		};
		std::mutex windowMutex;
		bool windowPending = false;
		PendingWindow pendingWindow {};

		std::atomic<bool> stopRequested { false };
		std::atomic<bool> saveToggleRequested { false };
		std::atomic<bool> requestedSaveEnabled { false };
		std::atomic<bool> refitRequested { false };
		std::atomic<bool> quitRequested { false };
};

/**
 * Runs a ViewerClient's blocking event loop on its own thread, so Qt's own
 * event loop (QApplication::exec, on the GUI thread) is untouched - see
 * gui/STATUS.md for why this is simpler than integrating the two loops
 * (unlike classic XFocusClient, which added the X11 display fd into RTS2's
 * own poll() loop - there is no equivalent hook for handing Qt's loop to
 * a foreign event source).
 */
class ClientThread : public QThread
{
	Q_OBJECT

	public:
		ClientThread (int argc, char **argv, QObject *parent = nullptr);

		void setActiveCamera (const std::string &name);
		void requestScript (const std::string &script);
		void requestStop ();
		void requestValueChange (const std::string &valueName, char op, std::variant<int, double, bool> value);
		void requestSaveToggle (bool enabled);
		void requestWindowChange (int x, int y, int w, int h);
		void requestRefit ();
		void requestQuit ();

	signals:
		void cameraCreated (QString name, ViewerCamera *camera, bool selected);
		void modesFileResolved (QString path);
		void progressUpdated (QString cameraName, double start, double end);

	protected:
		virtual void run () override;

	private:
		int argc;
		char **argv;
		std::atomic<ViewerClient *> client { nullptr };
};

}
