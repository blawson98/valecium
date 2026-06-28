// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#ifndef PROCMM_H
#define PROCMM_H

#include <cpu/process.h>
#include <mem/mm_kernel.h>
#include <stddef.h>
#include <stdint.h>

/* Per-process heap management */
int Heap_ProcessInitialize(Process *proc, uint32_t heap_start_va);
int Heap_ProcessBrk(Process *proc, void *addr);
void *Heap_ProcessSbrk(Process *proc, intptr_t inc);

/* Physical memory manager status */
int PMM_IsInitialized(void);

/* Virtual Memory Manager (VMM) - Process level
 *
 * Per-process virtual memory operations.
 * Takes explicit page directory for context-specific mapping.
 */

/* Allocate and map virtual memory in a page directory.
 * If next_vaddr_state is NULL, uses the kernel allocator bump pointer.
 */
void *VMM_AllocateInDir(void *page_dir, uint32_t *next_vaddr_state,
                        uint32_t size, uint32_t flags);

/* Free previously allocated virtual memory in a page directory
 */
void VMM_FreeInDir(void *page_dir, void *vaddr, uint32_t size);

/* Map existing physical memory in a page directory
 */
int VMM_MapInDir(void *page_dir, uint32_t vaddr, uint32_t paddr, uint32_t size,
                 uint32_t flags);

/* Unmap virtual memory in a page directory (does not free physical pages)
 */
int VMM_UnmapInDir(void *page_dir, uint32_t vaddr, uint32_t size);

/* Get physical address of a virtual address in a page directory
 */
uint32_t VMM_GetPhysInDir(void *page_dir, uint32_t vaddr);

/* Stack Management - Process level */

// Create a new user stack for a process (grows downward, standard x86
// behavior).
Stack *Stack_Create(size_t size);

// Initialize a process's user stack at stack_top_va (e.g. 0xBFFFF000).
int Stack_ProcessInitialize(Process *proc, uint32_t stack_top_va, size_t size);

// Destroy a user stack, freeing allocated memory and the Stack structure.
void Stack_Destroy(Stack *stack);

// Push data onto a stack; returns new stack pointer or 0 on overflow.
uint32_t Stack_Push(Stack *stack, const void *data, size_t size);

// Pop data from a stack; returns new stack pointer or 0 on underflow.
uint32_t Stack_Pop(Stack *stack, void *data, size_t size);

// Get current stack pointer for a stack.
static inline uint32_t Stack_GetSP(Stack *stack)
{
   return stack ? stack->current : 0;
}

// Set current stack pointer. Returns 1 on success, 0 if SP out of range.
int Stack_SetSP(Stack *stack, uint32_t sp);

// Check if stack has enough free space. Returns 1 if yes, 0 otherwise.
int Stack_HasSpace(Stack *stack, size_t required);

// Setup stack for process execution (argc/argv, env, return address).
void Stack_SetupProcess(Stack *stack, uint32_t entry_point);

#endif