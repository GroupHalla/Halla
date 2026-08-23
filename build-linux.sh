#!/usr/bin/env bash
# Compila o Halla no Linux (instala dependências em Debian/Ubuntu se necessário)
set -e
cd "$(dirname "$0")"

if ! command -v cmake >/dev/null 2>&1 || ! pkg-config --exists Qt6Widgets 2>/dev/null; then
    echo ">> Instalando dependências (cmake, ninja, qt6-base-dev)..."
    sudo apt-get update -qq
    sudo apt-get install -y cmake ninja-build qt6-base-dev qt6-base-dev-tools
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
echo ""
echo ">> Pronto! Execute: ./build/Halla"
