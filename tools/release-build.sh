#!/bin/sh
# Build a release binary in the Ubuntu 20.04 rootfs, not on this machine.
#
# Why this exists: a bridge compiled here floors at GLIBC_2.38 and silently drops Ubuntu 20.04,
# Debian 11 and Rocky 8 - the distributions most likely to be running Resolve on a locked-down
# workstation. Compiled in the old rootfs it floors at GLIBC_2.16. The difference is invisible
# until someone's dlopen fails, which is why the release binary is never the one build.sh made.
#
#   tools/release-build.sh v0.2.4 [output-directory]
#
# The rootfs is built by fxbridge-sandbox-setup.sh and lives outside this repository.
set -e
TAG=${1:?usage: release-build.sh <tag> [output-directory]}
OUT=${2:-$(pwd)/release}
ROOT=$HOME/apps/fxbridge-sandbox/focal
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)

[ -x "$ROOT/usr/bin/g++" ] || { echo "no sandbox at $ROOT - run tools/release-sandbox.sh" >&2; exit 1; }
mkdir -p "$OUT"

# A worktree, so the build is the tag and not whatever is checked out.
git -C "$REPO" worktree add -q --detach "$WORK/src" "$TAG"
trap 'git -C "$REPO" worktree remove --force "$WORK/src" 2>/dev/null; rm -rf "$WORK"' EXIT

# Submodules are not populated in a worktree, so the headers come from the main checkout.
bwrap --bind "$ROOT" / --dev /dev --proc /proc --tmpfs /tmp \
      --bind "$WORK/src" /src --ro-bind "$REPO/third_party" /src/third_party \
      --ro-bind /usr/include/carla /usr/include/carla \
      --bind "$OUT" /out --unshare-user --uid 0 --gid 0 \
      /bin/sh -e -c '
        cd /src
        g++ -std=c++17 -shared -fPIC -O2 -w \
            -I third_party/clap/include \
            -I third_party/dpf/distrho/src/travesty \
            -I /usr/include/carla/includes \
            -o /out/libfxbridge.so \
            src/proxy.cpp src/carla_host.cpp src/vst2_plugin.cpp src/clap_plugin.cpp \
            src/host_thread.cpp src/vst3_plugin.cpp src/plugin_scan.cpp src/plugin_state.cpp \
            src/fx_categories.cpp src/plugin_window.cpp \
            -ldl -lX11 -lz -lpthread
        # The pre-launch scanner ships with the library, and floors at the same glibc, so it is
        # built in the same rootfs rather than on the machine that happens to be running this.
        g++ -std=c++17 -O2 -w \
            -I third_party/clap/include \
            -I third_party/dpf/distrho/src/travesty \
            -I /usr/include/carla/includes \
            -o /out/fxbridge-scan \
            src/scan_main.cpp src/plugin_scan.cpp src/vst3_plugin.cpp src/host_thread.cpp \
            src/plugin_window.cpp src/fx_categories.cpp \
            -ldl -lX11 -lz -lpthread
        # Both checks run INSIDE the old rootfs, which is the only place they mean anything.
        if ldd -r /out/libfxbridge.so 2>&1 | grep -q "undefined symbol"; then
            echo "REFUSING - undefined symbols against glibc 2.31:" >&2
            ldd -r /out/libfxbridge.so 2>&1 | grep "undefined symbol" >&2
            exit 1
        fi
        echo "floor: $(objdump -T /out/libfxbridge.so | grep -o "GLIBC[X]*_[0-9.]*" | sort -uV | tail -1) $(objdump -T /out/libfxbridge.so | grep -o "CXXABI_[0-9.]*" | sort -uV | tail -1)"
      '
( cd "$OUT" && sha256sum libfxbridge.so fxbridge-scan > SHA256SUMS && cat SHA256SUMS )
echo "built $TAG into $OUT"
