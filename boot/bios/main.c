// SPDX-License-Identifier: GPL-3.0-only

#include <stdint.h>

#include "video/video.h"
#include <constants.h>

// #define DL_RESOLVE
// #include <dl/binding_gen.h.h>
// #include <dl/dl.h>
// #undef DL_RESOLVE

typedef struct FsOperations FsOperations;
typedef struct MbiTagFramebuffer MbiTagFramebuffer;
typedef struct BootParams BootParams;

static void init_framebuffer_info(uint8_t *ptr);
static void print_bios_drive_list(const uint8_t *driveList,
                                  uint32_t driveCount);
static void print_stage3_fs_location(const BootParams *bootParams);

/* Multiboot2 tag types */
#define MBI_TAG_END 0
#define MBI_TAG_MMAP 6
#define MBI_TAG_FRAMEBUFFER 8

#if defined(RELEASE)
#define BUILD_TYPE "release"
#else
#define BUILD_TYPE "debug"
#endif

struct FsOperations
{
   uint32_t FS_Initialize;
   uint32_t FS_Open;
   uint32_t FS_Read;
   uint32_t FS_Close;
};

struct MbiTagFramebuffer
{
   uint32_t type;
   uint32_t size;
   uint64_t framebuffer_addr;
   uint32_t framebuffer_pitch;
   uint32_t framebuffer_width;
   uint32_t framebuffer_height;
   uint8_t framebuffer_bpp;
   uint8_t framebuffer_type;
   uint16_t reserved;
   uint8_t red_field_position;
   uint8_t red_mask_size;
   uint8_t green_field_position;
   uint8_t green_mask_size;
   uint8_t blue_field_position;
   uint8_t blue_mask_size;
   uint8_t rgb_reserved[2];
};

struct BootParams
{
   uint32_t mbiAddr;
   uint32_t corefsAddr;
   uint32_t availableOutputs;
   uint32_t bootDrive;
   uint32_t biosDriveListAddr;
   uint32_t biosDriveListCount;
   uint32_t corefsPartitionUuidAddr;
   uint32_t corefsPartitionLabelAddr;
};

int g_PrimaryOutputSystem = 0;
int preferredOutput = OUTPUT_VGATEXT;
const char *stage3Path =
    "/boot/libTheBootloader-" OS_VERSION "_" BUILD_TYPE ".so";

static void init_framebuffer_info(uint8_t *ptr)
{
   for (;;)
   {
      uint32_t type = *(uint32_t *)ptr;
      uint32_t size = *(uint32_t *)(ptr + 4);

      if (type == MBI_TAG_END) break;

      if (type == MBI_TAG_FRAMEBUFFER && size >= sizeof(MbiTagFramebuffer))
      {
         const MbiTagFramebuffer *tag = (const MbiTagFramebuffer *)ptr;
         if (tag->framebuffer_type == 1)
         {
            VBE_Info info;
            info.framebuffer_addr = tag->framebuffer_addr;
            info.pitch = tag->framebuffer_pitch;
            info.width = tag->framebuffer_width;
            info.height = tag->framebuffer_height;
            info.bpp = tag->framebuffer_bpp;
            info.red_field_position = tag->red_field_position;
            info.red_mask_size = tag->red_mask_size;
            info.green_field_position = tag->green_field_position;
            info.green_mask_size = tag->green_mask_size;
            info.blue_field_position = tag->blue_field_position;
            info.blue_mask_size = tag->blue_mask_size;
            VBE_SetInfo(&info);
         }
      }

      /* Advance to next tag (8-byte aligned) */
      ptr += size;
      ptr = (uint8_t *)(((uintptr_t)ptr + 7) & ~(uintptr_t)7);
   }
}

static void print_bios_drive_list(const uint8_t *driveList, uint32_t driveCount)
{
   uint32_t i;

   printf("Detected BIOS drives:\n");

   if (!driveList || driveCount == 0)
   {
      printf("  (none)\n\n");
      return;
   }

   for (i = 0; i < driveCount; i++)
   {
      printf("  0x%x\n", driveList[i]);
   }

   printf("\n");
}

static void print_stage3_fs_location(const BootParams *bootParams)
{
   printf("Partition label: \"%s\".\n",
          (const char *)(uintptr_t)bootParams->corefsPartitionLabelAddr);

   printf("Partition UUID: ");
   {
      const uint8_t *uuid =
          (const uint8_t *)bootParams->corefsPartitionUuidAddr;
      for (int i = 0; i < 16; i++)
      {
         printf("%x", uuid[i]);
      }
   }
   printf(".\n\n");
}

void print_memory_map(uint8_t *ptr)
{
   printf("Memory Map:\n");
   for (;;)
   {
      uint32_t type = *(uint32_t *)ptr;
      uint32_t size = *(uint32_t *)(ptr + 4);

      if (type == MBI_TAG_END) break;

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
            uint64_t base = *(uint64_t *)entry;
            uint64_t len = *(uint64_t *)(entry + 8);
            uint32_t type2 = *(uint32_t *)(entry + 16);

            printf("  base=%x\n", base);
            printf("  len =%x\n", len);
            printf("  type=%d\n", (int)type2);
            printf("  --\n");

            entry += entry_size;
         }
      }

      /* Advance to next tag (8-byte aligned) */
      ptr += size;
      ptr = (uint8_t *)(((uintptr_t)ptr + 7) & ~(uintptr_t)7);
   }
   printf("\n");
}

/* Print which output systems are reported as available. */
void print_available_outputs(uint8_t availableOutputs)
{
   printf("Available outputs:\n");

   if (availableOutputs & (1 << OUTPUT_VBE)) printf("  VBE\n");
   if (availableOutputs & (1 << OUTPUT_VGA)) printf("  VGA graphics\n");
   if (availableOutputs & (1 << OUTPUT_VGATEXT)) printf("  VGA text\n");
   if (availableOutputs & (1 << OUTPUT_SERIAL)) printf("  Serial (COM1)\n");

   printf("\n");
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

   printf("Boot drive information:\n");

   printf("  Boot Drive Number: 0x%x.\n", bootDrive);

   printf("  Booted from a %s.\n\n", driveType);
}

void print_corefs_memory_address(uint32_t address)
{
   printf("Corefs Module location: %x.\n\n", address);
}

void init_fs(FsOperations *fs_ops, const uint8_t *biosDriveList,
             uint32_t biosDriveListCount, const uint8_t *partitionUuid,
             const uint8_t *partitionLabel)
{
   printf("Entering filesystem setup.\n");

   typedef int (*fs_init_fn)(const uint8_t *, uint32_t, const uint8_t *,
                             const uint8_t *);
   fs_init_fn FS_Initialize = (fs_init_fn)fs_ops->FS_Initialize;
   int rc = FS_Initialize(biosDriveList, biosDriveListCount, partitionUuid,
                          partitionLabel);
   if (rc != SUCCESS)
   {
      printf("  FS_Initialize failed: %d.\n", rc);
   }
   else
   {
      printf("  FS initialized successful.\n");
   }
}

int main(const BootParams *bootParams)
{
   uint8_t *ptr = (uint8_t *)(uintptr_t)bootParams->mbiAddr + 8;
   uint8_t availableOutputs = (uint8_t)bootParams->availableOutputs;
   uint8_t bootDrive = (uint8_t)bootParams->bootDrive;
   uint32_t biosDriveListCount = bootParams->biosDriveListCount;
   FsOperations *fs_ops = (FsOperations *)bootParams->corefsAddr;
   const uint8_t *partitionUuid =
       (const uint8_t *)(uintptr_t)bootParams->corefsPartitionUuidAddr;
   const uint8_t *partitionLabel =
       (const uint8_t *)(uintptr_t)bootParams->corefsPartitionLabelAddr;
   const uint8_t *biosDriveList =
       (const uint8_t *)(uintptr_t)bootParams->biosDriveListAddr;

   /* Determine preferred output - highest available wins.
      Priority (ascending): serial - VGA text - VGA graphics - VBE. */
   init_framebuffer_info(ptr);

   preferredOutput = OUTPUT_SERIAL; /* fallback  */
   if (availableOutputs & (1 << OUTPUT_VGATEXT))
      preferredOutput = OUTPUT_VGATEXT;
   if (availableOutputs & (1 << OUTPUT_VGA)) preferredOutput = OUTPUT_VGA;
   if ((availableOutputs & (1 << OUTPUT_VBE)) && VBE_HasInfo())
      preferredOutput = OUTPUT_VBE;

   /* Initialise ONLY the chosen output system.
      VGA/VBE switch the hardware to graphics mode, which destroys text-mode
      output - so they must NOT be initialised unless they are the final pick.
    */
   switch (preferredOutput)
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

   print_available_outputs(availableOutputs);
   print_memory_map(ptr);
   print_boot_drive_number(bootDrive);
   print_bios_drive_list(biosDriveList, biosDriveListCount);
   print_corefs_memory_address(bootParams->corefsAddr);
   print_stage3_fs_location(bootParams);

   init_fs(fs_ops, biosDriveList, biosDriveListCount, partitionUuid,
           partitionLabel);

   for (;;)
      ;

   return 0;
}
