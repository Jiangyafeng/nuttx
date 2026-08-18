/****************************************************************************
 * arch/risc-v/src/esp32s31/include/esp32s31/rom/ets_sys.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* The initial ESP32-S31 ROM header exports a legacy STATUS enum containing
 * the unscoped name OK.  NuttX already defines OK in sys/types.h.  Keep the
 * ROM ABI/type intact while giving its enumerators target-private names.
 */

#ifndef __ARCH_RISCV_SRC_ESP32S31_INCLUDE_ESP32S31_ROM_ETS_SYS_H
#define __ARCH_RISCV_SRC_ESP32S31_INCLUDE_ESP32S31_ROM_ETS_SYS_H

#include <sys/types.h>

#define OK      ESP32S31_ROM_STATUS_OK
#define FAIL    ESP32S31_ROM_STATUS_FAIL
#define PENDING ESP32S31_ROM_STATUS_PENDING
#define BUSY    ESP32S31_ROM_STATUS_BUSY
#define CANCEL  ESP32S31_ROM_STATUS_CANCEL

#include <rom/ets_sys.h>

#undef OK
#undef FAIL
#undef PENDING
#undef BUSY
#undef CANCEL

#endif
