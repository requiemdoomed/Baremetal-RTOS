/*
 * BareMetal-RTOS - host unit tests for the lock-free SPSC ring buffer
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 *
 * The ring buffer header is portable, so the exact code that runs between an ISR and a
 * thread on Cortex-M is stress-tested here with two real OS threads on a multi-core host
 * (and optionally under ThreadSanitizer).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "rtos/ringbuf.h"

// The C++ wrapper only needs the header-only ring buffer for this test.
#include "rtos/rtos.hpp"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

static void test_init_validation()
{
    rtos_ringbuf_t rb;
    uint32_t storage[16];
    CHECK(!rtos_rb_init(nullptr, storage, 4, 16));
    CHECK(!rtos_rb_init(&rb, nullptr, 4, 16));
    CHECK(!rtos_rb_init(&rb, storage, 0, 16));
    CHECK(!rtos_rb_init(&rb, storage, 4, 0));
    CHECK(!rtos_rb_init(&rb, storage, 4, 12));
    CHECK(rtos_rb_init(&rb, storage, 4, 16));
    CHECK(rtos_rb_capacity(&rb) == 16u);
    CHECK(rtos_rb_count(&rb) == 0u);
}

static void test_fill_drain_wrap()
{
    rtos_ringbuf_t rb;
    uint32_t storage[8];
    rtos_rb_init(&rb, storage, sizeof(uint32_t), 8);
    uint32_t next_in = 0, next_out = 0, v = 0;
    // Push/pop in uneven batches many times so indices wrap the buffer repeatedly.
    for (int round = 0; round < 1000; round++) {
        int n_push = 1 + round % 8;
        for (int i = 0; i < n_push; i++) {
            bool full = rtos_rb_count(&rb) == 8u;
            bool ok = rtos_rb_push(&rb, &next_in);
            CHECK(ok == !full);
            if (ok) next_in++;
        }
        int n_pop = 1 + (round * 7) % 8;
        for (int i = 0; i < n_pop; i++) {
            bool empty = rtos_rb_count(&rb) == 0u;
            bool ok = rtos_rb_pop(&rb, &v);
            CHECK(ok == !empty);
            if (ok) {
                CHECK(v == next_out);
                next_out++;
            }
        }
    }
    CHECK(rtos_rb_count(&rb) == next_in - next_out);
}

static void test_index_overflow()
{
    // Free-running indices must survive wrap-around of the 32-bit counters.
    rtos_ringbuf_t rb;
    uint16_t storage[4];
    rtos_rb_init(&rb, storage, sizeof(uint16_t), 4);
    rb.head = rb.tail = 0xFFFFFFFEu;
    for (uint16_t i = 0; i < 4; i++) CHECK(rtos_rb_push(&rb, &i));
    uint16_t extra = 99;
    CHECK(!rtos_rb_push(&rb, &extra));
    CHECK(rtos_rb_count(&rb) == 4u);
    for (uint16_t i = 0; i < 4; i++) {
        uint16_t v = 0;
        CHECK(rtos_rb_pop(&rb, &v));
        CHECK(v == i);
    }
    CHECK(rtos_rb_count(&rb) == 0u);
}

struct Record {
    uint32_t seq;
    uint32_t check;
    uint8_t payload[24];
};

static void test_struct_elements()
{
    rtos::RingBuffer<Record, 4> rb;
    Record r{};
    for (uint32_t i = 0; i < 4; i++) {
        r.seq = i;
        std::memset(r.payload, static_cast<int>(i), sizeof(r.payload));
        CHECK(rb.push(r));
    }
    CHECK(!rb.push(r));
    for (uint32_t i = 0; i < 4; i++) {
        Record out{};
        CHECK(rb.pop(out));
        CHECK(out.seq == i && out.payload[0] == i && out.payload[23] == i);
    }
    CHECK(rb.size() == 0u);
}

// Producer and consumer on separate cores: every element arrives exactly once, in order,
// with an intact payload (detects torn reads from missing acquire/release ordering).
static void test_spsc_stress()
{
    constexpr uint32_t kItems = 2'000'000;
    static rtos::RingBuffer<Record, 64> rb;
    uint32_t producer_full_spins = 0;

    std::thread producer([&] {
        Record r{};
        for (uint32_t i = 0; i < kItems; i++) {
            r.seq = i;
            r.check = i * 2654435761u;
            std::memset(r.payload, static_cast<int>(i & 0xFF), sizeof(r.payload));
            while (!rb.push(r)) {
                producer_full_spins++;
                std::this_thread::yield();
            }
        }
    });

    uint32_t expected = 0, bad_order = 0, bad_payload = 0;
    Record r{};
    while (expected < kItems) {
        if (!rb.pop(r)) {
            std::this_thread::yield();
            continue;
        }
        if (r.seq != expected) bad_order++;
        if (r.check != r.seq * 2654435761u) bad_payload++;
        for (uint8_t b : r.payload) {
            if (b != (r.seq & 0xFF)) {
                bad_payload++;
                break;
            }
        }
        expected = r.seq + 1;
    }
    producer.join();
    CHECK(bad_order == 0u);
    CHECK(bad_payload == 0u);
    CHECK(rb.size() == 0u);
    std::printf("  stress: %u items transferred, producer hit full %u times\n", kItems,
                producer_full_spins);
}

int main()
{
    std::printf("=== BareMetal-RTOS host ring buffer tests ===\n");
    struct {
        const char *name;
        void (*fn)();
    } tests[] = {
        {"init validation", test_init_validation},
        {"fill / drain / wrap", test_fill_drain_wrap},
        {"32-bit index overflow", test_index_overflow},
        {"struct elements via C++ wrapper", test_struct_elements},
        {"two-thread SPSC stress", test_spsc_stress},
    };
    for (auto &t : tests) {
        int before = failures;
        t.fn();
        std::printf("[%s] %s\n", failures == before ? " OK " : "FAIL", t.name);
    }
    std::printf("=== %d/%d checks passed ===\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
