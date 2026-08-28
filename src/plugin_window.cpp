#include "plugin_window.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

// Owned by proxy.cpp: the one place that decides whether an editor should be on screen.
extern "C" void BridgeEditorWasClosedByUser(unsigned long window);
extern "C" void BridgeArmEditorTrace();
extern "C" void BridgeEditorReassert();

struct PluginWindow {
    // Last size we told the child about. A drag emits ConfigureNotify continuously, and resizing
    // the child on every one of them makes the drag crawl - that was a regression on 2026-08-25.
    unsigned int child_width = 0;
    unsigned int child_height = 0;
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

// Windows that an effect has finished with. They stay alive because a Wine host may still hold the
// id - see PluginWindowDestroy. Only the unload destroys them for real.
std::vector<Window> g_retired;

// Defined further down, used by the event pump above it.
void ResizeChildrenLocked(Window handle, unsigned int width, unsigned int height);

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
    // A named thread is what makes a CPU report actionable. Until this existed, `top -H`
    // against Resolve showed all 300-odd threads as "GUI", so a user reporting that the
    // bridge burns a quarter of a core had no way to say which part of it does.
    pthread_setname_np(pthread_self(), "fxb-xpump");
    while (g_pump_running.load()) {
        {
            std::lock_guard<std::mutex> held(g_lock);
            while (g_display != nullptr && XPending(g_display) > 0) {
                XEvent event;
                XNextEvent(g_display, &event);
                // Our own window changed size. Whoever did it - the window manager, a tiling
                // layout, the user dragging an edge - the embedded plugin has to follow, or it is
                // left hit-testing against a rectangle that no longer exists.
                if (event.type == ConfigureNotify) {
                    const XConfigureEvent& configured = event.xconfigure;
                    PluginWindow* const owner = FindLocked(configured.window);
                    const unsigned int width = static_cast<unsigned int>(configured.width);
                    const unsigned int height = static_cast<unsigned int>(configured.height);
                    if (owner != nullptr && width > 0 && height > 0 &&
                        (width != owner->child_width || height != owner->child_height)) {
                        owner->child_width = width;
                        owner->child_height = height;
                        ResizeChildrenLocked(configured.window, width, height);
                        XFlush(g_display);
                    }
                    continue;
                }
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
                BridgeEditorWasClosedByUser(static_cast<unsigned long>(window->handle));
                BridgeArmEditorTrace();
            }
        }
        // Outside the lock, always: opening an editor calls back into this file and takes g_lock
        // again. Also outside Resolve's main thread, which is the point - see BridgeEditorReassert.
        BridgeEditorReassert();
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

namespace {

// Tell the window manager what this window is and how big it must be.
//
// Without this, a tiling window manager is entitled to resize the window to whatever fits its
// layout - and COSMIC does. On 2026-08-25 a Smooth Operator Pro editor created at 1132x602 was
// handed to the plugin as 991x554, the size of the soothe2 editor beside it. The plugin drew at
// the size it asked for, inside a window that was smaller, so its own hit-testing pointed at
// coordinates that were not on screen. It looked exactly like the Wine mouse bug, and was not.
//
// A plugin editor is a fixed-size utility window: min and max are the same, and the type hint
// keeps a tiling layout from claiming it in the first place.
// A plugin embeds its own window inside ours and then forgets about it. When our window changes
// size - because the plugin asked, or because the window manager did it - the child keeps its old
// geometry, and from then on the drawn rectangle, the visible rectangle and the rectangle Wine
// hit-tests against are three different things. Measured on 2026-08-25:
//
//     0x2400005  1000x607   <- our parent
//       0x3800000  1132x602 <- the Wine window
//         0x3400005 1132x602 <- "yabridge plugin"
//
// Smooth Operator Pro drew correctly and took no clicks in that state; soothe2, which never asks to
// be resized, was fine in the same session. Resizing the child also gives it a real ConfigureNotify,
// which is how Wine learns its new position.
void ResizeChildrenLocked(Window handle, unsigned int width, unsigned int height)
{
    Window root = 0;
    Window parent = 0;
    Window* children = nullptr;
    unsigned int count = 0;
    if (XQueryTree(g_display, handle, &root, &parent, &children, &count) == 0) {
        return;
    }
    for (unsigned int index = 0; index < count; ++index) {
        XResizeWindow(g_display, children[index], width, height);
    }
    if (children != nullptr) {
        XFree(children);
    }
}

void ApplyWindowRulesLocked(Window handle, unsigned int width, unsigned int height)
{
    XSizeHints hints{};
    hints.flags = PMinSize | PMaxSize | PSize | PBaseSize;
    hints.min_width = hints.max_width = hints.base_width = static_cast<int>(width);
    hints.min_height = hints.max_height = hints.base_height = static_cast<int>(height);
    hints.width = static_cast<int>(width);
    hints.height = static_cast<int>(height);
    XSetWMNormalHints(g_display, handle, &hints);

    const Atom type = XInternAtom(g_display, "_NET_WM_WINDOW_TYPE", False);
    const Atom utility = XInternAtom(g_display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    XChangeProperty(g_display, handle, type, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&utility), 1);
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
    // Before the map, never after: a window manager reads these when it takes the window over.
    ApplyWindowRulesLocked(handle, width != 0 ? width : 800, height != 0 ? height : 600);
    XMapRaised(g_display, handle);
    XFlush(g_display);

    // Log the id. Three Wine hosts died on "BadWindow ... 0x4c00001" on 2026-08-25 and there was
    // no way to tell whose window that was, because the ids were never written down.
    Log("window: created 0x%lx at %ux%u for \"%s\"", static_cast<unsigned long>(handle),
        width != 0 ? width : 800, height != 0 ? height : 600, title != nullptr ? title : "Plugin");

    auto* const window = new PluginWindow();
    window->handle = handle;
    window->mapped = true;
    g_windows.push_back(window);

    PluginWindowStartPump();
    return window;
}

void PluginWindowStartPump()
{
    if (!g_pump_running.exchange(true)) {
        g_pump = std::thread(EventPump);
    }
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

// The window is retired, not destroyed. This looks wasteful and is not.
//
// A bridged plugin lives in a separate Wine process that keeps our window id and unmaps it on its
// own schedule. Destroying the window here makes that id invalid, and the next unmap from the Wine
// side is a BadWindow - at which point Xlib's default error handler calls exit(), the Wine host
// dies, its socket closes, and yabridge throws an exception that nothing catches. That was the
// abort on 2026-08-25:
//
//     X Error of failed request:  BadWindow (invalid Window parameter)
//       Major opcode of failed request:  10 (X_UnmapWindow)
//
// So the window is unmapped and kept. It costs one small unmapped X window per effect ever opened,
// and it is reclaimed by the next editor that fits, or at unload when no Wine host is left alive.
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
        XUnmapWindow(g_display, window->handle);
        XFlush(g_display);
        g_retired.push_back(window->handle);
    }
    delete window;
}

void PluginWindowResize(PluginWindow* window, unsigned int width, unsigned int height)
{
    std::lock_guard<std::mutex> held(g_lock);
    if (g_display == nullptr || window == nullptr || window->handle == 0 || width == 0 ||
        height == 0) {
        return;
    }
    // The hints move with it. Leaving them behind means asking the window manager to resize a
    // window it has been told is fixed at another size, and it is entitled to refuse.
    ApplyWindowRulesLocked(window->handle, width, height);
    XResizeWindow(g_display, window->handle, width, height);
    ResizeChildrenLocked(window->handle, width, height);
    XFlush(g_display);
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

namespace {

// A std::thread destroyed while still joinable calls std::terminate(). g_pump is a global, so its
// destructor runs at shutdown - and until this existed, it ran on a joinable thread every single
// time. That is one of the two reasons Resolve would not quit cleanly on 2026-08-25.
//
// Declared last in the file, so it is constructed last and therefore destroyed first: the stop has
// to happen while the thread object is still alive to be joined.
//
// The display is deliberately left open. Closing it here would destroy the parent windows while a
// plugin may still be tearing its editor down inside one, and the process is exiting anyway - the
// socket goes with it.
struct StopPumpOnUnload {
    ~StopPumpOnUnload()
    {
        std::fprintf(stderr, "[fxbridge] teardown: stopping the window pump\n");
        std::fflush(stderr);
        if (g_pump_running.exchange(false) && g_pump.joinable()) {
            g_pump.join();
        }
        // Now, and only now, the retired windows can go: the process is ending, so no Wine host is
        // going to unmap one of them afterwards.
        std::lock_guard<std::mutex> held(g_lock);
        if (g_display != nullptr) {
            for (Window handle : g_retired) {
                XDestroyWindow(g_display, handle);
            }
            XFlush(g_display);
        }
        g_retired.clear();
        std::fprintf(stderr, "[fxbridge] teardown: the window pump is stopped\n");
        std::fflush(stderr);
    }
};
StopPumpOnUnload g_stop_pump_on_unload;

}  // namespace
