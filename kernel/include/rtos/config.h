/*
 * BareMetal-RTOS - kernel configuration
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#ifndef RTOS_CONFIG_H
#define RTOS_CONFIG_H

/* Number of priority levels. 0 is reserved for the idle thread, higher is more urgent. */
#ifndef RTOS_MAX_PRIO
#define RTOS_MAX_PRIO 32u
#endif

/* Kernel tick rate in Hz (SysTick). */
#ifndef RTOS_TICK_HZ
#define RTOS_TICK_HZ 1000u
#endif

/* Core clock used to program SysTick. The LM3S6965 runs from a 12 MHz source after reset. */
#ifndef RTOS_CPU_HZ
#define RTOS_CPU_HZ 12000000u
#endif

/* Round-robin time slice for threads of equal priority, in ticks. */
#ifndef RTOS_TIMESLICE_TICKS
#define RTOS_TIMESLICE_TICKS 10u
#endif

/* Smallest stack accepted by rtos_thread_create, in 32-bit words. */
#ifndef RTOS_MIN_STACK_WORDS
#define RTOS_MIN_STACK_WORDS 64u
#endif

/* Stack of the built-in idle thread, in 32-bit words. */
#ifndef RTOS_IDLE_STACK_WORDS
#define RTOS_IDLE_STACK_WORDS 96u
#endif

/* Pattern used to paint stacks for high-water-mark and overflow detection. */
#define RTOS_STACK_FILL 0xDEADBEEFu

#endif /* RTOS_CONFIG_H */
