/****************************************************************************
 * arch/risc-v/src/esp32s31/riscv_extctx.S
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* ESP32-S31 uses a CLIC implementation whose xcause register contains the
 * previous interrupt level and xret return state.  Espressif's IDF vector
 * code therefore saves and restores the complete xcause value around every
 * context switch.  Keep it as the first RISC-V integer extension register.
 */

#define REG_S31_CLIC_CAUSE  (INT_REG_SIZE * (REG_INT_CTX_NDX + 1))
#define S31_CLIC_CAUSE_RET_MASK  0xc7ffffff
#define S31_CLIC_CAUSE_XPPS      0x10000000
#define S31_CLIC_CAUSE_XPIE      0x08000000

.macro save_extctx in
  /* xret clears its live XPP state after consuming it, so a task-context
   * save cannot copy xcause blindly.  Preserve the CLIC reason, MINHV and
   * XPIL fields, but derive XPP/XPIE from the context's saved xstatus just
   * as the architectural trap entry would have done.
   */

  csrr       t0, CSR_CAUSE
  li         t1, S31_CLIC_CAUSE_RET_MASK
  and        t0, t0, t1
  REGLOAD    t1, REG_INT_CTX(\in)
  andi       t2, t1, STATUS_PPP
  beqz       t2, 7f
  li         t2, S31_CLIC_CAUSE_XPPS
  or         t0, t0, t2
7:
  andi       t1, t1, STATUS_PIE
  beqz       t1, 7f
  li         t1, S31_CLIC_CAUSE_XPIE
  or         t0, t0, t1
7:
  REGSTORE   t0, REG_S31_CLIC_CAUSE(\in)
.endm

.macro save_extctx_task in
  /* A software-created scheduler frame did not enter through CLIC, so it
   * has no interrupt reason or previous interrupt level to retain.  Build
   * only the return privilege/enable fields consumed by xret.
   */

  li         t0, 0
  REGLOAD    t1, REG_INT_CTX(\in)
  andi       t2, t1, STATUS_PPP
  beqz       t2, 7f
  li         t2, S31_CLIC_CAUSE_XPPS
  or         t0, t0, t2
7:
  andi       t1, t1, STATUS_PIE
  beqz       t1, 7f
  li         t1, S31_CLIC_CAUSE_XPIE
  or         t0, t0, t1
7:
  REGSTORE   t0, REG_S31_CLIC_CAUSE(\in)
.endm

.macro load_extctx out
  REGLOAD    t0, REG_S31_CLIC_CAUSE(\out)
  csrw       CSR_CAUSE, t0
.endm
