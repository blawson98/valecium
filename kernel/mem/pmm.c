// SPDX-License-Identifier: GPL-3.0-only

#include "mm_kernel.h"
#include "mm_proc.h"
#include <std/stdio.h>
#include <stddef.h>
#include <stdint.h>

#define BITS_PER_BYTE 8

/* Bitmap to track free/allocated pages
 * bit=0: free, bit=1: allocated
 * We allocate the bitmap itself from identity-mapped kernel space
 */
static uint8_t *s_PageBitmap = NULL;
static uint32_t s_TotalPages = 0;
static uint32_t s_AllocatedCount = 0;
static int s_PmmInitialized = 0;

static void bitmap_set(uint32_t page_idx)
{
   uint32_t byte_idx = page_idx / BITS_PER_BYTE;
   uint32_t bit_idx = page_idx % BITS_PER_BYTE;
   s_PageBitmap[byte_idx] |= (1u << bit_idx);
   s_AllocatedCount++;
}

static void bitmap_clear(uint32_t page_idx)
{
   uint32_t byte_idx = page_idx / BITS_PER_BYTE;
   uint32_t bit_idx = page_idx % BITS_PER_BYTE;
   s_PageBitmap[byte_idx] &= ~(1u << bit_idx);
   if (s_AllocatedCount > 0) s_AllocatedCount--;
}

static int bitmap_is_set(uint32_t page_idx)
{
   uint32_t byte_idx = page_idx / BITS_PER_BYTE;
   uint32_t bit_idx = page_idx % BITS_PER_BYTE;
   return (s_PageBitmap[byte_idx] & (1u << bit_idx)) != 0;
}

void PMM_Initialize(uint32_t total_mem_bytes)
{
   s_PmmInitialized = 1;
   // Calculate number of pages
   s_TotalPages = (total_mem_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

   // Bitmap size in bytes
   uint32_t bitmap_bytes = (s_TotalPages + BITS_PER_BYTE - 1) / BITS_PER_BYTE;

   // Allocate bitmap from lower memory (identity-mapped)
   // For now, use a static buffer to avoid chicken-and-egg
   // In a real system, you'd place this in a reserved region
   static uint8_t bitmap_storage[131072]; // max ~1M pages (4 GiB)
   s_PageBitmap = bitmap_storage;

   if (bitmap_bytes > sizeof(bitmap_storage))
   {
      logfmt(LOG_WARNING, "[MEM] WARNING: bitmap too small for %u pages\n",
             s_TotalPages);
      s_TotalPages = sizeof(bitmap_storage) * BITS_PER_BYTE;
      bitmap_bytes = sizeof(bitmap_storage);
   }

   // Initially all pages are free (bitmap = 0)
   memset(s_PageBitmap, 0, bitmap_bytes);
   s_AllocatedCount = 0;

   // Reserve pages 0-2 MiB for kernel/boot (0x00000 - 0x200000)
   uint32_t reserved_pages = (2 * 1024 * 1024) / PAGE_SIZE;
   for (uint32_t i = 0; i < reserved_pages && i < s_TotalPages; ++i)
   {
      bitmap_set(i);
   }

   logfmt(LOG_INFO, "[MEM] init: total=%u pages, reserved=%u, free=%u\n",
          s_TotalPages, reserved_pages, s_TotalPages - s_AllocatedCount);
}

int PMM_IsInitialized(void) { return s_PmmInitialized; }

uint32_t PMM_AllocatePhysicalPage(void)
{
   if (!s_PageBitmap) return 0;

   // Simple linear search for a free page
   for (uint32_t i = 0; i < s_TotalPages; ++i)
   {
      if (!bitmap_is_set(i))
      {
         bitmap_set(i);
         return i * PAGE_SIZE;
      }
   }

   logfmt(LOG_ERROR, "[MEM] PMM_AllocatePhysicalPage: out of memory\n");
   return 0;
}

void PMM_FreePhysicalPage(uint32_t addr)
{
   if (!s_PageBitmap || (addr % PAGE_SIZE) != 0) return;

   uint32_t page_idx = addr / PAGE_SIZE;
   if (page_idx >= s_TotalPages) return;

   if (bitmap_is_set(page_idx))
   {
      bitmap_clear(page_idx);
   }
}

int PMM_IsPhysicalPageFree(uint32_t addr)
{
   if (!s_PageBitmap || (addr % PAGE_SIZE) != 0) return 0;

   uint32_t page_idx = addr / PAGE_SIZE;
   if (page_idx >= s_TotalPages) return 0;

   return !bitmap_is_set(page_idx);
}

uint32_t PMM_TotalMemory(void) { return s_TotalPages * PAGE_SIZE; }

uint32_t PMM_FreePages(void) { return s_TotalPages - s_AllocatedCount; }

uint32_t PMM_AllocatedPages(void) { return s_AllocatedCount; }

void PMM_SelfTest(void)
{
   logfmt(LOG_INFO, "[MEM] self-test: starting\n");

   // Allocate a few pages
   uint32_t p1 = PMM_AllocatePhysicalPage();
   uint32_t p2 = PMM_AllocatePhysicalPage();
   uint32_t p3 = PMM_AllocatePhysicalPage();

   if (!p1 || !p2 || !p3)
   {
      logfmt(LOG_ERROR, "[MEM] self-test: FAIL (alloc returned 0)\n");
      return;
   }

   // Check they're page-aligned and different
   if ((p1 % PAGE_SIZE) || (p2 % PAGE_SIZE) || (p3 % PAGE_SIZE))
   {
      logfmt(LOG_ERROR, "[MEM] self-test: FAIL (not page-aligned)\n");
      return;
   }

   if (p1 == p2 || p2 == p3 || p1 == p3)
   {
      logfmt(LOG_ERROR, "[MEM] self-test: FAIL (pages are same)\n");
      return;
   }

   // Free and check
   PMM_FreePhysicalPage(p2);
   if (!PMM_IsPhysicalPageFree(p2))
   {
      logfmt(LOG_ERROR, "[MEM] self-test: FAIL (free didn't work)\n");
      return;
   }

   // Reallocate should get p2 back
   uint32_t p2_new = PMM_AllocatePhysicalPage();
   if (p2_new != p2)
   {
      logfmt(LOG_ERROR,
             "[MEM] self-test: FAIL (realloc didn't get same page)\n");
      return;
   }

   logfmt(LOG_INFO,
          "[MEM] self-test: PASS (allocated %u, freed, reallocated)\n", p1);
}
