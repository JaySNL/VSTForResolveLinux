#!/bin/sh
# Build the Fairlight FX bridge.
set -e
root=$(cd "$(dirname "$0")" && pwd)
out="$root/build"
mkdir -p "$out"
g++ -std=c++17 -shared -fPIC -O2 -Wall -Wextra \
    -I "$root/third_party/clap/include" \
    -I "$root/third_party/dpf/distrho/src/travesty" \
    -I /usr/include/carla/includes \
    -o "$out/libfxbridge.so" \
    "$root/src/proxy.cpp" "$root/src/carla_host.cpp" "$root/src/vst2_plugin.cpp" "$root/src/clap_plugin.cpp" "$root/src/vst3_plugin.cpp" "$root/src/plugin_scan.cpp" "$root/src/plugin_window.cpp" -ldl -lX11
# Refuse to install a library Resolve cannot load.
#
# A shared library links happily with unresolved symbols, so a missing source file still produces a
# "built" message and a file that dlopen rejects at run time. That shipped once: vst2_plugin.cpp was
# left out of this list, the build reported success, and Resolve loaded no bridge at all - no log
# line, no effect in the menu, nothing to debug from.
if ldd -r "$out/libfxbridge.so" 2>&1 | grep -q "undefined symbol"; then
    echo "REFUSING TO INSTALL - the library has undefined symbols:" >&2
    ldd -r "$out/libfxbridge.so" 2>&1 | grep "undefined symbol" >&2
    exit 1
fi

# Install as part of the build.
#
# A build that is not installed is a build that is not being tested. Keeping these separate cost a
# crash report against a library that was two commits old, and a diagnosis of a bug that had
# already been fixed. Resolve loads this file at startup, so a restart is still required - but at
# least the file on disk is always the file that was just compiled.
install_dir="$HOME/.local/share/BMDAudioPlugins"
mkdir -p "$install_dir"
cp "$out/libfxbridge.so" "$install_dir/libfxbridge.so"

echo "built and installed $install_dir/libfxbridge.so"
