/*
 * BareMetal-RTOS - scheduler, threads and time services
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "kernel_internal.h"

rtos_thread_t *volatile rtos_cur;
rtos_thread_t *volatile rtos_next;

static rtos_list_t ready_lists[RTOS_MAX_PRIO];
static uint32_t ready_bitmap;          /* bit p set <=> ready_lists[p] non-empty */
static rtos_thread_t *delay_head;      /* sorted by wake_tick */
static volatile uint32_t tick_count;
static volatile uint32_t idle_tick_count;
static volatile bool started;

static rtos_thread_t idle_tcb;
static uint32_t idle_stack[RTOS_IDLE_STACK_WORDS] __attribute__((aligned(8)));

_Static_assert(RTOS_MAX_PRIO <= 32u, "ready bitmap is 32 bits wide");
_Static_assert(offsetof(rtos_thread_t, sp) == 0u, "PendSV expects sp at offset 0");

/* ------------------------------------------------------------------ lists */

void k_list_push_back(rtos_list_t *l, rtos_thread_t *t)
{
    t->next = NULL;
    t->prev = l->tail;
    if (l->tail) {
        l->tail->next = t;
    } else {
        l->head = t;
    }
    l->tail = t;
    t->list = l;
}

/* Highest priority first; FIFO among equal priorities. */
void k_list_insert_prio(rtos_list_t *l, rtos_thread_t *t)
{
    rtos_thread_t *p = l->head;
    while (p && p->prio >= t->prio) {
        p = p->next;
    }
    if (!p) {
        k_list_push_back(l, t);
        return;
    }
    t->next = p;
    t->prev = p->prev;
    if (p->prev) {
        p->prev->next = t;
    } else {
        l->head = t;
    }
    p->prev = t;
    t->list = l;
}

void k_list_remove(rtos_thread_t *t)
{
    rtos_list_t *l = t->list;
    if (!l) {
        return;
    }
    if (t->prev) {
        t->prev->next = t->next;
    } else {
        l->head = t->next;
    }
    if (t->next) {
        t->next->prev = t->prev;
    } else {
        l->tail = t->prev;
    }
    t->next = t->prev = NULL;
    t->list = NULL;
}

/* ------------------------------------------------------------- ready set */

static void ready_add(rtos_thread_t *t)
{
    k_list_push_back(&ready_lists[t->prio], t);
    ready_bitmap |= 1u << t->prio;
}

static void ready_remove(rtos_thread_t *t)
{
    uint8_t p = t->prio;
    k_list_remove(t);
    if (!ready_lists[p].head) {
        ready_bitmap &= ~(1u << p);
    }
}

static rtos_thread_t *highest_ready(void)
{
    uint32_t p = 31u - (uint32_t)__builtin_clz(ready_bitmap);
    return ready_lists[p].head;
}

/* ------------------------------------------------------------ delay list */

static void delay_insert(rtos_thread_t *t, uint32_t wake)
{
    rtos_thread_t **pp = &delay_head;
    t->wake_tick = wake;
    while (*pp && (int32_t)((*pp)->wake_tick - wake) <= 0) {
        pp = &(*pp)->dnext;
    }
    t->dnext = *pp;
    *pp = t;
    t->in_delay = true;
}

static void delay_remove(rtos_thread_t *t)
{
    if (!t->in_delay) {
        return;
    }
    rtos_thread_t **pp = &delay_head;
    while (*pp && *pp != t) {
        pp = &(*pp)->dnext;
    }
    if (*pp) {
        *pp = t->dnext;
    }
    t->dnext = NULL;
    t->in_delay = false;
}

/* ------------------------------------------------------- core primitives */

bool k_started(void) { return started; }

void k_reschedule(void)
{
    if (!started) {
        return;
    }
    rtos_thread_t *t = highest_ready();
    rtos_next = t;
    if (t != rtos_cur) {
        port_pend_switch();
    }
}

void k_block_current(rtos_list_t *wait_list, uint32_t timeout_ticks)
{
    rtos_thread_t *t = rtos_cur;
    ready_remove(t);
    t->state = RTOS_THREAD_BLOCKED;
    t->wait_result = RTOS_OK;
    if (wait_list) {
        k_list_insert_prio(wait_list, t);
    }
    if (timeout_ticks != RTOS_WAIT_FOREVER) {
        delay_insert(t, tick_count + timeout_ticks);
    }
    k_reschedule();
}

void k_wake(rtos_thread_t *t, int32_t result)
{
    k_list_remove(t);
    delay_remove(t);
    t->waiting_on = NULL;
    t->wait_result = result;
    t->state = RTOS_THREAD_READY;
    ready_add(t);
}

void k_set_prio(rtos_thread_t *t, uint8_t prio)
{
    if (t->prio == prio) {
        return;
    }
    if (t->state == RTOS_THREAD_READY) {
        ready_remove(t);
        t->prio = prio;
        ready_add(t);
    } else if (t->list) {
        rtos_list_t *l = t->list; /* a priority-ordered wait list */
        k_list_remove(t);
        t->prio = prio;
        k_list_insert_prio(l, t);
    } else {
        t->prio = prio;
    }
}

int k_check_can_block(uint32_t saved_irq)
{
    if (port_in_isr()) {
        return RTOS_ERR_ISR;
    }
    if (!started) {
        return RTOS_ERR_STATE;
    }
    if (saved_irq != 0u) {
        rtos_panic("blocking call with interrupts disabled");
    }
    return RTOS_OK;
}

/* ----------------------------------------------------------------- panic */

__attribute__((weak)) void rtos_panic_hook(const char *msg) { (void)msg; }

void rtos_panic(const char *msg)
{
    (void)port_irq_save();
    rtos_panic_hook(msg);
    for (;;) {
    }
}

/* --------------------------------------------------------------- threads */

static void thread_exit(void)
{
    uint32_t s = port_irq_save();
    rtos_thread_t *t = rtos_cur;
    if (t->held) {
        rtos_panic("thread exited while holding a mutex");
    }
    while (t->joiners.head) {
        k_wake(t->joiners.head, RTOS_OK);
    }
    ready_remove(t);
    t->state = RTOS_THREAD_DEAD;
    k_reschedule();
    port_irq_restore(s);
    for (;;) {
        /* PendSV switches away and never returns here */
    }
}

static int thread_setup(rtos_thread_t *t, const char *name, rtos_entry_t entry, void *arg,
                        uint8_t prio, uint32_t *stack, uint32_t stack_words)
{
    memset(t, 0, sizeof(*t));
    for (uint32_t i = 0; i < stack_words; i++) {
        stack[i] = RTOS_STACK_FILL;
    }
    t->name = name ? name : "?";
    t->prio = t->base_prio = prio;
    t->stack_base = stack;
    t->stack_words = stack_words;
    t->slice = RTOS_TIMESLICE_TICKS;
    t->sp = port_init_stack(stack + stack_words, entry, arg, thread_exit);
    t->state = RTOS_THREAD_READY;
    return RTOS_OK;
}

int rtos_thread_create(rtos_thread_t *t, const char *name, rtos_entry_t entry, void *arg,
                       uint8_t prio, uint32_t *stack, uint32_t stack_words)
{
    if (!t || !entry || !stack || stack_words < RTOS_MIN_STACK_WORDS || prio == RTOS_PRIO_IDLE ||
        prio >= RTOS_MAX_PRIO || ((uintptr_t)stack & 3u) != 0u) {
        return RTOS_ERR_PARAM;
    }
    uint32_t s = port_irq_save();
    if (t->state == RTOS_THREAD_READY || t->state == RTOS_THREAD_BLOCKED) {
        port_irq_restore(s);
        return RTOS_ERR_STATE;
    }
    thread_setup(t, name, entry, arg, prio, stack, stack_words);
    ready_add(t);
    k_reschedule();
    port_irq_restore(s);
    return RTOS_OK;
}

int rtos_thread_join(rtos_thread_t *t, uint32_t timeout_ticks)
{
    if (!t) {
        return RTOS_ERR_PARAM;
    }
    uint32_t s = port_irq_save();
    if (t->state == RTOS_THREAD_DEAD || t->state == RTOS_THREAD_INACTIVE) {
        port_irq_restore(s);
        return RTOS_OK;
    }
    if (t == rtos_cur) {
        port_irq_restore(s);
        return RTOS_ERR_STATE;
    }
    if (timeout_ticks == RTOS_NO_WAIT) {
        port_irq_restore(s);
        return RTOS_ERR_TIMEOUT;
    }
    int r = k_check_can_block(s);
    if (r != RTOS_OK) {
        port_irq_restore(s);
        return r;
    }
    k_block_current(&t->joiners, timeout_ticks);
    port_irq_restore(s);
    return rtos_cur->wait_result;
}

rtos_thread_t *rtos_self(void) { return rtos_cur; }

uint8_t rtos_thread_prio(const rtos_thread_t *t) { return t ? t->prio : 0u; }

uint32_t rtos_thread_stack_free(const rtos_thread_t *t)
{
    uint32_t n = 0;
    if (!t || !t->stack_base) {
        return 0;
    }
    while (n < t->stack_words && t->stack_base[n] == RTOS_STACK_FILL) {
        n++;
    }
    return n;
}

/* Called from PendSV with interrupts masked, just before rtos_cur changes. */
static uint32_t last_switch_cycles;
void k_account_switch(void)
{
    uint32_t now = port_cycles();
    if (rtos_cur) {
        rtos_cur->run_cycles += now - last_switch_cycles;
    }
    last_switch_cycles = now;
}

uint64_t rtos_thread_run_cycles(const rtos_thread_t *t)
{
    if (!t) {
        return 0u;
    }
    uint32_t s = port_irq_save();
    uint64_t v = t->run_cycles;
    if (t == rtos_cur) {
        v += port_cycles() - last_switch_cycles; /* include the slice in progress */
    }
    port_irq_restore(s);
    return v;
}

uint64_t rtos_idle_run_cycles(void) { return rtos_thread_run_cycles(&idle_tcb); }

void rtos_yield(void)
{
    if (!started || port_in_isr()) {
        return;
    }
    uint32_t s = port_irq_save();
    rtos_thread_t *c = rtos_cur;
    rtos_list_t *l = &ready_lists[c->prio];
    if (l->head != l->tail) {
        k_list_remove(c);
        k_list_push_back(l, c);
    }
    c->slice = RTOS_TIMESLICE_TICKS;
    k_reschedule();
    port_irq_restore(s);
}

void rtos_sleep(uint32_t ticks)
{
    if (ticks == 0u) {
        rtos_yield();
        return;
    }
    uint32_t s = port_irq_save();
    if (k_check_can_block(s) != RTOS_OK) {
        port_irq_restore(s);
        return;
    }
    k_block_current(NULL, ticks);
    port_irq_restore(s);
}

void rtos_sleep_until(uint32_t *last_wake, uint32_t period)
{
    uint32_t s = port_irq_save();
    if (k_check_can_block(s) != RTOS_OK) {
        port_irq_restore(s);
        return;
    }
    uint32_t wake = *last_wake + period;
    *last_wake = wake;
    int32_t remaining = (int32_t)(wake - tick_count);
    if (remaining > 0) {
        k_block_current(NULL, (uint32_t)remaining);
    }
    port_irq_restore(s);
}

/* ------------------------------------------------------------------ time */

uint32_t rtos_ticks(void) { return tick_count; }
uint32_t rtos_idle_ticks(void) { return idle_tick_count; }
bool rtos_is_running(void) { return started; }

void rtos_tick(void)
{
    uint32_t s = port_irq_save();
    tick_count++;

    rtos_thread_t *c = rtos_cur;
    if (c == &idle_tcb) {
        idle_tick_count++;
    }

    /* Release every thread whose deadline has arrived. */
    while (delay_head && (int32_t)(tick_count - delay_head->wake_tick) >= 0) {
        rtos_thread_t *t = delay_head;
        delay_head = t->dnext;
        t->dnext = NULL;
        t->in_delay = false;
        int32_t result = RTOS_OK; /* plain sleep */
        if (t->list) {            /* timed wait on an object */
            rtos_mutex_t *m = t->waiting_on;
            k_list_remove(t);
            t->waiting_on = NULL;
            if (m && m->owner) {
                k_mutex_recompute_prio(m->owner);
            }
            result = RTOS_ERR_TIMEOUT;
        }
        t->wait_result = result;
        t->state = RTOS_THREAD_READY;
        ready_add(t);
    }

    /* Round-robin among equal priorities. */
    if (c && c->state == RTOS_THREAD_READY && c != &idle_tcb) {
        if (--c->slice == 0u) {
            rtos_list_t *l = &ready_lists[c->prio];
            c->slice = RTOS_TIMESLICE_TICKS;
            if (l->head != l->tail) {
                k_list_remove(c);
                k_list_push_back(l, c);
            }
        }
    }

    /* Cheap stack overflow detection: the lowest word must still hold the fill pattern. */
    if (c && c->stack_base && c->stack_base[0] != RTOS_STACK_FILL) {
        rtos_panic("stack overflow");
    }

    k_reschedule();
    port_irq_restore(s);
}

/* ---------------------------------------------------------------- startup */

static void idle_entry(void *arg)
{
    (void)arg;
    for (;;) {
        port_wait_for_interrupt();
    }
}

void rtos_init(void)
{
    uint32_t s = port_irq_save();
    memset(ready_lists, 0, sizeof(ready_lists));
    ready_bitmap = 0;
    delay_head = NULL;
    tick_count = 0;
    idle_tick_count = 0;
    started = false;
    rtos_cur = NULL;
    rtos_next = NULL;
    thread_setup(&idle_tcb, "idle", idle_entry, NULL, RTOS_PRIO_IDLE, idle_stack,
                 RTOS_IDLE_STACK_WORDS);
    ready_add(&idle_tcb);
    port_irq_restore(s);
}

void rtos_start(void)
{
    (void)port_irq_save();
    started = true;
    rtos_next = highest_ready();
    port_start(); /* enables interrupts and enters the first thread */
}
