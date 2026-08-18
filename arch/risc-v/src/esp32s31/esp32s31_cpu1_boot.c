/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_cpu1_boot.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "hal/cpu_utility_ll.h"
#include "rom/ets_sys.h"

#include "esp32s31_cpu1_boot.h"

/****************************************************************************
 * Public Data
 ****************************************************************************/

volatile uint32_t g_esp32s31_cpu1_boot_state;
volatile uint32_t g_esp32s31_cpu1_hartid;
volatile uint32_t g_esp32s31_cpu1_smp_release;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

extern void esp32s31_cpu1_boot_entry(void);

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s31_cpu1_boot
 *
 * Description:
 *   Release CPU1 and wait, with a bound, for it to reach its S-mode entry.
 *   CPU1 parks there until up_cpu_start() sets g_esp32s31_cpu1_smp_release.
 *
 * Input Parameters:
 *   hartid - If non-NULL, receives the mhartid CPU1 reported.
 *   state  - If non-NULL, receives the last published bring-up state, which
 *            tells a failed boot apart from a hart that never started.
 *
 * Returned Value:
 *   Zero on success, -ETIMEDOUT if CPU1 never left STARTING, -EIO if it
 *   stopped in a fault state.
 *
 ****************************************************************************/

int IRAM_ATTR esp32s31_cpu1_boot(uint32_t *hartid, uint32_t *state)
{
  uint32_t timeout;

  g_esp32s31_cpu1_hartid = UINT32_MAX;
  g_esp32s31_cpu1_boot_state = ESP32S31_CPU1_BOOT_STARTING;
  g_esp32s31_cpu1_smp_release = 0;
  __asm__ volatile ("fence rw, rw" ::: "memory");

  /* Follow the S31 IDF startup ordering.  CPU1 initially waits in ROM for
   * ets_set_appcpu_boot_addr() after its stall, clock and reset gates have
   * been released.
   */

  esp_cpu_unstall(1);
  cpu_utility_ll_enable_clock_and_reset_app_cpu();
  cpu_utility_ll_enable_clock_and_reset_app_cpu_int_matrix();
  ets_set_appcpu_boot_addr((uint32_t)esp32s31_cpu1_boot_entry);

  for (timeout = CONFIG_ESP32S31_CPU1_BOOT_TIMEOUT_US;
       timeout > 0 &&
       g_esp32s31_cpu1_boot_state == ESP32S31_CPU1_BOOT_STARTING;
       timeout--)
    {
      esp_rom_delay_us(1);
    }

  __asm__ volatile ("fence rw, rw" ::: "memory");

  if (hartid != NULL)
    {
      *hartid = g_esp32s31_cpu1_hartid;
    }

  if (state != NULL)
    {
      *state = g_esp32s31_cpu1_boot_state;
    }

  if (g_esp32s31_cpu1_boot_state == ESP32S31_CPU1_SMP_READY)
    {
      return 0;
    }

  if (g_esp32s31_cpu1_boot_state == ESP32S31_CPU1_BOOT_STARTING)
    {
      return -ETIMEDOUT;
    }

  return -EIO;
}
