/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_pgalloc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>

#include <nuttx/debug.h>
#include <nuttx/pgalloc.h>

void up_allocate_pgheap(void **heap_start, size_t *heap_size)
{
  DEBUGASSERT(heap_start != NULL && heap_size != NULL);

  *heap_start = (void *)CONFIG_ARCH_PGPOOL_VBASE;
  *heap_size  = CONFIG_ARCH_PGPOOL_SIZE;
}
