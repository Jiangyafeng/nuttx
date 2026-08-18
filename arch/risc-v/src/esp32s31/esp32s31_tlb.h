/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_tlb.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32S31_ESP32S31_TLB_H
#define __ARCH_RISCV_SRC_ESP32S31_ESP32S31_TLB_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* The mapping-side entry points are the common riscv_tlb_shootdown_range()
 * and riscv_tlb_shootdown_all() declared in riscv_mmu.h.  Only the two hooks
 * below are chip specific.
 */

#ifdef CONFIG_ESP32S31_SMP_TLB_SHOOTDOWN

/* Service any inbound shootdown request for this hart.  Must be callable
 * with interrupts disabled (IPI handler and local wait-spin paths).
 */

void esp32s31_tlb_shootdown_process(void);

/* Register /dev/tlbshoot so smoke tests can read send/ack/timeout counts. */

int esp32s31_tlb_register_stats_device(void);

#else

static inline void esp32s31_tlb_shootdown_process(void)
{
}

static inline int esp32s31_tlb_register_stats_device(void)
{
  return 0;
}

#endif /* CONFIG_ESP32S31_SMP_TLB_SHOOTDOWN */

#endif /* __ARCH_RISCV_SRC_ESP32S31_ESP32S31_TLB_H */
