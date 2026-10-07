#!/bin/sh
# Installs what building OPM86 Player needs on Ubuntu: the compiler and build
# tools, and the development packages JUCE needs (sound, fonts, X11, OpenGL).
# Used by the Linux container (make linux) and by the GitHub workflow.
set -e
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build ccache git ca-certificates pkg-config \
    libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
    libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxi-dev \
    libxrandr-dev libxrender-dev libgl1-mesa-dev zlib1g-dev
