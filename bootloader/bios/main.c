// SPDX-License-Identifier: GPL-3.0-only

#include <stdint.h>
#include "video/video.h"

/* Multiboot2 tag types */
#define MBI_TAG_END       0
#define MBI_TAG_MMAP      6

void print_memory_map(uint8_t *ptr)
{
   puts("Memory Map:\n");
   for (;;)
   {
      uint32_t type = *(uint32_t *)ptr;
      uint32_t size = *(uint32_t *)(ptr + 4);

      if (type == MBI_TAG_END)
         break;

      if (type == MBI_TAG_MMAP)
      {
         uint32_t entry_size = *(uint32_t *)(ptr + 8);
         /* uint32_t entry_version = *(uint32_t *)(ptr + 12); */
         uint8_t *entry = ptr + 16;
         uint32_t total_size = size - 16;
         uint32_t count = total_size / entry_size;
         uint32_t i;

         for (i = 0; i < count; i++)
         {
            uint64_t base  = *(uint64_t *)entry;
            uint64_t len   = *(uint64_t *)(entry + 8);
            uint32_t type2 = *(uint32_t *)(entry + 16);

            puts("  base=");
            putx(base);
            putc('\n');

            puts("  len =");
            putx(len);
            putc('\n');

            puts("  type=");
            puti((int)type2);
            putc('\n');

            puts("  --\n");

            entry += entry_size;
         }
      }

      /* Advance to next tag (8-byte aligned) */
      ptr += size;
      ptr = (uint8_t *)(((uintptr_t)ptr + 7) & ~(uintptr_t)7);
   }
}

/* Walk the Multiboot2 Boot Information structure at @mbi_addr
 * and print the memory map entries. */
int main(uint32_t mbi_addr)
{
   uint8_t *ptr = (uint8_t *)(uintptr_t)mbi_addr + 8;  /* skip total_size + reserved */

   puts("Valecium Bootloader loaded.\n");

   print_memory_map(ptr);
   return 0;
}