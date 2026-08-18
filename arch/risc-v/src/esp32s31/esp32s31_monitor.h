/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_monitor.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32S31_ESP32S31_MONITOR_H
#define __ARCH_RISCV_SRC_ESP32S31_ESP32S31_MONITOR_H

/* Private S-mode to M-mode monitor ABI.  Service 2 deliberately accepts a
 * local external CLIC source number rather than an MMIO address.  The monitor
 * computes the per-hart register address and rejects changes outside the CLIC
 * fields that the S-mode interrupt adapter owns.
 */

#define ESP32S31_MCALL_CLIC_CONFIG          2
#define ESP32S31_MCALL_CLIC_SOURCE_COUNT    32
#define ESP32S31_MCALL_CLIC_CTRL_BASE       0x10801040
#define ESP32S31_MCALL_CLIC_WRITABLE_MASK   0xff070101
#define ESP32S31_MCALL_CLIC_RESERVED_MASK   0x00f8fefe

#endif /* __ARCH_RISCV_SRC_ESP32S31_ESP32S31_MONITOR_H */
