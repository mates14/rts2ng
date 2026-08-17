#include "workerpool.h"

using namespace rts2web;

WorkerPool::WorkerPool (size_t numThreads, std::function <void ()> _wakeupCallback):shuttingDown (false), wakeupCallback (_wakeupCallback)
{
	for (size_t i = 0; i < numThreads; i++)
		threads.emplace_back (&WorkerPool::workerLoop, this);
}

WorkerPool::~WorkerPool ()
{
	{
		std::lock_guard <std::mutex> lock (jobsMutex);
		shuttingDown = true;
	}
	jobsCv.notify_all ();
	for (std::thread &t : threads)
		t.join ();
}

void WorkerPool::submit (std::function <void ()> job)
{
	{
		std::lock_guard <std::mutex> lock (jobsMutex);
		jobs.push (std::move (job));
	}
	jobsCv.notify_one ();
}

void WorkerPool::workerLoop ()
{
	while (true)
	{
		std::function <void ()> job;
		{
			std::unique_lock <std::mutex> lock (jobsMutex);
			jobsCv.wait (lock, [this] { return shuttingDown || !jobs.empty (); });
			if (jobs.empty ())
			{
				if (shuttingDown)
					return;
				continue;
			}
			job = std::move (jobs.front ());
			jobs.pop ();
		}

		job ();

		if (wakeupCallback)
			wakeupCallback ();
	}
}
