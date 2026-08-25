#include "host_thread.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

std::mutex g_main_lock;
std::mutex g_client_lock;
std::vector<HostMainClient*> g_clients;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<unsigned int> g_period{16};

// True only on the tick thread. Cheaper and less racy than publishing its id.
thread_local bool t_is_host_main = false;

void (*g_logger)(const char*) = nullptr;

// Ticks every registered client in turn, on one thread, under one lock.
//
// The list is copied before the pass rather than held across the calls: a client is entitled
// to register or unregister from inside its own tick, and that would otherwise invalidate the
// iterator mid-loop.
void TickThread()
{
    t_is_host_main = true;
    while (g_running.load()) {
        const unsigned int period = g_period.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(period != 0 ? period : 16));
        if (!g_running.load()) {
            break;
        }
        std::lock_guard<std::mutex> main(g_main_lock);
        std::vector<HostMainClient*> clients;
        {
            std::lock_guard<std::mutex> held(g_client_lock);
            clients = g_clients;
        }
        for (HostMainClient* client : clients) {
            client->OnHostMainTick();
        }
    }
}

}  // namespace

void HostMainRegister(HostMainClient* client, unsigned int period_ms)
{
    if (client == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> held(g_client_lock);
    if (period_ms != 0 && period_ms < g_period.load()) {
        g_period.store(period_ms);
    }
    for (const HostMainClient* existing : g_clients) {
        if (existing == client) {
            return;
        }
    }
    g_clients.push_back(client);
    if (!g_running.exchange(true)) {
        g_thread = std::thread(TickThread);
        if (g_logger != nullptr) {
            g_logger("host: the main thread is up");
        }
    }
}

void HostMainUnregister(HostMainClient* client)
{
    std::lock_guard<std::mutex> held(g_client_lock);
    for (size_t index = 0; index < g_clients.size(); ++index) {
        if (g_clients[index] == client) {
            g_clients.erase(g_clients.begin() + static_cast<long>(index));
            return;
        }
    }
}

bool HostMainIsCurrentThread() { return t_is_host_main; }

std::mutex& HostMainLock() { return g_main_lock; }

void HostThreadSetLogger(void (*logger)(const char*)) { g_logger = logger; }
