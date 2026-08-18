/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_intctx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/sched.h>

#include <arch/mode.h>

#include "riscv_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP32S31_CLIC_CAUSE_REG_NDX  (REG_INT_CTX + 1)
#define ESP32S31_CLIC_CAUSE_XPPS     UINT32_C(0x10000000)
#define ESP32S31_CLIC_CAUSE_XPIE     UINT32_C(0x08000000)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void riscv_initial_extctx_state(struct tcb_s *tcb)
{
  uintptr_t status = tcb->xcp.regs[REG_INT_CTX];
  uintptr_t cause = 0;

  /* New tasks first enter the S-mode trampoline.  Mirror that initial
   * sstatus state into the S31 CLIC return fields.  riscv_jump_to_user later
   * clears sstatus.SPP but deliberately retains this supervisor-originated
   * CLIC state for the first S-to-U return.
   */

  if ((status & STATUS_PPP) != 0)
    {
      cause |= ESP32S31_CLIC_CAUSE_XPPS;
    }

  if ((status & STATUS_PIE) != 0)
    {
      cause |= ESP32S31_CLIC_CAUSE_XPIE;
    }

  tcb->xcp.regs[ESP32S31_CLIC_CAUSE_REG_NDX] = cause;
}
