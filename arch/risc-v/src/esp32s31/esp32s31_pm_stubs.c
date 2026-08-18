/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_pm_stubs.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include "esp_cpu.h"

#if !defined(CONFIG_ESPRESSIF_DFS) && !defined(CONFIG_ESPRESSIF_AUTO_SLEEP)
void esp_pm_impl_idle_hook(void)
{
}

void esp_pm_impl_waiti(void)
{
  esp_cpu_wait_for_intr();
}
#endif
