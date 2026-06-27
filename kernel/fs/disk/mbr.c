// SPDX-License-Identifier: GPL-3.0-only

#include <fs/devfs/devfs.h>
#include <fs/fs.h>
#include <mem/mm_kernel.h>
#include <std/stdio.h>
#include <stddef.h>
#include <sys/sys.h>

static DEVFS_DeviceOps s_PartitionOps = {.read = Partition_DevfsRead,
                                         .write = Partition_DevfsWrite};

typedef struct
{
   // 0x00	1	Drive attributes (bit 7 set = active or bootable)
   uint8_t attributes;

   // 0x01	3	CHS Address of partition start
   uint8_t chs_start[3];

   // 0x04	1	Partition type
   uint8_t partition_type;

   // 0x05	3	CHS address of last partition sector
   uint8_t chs_end[3];

   // 0x08	4	LBA of partition start
   uint32_t lba_start;

   // 0x0C	4	Number of sectors in partition
   uint32_t size;

} __attribute__((packed)) MBR_Entry;

Partition **MBR_DetectPartition(DISK *disk, int *out_count)
{
   if (!out_count || !disk) return NULL;

   *out_count = 0;

   /* Floppy disks have no MBR partition table.  DISK_Scan handles them
    * directly by creating a synthetic FAT12 volume entry; this function
    * must never be called for floppy devices. */
   if (disk->type == DISK_TYPE_FLOPPY)
   {
      logfmt(LOG_WARNING,
             "[MBR] MBR_DetectPartition called on floppy disk (fd%u) — "
             "floppy partitioning is handled by DISK_Scan directly\n",
             disk->id);
      *out_count = 0;
      return NULL;
   }

   // Hard disk: inspect MBR
   Partition **list = (Partition **)kzalloc(sizeof(Partition *) * 4);
   uint8_t *mbr_buffer = (uint8_t *)kmalloc(512);
   if (!mbr_buffer)
   {
      list[0] = NULL;
      *out_count = 0;
      return list;
   }
   int read_rc = DISK_ReadSectors(disk, 0, 1, mbr_buffer);

   int count = 0;

   if (read_rc == SUCCESS)
   {
      void *partition_entry = &mbr_buffer[446];

      for (int p = 0; p < 4; p++)
      {
         uint8_t *entry = (uint8_t *)partition_entry + (p * 16);
         uint8_t type = entry[4];

         // FAT variants we support
         if (type == 0x04 || type == 0x06 || type == 0x0B || type == 0x0C)
         {
            Partition *part = (Partition *)kzalloc(sizeof(Partition));
            if (!part) continue;

            part->disk = disk;
            part->partition_offset = *(uint32_t *)(entry + 8);
            part->partition_size = *(uint32_t *)(entry + 12);
            part->partition_type = type;

            /* Register ATA partition in devfs: hda1, hda2, hdb1, etc. */
            char devname[8];
            /* disk->id is BIOS drive number (0x80, 0x81, ...) */
            int disk_idx = (disk->id >= 0x80) ? (disk->id - 0x80) : 0;
            devname[0] = 'h';
            devname[1] = 'd';
            devname[2] = 'a' + disk_idx;
            devname[3] = '1' + count; /* partition number */
            devname[4] = '\0';
            uint32_t part_size = part->partition_size * 512;
            DEVFS_RegisterDevice(devname, DEVFS_TYPE_BLOCK, 3,
                                 disk_idx * 16 + count + 1, part_size,
                                 &s_PartitionOps, part);

            list[count++] = part;
         }
      }
   }

   // If nothing detected, fabricate a default partition so higher layers can
   // proceed
   if (count == 0)
   {
      Partition *part = (Partition *)kzalloc(sizeof(Partition));
      if (part)
      {
         part->disk = disk;
         part->partition_offset = (read_rc == SUCCESS) ? 16 : 0;
         part->partition_size = 0x100000;
         list[0] = part;
         count = 1;
      }
   }

   free(mbr_buffer);
   *out_count = count;
   return list;
}