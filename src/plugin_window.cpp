#include "plugin_window.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>

// Owned by proxy.cpp: the one place that decides whether an editor should be on screen.
extern "C" void BridgeEditorWasClosedByUser();
extern "C" void BridgeArmEditorTrace();

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
Window g_window = 0;
Atom g_delete_window = 0;
bool g_mapped = false;
std::thread g_pump;
std::atomic<bool> g_pump_running{false};

// The window manager's close button must not destroy the window: the plugin's editor lives inside
// it. Catch WM_DELETE_WINDOW, unmap instead, and tell the editor owner - so re-asserting the state
// never fights a close the user performed.
void EventPump()
{
    while (g_pump_running.load()) {
        while (g_display != nullptr && XPending(g_display) > 0) {
            XEvent event;
            XNextEvent(g_display, &event);
            if (event.type == ClientMessage &&
                static_cast<Atom>(event.xclient.data.l[0]) == g_delete_window) {
                XUnmapWindow(g_display, g_window);
                XFlush(g_display);
                g_mapped = false;
                Log("window: the window manager closed the editor");
                BridgeEditorWasClosedByUser();
                BridgeArmEditorTrace();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

}  // namespace

unsigned long PluginWindowOpen(unsigned int width, unsigned int height, const char* title)
{
    if (g_display != nullptr && g_window != 0) {
        PluginWindowShow();
        return static_cast<unsigned long>(g_window);
    }

    XInitThreads();  // the editor is used from Resolve's thread and pumped from ours
    g_display = XOpenDisplay(nullptr);
    if (g_display == nullptr) {
        Log("window: XOpenDisplay failed, no editor");
        return 0;
    }

    const int screen = DefaultScreen(g_display);
    g_window = XCreateSimpleWindow(g_display, RootWindow(g_display, screen), 0, 0,
                                   width != 0 ? width : 800, height != 0 ? height : 600, 0,
                                   BlackPixel(g_display, screen), BlackPixel(g_display, screen));
    if (g_window == 0) {
        Log("window: XCreateSimpleWindow failed");
        return 0;
    }

    XStoreName(g_display, g_window, title != nullptr ? title : "Plugin");
    XSelectInput(g_display, g_window, StructureNotifyMask);
    g_delete_window = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_display, g_window, &g_delete_window, 1);
    XMapRaised(g_display, g_window);
    XFlush(g_display);
    g_mapped = true;

    if (!g_pump_running.exchange(true)) {
        g_pump = std::thread(EventPump);
    }
    return static_cast<unsigned long>(g_window);
}

bool PluginWindowShow()
{
    if (g_display == nullptr || g_window == 0) {
        return false;
    }
    XMapRaised(g_display, g_window);
    XFlush(g_display);
    g_mapped = true;
    return true;
}

void PluginWindowHide()
{
    if (g_display == nullptr || g_window == 0) {
        return;
    }
    XUnmapWindow(g_display, g_window);
    XFlush(g_display);
    g_mapped = false;
}

bool PluginWindowExists() { return g_display != nullptr && g_window != 0; }

void* PluginWindowDisplay() { return g_display; }

void PluginWindowFlush()
{
    if (g_display != nullptr) {
        XFlush(g_display);
    }
}

void PluginWindowSetLogger(void (*logger)(const char*)) { g_logger = logger; }
