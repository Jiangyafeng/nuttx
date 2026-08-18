/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_start.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/init.h>

#include "riscv_internal.h"

#include "esp_irq.h"
#include "esp_libc_stubs.h"
#include "esp_lowputc.h"
#include "esp_start.h"

#include "esp_rom_sys.h"
#include "esp_clk_internal.h"
#include "esp_private/rtc_clk.h"
#include "esp_cpu.h"
#include "esp_private/esp_mmu_map_private.h"
#include "esp_private/brownout.h"
#include "hal/wdt_hal.h"
#include "hal/mmu_hal.h"
#include "hal/mmu_types.h"
#include "hal/cache_types.h"
#include "hal/cache_ll.h"
#include "hal/cache_hal.h"
#include "hal/rwdt_ll.h"
#include "soc/ext_mem_defs.h"
#include "soc/reg_base.h"
#include "spi_flash_mmap.h"
#include "rom/cache.h"
#include "riscv/csr.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"
#if defined(CONFIG_ARCH_CHIP_ESP32S31) && \
    defined(CONFIG_ARCH_USE_S_MODE)
#  include "soc/cpu_apm_reg.h"
#  include "soc/spi_mem_s_reg.h"
#  include "soc/tee_reg.h"
#  ifdef CONFIG_BUILD_KERNEL
#    include "esp32s31_mm_init.h"
#  endif
#endif
#ifdef CONFIG_ARCH_CHIP_ESP32P4
#  include "soc/hp_peri_pms_reg.h"
#  include "soc/lp_peri_pms_reg.h"
#endif
#include "soc/rtc.h"

#include "bootloader_init.h"
#include "bootloader_sha.h"

#ifdef CONFIG_ESPRESSIF_SIMPLE_BOOT
#include "esp_rom_serial_output.h"
#include "esp_app_format.h"
#endif

#include "bootloader_mem.h"
#include "bootloader_flash_priv.h"
#include "esp_private/startup_internal.h"
#include "esp_private/spi_flash_os.h"
#ifdef CONFIG_ESPRESSIF_SPIRAM
#  include "esp_psram.h"
#  include "esp_private/esp_psram_extram.h"
#endif

#if SOC_APM_SUPPORTED
#  include "hal/apm_hal.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_DEBUG_FEATURES
#  define showprogress(c)     esp_rom_printf(c)
#else
#  define showprogress(c)
#endif

#if defined(CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT) || \
    defined (CONFIG_ESPRESSIF_SIMPLE_BOOT)
#  ifdef CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT
#    define PRIMARY_SLOT_OFFSET   CONFIG_ESPRESSIF_OTA_PRIMARY_SLOT_OFFSET
#  else
#    define PRIMARY_SLOT_OFFSET   0  /* Force offset to the beginning of the whole image */
#  endif
#  define HDR_ATTR              __attribute__((section(".entry_addr"))) \
                                __attribute__((used))
#  define CHECKSUM_ALIGN        16
#  define IS_PADD(addr) ((addr) == 0)
#if defined(SOC_TCM_LOW) || defined(SOC_TCM_HIGH)
#  define IS_TCM(addr)  ((addr) >= SOC_TCM_LOW && (addr) < SOC_TCM_HIGH)
#else
#  define IS_TCM(addr) false
#endif
#  define IS_DRAM(addr) ((addr) >= SOC_DRAM_LOW && (addr) < SOC_DRAM_HIGH)
#  define IS_IRAM(addr) ((addr) >= SOC_IRAM_LOW && (addr) < SOC_IRAM_HIGH)
#  define IS_IROM(addr) ((addr) >= SOC_IROM_LOW && (addr) < SOC_IROM_HIGH)
#  define IS_DROM(addr) ((addr) >= SOC_DROM_LOW && (addr) < SOC_DROM_HIGH)
#  define IS_SRAM(addr) (IS_TCM(addr) || IS_IRAM(addr) || IS_DRAM(addr))
#  define IS_MMAP(addr) (IS_IROM(addr) || IS_DROM(addr))
#  ifdef SOC_RTC_FAST_MEM_SUPPORTED
#    define IS_RTC_FAST_IRAM(addr) \
                        ((addr) >= SOC_RTC_IRAM_LOW \
                         && (addr) < SOC_RTC_IRAM_HIGH)
#    define IS_RTC_FAST_DRAM(addr) \
                        ((addr) >= SOC_RTC_DRAM_LOW \
                         && (addr) < SOC_RTC_DRAM_HIGH)
#  else
#    define IS_RTC_FAST_IRAM(addr) false
#    define IS_RTC_FAST_DRAM(addr) false
#  endif
#  ifdef SOC_RTC_SLOW_MEM_SUPPORTED
#    define IS_RTC_SLOW_DRAM(addr) \
                        ((addr) >= SOC_RTC_DATA_LOW \
                         && (addr) < SOC_RTC_DATA_HIGH)
#  else
#    define IS_RTC_SLOW_DRAM(addr) false
#  endif
#  define IS_NONE(addr) (!IS_IROM(addr) \
                         && !IS_DROM(addr) \
                         && !IS_IRAM(addr) \
                         && !IS_TCM(addr) \
                         && !IS_DRAM(addr) \
                         && !IS_RTC_FAST_IRAM(addr) \
                         && !IS_RTC_FAST_DRAM(addr) \
                         && !IS_RTC_SLOW_DRAM(addr) \
                         && !IS_PADD(addr))

#  define IS_MAPPING(addr) IS_IROM(addr) || IS_DROM(addr)
#endif

#define NAPOT_RWX   (PMPCFG_A_NAPOT | PMPCFG_RWX_MASK)

#if defined(CONFIG_ARCH_CHIP_ESP32S31) && \
    defined(CONFIG_ARCH_USE_S_MODE)
#  define ESP32S31_CPU_APM_REE0_RWX 0x7
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

#if defined(CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT) || \
    defined (CONFIG_ESPRESSIF_SIMPLE_BOOT)
extern uint8_t _image_irom_vma[];
extern uint8_t _image_irom_lma[];
extern uint8_t _image_irom_size[];

extern uint8_t _image_drom_vma[];
extern uint8_t _image_drom_lma[];
extern uint8_t _image_drom_size[];
#endif

extern int _vector_table;

#if SOC_INT_CLIC_SUPPORTED
extern int _mtvt_table;
#endif

/****************************************************************************
 * ROM Function Prototypes
 ****************************************************************************/

#if defined(CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT) || \
    defined (CONFIG_ESPRESSIF_SIMPLE_BOOT)
extern int ets_printf(const char *fmt, ...) printf_like(1, 2);
#endif

extern void cache_set_idrom_mmu_size(uint32_t irom_size, uint32_t drom_size);
extern void ets_delay_us(uint32_t us);

#if defined(CONFIG_ARCH_CHIP_ESP32S31) && \
    defined(CONFIG_ARCH_USE_S_MODE)
extern noreturn_function void esp32s31_start_smode(void (*entry)(void));
#endif

#if defined(CONFIG_ARCH_CHIP_ESP32S31) && defined(CONFIG_ESP32S31_SMP)
#  include "../../esp32s31/esp32s31_cpu1_boot.h"
#endif

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

#if defined(CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT) || \
    defined (CONFIG_ESPRESSIF_SIMPLE_BOOT)
IRAM_ATTR noreturn_function void __start(void);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

#if defined(CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT) || \
    defined (CONFIG_ESPRESSIF_SIMPLE_BOOT)
HDR_ATTR static void (*_entry_point)(void) = __start;
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern uint8_t _instruction_reserved_start[];
extern uint8_t _instruction_reserved_end[];
extern uint8_t _rodata_reserved_start[];
extern uint8_t _rodata_reserved_end[];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: map_rom_segments
 *
 * Description:
 *   Configure the MMU and Cache peripherals for accessing ROM code and data.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

#if defined(CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT) || \
    defined(CONFIG_ESPRESSIF_SIMPLE_BOOT)
static int map_rom_segments(uint32_t app_drom_start, uint32_t app_drom_vaddr,
                            uint32_t app_drom_size, uint32_t app_irom_start,
                            uint32_t app_irom_vaddr, uint32_t app_irom_size)
{
  uint32_t rc = 0;
  uint32_t actual_mapped_len = 0;
  uint32_t app_irom_start_aligned = app_irom_start & MMU_FLASH_MASK;
  uint32_t app_irom_vaddr_aligned = app_irom_vaddr & MMU_FLASH_MASK;
  uint32_t app_drom_start_aligned = app_drom_start & MMU_FLASH_MASK;
  uint32_t app_drom_vaddr_aligned = app_drom_vaddr & MMU_FLASH_MASK;
  uint32_t app_irom_map_size;
  uint32_t app_drom_map_size;
#ifdef CONFIG_ESPRESSIF_SIMPLE_BOOT
  esp_image_header_t image_header; /* Header for entire image */
  esp_image_segment_header_t WORD_ALIGNED_ATTR segment_hdr;
  bool padding_checksum = false;
  unsigned int segments = 0;
  unsigned int ram_segments = 0;
  unsigned int rom_segments = 0;
  size_t offset = CONFIG_BOOTLOADER_OFFSET_IN_FLASH;
#endif

#ifdef CONFIG_ESPRESSIF_SIMPLE_BOOT
  /* Read image header */

  if (bootloader_flash_read(offset, &image_header,
                            sizeof(esp_image_header_t),
                            true) != ESP_OK)
    {
      ets_printf("Failed to load image header!\n");
      abort();
    }

  offset += sizeof(esp_image_header_t);

  /* Iterate for segment information parsing */

  while (segments++ < 16 && rom_segments < 2)
    {
      /* Read segment header */

      if (bootloader_flash_read(offset, &segment_hdr,
                                sizeof(esp_image_segment_header_t),
                                true) != ESP_OK)
        {
          ets_printf("failed to read segment header at %x\n", offset);
          abort();
        }

      if (IS_NONE(segment_hdr.load_addr))
        {
          /* Total segment count = (segments - 1) */

          break;
        }

      if (IS_RTC_FAST_IRAM(segment_hdr.load_addr) ||
          IS_RTC_FAST_DRAM(segment_hdr.load_addr) ||
          IS_RTC_SLOW_DRAM(segment_hdr.load_addr))
        {
          /* RTC segment is loaded by ROM bootloader */

          ram_segments++;
        }

      ets_printf("%s: lma 0x%08x vma 0x%08lx len 0x%-6lx (%lu)\n",
          IS_NONE(segment_hdr.load_addr) ? "???" :
            IS_RTC_FAST_IRAM(segment_hdr.load_addr) ||
            IS_RTC_FAST_DRAM(segment_hdr.load_addr) ||
            IS_RTC_SLOW_DRAM(segment_hdr.load_addr) ? "rtc" :
              IS_MMAP(segment_hdr.load_addr) ?
                IS_IROM(segment_hdr.load_addr) ? "imap" : "dmap" :
                  IS_PADD(segment_hdr.load_addr) ? "padd" :
                    IS_TCM(segment_hdr.load_addr) ? "tcm" :
                      IS_DRAM(segment_hdr.load_addr) ? "dram" : "iram",
          offset + sizeof(esp_image_segment_header_t),
          segment_hdr.load_addr, segment_hdr.data_len,
          segment_hdr.data_len);

      /* Fix drom and irom produced be the linker, as this
       * is later invalidated by the elf2image command.
       */

      if (IS_DROM(segment_hdr.load_addr) &&
          segment_hdr.load_addr == (uint32_t)_image_drom_vma)
        {
          app_drom_start = offset + sizeof(esp_image_segment_header_t);
          app_drom_start_aligned = app_drom_start & MMU_FLASH_MASK;
          rom_segments++;
        }

      if (IS_IROM(segment_hdr.load_addr) &&
          segment_hdr.load_addr == (uint32_t)_image_irom_vma)
        {
          app_irom_start = offset + sizeof(esp_image_segment_header_t);
          app_irom_start_aligned = app_irom_start & MMU_FLASH_MASK;
          rom_segments++;
        }

      if (IS_SRAM(segment_hdr.load_addr))
        {
          ram_segments++;
        }

      offset += sizeof(esp_image_segment_header_t) + segment_hdr.data_len;
      if (ram_segments == image_header.segment_count && !padding_checksum)
        {
          offset += (CHECKSUM_ALIGN - 1) - (offset % CHECKSUM_ALIGN) + 1;
          if (image_header.hash_appended)
            {
              offset += ESP_IMAGE_HASH_LEN;
            }

          padding_checksum = true;
        }
    }

  if (segments == 0 || segments == 16)
    {
      ets_printf("Error parsing segments\n");
    }

  ets_printf("total segments stored %d\n", segments - 1);
#endif

  cache_hal_disable(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL);

  /* The MMU mappings start at page-aligned virtual addresses.  Include the
   * segment's leading page offset so a non-aligned segment does not lose its
   * final page when the payload crosses a page boundary. */

  app_irom_map_size = app_irom_size +
                      (app_irom_vaddr - app_irom_vaddr_aligned);
  app_drom_map_size = app_drom_size +
                      (app_drom_vaddr - app_drom_vaddr_aligned);

  /* Clear the MMU entries that are already set up,
   * so the new app only has the mappings it creates.
   */

  mmu_hal_unmap_all();

  mmu_hal_map_region(0, MMU_TARGET_FLASH0,
                     app_drom_vaddr_aligned, app_drom_start_aligned,
                     app_drom_map_size, &actual_mapped_len);

  mmu_hal_map_region(0, MMU_TARGET_FLASH0,
                     app_irom_vaddr_aligned, app_irom_start_aligned,
                     app_irom_map_size, &actual_mapped_len);

  /* ------------------Enable corresponding buses--------------------- */

  cache_bus_mask_t bus_mask = cache_ll_l1_get_bus(0, app_drom_vaddr_aligned,
                                                  app_drom_map_size);
  cache_ll_l1_enable_bus(0, bus_mask);
  bus_mask = cache_ll_l1_get_bus(0, app_irom_vaddr_aligned,
                                 app_irom_map_size);
  cache_ll_l1_enable_bus(0, bus_mask);
#if CONFIG_ESPRESSIF_NUM_CPUS > 1
  bus_mask = cache_ll_l1_get_bus(1, app_drom_vaddr_aligned,
                                 app_drom_map_size);
  cache_ll_l1_enable_bus(1, bus_mask);
  bus_mask = cache_ll_l1_get_bus(1, app_irom_vaddr_aligned,
                                 app_irom_map_size);
  cache_ll_l1_enable_bus(1, bus_mask);
#endif

#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
  cache_ll_invalidate_addr(CACHE_LL_LEVEL_ALL, CACHE_TYPE_ALL,
                           CACHE_LL_ID_ALL, app_irom_vaddr_aligned,
                           actual_mapped_len);
#endif

  /* ------------------Enable Cache----------------------------------- */

  cache_hal_enable(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL);

  return (int)rc;
}
#endif

/****************************************************************************
 * Name: recalib_bbpll
 *
 * Description:
 *   Workaround for bootloader calibration issues. This function is placed in
 *   IRAM because disabling BBPLL may influence the cache.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

#if defined(CONFIG_ARCH_CHIP_ESP32C6) || defined(CONFIG_ARCH_CHIP_ESP32H2)
static void IRAM_ATTR NOINLINE_ATTR recalib_bbpll(void)
{
    rtc_cpu_freq_config_t old_config;
    rtc_clk_cpu_freq_get_config(&old_config);

  if (old_config.source == SOC_CPU_CLK_SRC_PLL
#ifdef CONFIG_ARCH_CHIP_ESP32H2
      || old_config.source == SOC_CPU_CLK_SRC_FLASH_PLL
#endif
      )
    {
      rtc_clk_cpu_freq_set_xtal();
      rtc_clk_cpu_freq_set_config(&old_config);
    }
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

extern void esp_chip_revision_check(void);

/****************************************************************************
 * Name: riscv_soc_initialize
 *
 * Description:
 *   Initialize SoC-specific initialization.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void weak_function riscv_soc_initialize(void)
{
#ifndef CONFIG_ARCH_CHIP_ESP32S31
  sys_startup_fn();
#else
  /* The S31 IDF startup list assumes the ESP-IDF application startup
   * sequence.  NuttX initializes the required clocks and devices through
   * its architecture and driver startup paths instead.
   */
#endif
}

/****************************************************************************
 * Name: sys_startup_fn
 *
 * Description:
 *   Execute the system layer startup function for the current CPU core.
 *   This function calls the appropriate startup function from the per-CPU
 *   startup function array (g_startup_fn) based on the current core ID.
 *   The SYS_STARTUP_FN() macro retrieves the core ID, indexes into the
 *   g_startup_fn array, and invokes the corresponding startup function.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void sys_startup_fn(void)
{
  SYS_STARTUP_FN();
}

#if defined(CONFIG_ARCH_CHIP_ESP32S31) && \
    defined(CONFIG_ARCH_USE_S_MODE)

/****************************************************************************
 * Name: esp32s31_mmode_fault
 ****************************************************************************/

noreturn_function void esp32s31_mmode_fault(uint32_t cause, uint32_t epc,
                                             uint32_t tval,
                                             const uint32_t *regs)
{
  ets_printf("\nS31SM:M-TRAP mcause=0x%08" PRIx32
             " mepc=0x%08" PRIx32 " mtval=0x%08" PRIx32
             " sscratch=0x%08" PRIx32 " sepc=0x%08" PRIx32
             " scause=0x%08" PRIx32 " sstatus=0x%08" PRIx32 "\n",
             cause, epc, tval, (uint32_t)READ_CSR(CSR_SCRATCH),
             (uint32_t)READ_CSR(CSR_EPC),
             (uint32_t)READ_CSR(CSR_CAUSE),
             (uint32_t)READ_CSR(CSR_STATUS));
  ets_printf("S31SM:FAULT-REGS a0=0x%08" PRIx32
             " a1=0x%08" PRIx32 " a2=0x%08" PRIx32
             " a3=0x%08" PRIx32 " a4=0x%08" PRIx32
             " a5=0x%08" PRIx32 " a6=0x%08" PRIx32
             " a7=0x%08" PRIx32 " ra=0x%08" PRIx32
             " sp=0x%08" PRIx32 "\n",
             regs[0], regs[1], regs[2], regs[3], regs[4],
             regs[5], regs[6], regs[7], regs[8], regs[9]);
  ets_printf("S31SM:CPU-APM filter=0x%08" PRIx32
             " r0_attr=0x%08" PRIx32 " m1_status=0x%08" PRIx32
             " m1_info0=0x%08" PRIx32 " m1_addr=0x%08" PRIx32 "\n",
             REG_READ(CPU_APM_REGION_FILTER_EN_REG),
             REG_READ(CPU_APM_REGION0_ATTR_REG),
             REG_READ(CPU_APM_M1_STATUS_REG),
             REG_READ(CPU_APM_M1_EXCEPTION_INFO0_REG),
             REG_READ(CPU_APM_M1_EXCEPTION_INFO1_REG));
  ets_printf("S31SM:PMA7 cfg=0x%08" PRIx32 " addr=0x%08" PRIx32 "\n",
             (uint32_t)RV_READ_CSR(CSR_PMACFG(7)),
             (uint32_t)RV_READ_CSR(CSR_PMAADDR(7)));
  ets_printf("S31SM:PMP cfg0=0x%08" PRIx32 " addr0=0x%08" PRIx32
             " psram_pms0=0x%08" PRIx32 " psram_reject=0x%08" PRIx32
             "\n", (uint32_t)READ_CSR(CSR_PMPCFG0),
             (uint32_t)READ_CSR(CSR_PMPADDR0),
             REG_READ(SPI_SMEM_S_PMS0_ATTR_REG),
             REG_READ(SPI_MEM_S_PMS_REJECT_REG));
  ets_printf("S31SM:machine mstatus=0x%08" PRIx32
             " misa=0x%08" PRIx32 " medeleg=0x%08" PRIx32
             " mideleg=0x%08" PRIx32 " satp=0x%08" PRIx32
             " tee_spriv=0x%08" PRIx32 "\n",
             (uint32_t)RV_READ_CSR(mstatus),
             (uint32_t)RV_READ_CSR(misa),
             (uint32_t)RV_READ_CSR(medeleg),
             (uint32_t)RV_READ_CSR(mideleg),
             (uint32_t)RV_READ_CSR(satp),
             REG_READ(TEE_SUPERVISOR_PRIV_SEL_REG));

  for (; ; )
    {
      __asm__ volatile ("wfi");
    }
}

/****************************************************************************
 * Name: esp32s31_prepare_hart_smode
 ****************************************************************************/

void IRAM_ATTR esp32s31_prepare_hart_smode(void)
{
  /* PMA CSRs are private to each hart.  ROM leaves PMA15 as one broad RX
   * cache window over Flash and PSRAM.  Replace it on every core with
   * separate Flash RX and PSRAM RWX apertures before that core enters
   * S-mode.  Sv32 remains responsible for per-page U/S and W^X policy.
   */

  PMA_ENTRY_CFG_RESET(15);
  PMA_RESET_AND_ENTRY_SET_NAPOT(6, SOC_IROM_LOW,
                                SOC_IROM_HIGH - SOC_IROM_LOW,
                                PMA_NAPOT | PMA_L | PMA_EN |
                                PMA_R | PMA_X);
  PMA_RESET_AND_ENTRY_SET_NAPOT(7, SOC_EXTRAM_LOW, SOC_EXTRAM_SIZE,
                                PMA_NAPOT | PMA_L | PMA_EN |
                                PMA_R | PMA_W | PMA_X);
  __asm__ volatile ("fence rw, rw" ::: "memory");
  __asm__ volatile ("fence.i" ::: "memory");
}

/****************************************************************************
 * Name: esp32s31_prepare_smode
 ****************************************************************************/

static void esp32s31_prepare_smode(void)
{
  uint32_t tee_supervisor;
  uint32_t apm_region0;

#ifdef CONFIG_ESP32S31_SMP
  /* CPU1 is bus master M1.  Its reset security mode is REE2, which cannot
   * execute the cached kernel image protected for TEE/REE0.  Classify M1
   * like CPU0's M0 before release; supervisor_priv_sel below still keeps
   * S-mode privileged and U-mode subject to the REE0 and Sv32 permissions. */

  REG_CLR_BIT(TEE_M1_MODE_CTRL_REG, TEE_M1_MODE_M);
#endif

  /* Treat S-mode as the privileged SoC execution environment while keeping
   * U-mode in the unprivileged environment selected by the Sv32 page tables.
   */

  tee_supervisor = REG_READ(TEE_SUPERVISOR_PRIV_SEL_REG);
#ifdef CONFIG_ESP32S31_SMP
  tee_supervisor |= TEE_CORE1_SUPERVISOR_PRIV_SEL;
#endif
  REG_WRITE(TEE_SUPERVISOR_PRIV_SEL_REG,
            tee_supervisor | TEE_CORE0_SUPERVISOR_PRIV_SEL);

  /* The reset-time CPU APM region covers the cached Flash/PSRAM windows, but
   * REE0 starts with no permissions.  Allow the S-mode kernel to use the
   * already initialized cache path; Sv32 later applies W^X and U permissions.
   */

  apm_region0 = REG_READ(CPU_APM_REGION0_ATTR_REG);
  REG_WRITE(CPU_APM_REGION0_ATTR_REG,
            apm_region0 | ESP32S31_CPU_APM_REE0_RWX);

  esp32s31_prepare_hart_smode();
}

void esp32s31_smode_irq_drop(int cpuint)
{
  register uint32_t a0 __asm__("a0") = 4;
  register uint32_t a1 __asm__("a1") = (uint32_t)cpuint;

  __asm__ volatile ("ecall"
                    : "+r"(a0), "+r"(a1)
                    :
                    : "a2", "a3", "a4", "a5", "a6", "a7",
                      "t0", "t1", "t2", "t3", "t4", "t5", "t6",
                      "memory");
}

static void esp32s31_nx_start_smode(void)
{
#ifdef CONFIG_BUILD_KERNEL
  esp32s31_mm_init();
#endif

  nx_start();
}

#endif

/****************************************************************************
 * Name: __esp_start
 ****************************************************************************/

void __esp_start(void)
{
  esp_err_t ret;
#if defined(CONFIG_ARCH_CHIP_ESP32S31) && \
    defined(CONFIG_ARCH_USE_S_MODE) && defined(CONFIG_ESP32S31_SMP)
  uintptr_t cpu0_percpu;
#endif

  esp_cpu_intr_set_ivt_addr(&_vector_table);

#if SOC_INT_CLIC_SUPPORTED
  /* When hardware vectored interrupts are enabled in CLIC,
   * the CPU jumps to this base address + 4 * interrupt_id.
   */

  esp_cpu_intr_set_mtvt_addr(&_mtvt_table);
#endif

#ifdef CONFIG_ESP_ROM_NEEDS_SET_CACHE_MMU_SIZE
  uint32_t _instruction_size;
  uint32_t instruction_page_offset;
  uint32_t cache_mmu_irom_size;
#endif

  bootloader_clear_bss_section();

#if defined(CONFIG_ARCH_CHIP_ESP32S31) && \
    defined(CONFIG_ARCH_USE_S_MODE) && defined(CONFIG_ESP32S31_SMP) && \
    defined(CONFIG_RISCV_PERCPU_SCRATCH)
  /* CONFIG_SMP makes the HAL's startup locks query this_cpu().  Establish
   * CPU0's scratch area before bootloader_init() reaches any such lock.
   * Some later ROM/HAL calls reset supervisor CSRs, so retain the pointer
   * and restore it immediately before entering S-mode. */

  riscv_percpu_add_hart(0);
  cpu0_percpu = READ_CSR(CSR_SCRATCH);
#endif

#ifdef CONFIG_ESPRESSIF_SIMPLE_BOOT
  ret = bootloader_init();
  if (ret != 0)
    {
      ets_printf("Hardware init failed, aborting: %d (0x%x)\n", ret, ret);
      while (true);
    }
#endif

  /* Initialize the per CPU areas */

#ifdef CONFIG_RISCV_PERCPU_SCRATCH
#  if !defined(CONFIG_ARCH_CHIP_ESP32S31) || \
      !defined(CONFIG_ARCH_USE_S_MODE)
  riscv_percpu_add_hart(0);
#  endif
#endif

#if defined(CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT) || \
    defined(CONFIG_ESPRESSIF_SIMPLE_BOOT)
  size_t partition_offset = PRIMARY_SLOT_OFFSET;
  uint32_t app_irom_start = partition_offset + (uint32_t)_image_irom_lma;
  uint32_t app_irom_size  = (uint32_t)_image_irom_size;
  uint32_t app_irom_vaddr = (uint32_t)_image_irom_vma;
  uint32_t app_drom_start = partition_offset + (uint32_t)_image_drom_lma;
  uint32_t app_drom_size  = (uint32_t)_image_drom_size;
  uint32_t app_drom_vaddr = (uint32_t)_image_drom_vma;

  if (map_rom_segments(app_drom_start, app_drom_vaddr, app_drom_size,
                       app_irom_start, app_irom_vaddr, app_irom_size) != 0)
    {
      ets_printf("Failed to setup XIP, aborting\n");
      while (true);
    }
#endif

#if CONFIG_ESP_ROM_NEEDS_SET_CACHE_MMU_SIZE
  _instruction_size = (uint32_t)&_instruction_reserved_end - \
                      (uint32_t)&_instruction_reserved_start;
  instruction_page_offset =
      (uint32_t)&_instruction_reserved_start &
      (SPI_FLASH_MMU_PAGE_SIZE - 1);
  cache_mmu_irom_size =
      ((instruction_page_offset + _instruction_size +
        SPI_FLASH_MMU_PAGE_SIZE - 1) / \
      SPI_FLASH_MMU_PAGE_SIZE) * sizeof(uint32_t);

  /* Configure the Cache MMU size for instruction and rodata in flash. */

  cache_set_idrom_mmu_size(cache_mmu_irom_size,
                           CACHE_DROM_MMU_MAX_END - cache_mmu_irom_size);
#endif /* CONFIG_ESP_ROM_NEEDS_SET_CACHE_MMU_SIZE */

#if CONFIG_ESP_SYSTEM_BBPLL_RECALIB
  recalib_bbpll();
#endif

#ifdef CONFIG_ESPRESSIF_REGION_PROTECTION
  /* Configure region protection */

  esp_cpu_configure_region_protection();
#endif

  /* Configure the power related stuff. */

  esp_rtc_init();

  esp_mspi_pin_init();

  /* Configure SPI Flash chip state */

  spi_flash_init_chip_state();

  esp_mmu_map_init();

#if CONFIG_ESP_ROM_NEEDS_SET_CACHE_MMU_SIZE
  /* esp_mmu_map_init() applies ESP-IDF's size-only calculation again.  NuttX
   * starts IROM at a non-page-aligned address, so restore the offset-aware
   * instruction-page count after that initialization. */

  cache_set_idrom_mmu_size(cache_mmu_irom_size,
                           CACHE_DROM_MMU_MAX_END - cache_mmu_irom_size);
#endif

#ifdef CONFIG_ESPRESSIF_SPIRAM
  ret = esp_psram_chip_init();
  if (ret != ESP_OK)
    {
#  ifndef CONFIG_ESPRESSIF_SPIRAM_IGNORE_NOTFOUND
      PANIC();
#  endif
    }

#  ifdef CONFIG_ESPRESSIF_SPIRAM_BOOT_INIT
  if (ret == ESP_OK)
    {
      ret = esp_psram_init();
      if (ret != ESP_OK)
        {
#    ifndef CONFIG_ESPRESSIF_SPIRAM_IGNORE_NOTFOUND
          PANIC();
#    endif
        }
    }
#  endif
#endif

  /* Configures the CPU clock, RTC slow and fast clocks, and performs
   * RTC slow clock calibration.
   */

  esp_clk_init();

  esp_mspi_pin_reserve();

  bootloader_init_mem();

#ifdef CONFIG_ESPRESSIF_SPIRAM_MEMTEST
  if (esp_psram_is_initialized() && !esp_psram_extram_test())
    {
#if defined(CONFIG_ARCH_CHIP_ESP32S31) && \
    defined(CONFIG_ARCH_USE_S_MODE)
      ets_printf("PSRAM failure: APM status=0x%08" PRIx32
                 " info0=0x%08" PRIx32 " addr=0x%08" PRIx32
                 " PMS=0x%08" PRIx32 " PMA7=0x%08" PRIx32
                 "/0x%08" PRIx32 "\n",
                 REG_READ(CPU_APM_M1_STATUS_REG),
                 REG_READ(CPU_APM_M1_EXCEPTION_INFO0_REG),
                 REG_READ(CPU_APM_M1_EXCEPTION_INFO1_REG),
                 REG_READ(SPI_MEM_S_PMS_REJECT_REG),
                 (uint32_t)RV_READ_CSR(CSR_PMACFG(7)),
                 (uint32_t)RV_READ_CSR(CSR_PMAADDR(7)));
#endif
      PANIC();
    }

#endif

#ifdef CONFIG_ESPRESSIF_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
  esp_psram_bss_init();
#endif

  /* Disable clock of unused peripherals */

  esp_perip_clk_init();

#ifdef CONFIG_ESPRESSIF_BROWNOUT_DET
  /* Initialize hardware brownout check and reset */

  esp_brownout_init();
#endif

  /* Configure the UART so we can get debug output */

#ifndef CONFIG_ARCH_CHIP_ESP32S31
  esp_lowsetup();
#else
  /* The S31 bootloader already configured the console.  Defer the full
   * NuttX UART setup until up_initialize(), after scheduler primitives exist.
   */
#endif

#ifdef USE_EARLYSERIALINIT
  /* Perform early serial initialization */

#ifndef CONFIG_ARCH_CHIP_ESP32S31
  riscv_earlyserialinit();
#endif
#endif

  esp_chip_revision_check();

  showprogress("A");

  /* Setup the syscall table needed by the ROM code */

  esp_setup_syscall_table();

  showprogress("B");

  /* The 2nd stage bootloader enables RTC WDT to monitor any issues that may
   * prevent the startup sequence from finishing correctly. Hence disable it
   * as NuttX is about to start.
   */

  wdt_hal_context_t rwdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();
  wdt_hal_write_protect_disable(&rwdt_ctx);
  wdt_hal_set_flashboot_en(&rwdt_ctx, false);
  wdt_hal_disable(&rwdt_ctx);
  wdt_hal_write_protect_enable(&rwdt_ctx);

  showprogress("C");

  /* Initialize onboard resources */

  esp_board_initialize();

  showprogress("D");

#if defined(CONFIG_ARCH_CHIP_ESP32S31) && \
    defined(CONFIG_ARCH_USE_S_MODE)
  esp32s31_prepare_smode();
#  ifdef CONFIG_ESP32S31_SMP
  {
    uint32_t cpu1_hartid;
    uint32_t cpu1_state;

    ret = esp32s31_cpu1_boot(&cpu1_hartid, &cpu1_state);
    if (ret < 0)
      {
        ets_printf("\nCPU1 bring-up failed: ret=%d hart=%u state=0x%08x\n",
                   ret, (unsigned int)cpu1_hartid,
                   (unsigned int)cpu1_state);
        PANIC();
      }
  }
#  endif
#  ifdef CONFIG_RISCV_PERCPU_SCRATCH
  /* Some ROM/HAL initialization paths reset supervisor CSRs.  Install the
   * NuttX sscratch/per-CPU pointer only after all of them have completed and
   * immediately before entering S-mode.
   */

#    ifdef CONFIG_ESP32S31_SMP
  WRITE_CSR(CSR_SCRATCH, cpu0_percpu);
  UP_DSB();
#    else
  riscv_percpu_add_hart(0);
#    endif
#  endif
  esp32s31_start_smode(esp32s31_nx_start_smode);
#else
#  ifdef CONFIG_ARCH_FPU
  /* The supervisor entry path enables the FPU while building its first
   * context frame.  A flat build never goes through it, so the unit has to
   * be turned on here or the first context save traps.
   */

  riscv_fpuconfig();
  showprogress("E");
#  endif

  showprogress("F");
  nx_start();
#endif

  UNUSED(ret);

  for (; ; );
}
