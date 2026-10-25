/*
 * BareMetal-RTOS - kernel internals shared between kernel modules and the port
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#ifndef RTOS_KERNEL_INTERNAL_H
#define RTOS_KERNEL_INTERNAL_H

#include "rtos/rtos.h"
#include "rtos_port.h"

/* Read by the PendSV handler. */
extern rtos_thread_t *volatile rtos_cur;
extern rtos_thread_t *volatile rtos_next;

void k_list_push_back(rtos_list_t *l, rtos_thread_t *t);
void k_list_insert_prio(rtos_list_t *l, rtos_thread_t *t);
void k_list_remove(rtos_thread_t *t);

/* All k_* functions below must be called with interrupts masked. */
void k_block_current(rtos_list_t *wait_list, uint32_t timeout_ticks);
void k_wake(rtos_thread_t *t, int32_t result);
void k_reschedule(void);
void k_set_prio(rtos_thread_t *t, uint8_t prio);
bool k_started(void);
void k_account_switch(void); /* PendSV: charge elapsed cycles to the outgoing thread */

/* sync.c: recompute an owner's priority after its waiter set changed. */
void k_mutex_recompute_prio(rtos_thread_t *owner);

/* Common guard for calls that may block. Returns RTOS_OK if blocking is allowed. */
int k_check_can_block(uint32_t saved_irq);

#endif /* RTOS_KERNEL_INTERNAL_H */
