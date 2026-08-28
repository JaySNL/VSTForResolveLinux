// The window a plugin hangs its editor in - one per plugin, not one for all.
//
// Resolve's side of the editor - HasEditor, InitializeEffectEdit, UpdateEffectEditTitle,
// HideSubWindows - is owned by proxy.cpp and is the same for every format. This is the other half:
// the X11 parent window a plugin embeds into, with one close handler and one event pump.
//
// CLAP, VST2 and VST3 all embed the same way: the host supplies a native window handle and the
// plugin draws into it. So they share this, rather than each carrying its own copy of the X11
// plumbing and its own idea of what "closed" means.
//
// One X11 display and one pump thread serve every window. The window itself is per plugin, because
// two effects in one project are two editors, and a shared parent would have them drawing over
// each other.
#ifndef FXBRIDGE_PLUGIN_WINDOW_H
#define FXBRIDGE_PLUGIN_WINDOW_H

struct PluginWindow;

// Creates a parent window at the size the plugin asked for. Returns null on failure.
PluginWindow* PluginWindowCreate(unsigned int width, unsigned int height, const char* title);

// The X11 Window id, which is what every plugin format wants as its parent. Zero if there is none.
unsigned long PluginWindowHandle(const PluginWindow* window);

// Maps the window again. False when there is nothing to show.
bool PluginWindowShow(PluginWindow* window);

// Hides without destroying: the plugin's editor lives inside, and reopening should be a remap.
void PluginWindowHide(PluginWindow* window);

// Resizes the parent. A VST3 editor asks for this through IPlugFrame::resize_view, and a host that
// ignores the request leaves the plugin drawing into a window of the wrong size.
void PluginWindowResize(PluginWindow* window, unsigned int width, unsigned int height);

// Destroys it. The plugin must have released its editor first, or it is left drawing into a window
// that no longer exists.
void PluginWindowDestroy(PluginWindow* window);

void PluginWindowFlush(PluginWindow* window);

// The shared X11 display, for a loader that needs to flush after its own calls.
void* PluginWindowDisplay();

void PluginWindowSetLogger(void (*logger)(const char*));

// Starts the X event pump without a window to pump.
//
// The pump is also what re-asserts a wanted editor, and that is a thread away from Resolve's
// main thread on purpose. Starting it only when a window is created made the first editor
// unreachable: nothing could create the window that starts the thread that opens it.
void PluginWindowStartPump();

#endif
