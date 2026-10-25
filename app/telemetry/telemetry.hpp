/*
 * BareMetal-RTOS - telemetry demo: shared types
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_HPP
#define TELEMETRY_HPP

#include <stdint.h>

#include "rtos/rtos.hpp"

namespace telemetry {

constexpr uint32_t kSampleRateHz = 1000;   // sensor interrupt rate
constexpr uint32_t kWindowSamples = 250;   // samples per published summary
constexpr uint32_t kControlPeriodTicks = 10;
constexpr int32_t kShockThresholdMg = 2000;
constexpr int32_t kShockRearmMg = 1500;
constexpr int32_t kTargetMg = 1000;        // controller set-point for |a|

// One raw IMU reading, captured in the sensor interrupt.
struct Sample {
    uint32_t seq;
    uint32_t t_cycles;   // capture time (port_cycles)
    int16_t ax, ay, az;  // acceleration in milli-g
};

// Windowed statistics published by the processing thread to the logger.
struct Summary {
    uint32_t window;
    uint32_t samples;
    int32_t mean_mag_mg;
    int32_t peak_mag_mg;
    uint32_t shock_events;  // cumulative
    uint32_t lat_min_us;
    uint32_t lat_avg_us;
    uint32_t lat_max_us;
    uint32_t dropped;       // cumulative ring-buffer overflows
    int32_t control_out;    // actuator correction in mg
};

// State shared between processing and control threads (guarded by a mutex).
struct SharedState {
    int32_t filtered_mag_mg = 1000;
    uint32_t shock_events = 0;
    int32_t control_out = 0;
};

namespace sensor {
void reset();
Sample read(uint32_t seq, uint32_t t_cycles);  // deterministic synthetic IMU
uint32_t shocks_injected();
void set_actuator(int32_t mg);  // correction applied by the controller
}  // namespace sensor

void pipeline_start();
uint32_t pipeline_stop_sensor();  // returns ticks the sensor was active
void pipeline_print_report(uint32_t run_ticks);
bool pipeline_self_check(uint32_t run_ticks);

void console_printf(const char *fmt, ...);

}  // namespace telemetry

#endif  // TELEMETRY_HPP
