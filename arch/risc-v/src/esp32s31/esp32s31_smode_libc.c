/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_smode_libc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/compiler.h>

#define S31_SMODE_LIBC locate_code(".iram1")

/* The ESP32-S31 ROM implementations use a machine-only optimized memory
 * access path.  Keep the kernel variants in IRAM so they are usable both
 * during early M-mode startup and later from the S-mode kernel.  U-mode ELF
 * applications continue to link the normal portable NuttX libc variants.
 */

void S31_SMODE_LIBC esp32s31_smode_libc_keep(void)
{
}

#undef memset
no_builtin("memset")
void *S31_SMODE_LIBC memset(void *dest, int value, size_t n)
{
  uint8_t *out = dest;

  /* The Wi-Fi key table only accepts naturally aligned 32-bit accesses.
   * Use word stores whenever the destination range permits it; in
   * particular, do not clear an aligned key slot with byte stores.
   */

  if ((((uintptr_t)out | n) & 3u) == 0)
    {
      uint32_t byte = (uint8_t)value;
      uint32_t fill = byte | byte << 8 | byte << 16 | byte << 24;
      uint32_t *out32 = (uint32_t *)out;

      while (n > 0)
        {
          *out32++ = fill;
          n -= sizeof(uint32_t);
        }

      return dest;
    }

  while (n-- > 0)
    {
      *out++ = (uint8_t)value;
    }

  return dest;
}

#undef memcpy
no_builtin("memcpy")
void *S31_SMODE_LIBC memcpy(void *dest, const void *src, size_t n)
{
  uint8_t *out = dest;
  const uint8_t *in = src;

  /* Some S31 memory-mapped blocks, including the Wi-Fi key table, require
   * naturally aligned 32-bit accesses.  The ROM memcpy uses word accesses
   * for aligned buffers, but it is not callable after entering S-mode.
   * Preserve that behavior in the S-mode replacement.
   */

  if ((((uintptr_t)out | n) & 3u) == 0)
    {
      uint32_t *out32 = (uint32_t *)out;

      if (((uintptr_t)in & 3u) == 0)
        {
          const uint32_t *in32 = (const uint32_t *)in;

          while (n > 0)
            {
              *out32++ = *in32++;
              n -= sizeof(uint32_t);
            }
        }
      else
        {
          /* An unaligned RAM source must not force byte stores to an
           * aligned MMIO destination.  Assemble each little-endian word
           * from the source bytes, then issue one aligned store.
           */

          while (n > 0)
            {
              uint32_t word = (uint32_t)in[0] |
                              (uint32_t)in[1] << 8 |
                              (uint32_t)in[2] << 16 |
                              (uint32_t)in[3] << 24;

              *out32++ = word;
              in += sizeof(uint32_t);
              n -= sizeof(uint32_t);
            }
        }

      return dest;
    }

  if ((((uintptr_t)in | n) & 3u) == 0)
    {
      const uint32_t *in32 = (const uint32_t *)in;

      /* Likewise, never turn an aligned MMIO source into byte reads just
       * because the RAM destination is unaligned.
       */

      while (n > 0)
        {
          uint32_t word = *in32++;

          out[0] = (uint8_t)word;
          out[1] = (uint8_t)(word >> 8);
          out[2] = (uint8_t)(word >> 16);
          out[3] = (uint8_t)(word >> 24);
          out += sizeof(uint32_t);
          n -= sizeof(uint32_t);
        }

      return dest;
    }

  while (n-- > 0)
    {
      *out++ = *in++;
    }

  return dest;
}

#undef memmove
no_builtin("memmove")
void *S31_SMODE_LIBC memmove(void *dest, const void *src, size_t n)
{
  uint8_t *out = dest;
  const uint8_t *in = src;

  if (out < in)
    {
      while (n-- > 0)
        {
          *out++ = *in++;
        }
    }
  else if (out > in)
    {
      while (n-- > 0)
        {
          out[n] = in[n];
        }
    }

  return dest;
}

#undef memcmp
int S31_SMODE_LIBC memcmp(const void *lhs, const void *rhs, size_t n)
{
  const uint8_t *a = lhs;
  const uint8_t *b = rhs;

  while (n-- > 0)
    {
      if (*a != *b)
        {
          return (int)*a - (int)*b;
        }

      a++;
      b++;
    }

  return 0;
}

#undef memchr
void *S31_SMODE_LIBC memchr(const void *src, int value, size_t n)
{
  const uint8_t *in = src;

  while (n-- > 0)
    {
      if (*in == (uint8_t)value)
        {
          return (void *)in;
        }

      in++;
    }

  return NULL;
}

#undef strlen
size_t S31_SMODE_LIBC strlen(const char *s)
{
  const char *end = s;

  while (*end != '\0')
    {
      end++;
    }

  return (size_t)(end - s);
}

#undef strnlen
size_t S31_SMODE_LIBC strnlen(const char *s, size_t maxlen)
{
  size_t len = 0;

  while (len < maxlen && s[len] != '\0')
    {
      len++;
    }

  return len;
}

#undef strcmp
int S31_SMODE_LIBC strcmp(const char *lhs, const char *rhs)
{
  while (*lhs != '\0' && *lhs == *rhs)
    {
      lhs++;
      rhs++;
    }

  return (int)(uint8_t)*lhs - (int)(uint8_t)*rhs;
}

#undef strchr
char *S31_SMODE_LIBC strchr(const char *s, int value)
{
  char c = (char)value;

  do
    {
      if (*s == c)
        {
          return (char *)s;
        }
    }
  while (*s++ != '\0');

  return NULL;
}

#undef strcpy
char *S31_SMODE_LIBC strcpy(char *dest, const char *src)
{
  char *out = dest;

  do
    {
      *out++ = *src;
    }
  while (*src++ != '\0');

  return dest;
}

#undef strncpy
char *S31_SMODE_LIBC strncpy(char *dest, const char *src, size_t n)
{
  char *out = dest;

  while (n > 0 && *src != '\0')
    {
      *out++ = *src++;
      n--;
    }

  while (n-- > 0)
    {
      *out++ = '\0';
    }

  return dest;
}

#undef strlcpy
size_t S31_SMODE_LIBC strlcpy(char *dest, const char *src, size_t size)
{
  const char *start = src;

  if (size > 0)
    {
      while (--size > 0 && *src != '\0')
        {
          *dest++ = *src++;
        }

      *dest = '\0';
    }

  while (*src++ != '\0')
    {
    }

  return (size_t)(src - start - 1);
}

#undef bzero
void S31_SMODE_LIBC bzero(void *dest, size_t n)
{
  memset(dest, 0, n);
}
