// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#ifndef I686_STACK_H
#define I686_STACK_H

#include <mem/mm_kernel.h>
#include <stdint.h>

// x86 32-bit Stack Management — platform-specific stack setup for i686:
// ESP/EBP registers, process entry point setup, stack frame creation.

// x86 stack frame: [ESP] -> return address, [EBP] -> previous frame pointer.
// Stack grows downward (high to low addresses).

// Prepare user stack for process execution: sets return address to exit
// handler, initializes EBP, prepares stack for entry into process main.
void i686_Stack_SetupProcess(Stack *stack, uint32_t entry_point);

// Get current ESP register value.
uint32_t i686_Stack_GetESP(void);

// Get current EBP register value.
uint32_t i686_Stack_GetEBP(void);

// Set ESP and EBP for context switching.
void i686_Stack_SetRegisters(uint32_t esp, uint32_t ebp);

// Get a snapshot of current stack registers.
void i686_Stack_GetRegisters(uint32_t *esp_out, uint32_t *ebp_out);

// Prepare kernel stack for a CPU exception context.
void i686_Stack_SetupException(Stack *stack, uint32_t handler,
                               uint32_t error_code);

void i686_Stack_InitializeKernel(void);

#endif