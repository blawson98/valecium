// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

typedef struct
{
   int (*DISK_Read)(uint8_t drive, uint16_t cylinder, uint8_t sector,
                    uint8_t head, uint8_t count, void *buffer);
   int (*DISK_ReadLBA)(uint8_t drive, uint64_t lba, uint16_t count,
                       void *buffer);
} __attribute__((packed)) DL_CallbackOperations;

DL_CallbackOperations *g_DlCallbackOps = NULL;