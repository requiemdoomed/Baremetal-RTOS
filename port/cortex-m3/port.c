/*
 * BareMetal-RTOS - Cortex-M3 port: stack frames, SysTick, exception priorities
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#include "rtos/rtos.h"
#include "rtos_port.h"

#include <stddef.h>

_Static_assert(offsetof(rtos_thread_t, switches) == 64u, "update TCB_SWITCHES_OFFSET in context_switch.S");

#define SYSTICK_RELOAD (RTOS_CPU_HZ / RTOS_TICK_HZ)

extern void port_start_first(void) __attribute__((noreturn));

/*
 * Build the frame the PendSV handler pops on the first switch-in:
 *   [r4-r11 saved by software] [r0-r3, r12, lr, pc, xpsr stacked by hardware]
 */
uint32_t *port_init_stack(uint32_t *stack_top, void (*entry)(void *), void *arg,
                          void (*exit_fn)(void))
{
    uint32_t *sp = (uint32_t *)((uintptr_t)stack_top & ~(uintptr_t)7u); /* AAPCS: 8-byte */
    *--sp = 0x01000000u;                       /* xPSR: Thumb bit */
    *--sp = (uint32_t)(uintptr_t)entry & ~1u;  /* PC */
    *--sp = (uint32_t)(uintptr_t)exit_fn;      /* LR: entry returning ends the thread */
    *--sp = 0x12121212u;                       /* R12 */
    *--sp = 0x03030303u;                       /* R3 */
    *--sp = 0x02020202u;                       /* R2 */
    *--sp = 0x01010101u;                       /* R1 */
    *--sp = (uint32_t)(uintptr_t)arg;          /* R0: entry argument */
    for (uint32_t r = 11u; r >= 4u; r--) {     /* R11..R4 */
        *--sp = r * 0x01010101u;
    }
    return sp;
}

void port_start(void)
{
    /* PendSV and SysTick at the lowest priority so device ISRs always preempt the kernel. */
    SCB_SHPR3 = (SCB_SHPR3 & 0x0000FFFFu) | (0xFFu << 16) | (0xF0u << 24);

    SYST_CSR = 0u;
    SYST_RVR = SYSTICK_RELOAD - 1u;
    SYST_CVR = 0u;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;

    port_start_first();
}

void SysTick_Handler(void) { rtos_tick(); }

uint32_t port_cycles(void)
{
    uint32_t s = port_irq_save();
    uint32_t ticks = rtos_ticks();
    uint32_t val = SYST_CVR;
    /* A SysTick that wrapped but has not been serviced yet belongs to the next tick. */
    if ((SCB_ICSR & SCB_ICSR_PENDSTSET) != 0u) {
        ticks++;
        val = SYST_CVR;
    }
    port_irq_restore(s);
    return ticks * SYSTICK_RELOAD + (SYSTICK_RELOAD - 1u - val);
}
