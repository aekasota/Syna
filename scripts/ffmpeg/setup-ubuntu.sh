#!/bin/bash
# Installs, as root on Ubuntu 24.04, the tools scripts/ffmpeg/build.sh needs. Packages come from the
# Ubuntu archive as it was at SNAPSHOT, so every build uses the same tool versions. The compiler is
# not among them: build.sh downloads a pinned llvm-mingw.
set -euo pipefail

SNAPSHOT=20261001T000000Z

[[ $(id -u) == 0 ]] || { echo "Run as root." >&2; exit 1; }
. /etc/os-release
[[ $VERSION_ID == 24.04 ]] || { echo "Expected Ubuntu 24.04, found $PRETTY_NAME." >&2; exit 1; }

export DEBIAN_FRONTEND=noninteractive

# The snapshot archive is served over HTTPS only, and minimal images (the CI container) have no CA
# certificates. Install them first from the image's own sources, whose package lists apt verifies
# with the Ubuntu archive key; nothing else comes from there.
apt-get update
apt-get install -y --no-install-recommends ca-certificates

# Everything else from the snapshot. --error-on=any stops here if the snapshot cannot be read, instead
# of failing later with packages that cannot be found.
apt-get --error-on=any -o APT::Snapshot="$SNAPSHOT" update
apt-get -o APT::Snapshot="$SNAPSHOT" install -y --no-install-recommends \
    autoconf automake cmake curl libtool make meson nasm ninja-build patch perl pkgconf python3 xz-utils zip
