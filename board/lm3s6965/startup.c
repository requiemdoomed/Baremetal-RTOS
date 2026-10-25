/*
 * BareMetal-RTOS - LM3S6965 (Cortex-M3) vector table and reset handler
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#include "board.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern void (*__preinit_array_start[])(void);
extern void (*__preinit_array_end[])(void);
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

extern int main(void);

void Reset_Handler(void);
void Default_Handler(void);
void HardFault_Handler(void);

#define WEAK_ALIAS __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void) WEAK_ALIAS;
void MemManage_Handler(void) WEAK_ALIAS;
void BusFault_Handler(void) WEAK_ALIAS;
void UsageFault_Handler(void) WEAK_ALIAS;
void SVC_Handler(void) WEAK_ALIAS;
void DebugMon_Handler(void) WEAK_ALIAS;
void PendSV_Handler(void) WEAK_ALIAS;
void SysTick_Handler(void) WEAK_ALIAS;
void TIMER0A_IRQHandler(void) WEAK_ALIAS;
void TIMER1A_IRQHandler(void) WEAK_ALIAS;

typedef void (*vector_t)(void);

__attribute__((section(".isr_vector"), used)) const vector_t vector_table[16 + 48] = {
    (vector_t)(uintptr_t)&_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,
    SVC_Handler,
    DebugMon_Handler,
    0,
    PendSV_Handler,
    SysTick_Handler,
    /* Device interrupts. Unused entries trap in Default_Handler. */
    /* IRQ  0- 3 */ Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    /* IRQ  4- 7 */ Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    /* IRQ  8-11 */ Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    /* IRQ 12-15 */ Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    /* IRQ 16-19 */ Default_Handler, Default_Handler, Default_Handler, TIMER0A_IRQHandler,
    /* IRQ 20-23 */ Default_Handler, TIMER1A_IRQHandler, Default_Handler, Default_Handler,
    /* IRQ 24-27 */ Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    /* IRQ 28-31 */ Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    /* IRQ 32-35 */ Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    /* IRQ 36-39 */ Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    /* IRQ 40-43 */ Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    /* IRQ 44-47 */ Default_Handler, Default_Handler, Default_Handler, Default_Handler,
};

_Static_assert(BOARD_IRQ_TIMER0A == 19u && BOARD_IRQ_TIMER1A == 21u, "vector table layout");

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata;) {
        *dst++ = *src++;
    }
    for (uint32_t *dst = &_sbss; dst < &_ebss;) {
        *dst++ = 0u;
    }
    for (void (**f)(void) = __preinit_array_start; f < __preinit_array_end; f++) {
        (*f)();
    }
    for (void (**f)(void) = __init_array_start; f < __init_array_end; f++) {
        (*f)();
    }
    board_exit(main());
}

void HardFault_Handler(void)
{
    board_puts("\r\n*** HARD FAULT  HFSR=0x");
    board_put_hex(*(volatile uint32_t *)0xE000ED2Cu);
    board_puts(" CFSR=0x");
    board_put_hex(*(volatile uint32_t *)0xE000ED28u);
    board_puts("\r\n");
    board_exit(2);
}

void Default_Handler(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    board_puts("\r\n*** UNHANDLED EXCEPTION ");
    board_put_hex(ipsr);
    board_puts("\r\n");
    board_exit(3);
}
