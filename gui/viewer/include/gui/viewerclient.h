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
class ViewerClient : public QObject, public rts2core::Client
{
	Q_OBJECT

	public:
		ViewerClient (int argc, char **argv);

		/**
		 * All of the below are thread-safe: callable from any thread (the
		 * GUI thread's widget handlers call these on the ClientThread
		 * wrapper, which forwards here). Consumed on this Client's own next
		 * idle() tick, since its run() loop is a plain blocking poll() loop
		 * with no Qt event loop of its own to deliver a queued signal to.
		 */
		void setActiveCamera (const std::string &name);
		void requestExposure (double exptime);
		void requestStop ();
		void requestValueChange (const std::string &valueName, char op, std::variant<int, double, bool> value);

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
		 * Queued ahead of the next requestExposure()'s commands (idle()
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
		void cameraCreated (QString name, ViewerCamera *camera);

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
		virtual rts2core::DevClient *createOtherType (rts2core::Connection *conn, int other_device_type) override;
		virtual int idle () override;
		virtual int progress (rts2core::Connection *conn, double start, double end) override;

	private:
		std::string initialDevice;

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

		std::atomic<bool> exposeRequested { false };
		std::atomic<double> requestedExptime { 1.0 };
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
		void requestExposure (double exptime);
		void requestStop ();
		void requestValueChange (const std::string &valueName, char op, std::variant<int, double, bool> value);
		void requestSaveToggle (bool enabled);
		void requestWindowChange (int x, int y, int w, int h);
		void requestRefit ();
		void requestQuit ();

	signals:
		void cameraCreated (QString name, ViewerCamera *camera);
		void progressUpdated (QString cameraName, double start, double end);

	protected:
		virtual void run () override;

	private:
		int argc;
		char **argv;
		std::atomic<ViewerClient *> client { nullptr };
};

}
