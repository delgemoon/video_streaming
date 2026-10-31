#!/usr/bin/env bash
# Configures, builds and unit-tests the C++ backend (Release by default).
set -euo pipefail
cd "$(dirname "$0")/../backend"
BUILD_TYPE="${1:-Release}"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build build -j "$(nproc)"
./build/vsp-mp4-tests
echo
echo "Built: $(pwd)/build/vsp-server"
echo "Run:   ./build/vsp-server --data-dir ./data   (or copy config.example.json to config.json)"
