/*
 * BareMetal-RTOS - lock-free single-producer / single-consumer ring buffer
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 *
 * One context (typically an interrupt handler) pushes, one context (a thread) pops.
 * No locks and no interrupt masking: the producer owns `head`, the consumer owns `tail`,
 * and acquire/release ordering publishes the element data. Indices run freely and are
 * masked on access, so all `capacity` slots are usable. Capacity must be a power of two.
 *
 * Header-only and portable: the same code runs on Cortex-M and on the host test suite.
 */
#ifndef RTOS_RINGBUF_H
#define RTOS_RINGBUF_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;
    uint32_t elem_size;
    uint32_t mask;
    uint32_t head; /* written only by the producer */
    uint32_t tail; /* written only by the consumer */
} rtos_ringbuf_t;

static inline bool rtos_rb_init(rtos_ringbuf_t *rb, void *storage, uint32_t elem_size,
                                uint32_t capacity)
{
    if (!rb || !storage || elem_size == 0u || capacity == 0u ||
        (capacity & (capacity - 1u)) != 0u) {
        return false;
    }
    rb->buf = (uint8_t *)storage;
    rb->elem_size = elem_size;
    rb->mask = capacity - 1u;
    rb->head = 0u;
    rb->tail = 0u;
    return true;
}

static inline uint32_t rtos_rb_capacity(const rtos_ringbuf_t *rb) { return rb->mask + 1u; }

static inline uint32_t rtos_rb_count(const rtos_ringbuf_t *rb)
{
    uint32_t h = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    return h - t;
}

/* Producer side. Returns false (and drops nothing) when full. */
static inline bool rtos_rb_push(rtos_ringbuf_t *rb, const void *item)
{
    uint32_t h = __atomic_load_n(&rb->head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    if (h - t > rb->mask) {
        return false;
    }
    memcpy(rb->buf + (size_t)(h & rb->mask) * rb->elem_size, item, rb->elem_size);
    __atomic_store_n(&rb->head, h + 1u, __ATOMIC_RELEASE);
    return true;
}

/* Consumer side. Returns false when empty. */
static inline bool rtos_rb_pop(rtos_ringbuf_t *rb, void *item)
{
    uint32_t t = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    uint32_t h = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    if (h == t) {
        return false;
    }
    memcpy(item, rb->buf + (size_t)(t & rb->mask) * rb->elem_size, rb->elem_size);
    __atomic_store_n(&rb->tail, t + 1u, __ATOMIC_RELEASE);
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* RTOS_RINGBUF_H */
