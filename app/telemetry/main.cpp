/*
 * BareMetal-RTOS - telemetry demo entry point
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#include "board.h"
#include "telemetry.hpp"

#ifndef DEMO_RUN_MS
#define DEMO_RUN_MS 3000u
#endif

extern "C" void rtos_panic_hook(const char *msg)
{
    board_printf("\nPANIC: %s\n", msg);
    board_exit(100);
}

namespace {

rtos::Thread<384> supervisor;

void supervisor_entry(void *)
{
    using namespace telemetry;
    console_printf("\nBareMetal-RTOS telemetry demo  |  Cortex-M3 @ %u MHz, tick %u Hz, sensor %u Hz\n",
                   RTOS_CPU_HZ / 1000000u, RTOS_TICK_HZ, kSampleRateHz);
    console_printf("Running for %u ms...\n\n", DEMO_RUN_MS);

    pipeline_start();
    rtos_sleep(rtos::ms_to_ticks(DEMO_RUN_MS));
    uint32_t run_ticks = pipeline_stop_sensor();

    pipeline_print_report(run_ticks);
    bool ok = pipeline_self_check(run_ticks);
    console_printf("\nDEMO RESULT: %s\n", ok ? "PASS" : "FAIL");
    board_exit(ok ? 0 : 1);
}

}  // namespace

int main()
{
    board_init();
    rtos_init();
    supervisor.start("supervisor", supervisor_entry, nullptr, 10);
    rtos_start();
}
