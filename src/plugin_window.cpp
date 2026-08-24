#include "plugin_window.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

// Owned by proxy.cpp: the one place that decides whether an editor should be on screen.
extern "C" void BridgeEditorWasClosedByUser();
extern "C" void BridgeArmEditorTrace();

struct PluginWindow {
    Window handle = 0;
    bool mapped = false;
};

namespace {

void (*g_logger)(const char*) = nullptr;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

void Log(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (g_logger != nullptr) {
        g_logger(line);
    }
}

Display* g_display = nullptr;
Atom g_delete_window = 0;
std::thread g_pump;
std::atomic<bool> g_pump_running{false};

// Every open window, so the pump can match an event to the object it belongs to. Guarded because
// windows are created on Resolve's thread and read on ours.
std::mutex g_lock;
std::vector<PluginWindow*> g_windows;

PluginWindow* FindLocked(Window handle)
{
    for (PluginWindow* window : g_windows) {
        if (window->handle == handle) {
            return window;
        }
    }
    return nullptr;
}

// The window manager's close button must not destroy the window: the plugin's editor lives inside
// it. Catch WM_DELETE_WINDOW, unmap instead, and tell the editor owner - so re-asserting the state
// never fights a close the user performed.
void EventPump()
{
    while (g_pump_running.load()) {
        {
            std::lock_guard<std::mutex> held(g_lock);
            while (g_display != nullptr && XPending(g_display) > 0) {
                XEvent event;
                XNextEvent(g_display, &event);
                if (event.type != ClientMessage ||
                    static_cast<Atom>(event.xclient.data.l[0]) != g_delete_window) {
                    continue;
                }
                PluginWindow* const window = FindLocked(event.xclient.window);
                if (window == nullptr) {
                    continue;
                }
                XUnmapWindow(g_display, window->handle);
                XFlush(g_display);
                window->mapped = false;
                Log("window: the window manager closed an editor");
                BridgeEditorWasClosedByUser();
                BridgeArmEditorTrace();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

bool OpenDisplayLocked()
{
    if (g_display != nullptr) {
        return true;
    }
    XInitThreads();  // the editor is used from Resolve's thread and pumped from ours
    g_display = XOpenDisplay(nullptr);
    if (g_display == nullptr) {
        Log("window: XOpenDisplay failed, no editor");
        return false;
    }
    g_delete_window = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    return true;
}

}  // namespace

PluginWindow* PluginWindowCreate(unsigned int width, unsigned int height, const char* title)
{
    std::lock_guard<std::mutex> held(g_lock);
    if (!OpenDisplayLocked()) {
        return nullptr;
    }

    const int screen = DefaultScreen(g_display);
    const Window handle =
        XCreateSimpleWindow(g_display, RootWindow(g_display, screen), 0, 0,
                            width != 0 ? width : 800, height != 0 ? height : 600, 0,
                            BlackPixel(g_display, screen), BlackPixel(g_display, screen));
    if (handle == 0) {
        Log("window: XCreateSimpleWindow failed");
        return nullptr;
    }

    XStoreName(g_display, handle, title != nullptr ? title : "Plugin");
    XSelectInput(g_display, handle, StructureNotifyMask);
    XSetWMProtocols(g_display, handle, &g_delete_window, 1);
    XMapRaised(g_display, handle);
    XFlush(g_display);

    auto* const window = new PluginWindow();
    window->handle = handle;
    window->mapped = true;
    g_windows.push_back(window);

    if (!g_pump_running.exchange(true)) {
        g_pump = std::thread(EventPump);
    }
    return window;
}

unsigned long PluginWindowHandle(const PluginWindow* window)
{
    return window != nullptr ? static_cast<unsigned long>(window->handle) : 0;
}

bool PluginWindowShow(PluginWindow* window)
{
    std::lock_guard<std::mutex> held(g_lock);
    if (g_display == nullptr || window == nullptr || window->handle == 0) {
        return false;
    }
    XMapRaised(g_display, window->handle);
    XFlush(g_display);
    window->mapped = true;
    return true;
}

void PluginWindowHide(PluginWindow* window)
{
    std::lock_guard<std::mutex> held(g_lock);
    if (g_display == nullptr || window == nullptr || window->handle == 0) {
        return;
    }
    XUnmapWindow(g_display, window->handle);
    XFlush(g_display);
    window->mapped = false;
}

void PluginWindowDestroy(PluginWindow* window)
{
    std::lock_guard<std::mutex> held(g_lock);
    if (window == nullptr) {
        return;
    }
    for (size_t index = 0; index < g_windows.size(); ++index) {
        if (g_windows[index] == window) {
            g_windows.erase(g_windows.begin() + static_cast<long>(index));
            break;
        }
    }
    if (g_display != nullptr && window->handle != 0) {
        XDestroyWindow(g_display, window->handle);
        XFlush(g_display);
    }
    delete window;
}

void PluginWindowFlush(PluginWindow*)
{
    std::lock_guard<std::mutex> held(g_lock);
    if (g_display != nullptr) {
        XFlush(g_display);
    }
}

void* PluginWindowDisplay() { return g_display; }

void PluginWindowSetLogger(void (*logger)(const char*)) { g_logger = logger; }
