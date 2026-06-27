// SPDX-License-Identifier: GPL-3.0-only

#ifndef I686_TLB_H
#define I686_TLB_H

#include <stdint.h>

// TLB (Translation Lookaside Buffer) Management for x86 i686.
// The TLB caches virtual-to-physical translations. We must invalidate
// entries when modifying page tables to prevent stale cached translations.

// Invalidate a single TLB entry for a virtual address (INVLPG instruction).
static inline void tlb_invalidate_entry(uintptr_t vaddr)
{
   __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr) : "memory");
}

// Invalidate the entire TLB by reloading CR3. Heavy operation, used when
// switching page directories or during major memory subsystem changes.
static inline void tlb_invalidate_all(void)
{
   uint32_t cr3;
   __asm__ __volatile__("movl %%cr3, %0" : "=r"(cr3));
   __asm__ __volatile__("movl %0, %%cr3" : : "r"(cr3));
}

/**
 * Invalidate TLB for a range of virtual addresses
 *
 * @param vaddr_start Start of virtual address range
 * @param vaddr_end   End of virtual address range (exclusive)
 *
 * For large ranges, it may be faster to use tlb_invalidate_all()
 * This is efficient for smaller ranges (e.g., unmapping a few pages).
 */
static inline void tlb_invalidate_range(uintptr_t vaddr_start,
                                        uintptr_t vaddr_end)
{
   for (uintptr_t vaddr = vaddr_start; vaddr < vaddr_end; vaddr += 0x1000)
   {
      tlb_invalidate_entry(vaddr);
   }
}

// Get current CR3 (page directory base address).
static inline uint32_t tlb_get_cr3(void)
{
   uint32_t cr3;
   __asm__ __volatile__("movl %%cr3, %0" : "=r"(cr3));
   return cr3;
}

// Set CR3 to a new page directory. Switches address space and invalidates
// all TLB entries. Used during process context switches.
static inline void tlb_set_cr3(uint32_t page_dir_phys)
{
   __asm__ __volatile__("movl %0, %%cr3" : : "r"(page_dir_phys));
}

// TLB statistics: x86 does not provide direct access to TLB stats.
// Architecture-dependent; not available on i386/i686.

// Prefetch into TLB (hints CPU to load a TLB entry). Rarely used in practice.
static inline void tlb_prefetch(uintptr_t vaddr)
{
   __asm__ __volatile__("prefetcht0 (%0)" : : "r"(vaddr));
}

#endif