// SPDX-License-Identifier: GPL-3.0-only
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUCCESS 0
#define EINVAL (-22)
#define ENODEV (-19)

#define SECTOR_SIZE 2048

struct fs_operations
{
   uint32_t FS_Initialize;
   uint32_t FS_Open;
   uint32_t FS_Read;
   uint32_t FS_Close;
};

extern int DISK_Read(uint8_t drive, uint16_t cylinder, uint8_t sector,
                     uint8_t head, uint8_t count, void *buffer);
extern bool MBR_Probe(int driveId);
extern int MBR_List(int driveId, int **offset);
extern bool GPT_Probe(int driveId);
extern int GPT_List(int driveId, int **offset);

int FS_Initialize(const uint8_t *biosDriveList, uint32_t biosDriveListCount,
                  const uint8_t *partitionUuid, const uint8_t *partitionLabel)
{
   (void)partitionUuid;
   (void)partitionLabel;

   if (!biosDriveList) return SUCCESS;

   int total_partitions = 0;
   if (biosDriveListCount > 0)
   {
      for (uint32_t i = 0; i < biosDriveListCount; i++)
      {
         uint8_t drive = biosDriveList[i];
         int *offsets = NULL;
         int count = -1;

         if (GPT_Probe(drive))
         {
            count = GPT_List(drive, &offsets);
         }
         else if (MBR_Probe(drive))
         {
            count = MBR_List(drive, &offsets);
         }
         else
         {
            int fallback_offset = 0;
            offsets = &fallback_offset;
            count = 1;
         }

         if (count > 0)
         {
            for (int j = 0; j < count; j++)
            {
               (void)offsets[j];
               total_partitions++;
            }
         }
      }
   }
   else
   {
      for (const uint8_t *drive = biosDriveList; *drive != 0; drive++)
      {
         int *offsets = NULL;
         int count = -1;

         if (GPT_Probe(*drive))
         {
            count = GPT_List(*drive, &offsets);
         }
         else if (MBR_Probe(*drive))
         {
            count = MBR_List(*drive, &offsets);
         }
         else
         {
            int fallback_offset = 0;
            offsets = &fallback_offset;
            count = 1;
         }

         if (count > 0)
         {
            for (int i = 0; i < count; i++)
            {
               (void)offsets[i];
               total_partitions++;
            }
         }
      }
   }

   return -total_partitions;
}
int FS_Open(void) { return -ENODEV; }
int FS_Read(void) { return -EINVAL; }
int FS_Close(void) { return SUCCESS; }

#ifdef COREFS

static const struct fs_operations fs_exports
    __attribute__((section(".exports"), used)) = {
        .FS_Initialize = (uint32_t)FS_Initialize,
        .FS_Open = (uint32_t)FS_Open,
        .FS_Read = (uint32_t)FS_Read,
        .FS_Close = (uint32_t)FS_Close,
};

#endif /* COREFS */
