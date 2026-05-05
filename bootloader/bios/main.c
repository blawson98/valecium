// SPDX-License-Identifier: GPL-3.0-only

#include <stdint.h>
#include "video/video.h"

/* Multiboot2 tag types */
#define MBI_TAG_END       0
#define MBI_TAG_MMAP      6

int g_PrimaryOutputSystem = 0;

int preferedOutput = OUTPUT_VGATEXT;

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

/* Print which output systems are reported as available. */
void print_available_outputs(uint8_t availableOutputs)
{
   puts("Available outputs:\n");

   if (availableOutputs & (1 << OUTPUT_VBE))
      puts("  VBE\n");
   if (availableOutputs & (1 << OUTPUT_VGA))
      puts("  VGA graphics\n");
   if (availableOutputs & (1 << OUTPUT_VGATEXT))
      puts("  VGA text\n");
   if (availableOutputs & (1 << OUTPUT_SERIAL))
      puts("  Serial (COM1)\n");

   putc('\n');
}

void print_boot_drive_number(int bootDrive)
{
   char *driveType;
   if (bootDrive == 0xe0)
      driveType = "CD/DVD";
   else if (bootDrive < 0x80)
      driveType = "Floppy Disk";
   else
      driveType = "Hard Disk";

   puts("Boot drive information:\n");

   puts("  Boot Drive Number: ");
   puts("0x");
   putx(bootDrive);
   puts(".\n");
   
   puts("  Booted from a ");
   puts(driveType);
   puts(".\n");
}

/* Walk the Multiboot2 Boot Information structure at @mbi_addr
 * and print the memory map entries. */
int main(uint32_t mbi_addr, uint8_t availableOutputs, uint8_t bootDrive)
{
   uint8_t *ptr = (uint8_t *)(uintptr_t)mbi_addr + 8;  /* skip total_size + reserved */

   /* Determine preferred output — highest available wins.
      Priority (ascending): serial → VGA text → VGA graphics → VBE. */
   preferedOutput = OUTPUT_SERIAL;                          /* fallback  */
   if (availableOutputs & (1 << OUTPUT_VGATEXT))
      preferedOutput = OUTPUT_VGATEXT;
   if (availableOutputs & (1 << OUTPUT_VGA))
      preferedOutput = OUTPUT_VGA;
   if (availableOutputs & (1 << OUTPUT_VBE))
      preferedOutput = OUTPUT_VBE;

   preferedOutput = OUTPUT_VGATEXT;

   /* Initialise ONLY the chosen output system.
      VGA/VBE switch the hardware to graphics mode, which destroys text-mode
      output — so they must NOT be initialised unless they are the final pick. */
   switch (preferedOutput)
   {
   case OUTPUT_SERIAL:
      Serial_Initialize();
      break;
   case OUTPUT_VGATEXT:
      VGATEXT_Initialize();
      break;
   case OUTPUT_VGA:
      VGA_Initialize();
      break;
   case OUTPUT_VBE:
      VBE_Initialize();
      break;
   }

   g_PrimaryOutputSystem = availableOutputs;

   puts("Valecium Bootloader loaded.\n");

   print_available_outputs(availableOutputs);
   print_memory_map(ptr);
   print_boot_drive_number(bootDrive);
   return 0;
}