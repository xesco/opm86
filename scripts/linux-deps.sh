#!/bin/sh
# Installs what building OPM86 Player needs on Linux: the compiler and build
# tools, and the development packages JUCE needs (sound, fonts, X11, OpenGL).
# Works on Ubuntu and Debian (apt) and on Arch and Omarchy (pacman). Used by
# the Linux container (make linux on a Mac) and by the GitHub workflow.
set -e
if command -v apt-get >/dev/null; then
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build ccache git ca-certificates pkg-config \
        libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
        libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxi-dev \
        libxrandr-dev libxrender-dev libgl1-mesa-dev zlib1g-dev
elif command -v pacman >/dev/null; then
    pacman -S --needed --noconfirm \
        base-devel cmake ninja ccache git pkgconf \
        alsa-lib jack2 freetype2 fontconfig \
        libx11 libxcomposite libxcursor libxext libxinerama libxi libxrandr libxrender mesa zlib
else
    echo "linux-deps: no apt-get or pacman here; install a C++ compiler, CMake, and the development packages of ALSA, JACK, FreeType, Fontconfig, X11 and OpenGL" >&2
    exit 1
fi
