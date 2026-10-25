/*
 * BareMetal-RTOS - public kernel API
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 *
 * A small preemptive, priority-based kernel for ARM Cortex-M.
 *  - 32 priority levels (0 = idle, 31 = most urgent), O(1) selection via a bitmap
 *  - round-robin time slicing among threads of equal priority
 *  - SysTick-driven tick, context switches in PendSV
 *  - mutexes with priority inheritance, counting semaphores, message queues
 *
 * All objects are statically allocated by the caller; the kernel never calls malloc.
 */
#ifndef RTOS_H
#define RTOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtos/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTOS_WAIT_FOREVER 0xFFFFFFFFu
#define RTOS_NO_WAIT      0u
#define RTOS_PRIO_IDLE    0u

typedef enum {
    RTOS_OK          = 0,
    RTOS_ERR_TIMEOUT = -1, /* wait expired, or resource unavailable with RTOS_NO_WAIT */
    RTOS_ERR_PARAM   = -2, /* invalid argument */
    RTOS_ERR_STATE   = -3, /* object or kernel in the wrong state (e.g. recursive lock) */
    RTOS_ERR_PERM    = -4, /* caller is not allowed (e.g. unlocking a mutex it does not own) */
    RTOS_ERR_FULL    = -5, /* semaphore already at its maximum count */
    RTOS_ERR_ISR     = -6  /* blocking call attempted from an interrupt handler */
} rtos_status_t;

typedef enum {
    RTOS_THREAD_INACTIVE = 0, /* never started */
    RTOS_THREAD_READY,        /* runnable or running */
    RTOS_THREAD_BLOCKED,      /* sleeping or waiting on an object */
    RTOS_THREAD_DEAD          /* entry function returned */
} rtos_thread_state_t;

typedef void (*rtos_entry_t)(void *arg);

struct rtos_thread;
struct rtos_mutex;

typedef struct rtos_list {
    struct rtos_thread *head;
    struct rtos_thread *tail;
} rtos_list_t;

/* Thread control block. Fields are private to the kernel; treat as opaque. */
typedef struct rtos_thread {
    uint32_t *sp;                 /* saved process stack pointer: MUST stay first */
    struct rtos_thread *next;     /* ready-list or wait-list links */
    struct rtos_thread *prev;
    rtos_list_t *list;            /* list this thread is currently linked into */
    struct rtos_thread *dnext;    /* timed-wait (delay) list link */
    uint32_t wake_tick;
    bool in_delay;
    uint8_t prio;                 /* effective priority (may be boosted) */
    uint8_t base_prio;            /* priority assigned at creation */
    uint8_t state;                /* rtos_thread_state_t */
    int32_t wait_result;
    uint32_t slice;               /* remaining round-robin ticks */
    struct rtos_mutex *held;      /* mutexes owned by this thread */
    struct rtos_mutex *waiting_on;
    rtos_list_t joiners;
    const char *name;
    uint32_t *stack_base;
    uint32_t stack_words;
    uint32_t switches;            /* number of times this thread was switched in */
    uint64_t run_cycles;          /* CPU cycles spent running, charged at each switch */
} rtos_thread_t;

typedef struct rtos_mutex {
    rtos_thread_t *owner;
    rtos_list_t waiters;          /* priority ordered */
    struct rtos_mutex *next_held; /* link in the owner's held list */
} rtos_mutex_t;

typedef struct rtos_sem {
    uint32_t count;
    uint32_t max;
    rtos_list_t waiters;          /* priority ordered */
} rtos_sem_t;

typedef struct rtos_queue {
    uint8_t *buf;
    uint32_t item_size;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    rtos_sem_t items;
    rtos_sem_t spaces;
} rtos_queue_t;

/* ---- kernel ---- */
void rtos_init(void);
void rtos_start(void) __attribute__((noreturn));
bool rtos_is_running(void);
uint32_t rtos_ticks(void);
uint32_t rtos_idle_ticks(void);
void rtos_tick(void); /* called by the port's SysTick handler */
void rtos_panic(const char *msg) __attribute__((noreturn));
/* Weak hook: override to report panics (default spins forever). */
void rtos_panic_hook(const char *msg);

/* ---- threads ---- */
int rtos_thread_create(rtos_thread_t *t, const char *name, rtos_entry_t entry, void *arg,
                       uint8_t prio, uint32_t *stack, uint32_t stack_words);
int rtos_thread_join(rtos_thread_t *t, uint32_t timeout_ticks);
rtos_thread_t *rtos_self(void);
void rtos_yield(void);
void rtos_sleep(uint32_t ticks);
void rtos_sleep_until(uint32_t *last_wake, uint32_t period);
uint8_t rtos_thread_prio(const rtos_thread_t *t);
uint32_t rtos_thread_stack_free(const rtos_thread_t *t); /* unused stack, in words */
uint64_t rtos_thread_run_cycles(const rtos_thread_t *t); /* exact CPU time in core cycles */
uint64_t rtos_idle_run_cycles(void);

/* ---- mutex (priority inheritance, non-recursive, thread context only, after rtos_start) ---- */
int rtos_mutex_init(rtos_mutex_t *m);
int rtos_mutex_lock(rtos_mutex_t *m, uint32_t timeout_ticks);
int rtos_mutex_unlock(rtos_mutex_t *m);

/* ---- counting semaphore (give is ISR safe; take with RTOS_NO_WAIT is ISR safe) ---- */
int rtos_sem_init(rtos_sem_t *s, uint32_t initial, uint32_t max);
int rtos_sem_take(rtos_sem_t *s, uint32_t timeout_ticks);
int rtos_sem_give(rtos_sem_t *s);
uint32_t rtos_sem_count(const rtos_sem_t *s);

/* ---- message queue of fixed-size items (RTOS_NO_WAIT calls are ISR safe) ---- */
int rtos_queue_init(rtos_queue_t *q, void *storage, uint32_t item_size, uint32_t capacity);
int rtos_queue_send(rtos_queue_t *q, const void *item, uint32_t timeout_ticks);
int rtos_queue_recv(rtos_queue_t *q, void *item, uint32_t timeout_ticks);
uint32_t rtos_queue_count(const rtos_queue_t *q);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_H */
