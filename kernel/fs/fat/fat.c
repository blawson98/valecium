// SPDX-License-Identifier: GPL-3.0-only

#include <stddef.h>

#include <crypto/crypto.h>
#include <drivers/ata/ata.h>
#include <drivers/fdc/fdc.h>
#include <fs/fs.h>
#include <fs/vfs/vfs.h>
#include <mem/mm_kernel.h>
#include <std/ctype.h>
#include <std/minmax.h>
#include <std/stdio.h>
#include <std/string.h>
#include <sys/sys.h>

#include "fat.h"

#define SECTOR_SIZE 512
#define MAX_PATH_SIZE 256
#define MAX_FILE_HANDLES 10
#define ROOT_DIRECTORY_HANDLE -1
#define FAT_CACHE_SIZE 5
#define FAT_METADATA_PATH "/.vkmeta"
#define FAT_METADATA_FLAG_VALID 0x01
#define FAT_METADATA_FLAG_DELETED 0x02

// Forward declarations for internal helpers
static uint32_t FAT_ClusterToLba(const FAT_Instance *inst, uint32_t cluster);
static int FAT_ReadFat(FAT_Instance *inst, Partition *disk, size_t lba_index);

typedef struct
{
   uint8_t hash[SHA1_DIGEST_SIZE];
   uint16_t mode;
   uint32_t uid;
   uint32_t gid;
   uint8_t flags;
   uint8_t _reserved[5];
} __attribute__((packed)) FAT_MetadataRecord;

typedef struct
{
   // extended boot record
   uint8_t drive_number;
   uint8_t _reserved;
   uint8_t signature;
   uint32_t volume_id;       // serial number, value doesn't matter
   uint8_t volume_label[11]; // 11 bytes, padded with spaces
   uint8_t system_id[8];
} __attribute__((packed)) FAT_ExtendedBootRecord;

typedef struct
{
   uint32_t sectors_per_fat;
   uint16_t flags;
   uint16_t fat_version_number;
   uint32_t root_directory_cluster;
   uint16_t fs_info_sector;
   uint16_t backup_boot_sector;
   uint8_t _reserved[12];
   FAT_ExtendedBootRecord ebr;
} __attribute__((packed)) FAT32_ExtendedBootRecord;

typedef struct
{
   uint8_t boot_jump_instruction[3];
   uint8_t oem_identifier[8];
   uint16_t bytes_per_sector;
   uint8_t sectors_per_cluster;
   uint16_t reserved_sectors;
   uint8_t fat_count;
   uint16_t dir_entry_count;
   uint16_t total_sectors;
   uint8_t media_descriptor_type;
   uint16_t sectors_per_fat;
   uint16_t sectors_per_track;
   uint16_t heads;
   uint32_t hidden_sectors;
   uint32_t large_sector_count;

   union
   {
      FAT_ExtendedBootRecord ebr_1216;
      FAT32_ExtendedBootRecord ebr_32;
   } extended_boot_record;
} __attribute__((packed)) FAT_BootSector;

typedef struct
{
   uint8_t buffer[SECTOR_SIZE];
   FAT_File public_file;
   bool opened;
   bool truncated; // Track if file has been truncated for writing
   uint32_t first_cluster;
   uint32_t current_cluster;
   uint32_t current_sector_in_cluster;

   // Track parent directory so we can update the owning directory entry.
   uint32_t parent_cluster;
   bool parent_is_root;

} FAT_FileData;

// FAT_Instance — encapsulates ALL per-volume state. One instance allocated
// by FAT_Initialize() per mounted partition, stored in
// Partition->fs->private_data.
struct FAT_Instance
{
   // Boot sector (formerly FAT_Data::bs)
   union
   {
      FAT_BootSector boot_sector;
      uint8_t boot_sector_bytes[SECTOR_SIZE];
   } bs;

   // Root-directory pseudo-handle (always open, always index -1)
   FAT_FileData root_directory;

   // Per-handle open-file table
   FAT_FileData opened_files[MAX_FILE_HANDLES];

   // FAT sector cache
   uint8_t fat_cache[FAT_CACHE_SIZE * SECTOR_SIZE];
   uint32_t fat_cache_pos;

   // Derived filesystem geometry (formerly g_* globals)
   uint32_t data_section_lba;
   uint8_t fat_type; /* 12, 16, or 32 */
   uint32_t total_sectors;
   uint32_t sectors_per_fat;
   uint32_t root_dir_lba; /* FAT12/16 fixed-root start LBA (0 for FAT32) */
   uint32_t
       root_dir_sectors; /* FAT12/16 fixed-root sector count (0 for FAT32) */
};

static uint16_t fat_normalize_mode(uint16_t mode)
{
   uint16_t masked = mode & 0777u;
   if (masked == 0) return 0644u;
   return masked;
}

static int fat_is_metadata_path(const char *path)
{
   return (path && strcmp(path, FAT_METADATA_PATH) == 0) ? SUCCESS : -ENOENT;
}

static void fat_normalize_absolute_path(const char *path, char *out,
                                        size_t out_size)
{
   if (!out || out_size == 0) return;

   if (!path || path[0] == '\0')
   {
      out[0] = '/';
      if (out_size > 1) out[1] = '\0';
      return;
   }

   if (path[0] == '/')
   {
      strncpy(out, path, out_size - 1);
      out[out_size - 1] = '\0';
      return;
   }

   out[0] = '/';
   strncpy(out + 1, path, out_size - 2);
   out[out_size - 1] = '\0';
}

// Retrieve the FAT_Instance stored in a Partition's filesystem slot.
static inline FAT_Instance *fat_inst(const Partition *disk)
{
   if (!disk || !disk->fs) return NULL;
   return (FAT_Instance *)disk->fs->private_data;
}

static int FAT_ReadFat(FAT_Instance *inst, Partition *disk, size_t lba_index)
{
   return Partition_ReadSectors(
       disk, inst->bs.boot_sector.reserved_sectors + lba_index, FAT_CACHE_SIZE,
       inst->fat_cache);
}

static void FAT_Detect(FAT_Instance *inst)
{
   uint32_t data_clusters = (inst->total_sectors - inst->data_section_lba) /
                            inst->bs.boot_sector.sectors_per_cluster;
   if (data_clusters < 0xFF5)
      inst->fat_type = 12;
   else if (inst->bs.boot_sector.sectors_per_fat != 0)
      inst->fat_type = 16;
   else
      inst->fat_type = 32;
}

FAT_Instance *FAT_Initialize(Partition *disk)
{
   // Allocate and zero-initialise the per-volume instance.
   FAT_Instance *inst = (FAT_Instance *)kmalloc(sizeof(FAT_Instance));
   if (!inst)
   {
      logfmt(LOG_ERROR, "[FAT] Failed to allocate FAT_Instance\n");
      return NULL;
   }
   memset(inst, 0, sizeof(FAT_Instance));

   // Read boot sector from partition
   uint8_t *boot_sector = (uint8_t *)kmalloc(512);
   if (!boot_sector)
   {
      logfmt(LOG_ERROR, "[FAT] Failed to allocate boot sector buffer\n");
      free(inst);
      return NULL;
   }
   if (Partition_ReadSectors(disk, 0, 1, boot_sector) < 0)
   {
      logfmt(LOG_ERROR, "[FAT] Failed to read boot sector\n");
      free(boot_sector);
      free(inst);
      return NULL;
   }

   // Check for valid FAT signature (0x55AA at bytes 510-511)
   if (boot_sector[510] != 0x55 || boot_sector[511] != 0xAA)
   {
      logfmt(LOG_ERROR, "[FAT] Invalid boot sector signature\n");
      free(boot_sector);
      free(inst);
      return NULL;
   }

   // Copy boot sector into instance
   memcpy(inst->bs.boot_sector_bytes, boot_sector, SECTOR_SIZE);
   free(boot_sector);

   // Debug: print BPB values
   logfmt(LOG_INFO, "[FAT] BPB bytes_per_sector=%u, sectors_per_cluster=%u\n",
          inst->bs.boot_sector.bytes_per_sector,
          inst->bs.boot_sector.sectors_per_cluster);

   // Validate critical BPB values to prevent divide-by-zero later
   if (inst->bs.boot_sector.bytes_per_sector == 0 ||
       inst->bs.boot_sector.sectors_per_cluster == 0)
   {
      logfmt(LOG_ERROR,
             "[FAT] Invalid BPB (bytes_per_sector=%u, "
             "sectors_per_cluster=%u)\n",
             inst->bs.boot_sector.bytes_per_sector,
             inst->bs.boot_sector.sectors_per_cluster);
      free(inst);
      return NULL;
   }

   // Initialise FAT cache as invalid
   inst->fat_cache_pos = 0xFFFFFFFF;

   inst->total_sectors = inst->bs.boot_sector.total_sectors;
   if (inst->total_sectors == 0)
   { // fat32
      inst->total_sectors = inst->bs.boot_sector.large_sector_count;
   }

   bool is_fat32 = false;
   inst->sectors_per_fat = inst->bs.boot_sector.sectors_per_fat;
   uint32_t root_dir_cluster = 0;

   if (inst->sectors_per_fat == 0)
   { // fat32
      is_fat32 = true;
      root_dir_cluster = inst->bs.boot_sector.extended_boot_record.ebr_32
                             .root_directory_cluster;
      inst->sectors_per_fat =
          inst->bs.boot_sector.extended_boot_record.ebr_32.sectors_per_fat;
   }

   // open root directory file
   uint32_t root_dir_lba;
   uint32_t root_dir_size;
   if (is_fat32)
   {
      // Data section starts after reserved + FAT areas
      inst->data_section_lba =
          inst->bs.boot_sector.reserved_sectors +
          inst->sectors_per_fat * inst->bs.boot_sector.fat_count;

      // For FAT32 the root directory is a normal cluster chain starting at
      // RootDirectoryCluster. Keep cluster number in
      // RootDirectory.first_cluster. root_dir_lba/RootDirSectors = 0 indicates
      // a clustered root.
      inst->root_dir_lba = 0;
      inst->root_dir_sectors = 0;
      root_dir_lba =
          FAT_ClusterToLba(inst, root_dir_cluster); // first cluster LBA
      root_dir_size = 0;
   }
   else
   {
      // FAT12/16: root directory stored in a fixed area (immediately after
      // FATs)
      root_dir_lba = inst->bs.boot_sector.reserved_sectors +
                     inst->sectors_per_fat * inst->bs.boot_sector.fat_count;
      root_dir_size =
          sizeof(FAT_DirectoryEntry) * inst->bs.boot_sector.dir_entry_count;
      uint32_t root_dir_sectors =
          (root_dir_size + inst->bs.boot_sector.bytes_per_sector - 1) /
          inst->bs.boot_sector.bytes_per_sector;
      // Data section starts AFTER the root directory
      inst->data_section_lba = root_dir_lba + root_dir_sectors;

      inst->root_dir_lba = root_dir_lba;
      inst->root_dir_sectors = root_dir_sectors;
   }

   inst->root_directory.public_file.handle = ROOT_DIRECTORY_HANDLE;
   inst->root_directory.public_file.is_directory = true;
   inst->root_directory.public_file.position = 0;
   inst->root_directory.public_file.instance = inst; // backpointer
   inst->root_directory.opened = true;
   inst->root_directory.truncated = false; // Root directory cannot be truncated
   if (is_fat32)
      // For FAT32, root is a cluster chain; use a large safe size
      inst->root_directory.public_file.size = 0x1000000; // 16 MiB max
   else
      inst->root_directory.public_file.size =
          sizeof(FAT_DirectoryEntry) * inst->bs.boot_sector.dir_entry_count;
   if (is_fat32)
   {
      inst->root_directory.first_cluster = root_dir_cluster;
      inst->root_directory.current_cluster = root_dir_cluster;
      inst->root_directory.current_sector_in_cluster = 0;

      // Read first sector of root cluster into buffer
      if (Partition_ReadSectors(disk, FAT_ClusterToLba(inst, root_dir_cluster),
                                1, inst->root_directory.buffer) < 0)
      {
         logfmt(LOG_WARNING,
                "[FAT] Warning: could not pre-load FAT32 root dir sector\n");
      }
   }
   else
   {
      // For FAT12/16 we treat first_cluster/CurrentCluster as the starting LBA
      inst->root_directory.first_cluster = root_dir_lba;
      inst->root_directory.current_cluster = root_dir_lba;
      inst->root_directory.current_sector_in_cluster = 0;

      // Read first sector of root directory from disk
      if (Partition_ReadSectors(disk, root_dir_lba, 1,
                                inst->root_directory.buffer) < 0)
      {
         logfmt(LOG_WARNING,
                "[FAT] Warning: could not pre-load FAT12/16 root dir sector\n");
      }
   }

   inst->root_directory.parent_cluster = inst->root_directory.first_cluster;
   inst->root_directory.parent_is_root = true;

   // Detect FAT type (12 / 16 / 32)
   FAT_Detect(inst);

   // Reset opened files
   for (int i = 0; i < MAX_FILE_HANDLES; i++)
   {
      inst->opened_files[i].opened = false;
      inst->opened_files[i].truncated = false;
   }

   return inst;
}

// LBA_cluster = data_section_lba + (cluster - 2) * sectors_per_cluster
static uint32_t FAT_ClusterToLba(const FAT_Instance *inst, uint32_t cluster)
{
   return inst->data_section_lba +
          (cluster - 2) * inst->bs.boot_sector.sectors_per_cluster;
}

// Write a FAT entry for the given cluster across all FAT copies and update the
// in-memory cache if it covers the written sector. Value should be masked to
// the appropriate width by caller (e.g., EOF marker or 0 for free).
static int FAT_WriteFatEntry(FAT_Instance *inst, Partition *disk,
                             uint32_t cluster, uint32_t value)
{
   if (!inst || !disk)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_WriteFatEntry: inst or disk is NULL\n");
      return -EINVAL;
   }

   uint32_t fat_byte_offset;
   if (inst->fat_type == 12)
      fat_byte_offset = cluster * 3 / 2;
   else if (inst->fat_type == 16)
      fat_byte_offset = cluster * 2;
   else
      fat_byte_offset = cluster * 4;

   uint32_t fat_sector_offset = fat_byte_offset / SECTOR_SIZE;
   uint32_t fat_byte_off_in_sector = fat_byte_offset % SECTOR_SIZE;

   // Iterate over all FAT copies
   for (uint32_t fat_idx = 0; fat_idx < inst->bs.boot_sector.fat_count;
        fat_idx++)
   {
      uint32_t fat_sector_lba = inst->bs.boot_sector.reserved_sectors +
                                fat_idx * inst->sectors_per_fat +
                                fat_sector_offset;

      uint8_t fat_buffer[SECTOR_SIZE * 2];
      if (Partition_ReadSectors(disk, fat_sector_lba, 1, fat_buffer) < 0)
         return -EIO;

      bool cross_boundary =
          (inst->fat_type == 12 && fat_byte_off_in_sector == SECTOR_SIZE - 1);
      if (cross_boundary)
      {
         if (Partition_ReadSectors(disk, fat_sector_lba + 1, 1,
                                   fat_buffer + SECTOR_SIZE) < 0)
            return -EIO;
      }

      if (inst->fat_type == 12)
      {
         uint16_t *p = (uint16_t *)(fat_buffer + fat_byte_off_in_sector);
         if (cluster % 2 == 0)
            *p = (*p & 0xF000) | (value & 0x0FFF);
         else
            *p = (*p & 0x000F) | ((value & 0x0FFF) << 4);
      }
      else if (inst->fat_type == 16)
      {
         *(uint16_t *)(fat_buffer + fat_byte_off_in_sector) = (uint16_t)value;
      }
      else // FAT32
      {
         uint32_t *entry = (uint32_t *)(fat_buffer + fat_byte_off_in_sector);
         uint32_t old_value = *entry;
         // Preserve top 4 bits, set lower 28 bits
         *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);
         if (cluster >= 9564 && cluster <= 9580)
         {
            logfmt(LOG_INFO,
                   "[FAT] FAT_WriteFatEntry: cluster=%u, old_value=0x%08x, "
                   "newValue=0x%08x, LBA=%u, offset=%u\n",
                   cluster, old_value, *entry, fat_sector_lba,
                   fat_byte_off_in_sector);
         }
      }

      if (Partition_WriteSectors(disk, fat_sector_lba, 1, fat_buffer) < 0)
         return -EIO;

      if (cross_boundary)
      {
         if (Partition_WriteSectors(disk, fat_sector_lba + 1, 1,
                                    fat_buffer + SECTOR_SIZE) < 0)
            return -EIO;
      }
   }

   // Update cache if this sector is currently cached (cache covers FAT copy 0)
   if (inst->fat_cache_pos != 0xFFFFFFFF)
   {
      // Check first sector
      if (fat_sector_offset >= inst->fat_cache_pos &&
          fat_sector_offset < inst->fat_cache_pos + FAT_CACHE_SIZE)
      {
         uint8_t *cache =
             inst->fat_cache +
             (fat_sector_offset - inst->fat_cache_pos) * SECTOR_SIZE;
         if (inst->fat_type == 12)
         {
            // Crossing a sector boundary in the cache is complex; just
            // invalidate to stay safe.
            inst->fat_cache_pos = 0xFFFFFFFF;
         }
         else if (inst->fat_type == 16)
         {
            *(uint16_t *)(cache + fat_byte_off_in_sector) = (uint16_t)value;
         }
         else // FAT32
         {
            uint32_t *entry = (uint32_t *)(cache + fat_byte_off_in_sector);
            // Preserve top 4 bits, set lower 28 bits
            *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);
         }
      }

      // Simpler to just invalidate cache for FAT12 to avoid boundary issues.
      if (inst->fat_type == 12) inst->fat_cache_pos = 0xFFFFFFFF;
   }

   return SUCCESS;
}

static FAT_File *FAT_OpenEntry(FAT_Instance *inst, Partition *disk,
                               FAT_DirectoryEntry *entry, FAT_FileData *parent)
{
   // find empty handle
   int handle = -1;
   for (int i = 0; i < MAX_FILE_HANDLES && handle < 0; i++)
   {
      if (!inst->opened_files[i].opened) handle = i;
   }

   // Out of handles
   if (handle < 0)
   {
      return NULL;
   }

   // Setup vars
   FAT_FileData *fd = &inst->opened_files[handle];
   fd->public_file.handle = handle;
   fd->public_file.is_directory =
       (entry->attributes & FAT_ATTRIBUTE_DIRECTORY) != 0;
   fd->public_file.position = 0;
   fd->public_file.size = entry->size;
   fd->public_file.instance =
       inst;              // backpointer so callers don't need Partition*
   fd->truncated = false; // Not yet truncated
   memcpy(fd->public_file.name, entry->name, 11); // Save the name
   fd->first_cluster =
       entry->first_cluster_low + ((uint32_t)entry->first_cluster_high << 16);

   // Validate cluster number
   if (fd->first_cluster != 0 && fd->public_file.size > 0)
   {
      uint32_t max_clusters = (inst->total_sectors - inst->data_section_lba) /
                              inst->bs.boot_sector.sectors_per_cluster;
      if (fd->first_cluster < 2 || fd->first_cluster >= max_clusters + 2)
      {
         logfmt(LOG_ERROR, "[FAT] invalid first_cluster=%u (max=%u) for file\n",
                fd->first_cluster, max_clusters + 2);
         return NULL;
      }
   }

   // Record parent directory information for later updates
   if (parent != NULL)
   {
      fd->parent_cluster = parent->first_cluster;
      fd->parent_is_root = (parent == &inst->root_directory);
   }
   else
   {
      // Fallback: assume root
      fd->parent_cluster = inst->root_directory.first_cluster;
      fd->parent_is_root = true;
   }

   fd->current_cluster = fd->first_cluster;
   fd->current_sector_in_cluster = 0;

   // Skip the initial sector read only when the entry has no data cluster.
   // Directories frequently have Size=0 on FAT but still require reading their
   // first cluster for iteration.
   if (fd->first_cluster == 0)
   {
      fd->opened = true;
      return &fd->public_file;
   }

   // Guard against bogus cluster numbers that would underflow LBA math
   if (fd->first_cluster < 2)
   {
      logfmt(LOG_ERROR,
             "[FAT] invalid first_cluster=%u for file, refusing to open\n",
             fd->first_cluster);
      return NULL;
   }

   uint32_t lba = FAT_ClusterToLba(inst, fd->current_cluster);

   if (Partition_ReadSectors(disk, lba, 1, fd->buffer) < 0)
   {
      logfmt(LOG_ERROR,
             "[FAT] open entry failed - read error cluster=%u lba=%u\n",
             fd->current_cluster, lba);
      // Don't open the file if we can't read its data
      return NULL;
   }

   fd->opened = true;
   return &fd->public_file;
}

static uint32_t FAT_NextCluster(FAT_Instance *inst, Partition *disk,
                                uint32_t current_cluster)
{
   uint32_t fat_index = 0;

   if (inst->fat_type == 12)
      fat_index = current_cluster * 3 / 2;
   else if (inst->fat_type == 16)
      fat_index = current_cluster * 2;
   else // FAT32
      fat_index = current_cluster * 4;

   uint32_t fat_index_sector = fat_index / SECTOR_SIZE;
   if (fat_index_sector < inst->fat_cache_pos ||
       fat_index_sector >= inst->fat_cache_pos + FAT_CACHE_SIZE)
   {
      if (FAT_ReadFat(inst, disk, fat_index_sector) < 0)
      {
         logfmt(LOG_ERROR,
                "[FAT] FAT_NextCluster: FAT_ReadFat failed for sector %u\n",
                fat_index_sector);
         return 0xFFFFFFFF; // Return EOC marker to stop cluster traversal
      }
      inst->fat_cache_pos = fat_index_sector;
   }

   fat_index -= (inst->fat_cache_pos * SECTOR_SIZE);
   uint32_t next_cluster = 0xFFFFFFFF;
   if (inst->fat_type == 12)
   {
      if (current_cluster % 2 == 0)
         next_cluster = (*(uint16_t *)(inst->fat_cache + fat_index)) & 0x0fff;
      else
         next_cluster = (*(uint16_t *)(inst->fat_cache + fat_index)) >> 4;

      if (next_cluster >= 0xff8) next_cluster |= 0xfffff000;
   }
   else if (inst->fat_type == 16)
   {
      next_cluster = *(uint16_t *)(inst->fat_cache + fat_index);
      if (next_cluster >= 0xfff8) next_cluster |= 0xffff0000;
   }
   else // FAT32
   {
      uint32_t raw = *(uint32_t *)(inst->fat_cache + fat_index);
      next_cluster = raw & 0x0FFFFFFF;
   }
   return next_cluster;
}

static int fat_metadata_append_record_full(Partition *disk, const char *path,
                                           uint16_t mode, uint32_t uid,
                                           uint32_t gid, uint8_t flags)
{
   if (!disk || !path) return -EINVAL;

   char normalized_path[MAX_PATH_SIZE];
   fat_normalize_absolute_path(path, normalized_path, sizeof(normalized_path));
   if (fat_is_metadata_path(normalized_path) == SUCCESS) return SUCCESS;

   FAT_File *meta = FAT_Open(disk, FAT_METADATA_PATH);
   if (!meta)
   {
      meta = FAT_Create(disk, FAT_METADATA_PATH, 0600u);
      if (!meta) return -EIO;
   }

   if (meta->handle == ROOT_DIRECTORY_HANDLE || meta->is_directory)
   {
      if (meta->handle != ROOT_DIRECTORY_HANDLE) FAT_Close(meta);
      return FAT_ESTATE;
   }

   FAT_MetadataRecord record;
   memset(&record, 0, sizeof(record));
   SHA1_Calculate(normalized_path, strlen(normalized_path), record.hash);
   record.mode = fat_normalize_mode(mode);
   record.uid = uid;
   record.gid = gid;
   record.flags = FAT_METADATA_FLAG_VALID | flags;

   if (FAT_Seek(disk, meta, meta->size) < 0)
   {
      FAT_Close(meta);
      return -EIO;
   }

   uint32_t written = FAT_Write(disk, meta, sizeof(record), &record);
   FAT_Close(meta);
   return (written == sizeof(record)) ? SUCCESS : -EIO;
}

static int fat_metadata_append_record(Partition *disk, const char *path,
                                      uint16_t mode, uint8_t flags)
{
   return fat_metadata_append_record_full(disk, path, mode, 0, 0, flags);
}

static int fat_metadata_lookup_latest(Partition *disk, const char *path,
                                      FAT_MetadataRecord *record_out,
                                      bool *found_out)
{
   if (!disk || !path)
   {
      if (found_out) *found_out = false;
      return -EINVAL;
   }

   char normalized_path[MAX_PATH_SIZE];
   fat_normalize_absolute_path(path, normalized_path, sizeof(normalized_path));

   uint8_t hash[SHA1_DIGEST_SIZE];
   SHA1_Calculate(normalized_path, strlen(normalized_path), hash);

   FAT_File *meta = FAT_Open(disk, FAT_METADATA_PATH);
   if (!meta)
   {
      if (found_out) *found_out = false;
      return SUCCESS;
   }

   if (meta->handle == ROOT_DIRECTORY_HANDLE || meta->is_directory)
   {
      if (meta->handle != ROOT_DIRECTORY_HANDLE) FAT_Close(meta);
      if (found_out) *found_out = false;
      return FAT_ESTATE;
   }

   if (FAT_Seek(disk, meta, 0) < 0)
   {
      FAT_Close(meta);
      if (found_out) *found_out = false;
      return -EIO;
   }

   FAT_MetadataRecord rec;
   bool found = false;
   while (FAT_Read(disk, meta, sizeof(rec), &rec) == sizeof(rec))
   {
      if ((rec.flags & FAT_METADATA_FLAG_VALID) == 0) continue;
      if (memcmp(rec.hash, hash, SHA1_DIGEST_SIZE) == 0)
      {
         if (record_out) *record_out = rec;
         found = true;
      }
   }

   FAT_Close(meta);
   if (found_out) *found_out = found;
   return SUCCESS;
}

static int fat_check_access_path(Partition *disk, const char *path,
                                 uint32_t uid, uint32_t gid,
                                 uint8_t access_mask)
{
   if (!disk || !path) return -EINVAL;
   if (uid == 0) return SUCCESS;

   FAT_MetadataRecord rec;
   bool found = false;
   int lookup_rc = fat_metadata_lookup_latest(disk, path, &rec, &found);
   if (lookup_rc < 0) return lookup_rc;
   if (!found) return SUCCESS;
   if (rec.flags & FAT_METADATA_FLAG_DELETED) return FAT_EPERM;

   uint8_t perm;
   if (uid == rec.uid)
      perm = (rec.mode >> 6) & 0x7;
   else if (gid == rec.gid)
      perm = (rec.mode >> 3) & 0x7;
   else
      perm = rec.mode & 0x7;

   if ((access_mask & 0x4) && ((perm & 0x4) == 0)) return FAT_EPERM;
   if ((access_mask & 0x2) && ((perm & 0x2) == 0)) return FAT_EPERM;
   if ((access_mask & 0x1) && ((perm & 0x1) == 0)) return FAT_EPERM;
   return SUCCESS;
}

static int fat_chmod_path(Partition *disk, const char *path, uint16_t mode)
{
   if (!disk || !path) return -EINVAL;

   FAT_MetadataRecord rec;
   bool found = false;
   int lookup_rc = fat_metadata_lookup_latest(disk, path, &rec, &found);
   if (lookup_rc < 0) return lookup_rc;

   uint32_t uid = found ? rec.uid : 0;
   uint32_t gid = found ? rec.gid : 0;
   return fat_metadata_append_record_full(disk, path, mode, uid, gid, 0);
}

static int fat_chown_path(Partition *disk, const char *path, uint32_t uid,
                          uint32_t gid)
{
   if (!disk || !path) return -EINVAL;

   FAT_MetadataRecord rec;
   bool found = false;
   int lookup_rc = fat_metadata_lookup_latest(disk, path, &rec, &found);
   if (lookup_rc < 0) return lookup_rc;

   uint16_t mode = found ? rec.mode : 0644u;
   return fat_metadata_append_record_full(disk, path, mode, uid, gid, 0);
}

uint32_t FAT_Read(Partition *disk, FAT_File *file, uint32_t byte_count,
                  void *data_out)
{
   FAT_Instance *inst = fat_inst(disk);
   if (!inst) return 0;

   // Validate file handle before accessing array
   if (!file || !data_out) return 0;
   if (file->handle != ROOT_DIRECTORY_HANDLE &&
       (file->handle < 0 || file->handle >= MAX_FILE_HANDLES))
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Read: invalid file handle %d\n",
             file->handle);
      return 0;
   }

   // Get file data
   FAT_FileData *fd = (file->handle == ROOT_DIRECTORY_HANDLE)
                          ? &inst->root_directory
                          : &inst->opened_files[file->handle];

   uint8_t *data_out_u8 = (uint8_t *)data_out;

   // For regular files (not directories), don't read empty files
   if (fd->public_file.size == 0 && !fd->public_file.is_directory)
   {
      logfmt(LOG_WARNING,
             "[FAT] FAT_Read: file is empty (Size=0), returning 0 bytes, "
             "IsDirectory=%u\n",
             fd->public_file.is_directory);
      return 0;
   }

   // don't read past the end of the file (once size is known)
   // For directories, Size becomes > 0 only after hitting the end of the
   // cluster chain.
   if (fd->public_file.size > 0)
   {
      if (fd->public_file.position >= fd->public_file.size) return 0;
      byte_count =
          min(byte_count, fd->public_file.size - fd->public_file.position);
   }

   // For root directory in FAT32, limit reading to a reasonable max size
   if (fd->public_file.handle == ROOT_DIRECTORY_HANDLE && inst->fat_type == 32)
   {
      // Root dir should not exceed a few clusters, limit to prevent infinite
      // reads
      uint32_t max_root_size =
          0x1000000; // 16 MiB max (as set in FAT_Initialize)
      if (fd->public_file.position + byte_count > max_root_size)
      {
         byte_count = min(byte_count, max_root_size - fd->public_file.position);
      }
   }

   uint32_t loop_counter = 0; // reset per read call

   while (byte_count > 0)
   {
      uint32_t left_in_buffer =
          SECTOR_SIZE - (fd->public_file.position % SECTOR_SIZE);
      uint32_t take = min(byte_count, left_in_buffer);

      memcpy(data_out_u8, fd->buffer + fd->public_file.position % SECTOR_SIZE,
             take);
      data_out_u8 += take;
      fd->public_file.position += take;
      byte_count -= take;

      if (left_in_buffer == take ||
          (fd->public_file.position > 0 &&
           fd->public_file.position % SECTOR_SIZE == 0))
      {
         // Prevent infinite loops - safety check (per call)
         if (++loop_counter > 10000)
         {
            logfmt(LOG_ERROR,
                   "[FAT] FAT_Read: infinite loop detected, breaking\n");
            break;
         }
         // Special handling for root directory
         if (fd->public_file.handle == ROOT_DIRECTORY_HANDLE)
         {
            if (inst->fat_type == 32)
            {
               // cluster-based root directory (FAT32)
               if (++fd->current_sector_in_cluster >=
                   inst->bs.boot_sector.sectors_per_cluster)
               {
                  fd->current_sector_in_cluster = 0;
                  uint32_t next =
                      FAT_NextCluster(inst, disk, fd->current_cluster);

                  // Treat 0 as end-of-chain to avoid scanning free space
                  if (next < 2)
                  {
                     fd->public_file.size = fd->public_file.position;
                     break;
                  }

                  fd->current_cluster = next;
               }

               // Check for end-of-chain
               uint32_t eof_marker = 0xFFFFFFF8;
               if (fd->current_cluster >= eof_marker)
               {
                  fd->public_file.size = fd->public_file.position;
                  break;
               }

               if (Partition_ReadSectors(
                       disk,
                       FAT_ClusterToLba(inst, fd->current_cluster) +
                           fd->current_sector_in_cluster,
                       1, fd->buffer) < 0)
               {
                  logfmt(LOG_ERROR, "[FAT] read error!\n");
                  break;
               }
            }
            else
            {
               // legacy root directory stored in reserved area (sector indexed)
               ++fd->current_cluster;

               if (fd->current_cluster >=
                   inst->root_dir_lba + inst->root_dir_sectors)
               {
                  fd->public_file.size = fd->public_file.position;
                  break;
               }

               if (Partition_ReadSectors(disk, fd->current_cluster, 1,
                                         fd->buffer) < 0)
               {
                  logfmt(LOG_ERROR, "[FAT] read error!\n");
                  break;
               }
            }
         }
         else
         {
            // calculate next cluster & sector to read
            if (++fd->current_sector_in_cluster >=
                inst->bs.boot_sector.sectors_per_cluster)
            {
               fd->current_sector_in_cluster = 0;
               uint32_t next = FAT_NextCluster(inst, disk, fd->current_cluster);

               // Treat 0 (free) or invalid as EOF to avoid looping into free
               // space
               if (next < 2)
               {
                  fd->public_file.size = fd->public_file.position;
                  break;
               }

               fd->current_cluster = next;
            }

            // Check for end-of-chain based on FAT type
            uint32_t eof_marker = (inst->fat_type == 12)   ? 0xFF8
                                  : (inst->fat_type == 16) ? 0xFFF8
                                                           : 0x0FFFFFF8;
            if (fd->current_cluster >= eof_marker)
            {
               // Mark end of file
               fd->public_file.size = fd->public_file.position;
               break;
            }

            // read next sector
            if (Partition_ReadSectors(
                    disk,
                    FAT_ClusterToLba(inst, fd->current_cluster) +
                        fd->current_sector_in_cluster,
                    1, fd->buffer) < 0)
            {
               logfmt(LOG_ERROR, "[FAT] read error!\n");
               break;
            }
         }
      }
   }

   return data_out_u8 - (uint8_t *)data_out;
}

int FAT_ReadEntry(Partition *disk, FAT_File *file,
                  FAT_DirectoryEntry *dir_entry)
{
   uint32_t bytes_read =
       FAT_Read(disk, file, sizeof(FAT_DirectoryEntry), dir_entry);
   return (bytes_read == sizeof(FAT_DirectoryEntry)) ? SUCCESS : -ENOENT;
}

void FAT_Close(FAT_File *file)
{
   if (!file) return;

   FAT_Instance *inst = (FAT_Instance *)file->instance;
   if (!inst) return;

   if (file->handle == ROOT_DIRECTORY_HANDLE)
   {
      file->position = 0;
      inst->root_directory.current_cluster = inst->root_directory.first_cluster;
   }
   else
   {
      // Validate handle before accessing array
      if (file->handle < 0 || file->handle >= MAX_FILE_HANDLES)
      {
         logfmt(LOG_ERROR, "[FAT] FAT_Close: invalid file handle %d\n",
                file->handle);
         return;
      }
      inst->opened_files[file->handle].opened = false;
   }
}

int FAT_FindFile(Partition *disk, FAT_File *file, const char *name,
                 FAT_DirectoryEntry *entry_out)
{
   // Reject paths; this helper expects a single 8.3 component
   if (strchr(name, '/'))
   {
      logfmt(
          LOG_WARNING,
          "[FAT] FAT_FindFile: received path '%s', expected single component\n",
          name);
      return -EINVAL;
   }

   char fat_name[12];
   FAT_DirectoryEntry entry;

   // Reset directory position to start searching from the beginning
   if (FAT_Seek(disk, file, 0) < 0)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_FindFile: FAT_Seek(0) failed for '%s'\n",
             name);
      return -EIO;
   }

   // convert from name to fat name
   memset(fat_name, ' ', sizeof(fat_name));
   fat_name[11] = '\0';

   const char *ext = strchr(name, '.');
   if (ext == NULL)
      ext = name + strlen(name); // Point to end of string if no extension

   // Copy basename (max 8 chars before extension)
   int nameLen = (ext - name > 8) ? 8 : (ext - name);

   for (int i = 0; i < nameLen && name[i] && name[i] != '.'; i++)
      fat_name[i] = toupper(name[i]);

   // Copy extension (max 3 chars after the dot)
   if (ext != name + strlen(name) && *ext == '.')
   {
      for (int i = 0; i < 3 && ext[i + 1]; i++)
         fat_name[i + 8] = toupper(ext[i + 1]);
   }

   while (FAT_ReadEntry(disk, file, &entry) == SUCCESS)
   {
      // FAT end marker: empty entry means end of directory
      if (entry.name[0] == 0x00) break;

      // Skip LFN entries (attribute 0x0F)
      if ((entry.attributes & 0x0F) == 0x0F) continue;

      if (memcmp(fat_name, entry.name, 11) == 0)
      {
         *entry_out = entry;
         return SUCCESS;
      }
   }
   return -ENOENT;
}

FAT_File *FAT_Open(Partition *disk, const char *path)
{
   if (!path) return NULL;

   FAT_Instance *inst = fat_inst(disk);
   if (!inst) return NULL;

   char *normalized_path = kmalloc(MAX_PATH_SIZE);
   char *name = kmalloc(MAX_PATH_SIZE);
   if (!normalized_path || !name)
   {
      if (normalized_path) free(normalized_path);
      if (name) free(name);
      return NULL;
   }

   strncpy(normalized_path, path, MAX_PATH_SIZE - 1);
   normalized_path[MAX_PATH_SIZE - 1] = '\0';

   const char *cursor = normalized_path;
   if (*cursor == '/') cursor++;

   // If path is empty or just "/", return root directory
   if (*cursor == '\0')
   {
      // Root directory handle is shared; always rewind on open so callers
      // observe deterministic directory iteration.
      FAT_Seek(disk, &inst->root_directory.public_file, 0);
      free(normalized_path);
      free(name);
      return &inst->root_directory.public_file;
   }

   FAT_File *current = &inst->root_directory.public_file;
   FAT_File *previous = NULL;

   while (*cursor)
   {
      bool isLast = false;
      const char *delim = strchr(cursor, '/');
      if (delim != NULL)
      {
         size_t len = (size_t)(delim - cursor);
         if (len >= MAX_PATH_SIZE) len = MAX_PATH_SIZE - 1;
         memcpy(name, cursor, len);
         name[len] = '\0';
         cursor = delim + 1;
      }
      else
      {
         size_t len = strlen(cursor);
         if (len >= MAX_PATH_SIZE) len = MAX_PATH_SIZE - 1;
         memcpy(name, cursor, len);
         name[len] = '\0';
         cursor += len;
         isLast = true;
      }

      FAT_DirectoryEntry entry;
      if (FAT_FindFile(disk, current, name, &entry) == SUCCESS)
      {
         if (previous != NULL && previous->handle != ROOT_DIRECTORY_HANDLE)
         {
            FAT_Close(previous);
         }

         if (!isLast && (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) == 0)
         {
            logfmt(LOG_ERROR, "[FAT] %s not a directory\n", name);
            if (current != NULL && current->handle != ROOT_DIRECTORY_HANDLE)
               FAT_Close(current);
            free(normalized_path);
            free(name);
            return NULL;
         }

         FAT_FileData *parentData = (current->handle == ROOT_DIRECTORY_HANDLE)
                                        ? &inst->root_directory
                                        : &inst->opened_files[current->handle];

         previous = current;
         current = FAT_OpenEntry(inst, disk, &entry, parentData);
      }
      else
      {
         if (isLast)
         {
            /* File not found.  FAT_Open is strictly read/navigate-only.
             * Callers that need to create a new file must use FAT_Create
             * (or VFS_Create at the VFS layer) explicitly. */
            if (current != NULL && current->handle != ROOT_DIRECTORY_HANDLE)
            {
               FAT_Close(current);
            }
            if (previous != NULL && previous->handle != ROOT_DIRECTORY_HANDLE)
            {
               FAT_Close(previous);
            }

            logfmt(LOG_INFO, "[FAT] %s not found\n", name);
            free(normalized_path);
            free(name);
            return NULL;
         }
         else
         {
            if (previous != NULL && previous->handle != ROOT_DIRECTORY_HANDLE)
            {
               FAT_Close(previous);
            }
            if (current != NULL && current->handle != ROOT_DIRECTORY_HANDLE)
            {
               FAT_Close(current);
            }

            logfmt(LOG_WARNING, "[FAT] %s not found\n", name);
            free(normalized_path);
            free(name);
            return NULL;
         }
      }
   }

   if (previous != NULL && previous != current &&
       previous->handle != ROOT_DIRECTORY_HANDLE)
   {
      FAT_Close(previous);
   }

   free(normalized_path);
   free(name);
   return current;
}

int FAT_Seek(Partition *disk, FAT_File *file, uint32_t position)
{
   if (!disk)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Seek: disk is NULL\n");
      return FAT_EDISK;
   }

   FAT_Instance *inst = fat_inst(disk);
   if (!inst) return FAT_EDISK;

   if (!file) return -EINVAL;

   // Validate handle before accessing array
   if (file->handle != ROOT_DIRECTORY_HANDLE &&
       (file->handle < 0 || file->handle >= MAX_FILE_HANDLES))
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Seek: invalid file handle %d\n",
             file->handle);
      return -EINVAL;
   }

   FAT_FileData *fd = (file->handle == ROOT_DIRECTORY_HANDLE)
                          ? &inst->root_directory
                          : &inst->opened_files[file->handle];

   // don't seek past end (but allow seeks in directories since they don't
   // track size)
   if (!fd->public_file.is_directory && position > fd->public_file.size)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Seek: position %u > size %u\n", position,
             fd->public_file.size);
      return -EINVAL;
   }

   fd->public_file.position = position;

   // compute cluster/sector for the position
   uint32_t bytesPerSector = inst->bs.boot_sector.bytes_per_sector;
   uint32_t sectorsPerCluster = inst->bs.boot_sector.sectors_per_cluster;

   // Guard against divide-by-zero from invalid FAT parameters
   if (bytesPerSector == 0 || sectorsPerCluster == 0)
   {
      logfmt(LOG_ERROR,
             "[FAT] FAT_Seek: invalid FAT parameters (bytes_per_sector=%u, "
             "sectors_per_cluster=%u)\n",
             bytesPerSector, sectorsPerCluster);
      return FAT_ESTATE;
   }

   uint32_t clusterBytes = bytesPerSector * sectorsPerCluster;

   if (fd->public_file.handle == ROOT_DIRECTORY_HANDLE)
   {
      if (inst->fat_type == 32)
      {
         uint32_t clusterIndex = position / clusterBytes;
         uint32_t sectorInCluster = (position % clusterBytes) / bytesPerSector;

         uint32_t cluster = fd->first_cluster;
         for (uint32_t i = 0; i < clusterIndex; i++)
         {
            cluster = FAT_NextCluster(inst, disk, cluster);
            uint32_t eof_marker = 0xFFFFFFF8;
            if (cluster >= eof_marker)
            {
               fd->public_file.size = fd->public_file.position;
               return -EIO;
            }
         }

         fd->current_cluster = cluster;
         fd->current_sector_in_cluster = sectorInCluster;

         if (Partition_ReadSectors(disk,
                                   FAT_ClusterToLba(inst, fd->current_cluster) +
                                       fd->current_sector_in_cluster,
                                   1, fd->buffer) < 0)
         {
            logfmt(LOG_ERROR, "[FAT] seek read error (root)\n");
            return -EIO;
         }
      }
      else
      {
         // root directory is organized by sectors (not clusters)
         uint32_t sectorIndex = position / bytesPerSector;
         uint32_t newCluster = fd->first_cluster + sectorIndex;

         /* Only re-read from disk if moving to a different sector.
          * Seeking back to position 0 when the buffer already holds the
          * first root-dir sector (loaded by FAT_Initialize) avoids a
          * redundant FDC read that could fail if the motor was off. */
         bool needs_read = (fd->current_cluster != newCluster);
         fd->current_cluster = newCluster;
         fd->current_sector_in_cluster = 0;

         if (needs_read && Partition_ReadSectors(disk, fd->current_cluster, 1,
                                                 fd->buffer) < 0)
         {
            logfmt(LOG_ERROR, "[FAT] seek read error (root)\n");
            return -EIO;
         }
      }
   }
   else
   {
      // Empty regular files only support seek to BOF.
      // This allows first read/write at offset 0 without forcing a failure.
      if (fd->public_file.size == 0 && !fd->public_file.is_directory)
      {
         if (position == 0) return SUCCESS;

         logfmt(LOG_ERROR,
                "[FAT] FAT_Seek: cannot seek to non-zero offset on empty "
                "regular file\n");
         return -EINVAL;
      }

      if (fd->first_cluster == 0)
      {
         logfmt(LOG_ERROR,
                "[FAT] FAT_Seek: first_cluster is 0 for non-empty file "
                "(size=%u)\n",
                fd->public_file.size);
         return FAT_ESTATE;
      }

      uint32_t clusterIndex = position / clusterBytes;
      uint32_t sectorInCluster = (position % clusterBytes) / bytesPerSector;

      // walk cluster chain clusterIndex times from first cluster
      uint32_t c = fd->first_cluster;
      for (uint32_t i = 0; i < clusterIndex; i++)
      {
         c = FAT_NextCluster(inst, disk, c);
         uint32_t eof_marker = (inst->fat_type == 12)   ? 0xFF8
                               : (inst->fat_type == 16) ? 0xFFF8
                                                        : 0x0FFFFFF8;
         if (c >= eof_marker)
         {
            // invalid / end of chain
            fd->public_file.size = fd->public_file.position;
            logfmt(LOG_WARNING,
                   "[FAT] FAT_Seek: reached end of cluster chain\n");
            return -EIO;
         }
      }

      fd->current_cluster = c;
      fd->current_sector_in_cluster = sectorInCluster;

      if (Partition_ReadSectors(disk,
                                FAT_ClusterToLba(inst, fd->current_cluster) +
                                    fd->current_sector_in_cluster,
                                1, fd->buffer) < 0)
      {
         logfmt(LOG_ERROR, "[FAT] seek read error (file)\n");
         return -EIO;
      }
   }

   return SUCCESS;
}

int FAT_WriteEntry(Partition *disk, FAT_File *file,
                   const FAT_DirectoryEntry *dir_entry)
{
   // Allow writing into root directory as well as opened directory files.
   if (!file) return -EINVAL;

   FAT_Instance *inst = fat_inst(disk);
   if (!inst) return FAT_EDISK;

   FAT_FileData *fd;
   bool isRoot = (file->handle == ROOT_DIRECTORY_HANDLE);
   if (isRoot)
      fd = &inst->root_directory;
   else
   {
      if (file->handle < 0 || file->handle >= MAX_FILE_HANDLES) return -EINVAL;
      fd = &inst->opened_files[file->handle];
   }

   if (!file->is_directory)
   {
      logfmt(LOG_ERROR, "[FAT] WriteEntry called on non-directory file\n");
      return -EINVAL;
   }

   // Calculate which sector and offset contains the current directory entry
   uint32_t entryOffset = file->position;
   uint32_t sectorIndex = entryOffset / SECTOR_SIZE;
   uint32_t offsetInSector = entryOffset % SECTOR_SIZE;

   // Determine absolute LBA for this sector
   uint32_t sectorLba = 0;
   if (!isRoot || inst->fat_type == 32)
   {
      sectorLba = FAT_ClusterToLba(inst, fd->current_cluster) +
                  fd->current_sector_in_cluster;
   }
   else
   {
      // legacy root - contiguous area
      sectorLba = inst->root_dir_lba + sectorIndex;
   }

   // Read the sector to modify it
   uint8_t *sectorBuffer = kmalloc(SECTOR_SIZE);
   if (!sectorBuffer)
   {
      logfmt(LOG_ERROR, "[FAT] WriteEntry kmalloc failed\n");
      return -EIO;
   }
   if (Partition_ReadSectors(disk, sectorLba, 1, sectorBuffer) < 0)
   {
      logfmt(LOG_ERROR, "[FAT] WriteEntry read error\n");
      free(sectorBuffer);
      return -EIO;
   }

   memcpy(&sectorBuffer[offsetInSector], dir_entry, sizeof(FAT_DirectoryEntry));

   if (Partition_WriteSectors(disk, sectorLba, 1, sectorBuffer) < 0)
   {
      logfmt(LOG_ERROR, "[FAT] WriteEntry write error\n");
      free(sectorBuffer);
      return -EIO;
   }

   // Update the file descriptor's buffer with the modified sector
   // so that subsequent reads see the updated entry
   memcpy(fd->buffer, sectorBuffer, SECTOR_SIZE);
   free(sectorBuffer);

   // Advance position by one directory entry (bytes)
   file->position += sizeof(FAT_DirectoryEntry);
   return SUCCESS;
}

uint32_t FAT_Write(Partition *disk, FAT_File *file, uint32_t byte_count,
                   const void *data_in)
{
   // Don't write to directories or root
   if (!file || file->is_directory || file->handle == ROOT_DIRECTORY_HANDLE)
   {
      logfmt(LOG_ERROR,
             "[FAT] FAT_Write: cannot write to directory or null file\n");
      return 0;
   }

   // Validate file handle BEFORE accessing array
   if (file->handle < 0 || file->handle >= MAX_FILE_HANDLES)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Write: invalid file handle %d\n",
             file->handle);
      return 0;
   }

   FAT_Instance *inst = fat_inst(disk);
   if (!inst) return 0;

   // Get file data
   FAT_FileData *fd = &inst->opened_files[file->handle];

   if (!fd->opened)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Write: file not opened\n");
      return 0;
   }

   // Validate FAT parameters
   if (inst->bs.boot_sector.bytes_per_sector == 0 ||
       inst->bs.boot_sector.sectors_per_cluster == 0)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Write: invalid BPB parameters\n");
      return 0;
   }

   // Auto-truncate file on first write if it has existing content and hasn't
   // been truncated
   if (!fd->truncated && fd->public_file.size > 0 &&
       fd->public_file.position == 0)
   {
      logfmt(LOG_INFO,
             "[FAT] FAT_Write: auto-truncating file (Size=%u) before first "
             "write\n",
             fd->public_file.size);
      if (FAT_Truncate(disk, file) < 0)
      {
         logfmt(LOG_ERROR, "[FAT] FAT_Write: auto-truncate failed\n");
         return 0;
      }
      fd->truncated = true;
   }

   // If writing to a newly created empty file, clear the buffer to avoid stale
   // data
   if (fd->public_file.size == 0 && fd->public_file.position == 0 &&
       !fd->truncated)
   {
      memset(fd->buffer, 0, SECTOR_SIZE);
      fd->truncated = true; // Mark as truncated to avoid re-clearing
   }

   const uint8_t *u8DataIn = (const uint8_t *)data_in;
   uint32_t bytesWritten = 0;

   while (byte_count > 0)
   {
      // Calculate position within current sector
      uint32_t offsetInSector = fd->public_file.position % SECTOR_SIZE;
      uint32_t spaceInSector = SECTOR_SIZE - offsetInSector;
      uint32_t take = min(byte_count, spaceInSector);

      // Hard guard against buffer overflow: offset must stay within sector
      if (offsetInSector >= SECTOR_SIZE || take > SECTOR_SIZE ||
          offsetInSector + take > SECTOR_SIZE)
      {
         logfmt(LOG_ERROR,
                "[FAT] FAT_Write: offset overflow (pos=%u off=%u take=%u)\n",
                fd->public_file.position, offsetInSector, take);
         return bytesWritten;
      }

      // Copy data to buffer
      memcpy(fd->buffer + offsetInSector, u8DataIn, take);

      // Update position and counters
      u8DataIn += take;
      fd->public_file.position += take;
      bytesWritten += take;
      byte_count -= take;

      // Update file size if we wrote past the current end
      if (fd->public_file.position > fd->public_file.size)
         fd->public_file.size = fd->public_file.position;

      // Write sector back to disk if we've filled it or reached end of request
      if (offsetInSector + take == SECTOR_SIZE || byte_count == 0)
      {
         uint32_t sectorLba = FAT_ClusterToLba(inst, fd->current_cluster) +
                              fd->current_sector_in_cluster;

         if (Partition_WriteSectors(disk, sectorLba, 1, fd->buffer) < 0)
         {
            logfmt(LOG_ERROR, "[FAT] FAT_Write: sector write error at LBA %u\n",
                   sectorLba);
            return bytesWritten;
         }

         if (byte_count == 0) break;

         bool needAdvance = (offsetInSector + take == SECTOR_SIZE);

         if (byte_count == 0 && !needAdvance) break;

         // Advance to next sector/cluster if we filled the sector
         if (needAdvance && ++fd->current_sector_in_cluster >=
                                inst->bs.boot_sector.sectors_per_cluster)
         {
            fd->current_sector_in_cluster = 0;

            // Need next cluster
            uint32_t next_cluster =
                FAT_NextCluster(inst, disk, fd->current_cluster);
            uint32_t eof_marker = (inst->fat_type == 12)   ? 0xFF8
                                  : (inst->fat_type == 16) ? 0xFFF8
                                                           : 0x0FFFFFF8;

            if (next_cluster >= eof_marker)
            {
               // Allocate new cluster
               uint32_t newCluster = 0;
               uint32_t max_clusters =
                   (inst->total_sectors - inst->data_section_lba) /
                   inst->bs.boot_sector.sectors_per_cluster;

               for (uint32_t testCluster = 2; testCluster < max_clusters;
                    testCluster++)
               {
                  if (FAT_NextCluster(inst, disk, testCluster) == 0)
                  {
                     newCluster = testCluster;
                     break;
                  }
               }

               if (newCluster == 0)
               {
                  logfmt(LOG_ERROR,
                         "[FAT] FAT_Write: no free clusters available\n");
                  return bytesWritten;
               }

               uint32_t eofVal = (inst->fat_type == 12)   ? 0x0FFF
                                 : (inst->fat_type == 16) ? 0xFFFF
                                                          : 0x0FFFFFFF;

               if (FAT_WriteFatEntry(inst, disk, fd->current_cluster,
                                     newCluster) < 0 ||
                   FAT_WriteFatEntry(inst, disk, newCluster, eofVal) < 0)
               {
                  logfmt(LOG_ERROR, "[FAT] FAT_Write: FAT write error "
                                    "linking/marking cluster\n");
                  return bytesWritten;
               }

               // Verify the EOF was actually written
               uint32_t verify = FAT_NextCluster(inst, disk, newCluster);
               if (verify != eofVal)
               {
                  logfmt(LOG_ERROR,
                         "[FAT] FAT_Write: ERROR: linked %u->%u, marked %u as "
                         "EOF (0x%08x), but verify=0x%08x\n",
                         fd->current_cluster, newCluster, newCluster, eofVal,
                         verify);
               }

               fd->current_cluster = newCluster;
               if (Partition_ReadSectors(disk,
                                         FAT_ClusterToLba(inst, newCluster), 1,
                                         fd->buffer) < 0)
               {
                  logfmt(LOG_ERROR,
                         "[FAT] FAT_Write: failed to read new cluster\n");
                  return bytesWritten;
               }
            }
            else
            {
               fd->current_cluster = next_cluster;
               if (Partition_ReadSectors(
                       disk, FAT_ClusterToLba(inst, fd->current_cluster), 1,
                       fd->buffer) < 0)
               {
                  logfmt(LOG_ERROR,
                         "[FAT] FAT_Write: failed to read next cluster\n");
                  return bytesWritten;
               }
            }
         }
         else
         {
            if (Partition_ReadSectors(
                    disk,
                    FAT_ClusterToLba(inst, fd->current_cluster) +
                        fd->current_sector_in_cluster,
                    1, fd->buffer) < 0)
            {
               logfmt(LOG_ERROR,
                      "[FAT] FAT_Write: failed to read next sector\n");
               return bytesWritten;
            }
         }

         if (byte_count == 0) break;
      }
   }

   // Verify the cluster chain integrity
   uint32_t chainLength = 0;
   uint32_t testCluster = fd->first_cluster;
   uint32_t eof_marker = (inst->fat_type == 12)   ? 0xFF8
                         : (inst->fat_type == 16) ? 0xFFF8
                                                  : 0x0FFFFFF8;

   while (testCluster < eof_marker && chainLength < 100)
   {
      uint32_t next = FAT_NextCluster(inst, disk, testCluster);
      if (next >= eof_marker)
      {
         chainLength++;
         break;
      }
      testCluster = next;
      chainLength++;
      if (next < 2)
      {
         logfmt(
             LOG_ERROR,
             "[FAT] FAT_Write: ERROR - chain broken at cluster %u (next=%u)\n",
             testCluster, next);
         break;
      }
   }

   if (FAT_UpdateEntry(disk, file) < 0)
   {
      char namebuf[12];
      memcpy(namebuf, fd->public_file.name, 11);
      namebuf[11] = '\0';
      logfmt(LOG_ERROR,
             "[FAT] FAT_Write: failed to update directory entry for '%s'\n",
             namebuf);
   }

   return bytesWritten;
}

int FAT_UpdateEntry(Partition *disk, FAT_File *file)
{
   // Update the directory entry in the *parent* directory of this file.
   if (!file) return -EINVAL;

   FAT_Instance *inst = fat_inst(disk);
   if (!inst) return FAT_EDISK;

   FAT_FileData *fd = (file->handle == ROOT_DIRECTORY_HANDLE)
                          ? &inst->root_directory
                          : &inst->opened_files[file->handle];

   if (file->handle != ROOT_DIRECTORY_HANDLE)
   {
      if (file->handle < 0 || file->handle >= MAX_FILE_HANDLES) return -EINVAL;
      if (!fd->opened) return FAT_ESTATE;
      inst->root_directory.current_sector_in_cluster = 0;
   }

   // Determine where the parent directory starts
   bool parentIsRoot = fd->parent_is_root;
   uint32_t parentCluster = fd->parent_cluster;

   // Guard against bogus parent cluster values (e.g., EOF markers)
   uint32_t eof_marker = (inst->fat_type == 12)   ? 0xFF8
                         : (inst->fat_type == 16) ? 0xFFF8
                                                  : 0x0FFFFFF8;
   if (parentCluster >= eof_marker)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_UpdateEntry: invalid parent cluster %u\n",
             parentCluster);
      return FAT_ESTATE;
   }

   // Safety caps to avoid runaway loops
   const uint32_t maxSectorsToScan = 4096;
   uint32_t sectorsScanned = 0;

   // Iterate over the parent directory sectors
   if (parentIsRoot && inst->fat_type != 32)
   {
      // Legacy FAT12/16 fixed root directory
      for (uint32_t s = 0;
           s < inst->root_dir_sectors && sectorsScanned < maxSectorsToScan;
           s++, sectorsScanned++)
      {
         uint8_t *sectorBuffer = kmalloc(SECTOR_SIZE);
         if (!sectorBuffer) return -EIO;
         uint32_t lba = inst->root_dir_lba + s;
         if (Partition_ReadSectors(disk, lba, 1, sectorBuffer) < 0)
         {
            logfmt(LOG_ERROR, "[FAT] FAT_UpdateEntry: failed to read root "
                              "directory sector\n");
            free(sectorBuffer);
            return -EIO;
         }

         for (uint32_t i = 0; i < SECTOR_SIZE; i += sizeof(FAT_DirectoryEntry))
         {
            FAT_DirectoryEntry *entry =
                (FAT_DirectoryEntry *)(sectorBuffer + i);
            if ((entry->attributes & 0x0F) == 0x0F || entry->name[0] == 0x00)
               continue;
            if (memcmp(entry->name, fd->public_file.name, 11) == 0)
            {
               FAT_DirectoryEntry updated = *entry;
               updated.size = fd->public_file.size;
               updated.first_cluster_low = fd->first_cluster & 0xFFFF;
               updated.first_cluster_high = (fd->first_cluster >> 16) & 0xFFFF;
               memcpy(sectorBuffer + i, &updated, sizeof(FAT_DirectoryEntry));
               int result = Partition_WriteSectors(disk, lba, 1, sectorBuffer);
               free(sectorBuffer);
               return (result < 0) ? -EIO : SUCCESS;
            }
         }
         free(sectorBuffer);
      }
   }
   else
   {
      // Cluster-based directory (FAT32 root or any subdirectory)
      uint32_t cluster = parentCluster;

      while (cluster < eof_marker && sectorsScanned < maxSectorsToScan)
      {
         for (uint32_t sec = 0;
              sec < inst->bs.boot_sector.sectors_per_cluster &&
              sectorsScanned < maxSectorsToScan;
              sec++, sectorsScanned++)
         {
            uint8_t *sectorBuffer = kmalloc(SECTOR_SIZE);
            if (!sectorBuffer) return -EIO;
            uint32_t lba = FAT_ClusterToLba(inst, cluster) + sec;
            if (Partition_ReadSectors(disk, lba, 1, sectorBuffer) < 0)
            {
               logfmt(LOG_ERROR, "[FAT] FAT_UpdateEntry: failed to read "
                                 "directory cluster sector\n");
               free(sectorBuffer);
               return -EIO;
            }

            for (uint32_t i = 0; i < SECTOR_SIZE;
                 i += sizeof(FAT_DirectoryEntry))
            {
               FAT_DirectoryEntry *entry =
                   (FAT_DirectoryEntry *)(sectorBuffer + i);
               if ((entry->attributes & 0x0F) == 0x0F || entry->name[0] == 0x00)
                  continue;
               if (memcmp(entry->name, fd->public_file.name, 11) == 0)
               {
                  FAT_DirectoryEntry updated = *entry;
                  updated.size = fd->public_file.size;
                  updated.first_cluster_low = fd->first_cluster & 0xFFFF;
                  updated.first_cluster_high =
                      (fd->first_cluster >> 16) & 0xFFFF;
                  memcpy(sectorBuffer + i, &updated,
                         sizeof(FAT_DirectoryEntry));
                  int result =
                      Partition_WriteSectors(disk, lba, 1, sectorBuffer);
                  free(sectorBuffer);
                  return (result < 0) ? -EIO : SUCCESS;
               }
            }
            free(sectorBuffer);
         }

         cluster = FAT_NextCluster(inst, disk, cluster);
      }
   }

   logfmt(LOG_WARNING,
          "[FAT] UpdateEntry - file not found in parent directory\n");
   return -ENOENT;
}

FAT_File *FAT_Create(Partition *disk, const char *path, uint16_t mode)
{
   if (!disk)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Create: disk is NULL!\n");
      return NULL;
   }

   if (!path) return NULL;

   FAT_Instance *inst = fat_inst(disk);
   if (!inst) return NULL;

   char metadataPath[MAX_PATH_SIZE];
   fat_normalize_absolute_path(path, metadataPath, sizeof(metadataPath));

   // Normalize leading slash
   if (path[0] == '/') path++;

   // Split path into parent + basename (allocate on heap to avoid stack blow)
   char *parentPath = kmalloc(MAX_PATH_SIZE);
   char *baseName = kmalloc(MAX_PATH_SIZE);
   if (!parentPath || !baseName)
   {
      if (parentPath) free(parentPath);
      if (baseName) free(baseName);
      return NULL;
   }

   const char *lastSlash = strrchr(path, '/');
   if (lastSlash)
   {
      size_t parentLen = lastSlash - path;
      if (parentLen >= MAX_PATH_SIZE) parentLen = MAX_PATH_SIZE - 1;
      memcpy(parentPath, path, parentLen);
      parentPath[parentLen] = '\0';
      strncpy(baseName, lastSlash + 1, MAX_PATH_SIZE - 1);
      baseName[MAX_PATH_SIZE - 1] = '\0';
   }
   else
   {
      parentPath[0] = '\0';
      strncpy(baseName, path, MAX_PATH_SIZE - 1);
      baseName[MAX_PATH_SIZE - 1] = '\0';
   }

   if (baseName[0] == '\0')
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Create: empty basename\n");
      free(parentPath);
      free(baseName);
      return NULL;
   }

   // Open parent directory
   FAT_File *parentFile = (parentPath[0] == '\0')
                              ? &inst->root_directory.public_file
                              : FAT_Open(disk, parentPath);
   if (!parentFile || !parentFile->is_directory)
   {
      free(parentPath);
      free(baseName);
      return NULL;
   }

   // Convert basename to FAT 8.3
   const char *name = baseName;
   char fat_name[12];
   memset(fat_name, ' ', sizeof(fat_name));
   fat_name[11] = '\0';

   const char *ext = strchr(name, '.');
   if (ext == NULL)
      ext = name + strlen(name); // Point to end of string if no extension

   int nameLen = (ext - name > 8) ? 8 : (ext - name);
   for (int i = 0; i < nameLen && name[i] && name[i] != '.'; i++)
      fat_name[i] = toupper(name[i]);

   if (ext != name + strlen(name) && *ext == '.')
   {
      for (int i = 0; i < 3 && ext[i + 1]; i++)
         fat_name[i + 8] = toupper(ext[i + 1]);
   }

   // Check if file already exists in parent
   FAT_DirectoryEntry existingEntry;

   if (FAT_FindFile(disk, parentFile, baseName, &existingEntry) == SUCCESS)
   {
      free(parentPath);
      free(baseName);
      return NULL;
   }

   // Find first free cluster for the file
   uint32_t firstFreeCluster = 0;
   uint32_t max_clusters = (inst->total_sectors - inst->data_section_lba) /
                           inst->bs.boot_sector.sectors_per_cluster;

   // Search all clusters (was previously limited to 1000 and could miss free
   // space on larger images)
   uint32_t maxSearchClusters = max_clusters;

   for (uint32_t testCluster = 2; testCluster < maxSearchClusters;
        testCluster++)
   {
      uint32_t nextClusterVal = FAT_NextCluster(inst, disk, testCluster);
      if (nextClusterVal == 0)
      {
         firstFreeCluster = testCluster;
         break;
      }
   }

   if (firstFreeCluster == 0)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Create: no free clusters available\n");
      free(parentPath);
      free(baseName);
      return NULL;
   }

   // Mark cluster as end-of-chain in all FATs
   uint32_t eofVal = (inst->fat_type == 12)   ? 0x0FFF
                     : (inst->fat_type == 16) ? 0xFFFF
                                              : 0x0FFFFFFF;
   if (FAT_WriteFatEntry(inst, disk, firstFreeCluster, eofVal) < 0)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Create: FAT write error\n");
      free(parentPath);
      free(baseName);
      return NULL;
   }

   // Create directory entry
   FAT_DirectoryEntry newEntry;
   memcpy(newEntry.name, fat_name, 11);
   newEntry.attributes = 0x20; // Archive attribute
   newEntry._reserved = 0;
   newEntry.created_time_tenths = 0;
   newEntry.created_time = 0;
   newEntry.created_date = 0;
   newEntry.accessed_date = 0;
   newEntry.first_cluster_high = (firstFreeCluster >> 16) & 0xFFFF;
   newEntry.modified_time = 0;
   newEntry.modified_date = 0;
   newEntry.first_cluster_low = firstFreeCluster & 0xFFFF;
   newEntry.size = 0; // Start with empty file

   // Find empty slot in parent directory
   FAT_Seek(disk, parentFile, 0);

   FAT_DirectoryEntry dir_entry;
   uint32_t entryPos = 0;
   int entryCount = 0;
   // For FAT32 DirEntryCount is 0; allow scanning until EOF with safety cap.
   int maxEntries = (inst->bs.boot_sector.dir_entry_count > 0)
                        ? inst->bs.boot_sector.dir_entry_count
                        : 65536;

   while (FAT_ReadEntry(disk, parentFile, &dir_entry) == SUCCESS &&
          entryCount < maxEntries)
   {
      entryCount++;
      entryPos = parentFile->position - sizeof(FAT_DirectoryEntry);
      // Found empty slot (first byte is 0x00) or deleted entry (0xE5)
      if (dir_entry.name[0] == 0x00 || (uint8_t)dir_entry.name[0] == 0xE5)
      {
         // Go back to this position
         FAT_Seek(disk, parentFile, entryPos);

         // Write the new entry
         if (FAT_WriteEntry(disk, parentFile, &newEntry) < 0)
         {
            logfmt(LOG_ERROR,
                   "[FAT] FAT_Create: failed to write directory entry\n");
            free(parentPath);
            free(baseName);
            return NULL;
         }

         // Open the file (with parent context)
         FAT_FileData *parentData =
             (parentFile->handle == ROOT_DIRECTORY_HANDLE)
                 ? &inst->root_directory
                 : &inst->opened_files[parentFile->handle];
         FAT_File *file = FAT_OpenEntry(inst, disk, &newEntry, parentData);

         // Close parent directory if it's not the root
         if (parentFile->handle != ROOT_DIRECTORY_HANDLE)
         {
            FAT_Close(parentFile);
         }

         if (file != NULL)
         {
            if (fat_metadata_append_record(disk, metadataPath, mode, 0) < 0)
            {
               logfmt(LOG_WARNING,
                      "[FAT] FAT_Create: failed to append metadata for '%s'\n",
                      metadataPath);
            }
         }
         free(parentPath);
         free(baseName);
         return file;
      }
   }

   logfmt(LOG_ERROR,
          "[FAT] FAT_Create: no space in root directory (checked %u entries)\n",
          entryCount);
   free(parentPath);
   free(baseName);
   return NULL;
}

int FAT_Delete(Partition *disk, const char *name)
{
   if (!name) return -EINVAL;

   char metadataPath[MAX_PATH_SIZE];
   fat_normalize_absolute_path(name, metadataPath, sizeof(metadataPath));

   FAT_Instance *inst = fat_inst(disk);
   if (!inst) return FAT_EDISK;

   // Normalize path and split into parent + basename
   if (name[0] == '/') name++;

   char *parentPath = kmalloc(MAX_PATH_SIZE);
   char *baseName = kmalloc(MAX_PATH_SIZE);
   if (!parentPath || !baseName)
   {
      if (parentPath) free(parentPath);
      if (baseName) free(baseName);
      return -EIO;
   }

   const char *lastSlash = strrchr(name, '/');
   if (lastSlash)
   {
      size_t parentLen = lastSlash - name;
      if (parentLen >= MAX_PATH_SIZE) parentLen = MAX_PATH_SIZE - 1;
      memcpy(parentPath, name, parentLen);
      parentPath[parentLen] = '\0';
      strncpy(baseName, lastSlash + 1, MAX_PATH_SIZE - 1);
      baseName[MAX_PATH_SIZE - 1] = '\0';
   }
   else
   {
      parentPath[0] = '\0';
      strncpy(baseName, name, MAX_PATH_SIZE - 1);
      baseName[MAX_PATH_SIZE - 1] = '\0';
   }

   if (baseName[0] == '\0')
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Delete: empty basename in path\n");
      free(parentPath);
      free(baseName);
      return -EINVAL;
   }

   FAT_File *parentDir = (parentPath[0] == '\0')
                             ? &inst->root_directory.public_file
                             : FAT_Open(disk, parentPath);
   if (!parentDir || !parentDir->is_directory)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Delete: parent directory '%s' not found\n",
             parentPath);
      free(parentPath);
      free(baseName);
      return -ENOENT;
   }

   FAT_DirectoryEntry entry;
   if (FAT_FindFile(disk, parentDir, baseName, &entry) < 0)
   {
      logfmt(LOG_WARNING, "[FAT] FAT_Delete: file '%s' not found in '%s'\n",
             baseName, parentPath[0] ? parentPath : "/");
      free(parentPath);
      free(baseName);
      return -ENOENT;
   }

   uint32_t firstCluster =
       entry.first_cluster_low + ((uint32_t)entry.first_cluster_high << 16);

   // If it's a directory, delete its contents best-effort
   if (entry.attributes & FAT_ATTRIBUTE_DIRECTORY)
   {
      FAT_FileData *parentData = (parentDir->handle == ROOT_DIRECTORY_HANDLE)
                                     ? &inst->root_directory
                                     : &inst->opened_files[parentDir->handle];

      FAT_File *dir = FAT_OpenEntry(inst, disk, &entry, parentData);
      if (dir)
      {
         FAT_DirectoryEntry subEntry;
         while (FAT_ReadEntry(disk, dir, &subEntry) == SUCCESS)
         {
            if ((subEntry.attributes & 0x0F) == 0x0F ||
                subEntry.name[0] == 0x00 || (uint8_t)subEntry.name[0] == 0xE5)
               continue;

            if ((subEntry.name[0] == '.' && subEntry.name[1] == ' ') ||
                (subEntry.name[0] == '.' && subEntry.name[1] == '.' &&
                 subEntry.name[2] == ' '))
               continue;

            char tempName[12];
            memcpy(tempName, subEntry.name, 11);
            tempName[11] = '\0';
            FAT_Delete(disk, tempName);
         }
         FAT_Close(dir);
      }
   }

   // Free all clusters in the chain
   uint32_t current_cluster = firstCluster;
   uint32_t eof_marker = (inst->fat_type == 12)   ? 0xFF8
                         : (inst->fat_type == 16) ? 0xFFF8
                                                  : 0x0FFFFFF8;
   const uint32_t largeClusterThreshold = 0x0FFFFF00;

   if (inst->bs.boot_sector.sectors_per_cluster == 0 ||
       inst->bs.boot_sector.bytes_per_sector == 0)
   {
      logfmt(
          LOG_ERROR,
          "[FAT] FAT_Delete: invalid FAT parameters, skipping cluster free\n");
      current_cluster = 0;
   }

   int clusterCount = 0;
   if (current_cluster >= 2 && current_cluster < eof_marker &&
       current_cluster < largeClusterThreshold)
   {
      while (current_cluster < eof_marker &&
             current_cluster < largeClusterThreshold && clusterCount < 10000)
      {
         clusterCount++;

         // Zero out the cluster data
         uint32_t sectorsPerCluster = inst->bs.boot_sector.sectors_per_cluster;
         uint32_t clusterLba = FAT_ClusterToLba(inst, current_cluster);
         uint8_t zeroBuffer[SECTOR_SIZE];
         memset(zeroBuffer, 0, SECTOR_SIZE);

         for (uint32_t s = 0; s < sectorsPerCluster; s++)
         {
            Partition_WriteSectors(disk, clusterLba + s, 1, zeroBuffer);
         }

         uint32_t next_cluster = FAT_NextCluster(inst, disk, current_cluster);
         if (FAT_WriteFatEntry(inst, disk, current_cluster, 0) < 0)
         {
            logfmt(LOG_ERROR,
                   "[FAT] FAT_Delete: FAT write error freeing cluster %u\n",
                   current_cluster);
            break;
         }

         current_cluster = next_cluster;
      }
   }

   // Mark directory entry as deleted within the parent directory
   FAT_FileData *parentData = (parentDir->handle == ROOT_DIRECTORY_HANDLE)
                                  ? &inst->root_directory
                                  : &inst->opened_files[parentDir->handle];

   uint32_t sectorsPerCluster = inst->bs.boot_sector.sectors_per_cluster;
   if (parentData == &inst->root_directory && inst->fat_type != 32)
   {
      for (uint32_t s = 0; s < inst->root_dir_sectors; s++)
      {
         uint8_t *sectorBuffer = kmalloc(SECTOR_SIZE);
         if (!sectorBuffer) continue;
         uint32_t lba = inst->root_dir_lba + s;
         if (Partition_ReadSectors(disk, lba, 1, sectorBuffer) < 0)
         {
            free(sectorBuffer);
            continue;
         }
         for (uint32_t off = 0; off < SECTOR_SIZE;
              off += sizeof(FAT_DirectoryEntry))
         {
            FAT_DirectoryEntry *e = (FAT_DirectoryEntry *)(sectorBuffer + off);
            if ((e->attributes & 0x0F) == 0x0F) continue;
            if (e->name[0] == 0x00)
            {
               free(sectorBuffer);
               break;
            }
            if (memcmp(e->name, entry.name, 11) == 0)
            {
               sectorBuffer[off] = 0xE5;
               Partition_WriteSectors(disk, lba, 1, sectorBuffer);
               free(sectorBuffer);
               if (fat_metadata_append_record(disk, metadataPath, 0,
                                              FAT_METADATA_FLAG_DELETED) < 0)
               {
                  logfmt(
                      LOG_WARNING,
                      "[FAT] FAT_Delete: metadata tombstone failed for '%s'\n",
                      metadataPath);
               }
               logfmt(LOG_INFO, "[FAT] FAT_Delete: deleted '%s'\n", name);
               free(parentPath);
               free(baseName);
               return SUCCESS;
            }
         }
         free(sectorBuffer);
      }
   }
   else
   {
      uint32_t cluster = parentData->first_cluster;
      uint32_t eof_marker_del = (inst->fat_type == 12)   ? 0xFF8
                                : (inst->fat_type == 16) ? 0xFFF8
                                                         : 0x0FFFFFF8;
      uint32_t scanned = 0;
      while (cluster < eof_marker_del && scanned < 10000)
      {
         for (uint32_t sec = 0; sec < sectorsPerCluster; sec++)
         {
            uint8_t *sectorBuffer = kmalloc(SECTOR_SIZE);
            if (!sectorBuffer) continue;
            uint32_t lba = FAT_ClusterToLba(inst, cluster) + sec;
            if (Partition_ReadSectors(disk, lba, 1, sectorBuffer) < 0)
            {
               free(sectorBuffer);
               continue;
            }
            for (uint32_t off = 0; off < SECTOR_SIZE;
                 off += sizeof(FAT_DirectoryEntry))
            {
               FAT_DirectoryEntry *e =
                   (FAT_DirectoryEntry *)(sectorBuffer + off);
               if ((e->attributes & 0x0F) == 0x0F) continue;
               if (e->name[0] == 0x00)
               {
                  free(sectorBuffer);
                  break;
               }
               if (memcmp(e->name, entry.name, 11) == 0)
               {
                  sectorBuffer[off] = 0xE5;
                  Partition_WriteSectors(disk, lba, 1, sectorBuffer);
                  free(sectorBuffer);
                  if (fat_metadata_append_record(disk, metadataPath, 0,
                                                 FAT_METADATA_FLAG_DELETED) < 0)
                  {
                     logfmt(LOG_WARNING,
                            "[FAT] FAT_Delete: metadata tombstone failed for "
                            "'%s'\n",
                            metadataPath);
                  }
                  logfmt(LOG_INFO, "[FAT] FAT_Delete: deleted '%s'\n", name);
                  free(parentPath);
                  free(baseName);
                  return SUCCESS;
               }
            }
            free(sectorBuffer);
         }
         cluster = FAT_NextCluster(inst, disk, cluster);
         scanned++;
      }
   }

   logfmt(LOG_WARNING,
          "[FAT] FAT_Delete: entry not found during mark phase for '%s'\n",
          name);
   free(parentPath);
   free(baseName);
   return -ENOENT;
}

int FAT_Truncate(Partition *disk, FAT_File *file)
{
   if (!file)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Truncate: file is NULL\n");
      return -EINVAL;
   }

   if (file->handle == ROOT_DIRECTORY_HANDLE)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Truncate: cannot truncate root directory\n");
      return -EINVAL;
   }

   if (file->handle < 0 || file->handle >= MAX_FILE_HANDLES)
   {
      logfmt(LOG_ERROR, "[FAT] FAT_Truncate: invalid file handle %d\n",
             file->handle);
      return -EINVAL;
   }

   logfmt(LOG_INFO, "[FAT] FAT_Truncate: called, file=%p, Handle=%d\n", file,
          file->handle);

   FAT_Instance *inst = fat_inst(disk);
   if (!inst) return FAT_EDISK;

   FAT_FileData *fd = &inst->opened_files[file->handle];
   logfmt(LOG_INFO, "[FAT] FAT_Truncate: fd=%p, Opened=%d\n", fd, fd->opened);
   if (!fd->opened) return FAT_ESTATE;

   // Validate FAT parameters to avoid divide-by-zero
   if (inst->bs.boot_sector.sectors_per_cluster == 0 ||
       inst->bs.boot_sector.bytes_per_sector == 0)
   {
      logfmt(LOG_ERROR,
             "[FAT] FAT_Truncate: invalid FAT parameters "
             "(sectors_per_cluster=%u, bytes_per_sector=%u)\n",
             inst->bs.boot_sector.sectors_per_cluster,
             inst->bs.boot_sector.bytes_per_sector);
      fd->first_cluster = 0;
      fd->current_cluster = 0;
      fd->current_sector_in_cluster = 0;
      fd->public_file.position = 0;
      fd->public_file.size = 0;
      return FAT_ESTATE;
   }

   uint32_t current_cluster = fd->first_cluster;
   uint32_t eof_marker = (inst->fat_type == 12)   ? 0xFF8
                         : (inst->fat_type == 16) ? 0xFFF8
                                                  : 0x0FFFFFF8;

   if (current_cluster < 2 || current_cluster >= eof_marker)
   {
      fd->first_cluster = 0;
      fd->current_cluster = 0;
      fd->current_sector_in_cluster = 0;
      fd->public_file.position = 0;
      fd->public_file.size = 0;
      return SUCCESS;
   }

   int clusterCount = 0;

   logfmt(LOG_INFO,
          "[FAT] FAT_Truncate: starting cluster chain cleanup, "
          "first_cluster=%u, FatType=%u\n",
          fd->first_cluster, inst->fat_type);
   logfmt(LOG_INFO, "[FAT] FAT_Truncate: eof_marker=%u (0x%x)\n", eof_marker,
          eof_marker);

   // Get the next cluster BEFORE freeing anything
   uint32_t next_cluster = FAT_NextCluster(inst, disk, current_cluster);
   logfmt(LOG_INFO,
          "[FAT] FAT_Truncate: first_cluster next_cluster=%u, eof_marker=%u\n",
          next_cluster, eof_marker);

   // Free all clusters EXCEPT the first one (we want to keep that for potential
   // writes)
   if (next_cluster < eof_marker)
   {
      current_cluster = next_cluster;
      while (current_cluster >= 2 && current_cluster < eof_marker &&
             clusterCount < 5000)
      {
         logfmt(LOG_INFO, "[FAT] FAT_Truncate: freeing cluster %u\n",
                current_cluster);
         clusterCount++;

         uint32_t tempNextCluster =
             FAT_NextCluster(inst, disk, current_cluster);
         if (FAT_WriteFatEntry(inst, disk, current_cluster, 0) < 0)
         {
            logfmt(LOG_ERROR,
                   "[FAT] FAT_Truncate: FAT write error freeing cluster %u\n",
                   current_cluster);
            return -EIO;
         }

         current_cluster = tempNextCluster;
      }
   }

   // Now mark the first cluster as EOF (end of chain)
   logfmt(LOG_INFO, "[FAT] FAT_Truncate: marking first cluster %u as EOF\n",
          fd->first_cluster);
   uint32_t eofVal = (inst->fat_type == 12)   ? 0x0FFF
                     : (inst->fat_type == 16) ? 0xFFFF
                                              : 0x0FFFFFFF;
   if (FAT_WriteFatEntry(inst, disk, fd->first_cluster, eofVal) < 0)
   {
      logfmt(
          LOG_ERROR,
          "[FAT] FAT_Truncate: FAT write error marking first cluster as EOF\n");
      return -EIO;
   }

   // Reset file position and size, but keep first_cluster and CurrentCluster
   // intact
   fd->public_file.position = 0;
   fd->public_file.size = 0;
   fd->truncated = true; // Mark as truncated
   fd->current_sector_in_cluster = 0;
   fd->current_cluster = fd->first_cluster;
   memset(fd->buffer, 0, SECTOR_SIZE);

   // Read the first cluster into buffer for potential writes
   if (Partition_ReadSectors(disk, FAT_ClusterToLba(inst, fd->first_cluster), 1,
                             fd->buffer) < 0)
   {
      logfmt(LOG_ERROR,
             "[FAT] FAT_Truncate: failed to read first cluster into buffer\n");
      return -EIO;
   }

   inst->fat_cache_pos = 0xFFFFFFFF;
   logfmt(LOG_INFO,
          "[FAT] FAT_Truncate: truncate complete, file ready for writes\n");
   return SUCCESS;
}

// Invalidate the FAT cache - call after operations that may leave cache
// inconsistent
void FAT_InvalidateCache(FAT_Instance *inst)
{
   if (!inst) return;

   // Invalidate FAT cache to force fresh reads
   inst->fat_cache_pos = 0xFFFFFFFF;

   // Close opened file handles (except root directory which is always open)
   for (int i = 0; i < MAX_FILE_HANDLES; i++)
   {
      if (inst->opened_files[i].opened)
      {
         inst->opened_files[i].opened = false;
      }
   }
}
// VFS Integration — FAT operations for Linux-style VFS

// FAT-specific VFS_Open wrapper that creates a VFS_File from FAT_File
static VFS_File *fat_vfs_open(Partition *partition, const char *path)
{
   if (!partition || !partition->fs || !path) return NULL;

   FAT_File *fat_file = FAT_Open(partition, path);
   if (!fat_file) return NULL;

   VFS_File *vf = (VFS_File *)kmalloc(sizeof(VFS_File));
   if (!vf) return NULL;

   vf->partition = partition;
   vf->type = partition->fs->type;
   vf->fs_file = fat_file;
   vf->is_directory = fat_file->is_directory;
   vf->size = fat_file->size;
   return vf;
}

// VFS create wrapper: opens a new (non-existing) file via FAT_Create
static VFS_File *fat_vfs_create(Partition *partition, const char *path,
                                uint16_t mode)
{
   if (!partition || !partition->fs || !path) return NULL;

   FAT_File *fat_file = FAT_Create(partition, path, mode);
   if (!fat_file) return NULL;

   VFS_File *vf = (VFS_File *)kmalloc(sizeof(VFS_File));
   if (!vf) return NULL;

   vf->partition = partition;
   vf->type = partition->fs->type;
   vf->fs_file = fat_file;
   vf->is_directory = fat_file->is_directory;
   vf->size = fat_file->size;
   return vf;
}

// Small wrapper to extract size from FAT_File
static uint32_t fat_vfs_get_size(void *fs_file)
{
   if (!fs_file) return 0;
   return ((FAT_File *)fs_file)->size;
}

static int fat_vfs_access(Partition *partition, const char *path, uint32_t uid,
                          uint32_t gid, uint8_t access_mask)
{
   return fat_check_access_path(partition, path, uid, gid, access_mask);
}

static void fat_short_name_to_cstr(const uint8_t fat_name[11], char *out,
                                   size_t out_size)
{
   if (!out || out_size == 0) return;

   size_t pos = 0;

   for (int i = 0; i < 8 && pos + 1 < out_size; i++)
   {
      if (fat_name[i] == ' ') break;
      out[pos++] = (char)fat_name[i];
   }

   bool hasExt = false;
   for (int i = 8; i < 11; i++)
   {
      if (fat_name[i] != ' ')
      {
         hasExt = true;
         break;
      }
   }

   if (hasExt && pos + 2 < out_size)
   {
      out[pos++] = '.';
      for (int i = 8; i < 11 && pos + 1 < out_size; i++)
      {
         if (fat_name[i] == ' ') break;
         out[pos++] = (char)fat_name[i];
      }
   }

   out[pos] = '\0';
}

static int fat_vfs_readdir(Partition *partition, void *fs_file,
                           VFS_DirEntry *entry_out)
{
   if (!partition || !fs_file || !entry_out) return -EINVAL;

   FAT_File *dir = (FAT_File *)fs_file;
   if (!dir->is_directory) return -EINVAL;

   FAT_DirectoryEntry entry;
   while (FAT_ReadEntry(partition, dir, &entry) == SUCCESS)
   {
      if (entry.name[0] == 0x00) return -ENOENT;
      if ((uint8_t)entry.name[0] == 0xE5) continue;
      if ((entry.attributes & FAT_ATTRIBUTE_LFN) == FAT_ATTRIBUTE_LFN) continue;
      if (entry.attributes & FAT_ATTRIBUTE_VOLUME_ID) continue;

      fat_short_name_to_cstr(entry.name, entry_out->name,
                             sizeof(entry_out->name));
      entry_out->is_directory =
          (entry.attributes & FAT_ATTRIBUTE_DIRECTORY) != 0;
      entry_out->size = entry.size;
      return SUCCESS;
   }

   return -ENOENT;
}

static int fat_vfs_chmod(Partition *partition, const char *path, uint16_t mode)
{
   return fat_chmod_path(partition, path, mode);
}

static int fat_vfs_chown(Partition *partition, const char *path, uint32_t uid,
                         uint32_t gid)
{
   return fat_chown_path(partition, path, uid, gid);
}

// FAT operations structure - directly points to FAT functions
static const VFS_Operations s_FatVfsOps = {
    .open =
        fat_vfs_open, /* Open an existing file (returns NULL if not found) */
    .create = fat_vfs_create, /* Create a new file */
    .readdir = fat_vfs_readdir,
    .read = (uint32_t (*)(Partition *, void *, uint32_t, void *))FAT_Read,
    .write =
        (uint32_t (*)(Partition *, void *, uint32_t, const void *))FAT_Write,
    .seek = (int (*)(Partition *, void *, uint32_t))FAT_Seek,
    .close = (void (*)(void *))FAT_Close,
    .get_size = fat_vfs_get_size, /* Simple wrapper for size extraction */
    .delete = (int (*)(Partition *, const char *))FAT_Delete,
    .access = fat_vfs_access,
    .chmod = fat_vfs_chmod,
    .chown = fat_vfs_chown};

// Public function to get FAT VFS operations
const VFS_Operations *FAT_GetVFSOperations(void) { return &s_FatVfsOps; }