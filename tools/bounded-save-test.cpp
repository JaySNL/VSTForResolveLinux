// Behaviour test for SaveStateBounded (src/bounded_save.cpp), against the real host main thread.
// Run it with tools/bounded-save-test.sh. No Resolve and no plugin needed.
//
// It proves the four promises bounded_save.h makes: the save runs on the host main thread, a
// plugin that never answers costs one bounded wait, every save after that returns at once until
// the stuck call comes back, and a job nobody waits for any more is never sent to its plugin.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "bounded_save.h"
#include "host_thread.h"
#include "plugin_instance.h"

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

class FakePlugin final : public HostedPlugin {
public:
    enum class Mode { Answer, Refuse, Hang };
    explicit FakePlugin(Mode mode) : mode_(mode) {}

    bool Process(float**, uint32_t, uint32_t) override { return false; }
    uint32_t ChannelCount() const override { return 0; }
    const char* Name() const override { return "fake"; }
    bool OpenEditor() override { return false; }
    void CloseEditor() override {}
    PluginFormat Format() const override { return PluginFormat::Unknown; }

    bool SaveState(std::vector<uint8_t>& out) override
    {
        ++calls;
        on_host_main = HostMainIsCurrentThread();
        if (mode_ == Mode::Refuse) {
            return false;
        }
        if (mode_ == Mode::Hang) {
            std::unique_lock<std::mutex> held(lock_);
            woken_.wait(held, [this] { return released_; });
        }
        out = {1, 2, 3};
        return true;
    }

    void Release()
    {
        {
            std::lock_guard<std::mutex> held(lock_);
            released_ = true;
        }
        woken_.notify_all();
    }

    std::atomic<int> calls{0};
    std::atomic<bool> on_host_main{false};

private:
    Mode mode_;
    std::mutex lock_;
    std::condition_variable woken_;
    bool released_ = false;
};

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

long long MsSince(Clock::time_point start)
{
    return std::chrono::duration_cast<milliseconds>(Clock::now() - start).count();
}

// Retries until the host main thread is free again, for at most two seconds.
BoundedSave SaveOnceFree(FakePlugin& plugin, std::vector<uint8_t>& out)
{
    BoundedSave result = BoundedSave::Stalled;
    for (int tries = 0; tries < 100 && result == BoundedSave::Stalled; ++tries) {
        result = SaveStateBounded(&plugin, out, milliseconds(2000));
        if (result == BoundedSave::Stalled) {
            std::this_thread::sleep_for(milliseconds(20));
        }
    }
    return result;
}

}  // namespace

int main()
{
    std::vector<uint8_t> out;

    FakePlugin answers(FakePlugin::Mode::Answer);
    Check(SaveStateBounded(&answers, out, milliseconds(2000)) == BoundedSave::Saved &&
              out == std::vector<uint8_t>{1, 2, 3},
          "a plugin that answers is saved");
    Check(answers.on_host_main, "the plugin was called on the host main thread");

    FakePlugin refuses(FakePlugin::Mode::Refuse);
    Check(SaveStateBounded(&refuses, out, milliseconds(2000)) == BoundedSave::Refused,
          "a plugin with nothing to report is Refused");

    FakePlugin hangs(FakePlugin::Mode::Hang);
    auto start = Clock::now();
    const BoundedSave first = SaveStateBounded(&hangs, out, milliseconds(300));
    const long long waited = MsSince(start);
    Check(first == BoundedSave::TimedOut && waited >= 300 && waited < 1500,
          "a plugin that never answers times out after the limit");

    FakePlugin bystander(FakePlugin::Mode::Answer);
    start = Clock::now();
    Check(SaveStateBounded(&bystander, out, milliseconds(2000)) == BoundedSave::Stalled &&
              MsSince(start) < 100 && bystander.calls == 0,
          "while that call is out, the next save returns at once and calls nothing");

    hangs.Release();
    Check(SaveOnceFree(bystander, out) == BoundedSave::Saved,
          "once the stuck call comes back, saving works again");

    // A job that times out before the host main thread ever reached it must be dropped, not run
    // late into a plugin nobody is waiting for. Holding the lock keeps the tick away from the queue.
    FakePlugin late(FakePlugin::Mode::Answer);
    {
        std::lock_guard<std::mutex> held(HostMainLock());
        Check(SaveStateBounded(&late, out, milliseconds(200)) == BoundedSave::TimedOut,
              "a save the host main thread cannot reach times out");
    }
    FakePlugin after(FakePlugin::Mode::Answer);
    Check(SaveOnceFree(after, out) == BoundedSave::Saved && late.calls == 0,
          "the abandoned job is skipped, never sent to its plugin");

    HostMainStop();
    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
