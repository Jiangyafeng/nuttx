/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_cpu1_boot.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32S31_ESP32S31_CPU1_BOOT_H
#define __ARCH_RISCV_SRC_ESP32S31_ESP32S31_CPU1_BOOT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifndef __ASSEMBLY__
#  include <stdint.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* CPU1 publishes its progress here so CPU0 can wait with a bound and report
 * exactly where a failed bring-up stopped.
 */

#define ESP32S31_CPU1_BOOT_STARTING      0x43505510
#define ESP32S31_CPU1_SMP_READY          0x43505519
#define ESP32S31_CPU1_SMP_ONLINE         0x4350551a
#define ESP32S31_CPU1_SMP_OFFLINE        0x4350551b
#define ESP32S31_CPU1_SMP_TIMEOUT        0x4350551c
#define ESP32S31_CPU1_BOOT_SMODE_FAULT   0x4350551d
#define ESP32S31_CPU1_BOOT_BAD_HART      0x4350551e
#define ESP32S31_CPU1_BOOT_MMODE_FAULT   0x4350551f

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__
extern volatile uint32_t g_esp32s31_cpu1_boot_state;
extern volatile uint32_t g_esp32s31_cpu1_hartid;
extern volatile uint32_t g_esp32s31_cpu1_smp_release;

int esp32s31_cpu1_boot(uint32_t *hartid, uint32_t *state);
noreturn_function void esp32s31_cpu1_smp_entry(void);
#endif

#endif /* __ARCH_RISCV_SRC_ESP32S31_ESP32S31_CPU1_BOOT_H */
