// SPDX-License-Identifier: GPL-3.0-only

// Kernel command-line parser — splits raw bootloader command line into
// a global key/value table. Tokens without '=' are stored as bare flags.

#include "cmdline.h"
#include <mem/mm_kernel.h>
#include <std/stdio.h>
#include <std/string.h>
#include <sys/sys.h>

// Module-internal state

#define CMDLINE_MAX_ARGS 16
#define CMDLINE_MAX_KEYLEN 64
#define CMDLINE_MAX_VALLEN 192 /* large enough for LABEL=, PARTUUID= values */

BOOT_Params s_params_table;
static int s_parsed = 0; /* 0 = not yet parsed */

// Internal helpers

// Extract next key=value token from command line. Splits on first '=';
// tokens without '=' get an empty value. Returns pointer after token or NULL.
static const char *next_token(const char *src, char *key_out, size_t key_max,
                              char *val_out, size_t val_max)
{
   /* Skip leading whitespace */
   while (*src == ' ')
      src++;
   if (!*src) return NULL;

   /* -- Copy key: stop at '=' or space ---------------------------------- */
   size_t ki = 0;
   while (*src && *src != ' ' && *src != '=' && ki + 1 < key_max)
      key_out[ki++] = *src++;
   key_out[ki] = '\0';

   /* -- Copy value (everything up to the next space) -------------------- */
   val_out[0] = '\0';
   if (*src == '=')
   {
      src++; /* consume '=' */
      size_t vi = 0;
      while (*src && *src != ' ' && vi + 1 < val_max)
         val_out[vi++] = *src++;
      val_out[vi] = '\0';
   }

   /* Skip trailing whitespace so the next call starts cleanly */
   while (*src == ' ')
      src++;

   return src; /* may point to '\0' — caller checks *src */
}

// Public functions

// Split raw command line into s_params_table key/value table. Heap-allocates
// each string copy. Safe to call multiple times; subsequent calls are no-ops.
void CmdLine_Initialize(void)
{
   if (s_parsed) return;

   const char *raw = g_SysInfo->boot.commandLine;
   if (!raw)
   {
      s_parsed = 1;
      return;
   }

   memset(&s_params_table, 0, sizeof(s_params_table));
   s_params_table.count = 0;

   /* Static scratch buffers — only used during the parse pass */
   static char s_KeyBuf[CMDLINE_MAX_KEYLEN];
   static char s_ValBuf[CMDLINE_MAX_VALLEN];

   const char *p = raw;
   while (p && *p && s_params_table.count < CMDLINE_MAX_ARGS)
   {
      s_KeyBuf[0] = '\0';
      s_ValBuf[0] = '\0';

      p = next_token(p, s_KeyBuf, CMDLINE_MAX_KEYLEN, s_ValBuf,
                     CMDLINE_MAX_VALLEN);

      /* Skip empty keys produced by leading/trailing spaces */
      if (strlen(s_KeyBuf) == 0) continue;

      /* Heap-allocate persistent copies */
      size_t klen = strlen(s_KeyBuf) + 1;
      size_t vlen = strlen(s_ValBuf) + 1;
      char *kheap = (char *)kmalloc(klen);
      char *vheap = (char *)kmalloc(vlen);

      if (!kheap || !vheap)
      {
         if (kheap) free(kheap);
         if (vheap) free(vheap);
         break; /* Out of memory — stop parsing */
      }

      memcpy(kheap, s_KeyBuf, klen);
      memcpy(vheap, s_ValBuf, vlen);

      s_params_table.args[s_params_table.count].key = kheap;
      s_params_table.args[s_params_table.count].value = vheap;
      s_params_table.count++;
   }

   g_SysInfo->boot_params = s_params_table;

   logfmt(LOG_INFO, "[CMDLINE] Parsed %u arguments from bootloader\n",
          s_params_table.count);
   for (uint32_t i = 0; i < s_params_table.count; i++)
   {
      if (s_params_table.args[i].value &&
          s_params_table.args[i].value[0] != '\0')
      {
         logfmt(LOG_INFO, "[CMDLINE]   %s=%s\n", s_params_table.args[i].key,
                s_params_table.args[i].value);
      }
      else
      {
         logfmt(LOG_INFO, "[CMDLINE]   %s\n", s_params_table.args[i].key);
      }
   }

   s_parsed = 1;
}
