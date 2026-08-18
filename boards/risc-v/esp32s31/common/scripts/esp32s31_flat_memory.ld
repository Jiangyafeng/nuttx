/****************************************************************************
 * boards/risc-v/esp32s31/common/scripts/esp32s31_flat_memory.ld
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* ESP32-S31 virtual address layout.  The upper SRAM limit excludes the ROM
 * stack/data area and matches the ESP-IDF 6.1 S31 linker description used by
 * the board experiments.
 */

#include "common.ld"

#define SRAM_START       0x2f000000
#define SRAM_END         0x2f07afc0
#define SRAM_SIZE        (SRAM_END - SRAM_START)
#define IDROM_SEG_SIZE   (CONFIG_MMU_PAGE_SIZE << 10)

MEMORY
{
  irom_seg (RX)       : org = 0x40000020, len = IDROM_SEG_SIZE - 0x20
  sram_seg (RWX)      : org = SRAM_START, len = SRAM_SIZE
  drom_seg (R)        : org = 0x40000020, len = IDROM_SEG_SIZE - 0x20
  /* Keep the IDF RTC time-retention record at the top of LP RAM. */
  lp_ram_seg (RWX)    : org = 0x2e000000, len = 0x8000 - RESERVE_RTC_MEM
  lp_reserved_seg(RW) : org = 0x2e008000 - RESERVE_RTC_MEM,
                         len = RESERVE_RTC_MEM
  extern_ram_seg(RWX) : org = 0x50000000, len = IDROM_SEG_SIZE
}

_data_seg_org = ORIGIN(rtc_data_seg);

REGION_ALIAS("rtc_iram_seg", lp_ram_seg);
REGION_ALIAS("rtc_data_seg", lp_ram_seg);
REGION_ALIAS("rtc_slow_seg", lp_ram_seg);
REGION_ALIAS("rtc_data_location", lp_ram_seg);
REGION_ALIAS("rtc_reserved_seg", lp_reserved_seg);

REGION_ALIAS("text_seg_low", irom_seg);
REGION_ALIAS("rodata_seg_low", drom_seg);
REGION_ALIAS("ext_ram_seg", extern_ram_seg);

ASSERT(_flash_rodata_dummy_start == ORIGIN(rodata_seg_low),
       ".flash_rodata_dummy must begin at the DROM origin")
