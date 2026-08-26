#!/bin/sh
# Build the Fairlight FX bridge.
set -e
root=$(cd "$(dirname "$0")" && pwd)
out="$root/build"
mkdir -p "$out"
# -lpthread is not optional, even though this machine links without it.
#
# glibc 2.34 merged libpthread into libc, so on a modern distribution pthread_create resolves out of
# libc and the undefined-symbol guard below passes. Below glibc 2.34 it does not, and the guard still
# passes, because it runs on the build machine rather than the target. The result is a library that
# reports a clean build and then fails to dlopen on Ubuntu 20.04, Debian 11 or Rocky 8. Verified by
# building in an Ubuntu 20.04 sandbox: without this flag, ldd -r there reports
# "undefined symbol: pthread_create"; with it, zero. On glibc 2.34+ libpthread.so.0 remains as a
# stub, so the flag costs nothing.
g++ -std=c++17 -shared -fPIC -O2 -Wall -Wextra \
    -I "$root/third_party/clap/include" \
    -I "$root/third_party/dpf/distrho/src/travesty" \
    -I /usr/include/carla/includes \
    -o "$out/libfxbridge.so" \
    "$root/src/proxy.cpp" "$root/src/carla_host.cpp" "$root/src/vst2_plugin.cpp" "$root/src/clap_plugin.cpp" "$root/src/host_thread.cpp" "$root/src/vst3_plugin.cpp" "$root/src/plugin_scan.cpp" "$root/src/fx_categories.cpp" "$root/src/plugin_window.cpp" -ldl -lX11 -lz -lpthread
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
# Install by rename, never by writing over the file in place. Resolve mmaps this library, and
# overwriting the bytes underneath a running process corrupts what it is executing. A rename gives
# the new file its own inode and leaves the running Resolve on the old one until it exits.
cp "$out/libfxbridge.so" "$install_dir/.libfxbridge.so.new"
chmod 755 "$install_dir/.libfxbridge.so.new"
mv -f "$install_dir/.libfxbridge.so.new" "$install_dir/libfxbridge.so"

echo "built and installed $install_dir/libfxbridge.so"

# Point Resolve at the library, because asking a person to do it by hand does not work.
#
# The first outside tester followed the readme, saw no plugins and no error, and had no way to tell
# why. Three things have to be exactly right and none of them announce themselves: the key lives in
# config-fairlight.dat and nowhere else (config.dat is kept across restarts and ignored), the value
# is the absolute path to libfxbridge.so and not the directory holding it, and a wrong value is not
# fatal - Resolve silently falls back to its own library. Silent fallback plus a hand-edited config
# is a support thread, so this writes it.
#
# Set FXBRIDGE_NO_CONFIGURE=1 to skip and print the line instead.
config_dir="$HOME/.local/share/DaVinciResolve/configs"
config="$config_dir/config-fairlight.dat"
target="$install_dir/libfxbridge.so"
line="BMDPlugins.Path = $target"

if [ -n "$FXBRIDGE_NO_CONFIGURE" ]; then
    echo "skipping the config; add this to $config while Resolve is closed:"
    echo "    $line"
    exit 0
fi

# Resolve owns this file and rewrites it on quit, so editing it under a running Resolve loses the
# edit. Its process name is GUI, not resolve, which is why this greps comm rather than using pgrep.
if ps -eo comm,args 2>/dev/null | awk '$1=="GUI" && /\/opt\/resolve\/bin\/resolve/ { found=1 } END { exit !found }'; then
    echo ""
    echo "Resolve is RUNNING. It owns $config and overwrites it on quit, so the setting"
    echo "would be lost. Close Resolve and run this script again, or add the line yourself:"
    echo "    $line"
    exit 0
fi

mkdir -p "$config_dir"
had_config=yes
[ -f "$config" ] || { had_config=no; : > "$config"; }

# Match the key whether or not it has the "=", because the readme published a form without one and
# people have that line in their file. Matching only "key =" left the broken line in place and
# appended a second one, and which of the two Resolve honours is not documented anywhere.
current=$(sed -n 's/^[[:space:]]*BMDPlugins\.Path[[:space:]]*=\{0,1\}[[:space:]]*//p' "$config" | tail -1)
if [ "$current" = "$target" ]; then
    echo "config already points at this library"
    exit 0
fi

if [ "$had_config" = yes ]; then
    backup="$config.before-fxbridge-$(date +%Y%m%d-%H%M%S)"
    cp "$config" "$backup"
fi

# One pass: replace the first BMDPlugins.Path line, drop any further ones, append if there were
# none. Duplicate keys are how the previous version of this script left a file worse than it found
# it.
tmp="$config.fxbridge-tmp"
awk -v repl="$line" '
    /^[[:space:]]*BMDPlugins\.Path([[:space:]]|=)/ {
        if (!done) { print repl; done = 1 }
        next
    }
    { print }
    END { if (!done) print repl }
' "$config" > "$tmp"
mv "$tmp" "$config"

if [ -n "$current" ]; then
    echo "replaced BMDPlugins.Path (was: $current)"
fi
echo "configured $config"
echo "  $line"
[ "$had_config" = yes ] && echo "  previous file kept at $backup"
echo "Start Resolve. The plugins are in the Audio FX panel."
