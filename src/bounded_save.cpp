#include "bounded_save.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#include "host_thread.h"
#include "plugin_instance.h"

namespace {

// One request, shared between the thread that asked and the host main thread that answers. Shared
// because either side can be the last one holding it: the asker leaves after a timeout, and the
// host main thread may still be inside the plugin with this as its destination.
//
// The plugin pointer stays valid for as long as the job does. The bridge never frees a hosted
// plugin while Resolve runs: an effect's plugin is created once and never cleared.
struct SaveJob {
    HostedPlugin* plugin = nullptr;
    std::vector<uint8_t> out;
    bool ok = false;
    bool done = false;       // under g_jobs_lock
    bool abandoned = false;  // under g_jobs_lock: the asker stopped waiting
};

struct SaveQueue {
    std::mutex lock;
    std::condition_variable done;
    std::deque<std::shared_ptr<SaveJob>> jobs;

    // The job an asker gave up on, held until the host main thread is done with it. While it is
    // set, the host main thread is either inside that plugin or stuck somewhere before it reached
    // the queue. Either way a new job would wait behind it, so none is queued.
    std::shared_ptr<SaveJob> overdue;
};

// Allocated once and never destroyed. The tick thread outlives this file's globals at exit: this
// file links after host_thread.cpp, so its globals are destroyed BEFORE host_thread.cpp's
// StopOnUnload stops the tick, and a tick in between would lock a destroyed mutex and call through
// a destroyed object. Nothing here owns anything the process needs back.
SaveQueue& Queue()
{
    static SaveQueue* const queue = new SaveQueue;
    return *queue;
}

void FinishLocked(SaveQueue& queue, const std::shared_ptr<SaveJob>& job)
{
    job->done = true;
    if (queue.overdue == job) {
        queue.overdue.reset();
    }
}

// Drains the queue on every tick. The tick already holds HostMainLock.
class SaveRunner final : public HostMainClient {
public:
    void OnHostMainTick() override
    {
        SaveQueue& queue = Queue();
        for (;;) {
            std::shared_ptr<SaveJob> job;
            {
                std::lock_guard<std::mutex> held(queue.lock);
                if (queue.jobs.empty()) {
                    return;
                }
                job = queue.jobs.front();
                queue.jobs.pop_front();
                // Nobody is waiting for this answer, so the plugin is not asked at all.
                if (job->abandoned) {
                    FinishLocked(queue, job);
                    continue;
                }
            }
            std::vector<uint8_t> out;
            const bool ok = job->plugin->SaveState(out);
            {
                std::lock_guard<std::mutex> held(queue.lock);
                job->out.swap(out);
                job->ok = ok;
                FinishLocked(queue, job);
            }
            queue.done.notify_all();
        }
    }
};

// Never destroyed, for the same reason as Queue(): the tick may still call it during exit.
SaveRunner& Runner()
{
    static SaveRunner* const runner = new SaveRunner;
    return *runner;
}

}  // namespace

BoundedSave SaveStateBounded(HostedPlugin* plugin, std::vector<uint8_t>& out,
                             std::chrono::milliseconds limit)
{
    if (plugin == nullptr) {
        return BoundedSave::Refused;
    }
    if (HostMainIsCurrentThread()) {
        return plugin->SaveState(out) ? BoundedSave::Saved : BoundedSave::Refused;
    }

    SaveQueue& queue = Queue();
    auto job = std::make_shared<SaveJob>();
    job->plugin = plugin;
    {
        std::lock_guard<std::mutex> held(queue.lock);
        if (queue.overdue != nullptr) {
            return BoundedSave::Stalled;
        }
        queue.jobs.push_back(job);
    }
    // Registering twice is a no-op, and the first call starts the host main thread if no loader
    // has yet. Period 0 leaves the tick rate to the loaders. Called outside queue.lock, so this
    // file never holds two locks at once.
    HostMainRegister(&Runner(), 0);

    std::unique_lock<std::mutex> held(queue.lock);
    if (!queue.done.wait_for(held, limit, [&job] { return job->done; })) {
        job->abandoned = true;
        queue.overdue = job;
        return BoundedSave::TimedOut;
    }
    out.swap(job->out);
    return job->ok ? BoundedSave::Saved : BoundedSave::Refused;
}

const char* BoundedSaveName(BoundedSave result)
{
    switch (result) {
    case BoundedSave::Saved: return "saved";
    case BoundedSave::Refused: return "refused";
    case BoundedSave::TimedOut: return "timed out";
    case BoundedSave::Stalled: return "skipped, an earlier save never came back";
    }
    return "?";
}
