/****************************************************************************
 * boards/risc-v/esp32s31/common/scripts/esp32s31_aliases.ld
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

_interrupt_handler = exception_common;
cache_invalidate_all = Cache_Invalidate_All;
cache_set_idrom_mmu_size = Cache_Set_IDROM_MMU_Size;
