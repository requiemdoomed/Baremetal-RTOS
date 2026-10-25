#!/usr/bin/env bash
# BareMetal-RTOS - run a firmware image on the emulated LM3S6965 (Cortex-M3) board.
# Copyright (c) 2026 Swayam Shakya
# SPDX-License-Identifier: MIT
#
# usage: scripts/run_qemu.sh [demo|tests|path/to/image.elf] [--realtime] [--debug]
#   --realtime  emulated clocks follow the host clock instead of instruction counting
#   --debug     halt at reset and wait for GDB on localhost:1234
set -euo pipefail
cd "$(dirname "$0")/.."

target="${1:-demo}"
shift || true
case "$target" in
    demo)  elf=build/telemetry_demo.elf ;;
    tests) elf=build/kernel_tests.elf ;;
    *)     elf="$target" ;;
esac

if [[ ! -f "$elf" ]]; then
    echo "error: $elf not found. Build first with scripts/build.sh" >&2
    exit 1
fi

timing=(-icount shift=6,align=off,sleep=on)
debug=()
for arg in "$@"; do
    case "$arg" in
        --realtime) timing=() ;;
        --debug)    debug=(-S -gdb tcp::1234); echo "Waiting for GDB: arm-none-eabi-gdb $elf -ex 'target remote :1234'" ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

exec qemu-system-arm -M lm3s6965evb -nographic -semihosting -monitor none -serial stdio \
    "${timing[@]}" "${debug[@]}" -kernel "$elf"
