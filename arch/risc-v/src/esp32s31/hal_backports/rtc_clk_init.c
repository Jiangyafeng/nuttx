/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Backported from esp-hal-3rdparty sync/master.c.  The release/master.c
 * snapshot currently used by NuttX predates the ESP32-S31 implementation.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "sdkconfig.h"
#include "esp32s31/rom/ets_sys.h"
#include "esp32s31/rom/rtc.h"
#include "soc/rtc.h"
#include "esp_cpu.h"
#include "regi2c_ctrl.h"
#include "soc/regi2c_dig_reg.h"
#include "soc/lp_clkrst_reg.h"
#include "soc/lp_system_reg.h"
#include "soc/pmu_reg.h"
#include "soc/chip_revision.h"
#include "esp_hw_log.h"
#include "esp_rom_serial_output.h"
#include "esp_private/esp_pmu.h"
#include "hal/clk_tree_ll.h"
#include "hal/efuse_hal.h"

ESP_HW_LOG_ATTR_TAG(TAG, "rtc_clk_init");

/* release/master.c exports the S31 implementation name but not the public
 * wrapper consumed by regi2c_ctrl.c.  The implementation is still marked as
 * a TODO by Espressif, so keep this compatibility shim visibly local.
 */

extern void regi2c_write_mask_impl(uint8_t block, uint8_t host_id,
                                   uint8_t reg_add, uint8_t msb,
                                   uint8_t lsb, uint8_t data);

void esp_rom_regi2c_write_mask(uint8_t block, uint8_t host_id,
                               uint8_t reg_add, uint8_t msb,
                               uint8_t lsb, uint8_t data)
{
  regi2c_write_mask_impl(block, host_id, reg_add, msb, lsb, data);
}

void rtc_clk_init(rtc_clk_config_t cfg)
{
  rtc_cpu_freq_config_t old_config;
  rtc_cpu_freq_config_t new_config;
  uint32_t freq_before;
  bool res;

  REGI2C_WRITE_MASK(I2C_DIG_REG, I2C_DIG_REG_SCK_DCAP,
                    cfg.slow_clk_dcap);
  REGI2C_WRITE_MASK(I2C_DIG_REG, I2C_DIG_REG_ENIF_RTC_DREG, 1);
  REGI2C_WRITE_MASK(I2C_DIG_REG, I2C_DIG_REG_ENIF_DIG_DREG, 1);
  REGI2C_WRITE_MASK(I2C_DIG_REG, I2C_DIG_REG_XPD_RTC_REG, 0);
  REGI2C_WRITE_MASK(I2C_DIG_REG, I2C_DIG_REG_XPD_DIG_REG, 0);

  rtc_clk_cpu_freq_get_config(&old_config);
  freq_before = old_config.freq_mhz;
  res = rtc_clk_cpu_freq_mhz_to_config(cfg.cpu_freq_mhz, &new_config);
  if (!res)
    {
      ESP_HW_LOGE(TAG, "invalid CPU frequency value");
      abort();
    }

  rtc_clk_cpu_freq_set_config(&new_config);
  esp_cpu_set_cycle_count((uint64_t)esp_cpu_get_cycle_count() *
                          cfg.cpu_freq_mhz / freq_before);

  if (cfg.slow_clk_src == SOC_RTC_SLOW_CLK_SRC_XTAL32K)
    {
      rtc_clk_32k_enable(true);
    }

  rtc_clk_8m_enable(true);
  rtc_clk_fast_src_set(cfg.fast_clk_src);
  rtc_clk_slow_src_set(cfg.slow_clk_src);
}
