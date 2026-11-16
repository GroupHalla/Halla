#!/usr/bin/env bash
# Builds Halla on Linux (installs dependencies on Debian/Ubuntu when needed)
#
# Without the WebRTC SDK: fallback build (Qt Multimedia voice + embedded
# RNNoise/speex DSP, JPEG screen share). For the native build (real WebRTC,
# X11 capture + PC audio through PulseAudio), download the linux-x64 SDK from
# https://github.com/GroupHalla/Halla-WebRTC-Builds/releases e configure:
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DHALLA_WEBRTC_SDK_DIR=/path/to/halla-webrtc-sdk \
#     -DHALLA_ENABLE_WEBRTC_NATIVE=ON
#
set -e
cd "$(dirname "$0")"

if ! command -v cmake >/dev/null 2>&1 || ! pkg-config --exists Qt6Widgets 2>/dev/null; then
    echo ">> Instalando dependências (cmake, ninja, Qt 6, opus, openssl, libsecret, X11, pulse)..."
    sudo apt-get update -qq
    sudo apt-get install -y cmake ninja-build pkg-config \
        qt6-base-dev qt6-base-dev-tools qt6-multimedia-dev qt6-speech-dev \
        libopus-dev libssl-dev libsecret-1-dev \
        libx11-dev libxext-dev libxrandr-dev libxfixes-dev libpulse-dev
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
echo ""
echo ">> Pronto! Execute: ./build/Halla"
