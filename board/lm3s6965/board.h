/*
 * BareMetal-RTOS - LM3S6965 board support: UART console, general-purpose timers, exit
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_IRQ_TIMER0A 19u
#define BOARD_IRQ_TIMER1A 21u

void board_init(void);

/* Interrupt handlers provided by the application (weak defaults trap). */
void TIMER0A_IRQHandler(void);
void TIMER1A_IRQHandler(void);

/* Polled UART0 console (safe from any context). */
void board_putc(char c);
void board_puts(const char *s);
void board_put_hex(uint32_t v);
/* Tiny printf: %s %c %d %u %x %X %%, width, "0"/"-" flags. Integer args are 32-bit. */
void board_printf(const char *fmt, ...);
void board_vprintf(const char *fmt, va_list ap);

/* Periodic interrupt from general-purpose timer `n` (0 or 1) at `hz`. */
void board_timer_start(uint32_t n, uint32_t hz, uint8_t irq_prio);
void board_timer_stop(uint32_t n);
void board_timer_ack(uint32_t n); /* call at the top of the timer ISR */

/* Leave the program. Under QEMU this ends the emulator with `code` via semihosting. */
void board_exit(int code) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* BOARD_H */
