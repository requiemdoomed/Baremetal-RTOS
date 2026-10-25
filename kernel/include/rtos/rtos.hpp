/*
 * BareMetal-RTOS - zero-overhead C++ wrappers over the C kernel API
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 *
 * Designed for -fno-exceptions -fno-rtti builds: no heap, no virtuals.
 */
#ifndef RTOS_HPP
#define RTOS_HPP

#include <stddef.h>
#include <stdint.h>

#include "rtos/ringbuf.h"
#include "rtos/rtos.h"

namespace rtos {

using Ticks = uint32_t;
constexpr Ticks kForever = RTOS_WAIT_FOREVER;
constexpr Ticks kNoWait = RTOS_NO_WAIT;

constexpr Ticks ms_to_ticks(uint32_t ms) { return (ms * RTOS_TICK_HZ + 999u) / 1000u; }

/* A thread with its stack embedded in the object. */
template <uint32_t StackWords>
class Thread {
    static_assert(StackWords >= RTOS_MIN_STACK_WORDS, "stack too small");

public:
    int start(const char *name, rtos_entry_t entry, void *arg, uint8_t prio)
    {
        return rtos_thread_create(&tcb_, name, entry, arg, prio, stack_, StackWords);
    }
    int join(Ticks timeout = kForever) { return rtos_thread_join(&tcb_, timeout); }
    rtos_thread_t *handle() { return &tcb_; }
    const rtos_thread_t *handle() const { return &tcb_; }
    uint32_t stack_free_words() const { return rtos_thread_stack_free(&tcb_); }
    static constexpr uint32_t stack_words() { return StackWords; }

private:
    rtos_thread_t tcb_{};
    alignas(8) uint32_t stack_[StackWords]{};
};

class Mutex {
public:
    Mutex() { rtos_mutex_init(&m_); }
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
    int lock(Ticks timeout = kForever) { return rtos_mutex_lock(&m_, timeout); }
    int unlock() { return rtos_mutex_unlock(&m_); }

private:
    rtos_mutex_t m_{};
};

class LockGuard {
public:
    explicit LockGuard(Mutex &m) : m_(m) { m_.lock(); }
    ~LockGuard() { m_.unlock(); }
    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

private:
    Mutex &m_;
};

class Semaphore {
public:
    Semaphore(uint32_t initial, uint32_t max) { rtos_sem_init(&s_, initial, max); }
    Semaphore(const Semaphore &) = delete;
    Semaphore &operator=(const Semaphore &) = delete;
    int take(Ticks timeout = kForever) { return rtos_sem_take(&s_, timeout); }
    int give() { return rtos_sem_give(&s_); } /* ISR safe */
    uint32_t count() const { return rtos_sem_count(&s_); }

private:
    rtos_sem_t s_{};
};

template <typename T, uint32_t N>
class Queue {
public:
    Queue() { rtos_queue_init(&q_, storage_, sizeof(T), N); }
    Queue(const Queue &) = delete;
    Queue &operator=(const Queue &) = delete;
    int send(const T &item, Ticks timeout = kForever) { return rtos_queue_send(&q_, &item, timeout); }
    int recv(T &item, Ticks timeout = kForever) { return rtos_queue_recv(&q_, &item, timeout); }
    uint32_t count() const { return rtos_queue_count(&q_); }

private:
    rtos_queue_t q_{};
    alignas(T) uint8_t storage_[sizeof(T) * N]{};
};

/* Lock-free SPSC ring buffer: push from one ISR/thread, pop from one thread. */
template <typename T, uint32_t N>
class RingBuffer {
    static_assert(N > 0u && (N & (N - 1u)) == 0u, "capacity must be a power of two");

public:
    RingBuffer() { rtos_rb_init(&rb_, storage_, sizeof(T), N); }
    RingBuffer(const RingBuffer &) = delete;
    RingBuffer &operator=(const RingBuffer &) = delete;
    bool push(const T &item) { return rtos_rb_push(&rb_, &item); }
    bool pop(T &item) { return rtos_rb_pop(&rb_, &item); }
    uint32_t size() const { return rtos_rb_count(&rb_); }
    static constexpr uint32_t capacity() { return N; }

private:
    rtos_ringbuf_t rb_{};
    alignas(T) uint8_t storage_[sizeof(T) * N]{};
};

} // namespace rtos

#endif /* RTOS_HPP */
