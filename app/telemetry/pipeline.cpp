/*
 * BareMetal-RTOS - telemetry demo: threads and the interrupt-driven data path
 *
 *   TIMER0A ISR (1 kHz) --push--> RingBuffer<Sample,64> --sem--> processing (prio 8)
 *                                                                   |  mutex: SharedState
 *                                                                   v
 *   logger (prio 2) <--Queue<Summary,4>--+                controller (prio 6, 100 Hz)
 *   background x2 (prio 1, round-robin)
 *
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>

#include "board.h"
#include "rtos_port.h"
#include "telemetry.hpp"

namespace telemetry {
namespace {

constexpr uint32_t kCyclesPerUs = RTOS_CPU_HZ / 1000000u;
constexpr uint8_t kSensorIrqPrio = 0x40;  // above SysTick/PendSV (0xF0/0xFF)

// ---------------------------------------------------------------- objects
rtos::RingBuffer<Sample, 64> sample_ring;
rtos::Semaphore data_ready(0, 0xFFFF);
rtos::Queue<Summary, 4> summary_queue;
rtos::Mutex state_mutex;
rtos::Mutex console_mutex;
SharedState shared;

rtos::Thread<384> processing_thread;
rtos::Thread<256> controller_thread;
rtos::Thread<384> logger_thread;
rtos::Thread<192> background_threads[2];

volatile uint32_t isr_seq = 0;
volatile uint32_t isr_dropped = 0;
volatile bool sensor_running = false;
volatile bool processing_done = false;
uint32_t sensor_start_tick = 0;

// Statistics gathered for the final report.
uint32_t processed = 0;
uint32_t sequence_gaps = 0;
uint32_t lat_min_all = 0xFFFFFFFFu, lat_max_all = 0;
uint64_t lat_sum_all = 0;
uint32_t windows_logged = 0;
uint32_t control_iterations = 0;
uint32_t control_max_jitter = 0;
volatile uint32_t background_work[2];

// ---------------------------------------------------------------- helpers
int32_t isqrt(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<int32_t>(r);
}

int32_t magnitude(int32_t x, int32_t y, int32_t z)
{
    return isqrt(static_cast<uint32_t>(x * x + y * y + z * z));
}

// ---------------------------------------------------------------- threads

// Drains the ring buffer, filters, detects shocks, measures ISR-to-thread latency.
void processing_entry(void *)
{
    int32_t fx = 0, fy = 0, fz = 1000;  // IIR low-pass state
    bool armed = true;
    uint32_t expected_seq = 0;
    Summary w{};
    int64_t mag_sum = 0;
    uint32_t lat_sum = 0;
    w.lat_min_us = 0xFFFFFFFFu;

    for (;;) {
        if (data_ready.take(rtos::ms_to_ticks(100)) != RTOS_OK) {
            if (!sensor_running && sample_ring.size() == 0u) {
                processing_done = true;  // sensor stopped and the ring is drained
                return;
            }
            continue;
        }
        Sample s;
        while (sample_ring.pop(s)) {
            uint32_t now = port_cycles();
            uint32_t lat_us = (now - s.t_cycles) / kCyclesPerUs;
            if (s.seq != expected_seq) {
                sequence_gaps++;
            }
            expected_seq = s.seq + 1u;

            fx += (s.ax - fx) >> 3;
            fy += (s.ay - fy) >> 3;
            fz += (s.az - fz) >> 3;
            int32_t raw_mag = magnitude(s.ax, s.ay, s.az);
            int32_t filt_mag = magnitude(fx, fy, fz);

            {
                rtos::LockGuard lock(state_mutex);
                shared.filtered_mag_mg = filt_mag;
                if (armed && raw_mag > kShockThresholdMg) {
                    shared.shock_events++;
                    armed = false;
                } else if (!armed && raw_mag < kShockRearmMg) {
                    armed = true;
                }
                w.shock_events = shared.shock_events;
                w.control_out = shared.control_out;
            }

            processed++;
            w.samples++;
            mag_sum += raw_mag;
            if (raw_mag > w.peak_mag_mg) {
                w.peak_mag_mg = raw_mag;
            }
            lat_sum += lat_us;
            if (lat_us < w.lat_min_us) w.lat_min_us = lat_us;
            if (lat_us > w.lat_max_us) w.lat_max_us = lat_us;
            if (lat_us < lat_min_all) lat_min_all = lat_us;
            if (lat_us > lat_max_all) lat_max_all = lat_us;
            lat_sum_all += lat_us;

            if (w.samples == kWindowSamples) {
                w.mean_mag_mg = static_cast<int32_t>(mag_sum / w.samples);
                w.lat_avg_us = lat_sum / w.samples;
                w.dropped = isr_dropped;
                summary_queue.send(w, rtos::ms_to_ticks(50));
                uint32_t next_window = w.window + 1u;
                w = Summary{};
                w.window = next_window;
                w.lat_min_us = 0xFFFFFFFFu;
                mag_sum = 0;
                lat_sum = 0;
            }
        }
    }
}

// 100 Hz integral controller: drives the actuator so the filtered |a| settles at 1 g.
void controller_entry(void *)
{
    int32_t integral = 0;  // actuator command in 0.1 mg
    uint32_t last = rtos_ticks();
    while (sensor_running) {
        rtos_sleep_until(&last, kControlPeriodTicks);
        uint32_t jitter = rtos_ticks() - last;
        if (jitter > control_max_jitter) {
            control_max_jitter = jitter;
        }
        rtos::LockGuard lock(state_mutex);
        int32_t error = shared.filtered_mag_mg - kTargetMg;
        if (error > 200) error = 200;  // ignore impact spikes
        if (error < -200) error = -200;
        integral += error * 2;
        if (integral > 2000) integral = 2000;
        if (integral < -2000) integral = -2000;
        shared.control_out = integral / 10;
        sensor::set_actuator(shared.control_out);
        control_iterations++;
    }
}

// Lowest-priority consumer: prints one line per window.
void logger_entry(void *)
{
    console_printf("  win | samples | mean |  peak | shocks | lat min/avg/max (us) | drop | ctrl\n");
    console_printf("  ----+---------+------+-------+--------+----------------------+------+-----\n");
    Summary s;
    for (;;) {
        if (summary_queue.recv(s, rtos::ms_to_ticks(100)) != RTOS_OK) {
            if (processing_done) {
                return;  // producer finished and the queue is empty
            }
            continue;
        }
        console_printf("  %3u | %7u | %4d | %5d | %6u | %5u / %5u / %5u  | %4u | %4d\n", s.window,
                       s.samples, s.mean_mag_mg, s.peak_mag_mg, s.shock_events, s.lat_min_us,
                       s.lat_avg_us, s.lat_max_us, s.dropped, s.control_out);
        windows_logged++;
    }
}

// Two equal-priority CPU hogs: they only run when everything else is idle and share the
// CPU through round-robin time slicing.
void background_entry(void *arg)
{
    uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    uint32_t crc = 0xFFFFFFFFu;
    while (sensor_running) {
        for (uint32_t i = 0; i < 64; i++) {
            crc ^= i;
            for (int b = 0; b < 8; b++) {
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
        }
        background_work[id]++;
    }
}

}  // namespace

// ---------------------------------------------------------------- ISR

extern "C" void TIMER0A_IRQHandler(void)
{
    board_timer_ack(0);
    uint32_t seq = isr_seq++;
    Sample s = sensor::read(seq, port_cycles());
    if (!sample_ring.push(s)) {
        isr_dropped++;
    }
    data_ready.give();
}

// ---------------------------------------------------------------- API

void console_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (rtos_is_running() && !port_in_isr()) {
        rtos::LockGuard lock(console_mutex);
        board_vprintf(fmt, ap);
    } else {
        board_vprintf(fmt, ap);
    }
    va_end(ap);
}

void pipeline_start()
{
    sensor::reset();
    sensor_running = true;
    processing_done = false;
    processing_thread.start("processing", processing_entry, nullptr, 8);
    controller_thread.start("controller", controller_entry, nullptr, 6);
    logger_thread.start("logger", logger_entry, nullptr, 2);
    background_threads[0].start("background-0", background_entry, reinterpret_cast<void *>(0), 1);
    background_threads[1].start("background-1", background_entry, reinterpret_cast<void *>(1), 1);
    sensor_start_tick = rtos_ticks();
    board_timer_start(0, kSampleRateHz, kSensorIrqPrio);
}

uint32_t pipeline_stop_sensor()
{
    board_timer_stop(0);
    uint32_t active = rtos_ticks() - sensor_start_tick;
    sensor_running = false;
    processing_thread.join();
    controller_thread.join();
    background_threads[0].join();
    background_threads[1].join();
    logger_thread.join();
    return active;
}

namespace {
void print_cpu_share(uint64_t cycles, uint64_t total)
{
    uint32_t pct10 = total ? static_cast<uint32_t>(cycles * 1000u / total) : 0u;
    console_printf("%3u.%u %%", pct10 / 10u, pct10 % 10u);
}

template <uint32_t N>
void print_thread_row(const char *name, const rtos::Thread<N> &t, uint64_t total)
{
    const rtos_thread_t *h = t.handle();
    uint32_t used = (N - t.stack_free_words()) * 4u;
    console_printf("  %-13s %4u %9u   %5u / %5u B   ", name, h->base_prio, h->switches, used, N * 4u);
    print_cpu_share(rtos_thread_run_cycles(h), total);
    console_printf("\n");
}
}  // namespace

void pipeline_print_report(uint32_t run_ticks)
{
    console_printf("\n--- Run report (sensor active %u ms) ---\n", run_ticks * 1000u / RTOS_TICK_HZ);
    console_printf("  samples produced by ISR   : %u\n", isr_seq);
    console_printf("  samples processed         : %u\n", processed);
    console_printf("  ring-buffer overflows     : %u\n", isr_dropped);
    console_printf("  sequence gaps             : %u\n", sequence_gaps);
    console_printf("  shocks injected/detected  : %u / %u\n", sensor::shocks_injected(),
                   shared.shock_events);
    console_printf("  ISR->thread latency (us)  : min %u, avg %u, max %u\n", lat_min_all,
                   processed ? static_cast<uint32_t>(lat_sum_all / processed) : 0u, lat_max_all);
    console_printf("  controller iterations     : %u (max wake jitter %u ticks)\n",
                   control_iterations, control_max_jitter);
    console_printf("  windows logged            : %u\n", windows_logged);
    console_printf("  background work units     : %u / %u\n", background_work[0], background_work[1]);
    // Exact per-thread CPU time, charged by the context switch. Time spent in interrupt
    // handlers is billed to whichever thread they interrupted.
    uint64_t total = port_cycles();
    console_printf("\n  thread        prio  switches   stack used / size      cpu*\n");
    print_thread_row("processing", processing_thread, total);
    print_thread_row("controller", controller_thread, total);
    print_thread_row("logger", logger_thread, total);
    print_thread_row("background-0", background_threads[0], total);
    print_thread_row("background-1", background_threads[1], total);
    console_printf("  %-13s %4s %9s   %15s   ", "idle", "0", "-", "-");
    print_cpu_share(rtos_idle_run_cycles(), total);
    console_printf("\n  * share of CPU cycles since boot (ISR time billed to the interrupted thread)\n");
}

bool pipeline_self_check(uint32_t run_ticks)
{
    bool ok = true;
    auto expect = [&ok](bool cond, const char *what) {
        if (!cond) {
            console_printf("  check failed: %s\n", what);
            ok = false;
        }
    };
    uint32_t expected_samples = run_ticks * kSampleRateHz / RTOS_TICK_HZ;
    expect(isr_seq + 5u >= expected_samples, "sensor ISR rate");
    expect(processed == isr_seq, "every sample processed");
    expect(isr_dropped == 0u, "no ring-buffer overflow");
    expect(sequence_gaps == 0u, "no sequence gaps");
    expect(shared.shock_events == sensor::shocks_injected(), "all shocks detected");
    expect(sensor::shocks_injected() > 0u, "shocks were injected");
    expect(control_iterations + 2u >= run_ticks / kControlPeriodTicks, "controller ran at 100 Hz");
    expect(windows_logged == processed / kWindowSamples, "every window logged");
    expect(background_work[0] > 0u && background_work[1] > 0u, "round-robin background threads");
    return ok;
}

}  // namespace telemetry
