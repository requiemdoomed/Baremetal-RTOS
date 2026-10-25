/*
 * BareMetal-RTOS - Cortex-M3 port interface
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#ifndef RTOS_PORT_H
#define RTOS_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "cm3_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t port_irq_save(void)
{
    uint32_t primask;
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
    return primask;
}

static inline void port_irq_restore(uint32_t primask)
{
    __asm volatile("msr primask, %0\n\tisb" ::"r"(primask) : "memory");
}

static inline bool port_in_isr(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return ipsr != 0u;
}

static inline void port_pend_switch(void)
{
    SCB_ICSR = SCB_ICSR_PENDSVSET;
    __asm volatile("dsb" ::: "memory");
}

static inline void port_wait_for_interrupt(void) { __asm volatile("wfi"); }

uint32_t *port_init_stack(uint32_t *stack_top, void (*entry)(void *), void *arg,
                          void (*exit_fn)(void));
void port_start(void) __attribute__((noreturn));

/* Exception handlers implemented by the port. */
void SysTick_Handler(void);
void PendSV_Handler(void);

/* Free-running cycle counter derived from the tick count and SysTick (wraps at 2^32). */
uint32_t port_cycles(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_PORT_H */
