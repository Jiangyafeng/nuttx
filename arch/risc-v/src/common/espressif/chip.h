/****************************************************************************
 * arch/risc-v/src/common/espressif/chip.h
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

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_CHIP_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_CHIP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "esp_memorymap.h"

#include "riscv_internal.h"
#include "riscv_percpu.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Section for exception handler. */

#define EXCEPTION_SECTION .iram1

#ifdef __ASSEMBLY__

#if CONFIG_ARCH_INTERRUPTSTACK > 15 && defined(CONFIG_ARCH_USE_S_MODE)

/* The supervisor trap entry cannot use the interrupted stack directly.
 * Each hart receives its own interrupt-stack top when its per-CPU scratch
 * area is initialized.  Always use that value here: g_intstacktop is the
 * top of the complete SMP allocation and would make all harts share CPU0's
 * slice.
 */

.macro  esp_setintstack_bounds high, low
  blt   sp, \low, 1f
  bgt   sp, \high, 1f
  j     2f
1:
  mv    sp, \high
2:
.endm

.macro  setintstack tmp0, tmp1
  csrr    \tmp0, CSR_SCRATCH
  REGLOAD \tmp0, RISCV_PERCPU_IRQSTACK(\tmp0)
  li      \tmp1, INT_STACK_SIZE
  sub     \tmp1, \tmp0, \tmp1
  esp_setintstack_bounds \tmp0, \tmp1
.endm

#endif /* CONFIG_ARCH_INTERRUPTSTACK > 15 && CONFIG_ARCH_USE_S_MODE */
#endif /* __ASSEMBLY__ */

#endif /* __ARCH_RISCV_SRC_COMMON_ESPRESSIF_CHIP_H */
