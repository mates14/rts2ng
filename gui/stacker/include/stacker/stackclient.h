#pragma once

#include <QObject>
#include <QStringList>
#include <QThread>
#include <QVariantList>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "client.h"

namespace stacker
{

class StackCamera;
class CalibrationLibrary;

/**
 * rts2-viewer's ViewerClient (gui/viewer/include/gui/viewerclient.h) for
 * the stacker: same centrald connection, camera discovery and
 * idle()-drained request queue, handing each camera to a StackCamera, plus
 *
 *   --calib DIR      master darks/flats (see CalibrationLibrary) - one
 *                    directory, applied to every camera, so a stacker
 *                    normally runs against one camera (--device)
 *   --stack-dir DIR  where resetStack() writes the summed frames
 *
 * and the stack's own requests (reset, stack/frame view).
 */
class StackClient : public QObject, public rts2core::Client
{
	Q_OBJECT

	public:
		StackClient (int argc, char **argv);
		virtual ~StackClient ();

		// Thread-safe, consumed on the next idle() - see ViewerClient.
		void setActiveCamera (const std::string &name);
		void requestExposure (double exptime);
		void requestStop ();
		void requestValueChange (const std::string &valueName, char op, std::variant<int, double, bool> value);
		void requestSaveToggle (bool enabled);
		void requestWindowChange (int x, int y, int w, int h);
		void requestRefit ();
		void requestShowStack (bool show);
		void requestResetStack ();
		void requestQuit ();

	signals:
		void cameraCreated (QString name, stacker::StackCamera *camera);
		void progressUpdated (QString cameraName, double start, double end);

		/**
		 * Emitted once from init(). darks holds one QVariantMap per master
		 * dark ({exptime, width, height, name}), flats one filter name per
		 * master flat. dir is empty when no --calib was given; error is set
		 * when it was given but unusable (startup then fails).
		 */
		void calibrationReady (QString dir, QVariantList darks, QStringList flats, QString error);

	protected:
		virtual int processOption (int in_opt) override;
		virtual int init () override;
		virtual rts2core::DevClient *createOtherType (rts2core::Connection *conn, int other_device_type) override;
		virtual int idle () override;
		virtual int progress (rts2core::Connection *conn, double start, double end) override;

	private:
		const char *configFile;
		std::string initialDevice;
		std::string imageExpandPath;
		std::string calibDir;
		std::string stackDir = ".";
		std::unique_ptr<CalibrationLibrary> calib;

		std::mutex camerasMutex;
		std::map<std::string, StackCamera *> cameras;
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
		std::atomic<bool> showStackRequested { false };
		std::atomic<bool> requestedShowStack { true };
		std::atomic<bool> resetRequested { false };
		std::atomic<bool> quitRequested { false };
};

/** ViewerClient's ClientThread, for StackClient. */
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
		void requestShowStack (bool show);
		void requestResetStack ();
		void requestQuit ();

	signals:
		void cameraCreated (QString name, stacker::StackCamera *camera);
		void progressUpdated (QString cameraName, double start, double end);
		void calibrationReady (QString dir, QVariantList darks, QStringList flats, QString error);

	protected:
		virtual void run () override;

	private:
		int argc;
		char **argv;
		std::atomic<StackClient *> client { nullptr };
};

}
