/*
 * BareMetal-RTOS - mutexes, semaphores and message queues
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "kernel_internal.h"

/* ================================================================= mutex */

int rtos_mutex_init(rtos_mutex_t *m)
{
    if (!m) {
        return RTOS_ERR_PARAM;
    }
    memset(m, 0, sizeof(*m));
    return RTOS_OK;
}

/*
 * Effective priority = max(base priority, highest waiter on any mutex it holds).
 * A change can ripple down a chain of blocked owners (C waits on B, B waits on A),
 * so keep walking while priorities still move.
 */
void k_mutex_recompute_prio(rtos_thread_t *owner)
{
    for (unsigned depth = 0; owner && depth < RTOS_MAX_PRIO; depth++) {
        uint8_t p = owner->base_prio;
        for (rtos_mutex_t *m = owner->held; m; m = m->next_held) {
            if (m->waiters.head && m->waiters.head->prio > p) {
                p = m->waiters.head->prio;
            }
        }
        if (p == owner->prio) {
            return;
        }
        k_set_prio(owner, p);
        owner = owner->waiting_on ? owner->waiting_on->owner : NULL;
    }
}

static void held_push(rtos_thread_t *t, rtos_mutex_t *m)
{
    m->owner = t;
    m->next_held = t->held;
    t->held = m;
}

static void held_remove(rtos_thread_t *t, rtos_mutex_t *m)
{
    rtos_mutex_t **pp = &t->held;
    while (*pp && *pp != m) {
        pp = &(*pp)->next_held;
    }
    if (*pp) {
        *pp = m->next_held;
    }
    m->next_held = NULL;
}

int rtos_mutex_lock(rtos_mutex_t *m, uint32_t timeout_ticks)
{
    if (!m) {
        return RTOS_ERR_PARAM;
    }
    if (port_in_isr()) {
        return RTOS_ERR_ISR;
    }
    if (!k_started()) {
        return RTOS_ERR_STATE;
    }
    uint32_t s = port_irq_save();
    rtos_thread_t *c = rtos_cur;
    if (!m->owner) {
        held_push(c, m);
        port_irq_restore(s);
        return RTOS_OK;
    }
    if (m->owner == c) {
        port_irq_restore(s);
        return RTOS_ERR_STATE; /* non-recursive */
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

    c->waiting_on = m;
    k_block_current(&m->waiters, timeout_ticks);

    /* Priority inheritance, propagated along the chain of blocked owners. */
    rtos_thread_t *o = m->owner;
    uint8_t p = c->prio;
    while (o && o->prio < p) {
        k_set_prio(o, p);
        o = o->waiting_on ? o->waiting_on->owner : NULL;
    }
    k_reschedule();
    port_irq_restore(s);
    /* Resumed: either ownership was handed to us (OK) or the wait timed out. */
    return rtos_cur->wait_result;
}

int rtos_mutex_unlock(rtos_mutex_t *m)
{
    if (!m) {
        return RTOS_ERR_PARAM;
    }
    if (port_in_isr()) {
        return RTOS_ERR_ISR;
    }
    if (!k_started()) {
        return RTOS_ERR_STATE;
    }
    uint32_t s = port_irq_save();
    rtos_thread_t *c = rtos_cur;
    if (m->owner != c) {
        port_irq_restore(s);
        return RTOS_ERR_PERM;
    }
    held_remove(c, m);
    if (m->waiters.head) {
        /* Hand ownership directly to the highest-priority waiter. */
        rtos_thread_t *t = m->waiters.head;
        k_wake(t, RTOS_OK);
        held_push(t, m);
        k_mutex_recompute_prio(t);
    } else {
        m->owner = NULL;
    }
    k_mutex_recompute_prio(c);
    k_reschedule();
    port_irq_restore(s);
    return RTOS_OK;
}

/* ============================================================= semaphore */

int rtos_sem_init(rtos_sem_t *sem, uint32_t initial, uint32_t max)
{
    if (!sem || max == 0u || initial > max) {
        return RTOS_ERR_PARAM;
    }
    memset(sem, 0, sizeof(*sem));
    sem->count = initial;
    sem->max = max;
    return RTOS_OK;
}

int rtos_sem_take(rtos_sem_t *sem, uint32_t timeout_ticks)
{
    if (!sem) {
        return RTOS_ERR_PARAM;
    }
    uint32_t s = port_irq_save();
    if (sem->count > 0u) {
        sem->count--;
        port_irq_restore(s);
        return RTOS_OK;
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
    k_block_current(&sem->waiters, timeout_ticks);
    port_irq_restore(s);
    return rtos_cur->wait_result;
}

int rtos_sem_give(rtos_sem_t *sem)
{
    if (!sem) {
        return RTOS_ERR_PARAM;
    }
    int r = RTOS_OK;
    uint32_t s = port_irq_save();
    if (sem->waiters.head) {
        /* The unit goes straight to the highest-priority waiter. */
        k_wake(sem->waiters.head, RTOS_OK);
        k_reschedule();
    } else if (sem->count < sem->max) {
        sem->count++;
    } else {
        r = RTOS_ERR_FULL;
    }
    port_irq_restore(s);
    return r;
}

uint32_t rtos_sem_count(const rtos_sem_t *sem) { return sem ? sem->count : 0u; }

/* ================================================================= queue */

int rtos_queue_init(rtos_queue_t *q, void *storage, uint32_t item_size, uint32_t capacity)
{
    if (!q || !storage || item_size == 0u || capacity == 0u) {
        return RTOS_ERR_PARAM;
    }
    memset(q, 0, sizeof(*q));
    q->buf = (uint8_t *)storage;
    q->item_size = item_size;
    q->capacity = capacity;
    rtos_sem_init(&q->items, 0u, capacity);
    rtos_sem_init(&q->spaces, capacity, capacity);
    return RTOS_OK;
}

/*
 * `spaces` counts free slots and `items` counts filled slots. A sender reserves a slot by
 * taking `spaces`, copies under a short critical section, then publishes with `items`.
 */
int rtos_queue_send(rtos_queue_t *q, const void *item, uint32_t timeout_ticks)
{
    if (!q || !item) {
        return RTOS_ERR_PARAM;
    }
    int r = rtos_sem_take(&q->spaces, timeout_ticks);
    if (r != RTOS_OK) {
        return r;
    }
    uint32_t s = port_irq_save();
    memcpy(q->buf + (size_t)q->tail * q->item_size, item, q->item_size);
    q->tail = (q->tail + 1u == q->capacity) ? 0u : q->tail + 1u;
    port_irq_restore(s);
    return rtos_sem_give(&q->items);
}

int rtos_queue_recv(rtos_queue_t *q, void *item, uint32_t timeout_ticks)
{
    if (!q || !item) {
        return RTOS_ERR_PARAM;
    }
    int r = rtos_sem_take(&q->items, timeout_ticks);
    if (r != RTOS_OK) {
        return r;
    }
    uint32_t s = port_irq_save();
    memcpy(item, q->buf + (size_t)q->head * q->item_size, q->item_size);
    q->head = (q->head + 1u == q->capacity) ? 0u : q->head + 1u;
    port_irq_restore(s);
    return rtos_sem_give(&q->spaces);
}

uint32_t rtos_queue_count(const rtos_queue_t *q) { return q ? q->items.count : 0u; }
