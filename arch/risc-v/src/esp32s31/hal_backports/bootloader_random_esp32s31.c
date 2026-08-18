/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Backported from esp-hal-3rdparty sync/master.c.  This is kept local until
 * the NuttX release branch contains the ESP32-S31 RNG LL header.
 */

#include <stdbool.h>

#include "bootloader_random.h"
#include "soc/trng_struct.h"
#include "soc/lp_peri_clkrst_struct.h"

/* Added to trng_struct.h on sync/master.c after release/master.c. */

extern trng_dev_t LP_TRNG;

static inline void esp32s31_rng_enable_bus_clock(bool enable)
{
  LP_PERI_CLKRST.rng_ctrl.lp_rng_clk_en = enable;
}

static inline void esp32s31_rng_reset(void)
{
  LP_PERI_CLKRST.rng_ctrl.lp_rng_rst_en = 1;
  LP_PERI_CLKRST.rng_ctrl.lp_rng_rst_en = 0;
}

void bootloader_random_enable(void)
{
  esp32s31_rng_enable_bus_clock(true);
  esp32s31_rng_reset();
  LP_TRNG.date.clk_en = true;
  LP_TRNG.conf.sample_enable = true;
  LP_TRNG.conf.noise_crc_en = true;
}

void bootloader_random_disable(void)
{
  LP_TRNG.conf.sample_enable = false;
  LP_TRNG.conf.noise_crc_en = false;
  LP_TRNG.date.clk_en = false;
  esp32s31_rng_enable_bus_clock(false);
}
