/*
 * BareMetal-RTOS - LM3S6965 board support
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#include "board.h"

#include <stdbool.h>

#include "cm3_regs.h"
#include "rtos/config.h"

/* System control */
#define SYSCTL_RCGC1 REG32(0x400FE104u)
#define SYSCTL_RCGC2 REG32(0x400FE108u)

/* UART0 */
#define UART0_BASE 0x4000C000u
#define UART0_DR   REG32(UART0_BASE + 0x000u)
#define UART0_FR   REG32(UART0_BASE + 0x018u)
#define UART0_CTL  REG32(UART0_BASE + 0x030u)
#define UART_FR_TXFF (1u << 5)

/* General-purpose timers */
#define GPTM_BASE(n)   (0x40030000u + 0x1000u * (n))
#define GPTM_CFG(n)    REG32(GPTM_BASE(n) + 0x000u)
#define GPTM_TAMR(n)   REG32(GPTM_BASE(n) + 0x004u)
#define GPTM_CTL(n)    REG32(GPTM_BASE(n) + 0x00Cu)
#define GPTM_IMR(n)    REG32(GPTM_BASE(n) + 0x018u)
#define GPTM_ICR(n)    REG32(GPTM_BASE(n) + 0x024u)
#define GPTM_TAILR(n)  REG32(GPTM_BASE(n) + 0x028u)
#define GPTM_TAMR_PERIODIC 0x2u
#define GPTM_CTL_TAEN      (1u << 0)
#define GPTM_IMR_TATOIM    (1u << 0)

void board_init(void)
{
    SYSCTL_RCGC1 |= (1u << 0) | (1u << 16) | (1u << 17); /* UART0, TIMER0, TIMER1 clocks */
    SYSCTL_RCGC2 |= 1u;                                    /* GPIOA (UART pins) */
    UART0_CTL = (1u << 0) | (1u << 8) | (1u << 9);         /* UARTEN, TXE, RXE */
}

void board_putc(char c)
{
    while ((UART0_FR & UART_FR_TXFF) != 0u) {
    }
    UART0_DR = (uint32_t)(uint8_t)c;
}

void board_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            board_putc('\r');
        }
        board_putc(*s++);
    }
}

void board_put_hex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        board_putc("0123456789ABCDEF"[(v >> i) & 0xFu]);
    }
}

static void put_padded(const char *s, int len, int width, bool left, char pad)
{
    if (!left) {
        for (int i = len; i < width; i++) {
            board_putc(pad);
        }
    }
    for (int i = 0; i < len; i++) {
        board_putc(s[i]);
    }
    if (left) {
        for (int i = len; i < width; i++) {
            board_putc(' ');
        }
    }
}

void board_vprintf(const char *fmt, va_list ap)
{
    char buf[12];
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (*fmt == '\n') {
                board_putc('\r');
            }
            board_putc(*fmt);
            continue;
        }
        fmt++;
        bool left = false;
        char pad = ' ';
        int width = 0;
        if (*fmt == '-') {
            left = true;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == 'l') {
            fmt++; /* long is 32 bits on this target */
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = 0;
            if (!s) {
                s = "(null)";
            }
            while (s[len]) {
                len++;
            }
            put_padded(s, len, width, left, ' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            put_padded(&c, 1, width, left, ' ');
            break;
        }
        case 'd':
        case 'u':
        case 'x':
        case 'X': {
            uint32_t v = va_arg(ap, uint32_t);
            bool neg = false;
            uint32_t base = (*fmt == 'x' || *fmt == 'X') ? 16u : 10u;
            const char *digits = (*fmt == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
            if (*fmt == 'd' && (int32_t)v < 0) {
                neg = true;
                v = (uint32_t)(-(int32_t)v);
            }
            int i = (int)sizeof(buf);
            do {
                buf[--i] = digits[v % base];
                v /= base;
            } while (v);
            if (neg) {
                if (pad == '0' && !left) {
                    board_putc('-');
                    width--;
                } else {
                    buf[--i] = '-';
                }
            }
            put_padded(&buf[i], (int)sizeof(buf) - i, width, left, pad);
            break;
        }
        case '%':
            board_putc('%');
            break;
        case '\0':
            return;
        default:
            board_putc('%');
            board_putc(*fmt);
            break;
        }
    }
}

void board_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    board_vprintf(fmt, ap);
    va_end(ap);
}

void board_timer_start(uint32_t n, uint32_t hz, uint8_t irq_prio)
{
    uint32_t irq = (n == 0u) ? BOARD_IRQ_TIMER0A : BOARD_IRQ_TIMER1A;
    GPTM_CTL(n) = 0u;
    GPTM_CFG(n) = 0u; /* 32-bit timer */
    GPTM_TAMR(n) = GPTM_TAMR_PERIODIC;
    GPTM_TAILR(n) = RTOS_CPU_HZ / hz - 1u;
    GPTM_ICR(n) = GPTM_IMR_TATOIM;
    GPTM_IMR(n) = GPTM_IMR_TATOIM;
    nvic_clear_pending(irq);
    nvic_set_priority(irq, irq_prio);
    nvic_enable_irq(irq);
    GPTM_CTL(n) = GPTM_CTL_TAEN;
}

void board_timer_stop(uint32_t n)
{
    uint32_t irq = (n == 0u) ? BOARD_IRQ_TIMER0A : BOARD_IRQ_TIMER1A;
    GPTM_CTL(n) = 0u;
    GPTM_IMR(n) = 0u;
    GPTM_ICR(n) = GPTM_IMR_TATOIM;
    nvic_disable_irq(irq);
    nvic_clear_pending(irq);
}

void board_timer_ack(uint32_t n) { GPTM_ICR(n) = GPTM_IMR_TATOIM; }

/*
 * ARM semihosting SYS_EXIT. On AArch32 r1 carries the stop reason: QEMU exits with status 0
 * for ADP_Stopped_ApplicationExit and 1 for anything else. Semihosting needs QEMU or an
 * attached debugger (OpenOCD: `arm semihosting enable`); on a free-running board, build
 * with -DBOARD_NO_SEMIHOSTING so the CPU simply parks here.
 */
void board_exit(int code)
{
    __asm volatile("cpsid i");
#ifndef BOARD_NO_SEMIHOSTING
    register uint32_t r0 __asm("r0") = 0x18u;
    register uint32_t r1 __asm("r1") = (code == 0) ? 0x20026u : 0x20023u;
    __asm volatile("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
#else
    (void)code;
#endif
    for (;;) {
        __asm volatile("wfi");
    }
}
