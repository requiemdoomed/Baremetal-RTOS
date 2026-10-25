/*
 * BareMetal-RTOS - Cortex-M3 context switch (PendSV) and first-thread entry
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 *
 * Threads run in Thread mode on the process stack (PSP). Hardware stacks r0-r3, r12,
 * lr, pc and xPSR on exception entry; PendSV saves/restores r4-r11 around the switch.
 */
    .syntax unified
    .cpu cortex-m3
    .thumb

    /* offsetof(rtos_thread_t, switches); checked by a static assert in port.c */
    .equ    TCB_SWITCHES_OFFSET, 64

    .global PendSV_Handler
    .type   PendSV_Handler, %function
    .thumb_func
PendSV_Handler:
    cpsid   i
    bl      k_account_switch        /* per-thread CPU time; clobbers only r0-r3, r12, lr */
    ldr     r2, =rtos_cur
    ldr     r1, [r2]                /* r1 = current TCB (NULL on the very first switch) */
    cbz     r1, 1f
    mrs     r0, psp
    stmdb   r0!, {r4-r11}           /* save callee-saved registers on the thread stack */
    str     r0, [r1]                /* tcb->sp = psp */
1:
    ldr     r3, =rtos_next
    ldr     r1, [r3]
    str     r1, [r2]                /* rtos_cur = rtos_next */
    ldr     r3, [r1, #TCB_SWITCHES_OFFSET]
    adds    r3, r3, #1
    str     r3, [r1, #TCB_SWITCHES_OFFSET]
    ldr     r0, [r1]                /* r0 = next->sp */
    ldmia   r0!, {r4-r11}
    msr     psp, r0
    cpsie   i
    ldr     lr, =0xFFFFFFFD         /* EXC_RETURN: Thread mode, PSP (lr was clobbered by bl) */
    bx      lr
    .size   PendSV_Handler, .-PendSV_Handler

    .global port_start_first
    .type   port_start_first, %function
    .thumb_func
port_start_first:
    ldr     r0, =_estack            /* reclaim the main stack for exception handlers */
    msr     msp, r0
    ldr     r0, =0xE000ED04         /* ICSR */
    ldr     r1, =0x10000000         /* PENDSVSET */
    str     r1, [r0]
    dsb
    isb
    cpsie   i                       /* PendSV fires here and enters the first thread */
2:
    b       2b
    .size   port_start_first, .-port_start_first
