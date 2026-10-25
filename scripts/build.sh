#!/usr/bin/env bash
# BareMetal-RTOS - build firmware and host tests, then run every test suite.
# Copyright (c) 2026 Swayam Shakya
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> Firmware (arm-none-eabi)"
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build -j

echo "==> Host tests"
cmake -S . -B build-host
cmake --build build-host -j

echo "==> Running host tests"
(cd build-host && ctest --output-on-failure)

if command -v qemu-system-arm >/dev/null 2>&1; then
    echo "==> Running on-target tests in QEMU"
    (cd build && ctest --output-on-failure)
else
    echo "qemu-system-arm not found: skipping on-target tests"
fi
