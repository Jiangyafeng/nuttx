/****************************************************************************
 * binfmt/elf_verify_app.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <crypto/sha2.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/addrenv.h>
#include <nuttx/elf.h>
#include <nuttx/fs/fs.h>
#include <nuttx/lib/elf.h>

#include "binfmt.h"

#ifdef CONFIG_ELF_APP_VERIFY

#define ELF_APP_HASH_HEX_SIZE 64
#define ELF_APP_IO_SIZE       512
#define ELF_APP_MAX_PHDRS     32
/* TLS-enabled applications retain one allocatable section per Mbed TLS
 * constant/data input section even though the linker folds them into two
 * PT_LOAD segments.  Keep a finite verifier bound, but allow those normal
 * section tables; every entry and range is still validated below.
 */

#define ELF_APP_MAX_SHDRS     256

static bool elf_app_file_range(uint64_t offset, uint64_t size,
                               uint64_t filelen)
{
  return offset <= filelen && size <= filelen - offset;
}

static bool elf_app_memory_range(uint64_t address, uint64_t size,
                                 uint64_t base, uint64_t length)
{
  return address >= base && address <= base + length &&
         size <= base + length - address;
}

static bool elf_app_power_of_two(uint32_t value)
{
  return value == 0 || (value & (value - 1)) == 0;
}

static int elf_app_hex_value(char value)
{
  if (value >= '0' && value <= '9')
    {
      return value - '0';
    }

  if (value >= 'a' && value <= 'f')
    {
      return value - 'a' + 10;
    }

  if (value >= 'A' && value <= 'F')
    {
      return value - 'A' + 10;
    }

  return -EINVAL;
}

static int elf_app_read_manifest(const char *filename,
                                 uint8_t expected[SHA256_DIGEST_LENGTH],
                                 char hex[ELF_APP_HASH_HEX_SIZE + 1])
{
  char path[PATH_MAX];
  char input[ELF_APP_HASH_HEX_SIZE + 2];
  size_t length;
  ssize_t nread;
  int high;
  int low;
  int fd;
  int i;

  length = strlen(filename);
  if (length + sizeof(".sha256") > sizeof(path))
    {
      return -ENAMETOOLONG;
    }

  snprintf(path, sizeof(path), "%s.sha256", filename);
  fd = nx_open(path, O_RDONLY);
  if (fd < 0)
    {
      return fd;
    }

  nread = nx_read(fd, input, sizeof(input));
  nx_close(fd);
  if (nread < ELF_APP_HASH_HEX_SIZE ||
      (nread > ELF_APP_HASH_HEX_SIZE && input[ELF_APP_HASH_HEX_SIZE] != '\n'))
    {
      return -EBADMSG;
    }

  for (i = 0; i < SHA256_DIGEST_LENGTH; i++)
    {
      high = elf_app_hex_value(input[i * 2]);
      low = elf_app_hex_value(input[i * 2 + 1]);
      if (high < 0 || low < 0)
        {
          return -EBADMSG;
        }

      expected[i] = (high << 4) | low;
    }

  memcpy(hex, input, ELF_APP_HASH_HEX_SIZE);
  hex[ELF_APP_HASH_HEX_SIZE] = '\0';
  return OK;
}

static int elf_app_verify_hash(const char *filename,
                               struct mod_loadinfo_s *loadinfo,
                               char hex[ELF_APP_HASH_HEX_SIZE + 1])
{
  uint8_t expected[SHA256_DIGEST_LENGTH];
  uint8_t actual[SHA256_DIGEST_LENGTH];
  uint8_t buffer[ELF_APP_IO_SIZE];
  SHA2_CTX context;
  size_t chunk;
  size_t offset;
  uint8_t different = 0;
  int ret;
  int i;

  ret = elf_app_read_manifest(filename, expected, hex);
  if (ret < 0)
    {
      return ret;
    }

  sha256init(&context);
  for (offset = 0; offset < loadinfo->filelen; offset += chunk)
    {
      chunk = loadinfo->filelen - offset;
      if (chunk > sizeof(buffer))
        {
          chunk = sizeof(buffer);
        }

      ret = libelf_read(loadinfo, buffer, chunk, offset);
      if (ret < 0)
        {
          return ret;
        }

      sha256update(&context, buffer, chunk);
    }

  sha256final(actual, &context);
  for (i = 0; i < SHA256_DIGEST_LENGTH; i++)
    {
      different |= actual[i] ^ expected[i];
    }

  return different == 0 ? OK : -EKEYREJECTED;
}

static int elf_app_verify_phdrs(struct mod_loadinfo_s *loadinfo)
{
  Elf_Phdr phdr;
  uint64_t offset;
  uint64_t text_total = 0;
  uint64_t data_total = 0;
  bool entry_found = false;
  bool load_found = false;
  int ret;
  int i;

  for (i = 0; i < loadinfo->ehdr.e_phnum; i++)
    {
      offset = loadinfo->ehdr.e_phoff +
               (uint64_t)i * loadinfo->ehdr.e_phentsize;
      ret = libelf_read(loadinfo, (uint8_t *)&phdr, sizeof(phdr), offset);
      if (ret < 0)
        {
          return ret;
        }

      if (phdr.p_type != PT_LOAD)
        {
          continue;
        }

      load_found = true;
      if (phdr.p_filesz > phdr.p_memsz ||
          !elf_app_file_range(phdr.p_offset, phdr.p_filesz,
                              loadinfo->filelen) ||
          !elf_app_power_of_two(phdr.p_align) ||
          (phdr.p_flags & (PF_W | PF_X)) == (PF_W | PF_X) ||
          (phdr.p_align > 1 &&
           (phdr.p_offset % phdr.p_align) !=
           (phdr.p_vaddr % phdr.p_align)))
        {
          return -ENOEXEC;
        }

      if ((phdr.p_flags & PF_X) != 0)
        {
          if (!elf_app_memory_range(phdr.p_vaddr, phdr.p_memsz,
                                    CONFIG_ARCH_TEXT_VBASE,
                                    ARCH_TEXT_SIZE))
            {
              return -ERANGE;
            }

          text_total += phdr.p_memsz;
          if (loadinfo->ehdr.e_entry >= phdr.p_vaddr &&
              loadinfo->ehdr.e_entry - phdr.p_vaddr < phdr.p_memsz)
            {
              entry_found = true;
            }
        }
      else
        {
          if (!elf_app_memory_range(phdr.p_vaddr, phdr.p_memsz,
                                    CONFIG_ARCH_DATA_VBASE,
                                    ARCH_DATA_SIZE))
            {
              return -ERANGE;
            }

          data_total += phdr.p_memsz;
        }
    }

  if (!load_found || !entry_found || text_total > ARCH_TEXT_SIZE ||
      data_total > ARCH_DATA_SIZE)
    {
      return -ENOEXEC;
    }

  return OK;
}

static int elf_app_verify_shdrs(struct mod_loadinfo_s *loadinfo)
{
  Elf_Shdr shdr;
  uint64_t offset;
  int ret;
  int i;

  for (i = 0; i < loadinfo->ehdr.e_shnum; i++)
    {
      offset = loadinfo->ehdr.e_shoff +
               (uint64_t)i * loadinfo->ehdr.e_shentsize;
      ret = libelf_read(loadinfo, (uint8_t *)&shdr, sizeof(shdr), offset);
      if (ret < 0)
        {
          return ret;
        }

      if (!elf_app_power_of_two(shdr.sh_addralign) ||
          (shdr.sh_type != SHT_NOBITS &&
           !elf_app_file_range(shdr.sh_offset, shdr.sh_size,
                               loadinfo->filelen)))
        {
          return -ENOEXEC;
        }

      if ((shdr.sh_flags & SHF_ALLOC) == 0 || shdr.sh_size == 0)
        {
          continue;
        }

      if ((shdr.sh_flags & SHF_EXECINSTR) != 0)
        {
          if ((shdr.sh_flags & SHF_WRITE) != 0 ||
              !elf_app_memory_range(shdr.sh_addr, shdr.sh_size,
                                    CONFIG_ARCH_TEXT_VBASE,
                                    ARCH_TEXT_SIZE))
            {
              return -ERANGE;
            }
        }
      else if ((shdr.sh_flags & SHF_WRITE) != 0)
        {
          if (!elf_app_memory_range(shdr.sh_addr, shdr.sh_size,
                                    CONFIG_ARCH_DATA_VBASE,
                                    ARCH_DATA_SIZE))
            {
              return -ERANGE;
            }
        }
      else if (!elf_app_memory_range(shdr.sh_addr, shdr.sh_size,
                                     CONFIG_ARCH_TEXT_VBASE,
                                     ARCH_TEXT_SIZE))
        {
          return -ERANGE;
        }
    }

  return OK;
}

int elf_verify_app(const char *filename, struct mod_loadinfo_s *loadinfo)
{
  char hash[ELF_APP_HASH_HEX_SIZE + 1];
  uint64_t table_size;
  int ret;

  if (loadinfo->filelen < sizeof(Elf_Ehdr) ||
      loadinfo->filelen > CONFIG_ELF_APP_MAX_FILESIZE)
    {
      ret = -EFBIG;
      goto reject;
    }

  if (loadinfo->ehdr.e_type != ET_EXEC ||
      loadinfo->ehdr.e_ehsize != sizeof(Elf_Ehdr) ||
      loadinfo->ehdr.e_phentsize != sizeof(Elf_Phdr) ||
      loadinfo->ehdr.e_shentsize != sizeof(Elf_Shdr) ||
      loadinfo->ehdr.e_phnum == 0 ||
      loadinfo->ehdr.e_phnum > ELF_APP_MAX_PHDRS ||
      loadinfo->ehdr.e_shnum == 0 ||
      loadinfo->ehdr.e_shnum > ELF_APP_MAX_SHDRS)
    {
      ret = -ENOEXEC;
      goto reject;
    }

  table_size = (uint64_t)loadinfo->ehdr.e_phentsize *
               loadinfo->ehdr.e_phnum;
  if (!elf_app_file_range(loadinfo->ehdr.e_phoff, table_size,
                          loadinfo->filelen))
    {
      ret = -ENOEXEC;
      goto reject;
    }

  table_size = (uint64_t)loadinfo->ehdr.e_shentsize *
               loadinfo->ehdr.e_shnum;
  if (!elf_app_file_range(loadinfo->ehdr.e_shoff, table_size,
                          loadinfo->filelen))
    {
      ret = -ENOEXEC;
      goto reject;
    }

  ret = elf_app_verify_phdrs(loadinfo);
  if (ret < 0)
    {
      goto reject;
    }

  ret = elf_app_verify_shdrs(loadinfo);
  if (ret < 0)
    {
      goto reject;
    }

  ret = elf_app_verify_hash(filename, loadinfo, hash);
  if (ret < 0)
    {
      goto reject;
    }

  syslog(LOG_INFO, "APPVERIFY:PASS %s sha256=%s\n", filename, hash);
  return OK;

reject:
  syslog(LOG_ERR, "APPVERIFY:REJECT %s error=%d\n", filename, ret);
  return ret;
}

#endif
