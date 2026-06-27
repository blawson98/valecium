// SPDX-License-Identifier: GPL-3.0-only

#include <stddef.h>
#include <stdint.h>

#include <constants.h>
#include <dl/loader.h>

/* ELF identification indices */
#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS32 1
#define ELFCLASS64 2

/* Section types */
#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_NOBITS 8
#define SHT_DYNSYM 11

/* Symbol binding and type helpers */
#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xf)
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define STB_GLOBAL 1
#define STT_FUNC 2

/* Opaque handle buffer size (must match dl_handle layout below) */
#define DL_HANDLE_SIZE 32

typedef struct
{
   unsigned char e_ident[16];
   uint16_t e_type;
   uint16_t e_machine;
   uint32_t e_version;
   uint32_t e_entry;
   uint32_t e_phoff;
   uint32_t e_shoff;
   uint32_t e_flags;
   uint16_t e_ehsize;
   uint16_t e_phentsize;
   uint16_t e_phnum;
   uint16_t e_shentsize;
   uint16_t e_shnum;
   uint16_t e_shstrndx;
} __attribute__((packed)) elf32_ehdr;

typedef struct
{
   uint32_t sh_name;
   uint32_t sh_type;
   uint32_t sh_flags;
   uint32_t sh_addr;
   uint32_t sh_offset;
   uint32_t sh_size;
   uint32_t sh_link;
   uint32_t sh_info;
   uint32_t sh_addralign;
   uint32_t sh_entsize;
} __attribute__((packed)) elf32_shdr;

typedef struct
{
   uint32_t st_name;
   uint32_t st_value;
   uint32_t st_size;
   uint8_t st_info;
   uint8_t st_other;
   uint16_t st_shndx;
} __attribute__((packed)) elf32_sym;

typedef struct
{
   unsigned char e_ident[16];
   uint16_t e_type;
   uint16_t e_machine;
   uint32_t e_version;
   uint64_t e_entry;
   uint64_t e_phoff;
   uint64_t e_shoff;
   uint32_t e_flags;
   uint16_t e_ehsize;
   uint16_t e_phentsize;
   uint16_t e_phnum;
   uint16_t e_shentsize;
   uint16_t e_shnum;
   uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr;

typedef struct
{
   uint32_t sh_name;
   uint32_t sh_type;
   uint64_t sh_flags;
   uint64_t sh_addr;
   uint64_t sh_offset;
   uint64_t sh_size;
   uint32_t sh_link;
   uint32_t sh_info;
   uint64_t sh_addralign;
   uint64_t sh_entsize;
} __attribute__((packed)) elf64_shdr;

typedef struct
{
   uint32_t st_name;
   uint8_t st_info;
   uint8_t st_other;
   uint16_t st_shndx;
   uint64_t st_value;
   uint64_t st_size;
} __attribute__((packed)) elf64_sym;

typedef struct
{
   void *symtab;  /* pointer to symbol table in file data */
   void *strtab;  /* pointer to string table in file data */
   int sym_count; /* number of symbol entries */
   int is_64bit;  /* 1 if 64-bit ELF, 0 if 32-bit */
} dl_handle;

/* Internal handle storage — one library at a time */
static dl_handle s_Handle;

void *DL_LoadLibrary(void *fileData)
{
   unsigned char *ident = (unsigned char *)fileData;
   dl_handle *h = &s_Handle;

   /* Validate ELF magic */
   if (ident[EI_MAG0] != ELFMAG0 || ident[EI_MAG1] != ELFMAG1 ||
       ident[EI_MAG2] != ELFMAG2 || ident[EI_MAG3] != ELFMAG3)
      return NULL;

   h->symtab = NULL;
   h->strtab = NULL;
   h->sym_count = 0;

   if (ident[EI_CLASS] == ELFCLASS32)
   {
      elf32_ehdr *ehdr = (elf32_ehdr *)fileData;
      elf32_shdr *shdr = (elf32_shdr *)((uintptr_t)fileData + ehdr->e_shoff);

      h->is_64bit = 0;

      for (int i = 0; i < ehdr->e_shnum; i++)
      {
         if (shdr[i].sh_type == SHT_SYMTAB || shdr[i].sh_type == SHT_DYNSYM)
         {
            h->symtab = (void *)((uintptr_t)fileData + shdr[i].sh_offset);
            h->sym_count = shdr[i].sh_size / shdr[i].sh_entsize;

            /* Companion string table is pointed to by sh_link */
            if (shdr[i].sh_link < (uint32_t)ehdr->e_shnum)
               h->strtab = (void *)((uintptr_t)fileData +
                                    shdr[shdr[i].sh_link].sh_offset);
            return h;
         }
      }
   }
   else if (ident[EI_CLASS] == ELFCLASS64)
   {
      elf64_ehdr *ehdr = (elf64_ehdr *)fileData;
      elf64_shdr *shdr = (elf64_shdr *)((uintptr_t)fileData + ehdr->e_shoff);

      h->is_64bit = 1;

      for (int i = 0; i < ehdr->e_shnum; i++)
      {
         if (shdr[i].sh_type == SHT_SYMTAB || shdr[i].sh_type == SHT_DYNSYM)
         {
            h->symtab = (void *)((uintptr_t)fileData + shdr[i].sh_offset);
            h->sym_count = shdr[i].sh_size / shdr[i].sh_entsize;

            if (shdr[i].sh_link < (uint32_t)ehdr->e_shnum)
               h->strtab = (void *)((uintptr_t)fileData +
                                    shdr[shdr[i].sh_link].sh_offset);
            return h;
         }
      }
   }

   return NULL;
}

void *DL_LoadSymbol(void *handle, const char *symbol)
{
   dl_handle *h = (dl_handle *)handle;

   if (!h->symtab || !h->strtab || !symbol) return NULL;

   if (h->is_64bit)
   {
      elf64_sym *sym = (elf64_sym *)h->symtab;
      char *strtab = (char *)h->strtab;

      for (int i = 0; i < h->sym_count; i++)
      {
         char *name = strtab + sym[i].st_name;
         if (name[0] == '\0') continue;

         int j = 0;
         while (name[j] == symbol[j] && name[j] != '\0')
            j++;
         if (name[j] == '\0' && symbol[j] == '\0')
            return (void *)(uintptr_t)sym[i].st_value;
      }
   }
   else
   {
      elf32_sym *sym = (elf32_sym *)h->symtab;
      char *strtab = (char *)h->strtab;

      for (int i = 0; i < h->sym_count; i++)
      {
         char *name = strtab + sym[i].st_name;
         if (name[0] == '\0') continue;

         int j = 0;
         while (name[j] == symbol[j] && name[j] != '\0')
            j++;
         if (name[j] == '\0' && symbol[j] == '\0')
            return (void *)(uintptr_t)sym[i].st_value;
      }
   }

   return NULL;
}