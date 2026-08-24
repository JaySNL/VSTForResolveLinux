// The window every loader hangs its editor in.
//
// Resolve's side of the editor - HasEditor, InitializeEffectEdit, UpdateEffectEditTitle,
// HideSubWindows - is owned by proxy.cpp and is the same for every format. This is the other half:
// one X11 parent window that a plugin embeds into, with one close handler and one event pump.
//
// CLAP, VST2 and VST3 all embed the same way: the host supplies a native window handle and the
// plugin draws into it. So they share this, rather than each carrying its own copy of the X11
// plumbing and its own idea of what "closed" means.
#ifndef FXBRIDGE_PLUGIN_WINDOW_H
#define FXBRIDGE_PLUGIN_WINDOW_H

#include <cstdint>

// Creates the parent window at the size the plugin asked for, or raises the existing one. The
// returned value is the X11 Window id, which is what every plugin format wants as its parent.
// Returns 0 on failure.
unsigned long PluginWindowOpen(unsigned int width, unsigned int height, const char* title);

// Maps an existing window again. False when there is nothing to show.
bool PluginWindowShow();

// Hides without destroying: the plugin's editor lives inside, and reopening should be a remap.
void PluginWindowHide();

// True while the window exists, whether mapped or not.
bool PluginWindowExists();

// The X11 display, for a loader that needs to flush after its own calls.
void* PluginWindowDisplay();
void PluginWindowFlush();

void PluginWindowSetLogger(void (*logger)(const char*));

#endif
