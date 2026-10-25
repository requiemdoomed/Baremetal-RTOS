/*
 * BareMetal-RTOS - minimal Cortex-M3 core register definitions (CMSIS-style names)
 * Copyright (c) 2026 Swayam Shakya
 * SPDX-License-Identifier: MIT
 */
#ifndef CM3_REGS_H
#define CM3_REGS_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* System Control Block */
#define SCB_ICSR            REG32(0xE000ED04u)
#define SCB_ICSR_PENDSVSET  (1u << 28)
#define SCB_ICSR_PENDSTSET  (1u << 26)
#define SCB_VTOR            REG32(0xE000ED08u)
#define SCB_SHPR3           REG32(0xE000ED20u)
#define SCB_CFSR            REG32(0xE000ED28u)
#define SCB_HFSR            REG32(0xE000ED2Cu)

/* SysTick */
#define SYST_CSR            REG32(0xE000E010u)
#define SYST_RVR            REG32(0xE000E014u)
#define SYST_CVR            REG32(0xE000E018u)
#define SYST_CSR_ENABLE     (1u << 0)
#define SYST_CSR_TICKINT    (1u << 1)
#define SYST_CSR_CLKSOURCE  (1u << 2)

/* NVIC */
#define NVIC_ISER(n)        REG32(0xE000E100u + 4u * (n))
#define NVIC_ICER(n)        REG32(0xE000E180u + 4u * (n))
#define NVIC_ICPR(n)        REG32(0xE000E280u + 4u * (n))
#define NVIC_IPR_BYTE(irq)  (*(volatile uint8_t *)(0xE000E400u + (irq)))

static inline void nvic_enable_irq(uint32_t irq) { NVIC_ISER(irq >> 5) = 1u << (irq & 31u); }
static inline void nvic_disable_irq(uint32_t irq) { NVIC_ICER(irq >> 5) = 1u << (irq & 31u); }
static inline void nvic_clear_pending(uint32_t irq) { NVIC_ICPR(irq >> 5) = 1u << (irq & 31u); }
static inline void nvic_set_priority(uint32_t irq, uint8_t prio) { NVIC_IPR_BYTE(irq) = prio; }

#endif /* CM3_REGS_H */
