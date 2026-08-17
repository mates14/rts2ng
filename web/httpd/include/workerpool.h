#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace rts2web
{

/**
 * Minimal fixed-size thread pool for offloading CPU-heavy work off
 * Block's main poll-loop thread (STATUS.md task 5). A job is just a
 * std::function<void()> closure - queuing and the worker threads
 * themselves are all this class owns; what a job actually does (preview
 * generation today) and how its result gets back to the main thread are
 * entirely the caller's concern.
 *
 * Completion notification: the constructor takes a single
 * wakeupCallback, invoked from a worker thread right after each job
 * finishes. The intended use is to write to an eventfd already
 * registered in Block's poll set (see HttpD::wakeup() in httpd.cpp) -
 * this class has no opinion on how results get back to the main thread;
 * that's up to what each job closure does (typically: push a result
 * onto its own queue, then rely on this callback to wake the poll loop
 * so the main thread drains it).
 */
class WorkerPool
{
	public:
		WorkerPool (size_t numThreads, std::function <void ()> wakeupCallback);
		~WorkerPool ();

		void submit (std::function <void ()> job);

	private:
		void workerLoop ();

		std::vector <std::thread> threads;
		std::queue <std::function <void ()>> jobs;
		std::mutex jobsMutex;
		std::condition_variable jobsCv;
		bool shuttingDown;

		std::function <void ()> wakeupCallback;
};

}
