// SPDX-License-Identifier: GPL-3.0-only
#include <stddef.h>
#include <stdint.h>

struct fs_operations
{
   uint32_t FS_Initialize;
   uint32_t FS_Open;
   uint32_t FS_Read;
   uint32_t FS_Close;
};

extern int DISK_Read(uint8_t drive, uint16_t cylinder, uint8_t sector,
                     uint8_t head, uint8_t count, void *buffer);

int FS_Initialize(const uint8_t *biosDriveList, uint32_t biosDriveListCount,
                  const uint8_t *partitionUuid)
{
   (void)biosDriveList;
   (void)biosDriveListCount;
   (void)partitionUuid;

   return 0;
}
int FS_Open(void) { return 2; }
int FS_Read(void) { return 3; }
int FS_Close(void) { return 4; }

#ifdef COREFS

static const struct fs_operations fs_exports
    __attribute__((section(".exports"), used)) = {
        .FS_Initialize = (uint32_t)FS_Initialize,
        .FS_Open = (uint32_t)FS_Open,
        .FS_Read = (uint32_t)FS_Read,
        .FS_Close = (uint32_t)FS_Close,
};

#endif /* COREFS */
