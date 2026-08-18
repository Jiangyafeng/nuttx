/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_monstat.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32S31_ESP32S31_MONSTAT_H
#define __ARCH_RISCV_SRC_ESP32S31_ESP32S31_MONSTAT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_ARCH_USE_S_MODE

/* Register /dev/s31stat.  It reports the M-mode monitor's interrupt deferral
 * and replay counters, the Wi-Fi compensation poller's own activity, and the
 * address environment create/destroy balance.  These are the three things that
 * cannot be derived from NSH output: a replayed interrupt is architecturally
 * indistinguishable from a directly forwarded one, and heap deltas cannot
 * separate a leaked address environment from allocator fragmentation.
 */

int esp32s31_monstat_register_device(void);

#else

static inline int esp32s31_monstat_register_device(void)
{
  return 0;
}

#endif /* CONFIG_ARCH_USE_S_MODE */

#endif /* __ARCH_RISCV_SRC_ESP32S31_ESP32S31_MONSTAT_H */
