#!/bin/sh
# Build the Ubuntu 20.04 rootfs the release binaries are compiled in.
#
# Why: a bridge built on this machine floors at GLIBC_2.38 and silently drops Ubuntu 20.04,
# Debian 11 and Rocky 8. Built here it floors at GLIBC_2.16. Unprivileged, no daemon, no root.
set -e
ROOT=/home/jooshua/apps/fxbridge-sandbox/focal
mkdir -p "$ROOT"
cd /home/jooshua/apps/fxbridge-sandbox
if [ ! -f base.tar.gz ]; then
    URL=https://cdimage.ubuntu.com/ubuntu-base/releases/20.04/release/
    FILE=$(curl -sL "$URL" | grep -oE 'ubuntu-base-20\.04[.0-9]*-base-amd64\.tar\.gz' | head -1)
    [ -n "$FILE" ] || { echo "no rootfs tarball found at $URL" >&2; exit 1; }
    echo "fetching $FILE"
    curl -fL -o base.tar.gz "$URL$FILE"
fi
if [ ! -x "$ROOT/bin/sh" ]; then
    echo "extracting"
    tar -xzf base.tar.gz -C "$ROOT"
fi
echo "installing the toolchain"
bwrap --bind "$ROOT" / --dev /dev --proc /proc --tmpfs /tmp \
      --ro-bind /etc/resolv.conf /etc/resolv.conf \
      --unshare-user --uid 0 --gid 0 \
      --setenv DEBIAN_FRONTEND noninteractive \
      /bin/sh -c "echo 'APT::Sandbox::User "root";' > /etc/apt/apt.conf.d/00sandbox && apt-get update -qq && apt-get install -y -qq --no-install-recommends g++ libx11-dev zlib1g-dev" 
echo "toolchain:"
bwrap --bind "$ROOT" / --dev /dev --proc /proc --tmpfs /tmp --unshare-user --uid 0 --gid 0 \
      /bin/sh -c "g++ --version | head -1; ldd --version | head -1"
