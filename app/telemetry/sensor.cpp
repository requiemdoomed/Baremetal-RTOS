/*
 * BareMetal-RTOS - telemetry demo: deterministic synthetic 3-axis accelerometer
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 *
 * Models a vibrating platform: two sinusoids on X/Y, gravity on Z plus a constant 80 mg
 * tilt error, pseudo-random noise, and a short impact spike every second. The controller
 * thread drives an actuator that cancels the tilt, closing the loop through the sensor.
 * Integer math only (Cortex-M3 has no FPU).
 */
#include "telemetry.hpp"

namespace telemetry {
namespace sensor {
namespace {

uint32_t rng_state = 0x12345678u;
uint32_t injected = 0;
volatile int32_t actuator_mg = 0;

uint32_t xorshift32()
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

// Bhaskara I approximation of sin(deg) scaled by 1000, |error| < 0.2 %.
int32_t sin_milli(uint32_t deg)
{
    deg %= 360u;
    int32_t sign = 1;
    if (deg >= 180u) {
        deg -= 180u;
        sign = -1;
    }
    int32_t x = static_cast<int32_t>(deg);
    int32_t num = 4 * x * (180 - x);
    int32_t den = 40500 - x * (180 - x);
    return sign * (num * 1000 / den);
}

int16_t noise(int32_t amplitude)
{
    return static_cast<int16_t>(static_cast<int32_t>(xorshift32() % (2u * amplitude + 1u)) -
                                amplitude);
}

}  // namespace

void reset()
{
    rng_state = 0x12345678u;
    injected = 0;
    actuator_mg = 0;
}

void set_actuator(int32_t mg) { actuator_mg = mg; }

Sample read(uint32_t seq, uint32_t t_cycles)
{
    Sample s{};
    s.seq = seq;
    s.t_cycles = t_cycles;
    // 5 Hz and 12 Hz vibration at 1 kHz sampling.
    int32_t ax = 300 * sin_milli((seq * 5u * 360u / kSampleRateHz)) / 1000;
    int32_t ay = 180 * sin_milli((seq * 12u * 360u / kSampleRateHz) + 90u) / 1000;
    int32_t az = 1000 + 80 - actuator_mg;  // gravity + tilt error - correction
    // Impact: 4-sample spike once per second.
    uint32_t phase = seq % kSampleRateHz;
    if (phase >= 500u && phase < 504u) {
        az += 2600;
        ax += 900;
        if (phase == 500u) {
            injected++;
        }
    }
    s.ax = static_cast<int16_t>(ax + noise(20));
    s.ay = static_cast<int16_t>(ay + noise(20));
    s.az = static_cast<int16_t>(az + noise(20));
    return s;
}

uint32_t shocks_injected() { return injected; }

}  // namespace sensor
}  // namespace telemetry
