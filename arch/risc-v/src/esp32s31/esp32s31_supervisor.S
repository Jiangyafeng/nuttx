/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_supervisor.S
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <arch/irq.h>

#include "esp32s31_monitor.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP32S31_PMP_ALL_RWX       0x1f
#define ESP32S31_MEDELEG_NUTTX     0xb1ff
#define ESP32S31_MIDELEG_STANDARD  0x0222
#define ESP32S31_SINTTHRESH_ENABLE 0x001f
#define ESP32S31_MINTTHRESH_MONITOR 0x00ff
#define ESP32S31_MHCR_BRANCH_MASK  0x1030
#define ESP32S31_MCAUSE_XPPS       0x10000000
#define ESP32S31_CPU_CORES          2
#define ESP32S31_MSTACK_SIZE        1024

/* Per-core monitor instrumentation.  The deferral path is the only place that
 * can observe a forwarded CLIC input being withheld from S-mode, and the
 * replay paths are the only places that can observe it being handed over, so
 * the conservation check between them has to be counted here rather than in
 * the kernel.  Eight words per core keeps the index scaling a single shift.
 */

#define ESP32S31_MONSTAT_SHIFT      5
#define ESP32S31_MONSTAT_DEFER      0
#define ESP32S31_MONSTAT_REPLAY     1
#define ESP32S31_MONSTAT_ACK        2
#define ESP32S31_MONSTAT_DROP       3
#define ESP32S31_MONSTAT_SREPLAY    4

/****************************************************************************
 * Public Symbols
 ****************************************************************************/

  .global esp32s31_start_smode
  .type   esp32s31_start_smode, function

  .global esp32s31_machine_trap
  .type   esp32s31_machine_trap, function

  .global esp32s31_supervisor_trap
  .type   esp32s31_supervisor_trap, function

  .extern _vector_table
  .extern _mtvt_table
  .extern esp32s31_mmode_fault
  .extern esp32s31_supervisor_return_ecall
  .extern esp32s31_user_return_ecall
  .extern g_interrupt_context
#ifdef CONFIG_ESP32S31_SMP
  .extern esp32s31_cpu1_supervisor_trap
#endif

/****************************************************************************
 * Assembly Macros
 ****************************************************************************/

  .macro esp32s31_percore_address destination, scratch, symbol
  la      \destination, \symbol
  csrr    \scratch, mhartid
  slli    \scratch, \scratch, 2
  add     \destination, \destination, \scratch
  .endm

/* Increment one per-core monitor counter.  Both operands are clobbered.  Every
 * use site sits where t0-t2 are already dead, so no counter update can perturb
 * the interrupted context.
 */

  .macro esp32s31_monstat_bump index, address, scratch
  la      \address, g_esp32s31_machine_monstat
  csrr    \scratch, mhartid
  slli    \scratch, \scratch, ESP32S31_MONSTAT_SHIFT
  add     \address, \address, \scratch
  lw      \scratch, ((\index) * 4)(\address)
  addi    \scratch, \scratch, 1
  sw      \scratch, ((\index) * 4)(\address)
  .endm

/****************************************************************************
 * Machine-mode monitor stack
 ****************************************************************************/

  .section .bss.esp32s31_mstack, "aw", @nobits
  .balign 16
.Lmachine_stacks:
  .space (ESP32S31_MSTACK_SIZE * ESP32S31_CPU_CORES)
  .global g_esp32s31_machine_forward_mask
  .type   g_esp32s31_machine_forward_mask, object
g_esp32s31_machine_forward_mask:
  .space (4 * ESP32S31_CPU_CORES)
  .size   g_esp32s31_machine_forward_mask, (4 * ESP32S31_CPU_CORES)
  .global g_esp32s31_machine_flash_mask
  .type   g_esp32s31_machine_flash_mask, object
g_esp32s31_machine_flash_mask:
  .space (4 * ESP32S31_CPU_CORES)
  .size   g_esp32s31_machine_flash_mask, (4 * ESP32S31_CPU_CORES)
  .global g_esp32s31_machine_monstat
  .type   g_esp32s31_machine_monstat, object
g_esp32s31_machine_monstat:
  .space ((1 << ESP32S31_MONSTAT_SHIFT) * ESP32S31_CPU_CORES)
  .size   g_esp32s31_machine_monstat, \
          ((1 << ESP32S31_MONSTAT_SHIFT) * ESP32S31_CPU_CORES)
/****************************************************************************
 * Machine-mode startup trampoline
 ****************************************************************************/

  .section .iram1, "ax", @progbits
  .balign 4

/****************************************************************************
 * Name: esp32s31_start_smode
 *
 * Input Parameters:
 *   a0 - Supervisor-mode entry point.  This function does not return.
 ****************************************************************************/

esp32s31_start_smode:
  mv      t6, a0

  /* Install the small M-mode monitor before changing delegation state. */

  la      t0, esp32s31_machine_trap
  ori     t0, t0, 3
  csrw    mtvec, t0
  la      t0, .Lmachine_stacks
  csrr    t1, mhartid
  addi    t1, t1, 1
  slli    t1, t1, 10
  add     t0, t0, t1
  csrw    mscratch, t0

  /* A vertical S-to-M exception enters CLIC M-mode at interrupt level zero.
   * Keep the monitor itself non-preemptible so a newly enabled CLIC input
   * cannot overwrite mepc/mcause in the middle of an ecall service.  The
   * machine threshold is ignored while executing in S-mode, so pending
   * M-mode inputs become eligible atomically with mret.
   */

  li      t0, ESP32S31_MINTTHRESH_MONITOR
  csrw    CSR_MINTTHRESH, t0

  /* S-mode is allowed to address the SoC physical map.  Sv32 page tables
   * provide the fine-grained kernel/user permissions after MMU bring-up.
   */

  li      t0, -1
  csrw    pmpaddr0, t0
  li      t0, ESP32S31_PMP_ALL_RWX
  csrw    pmpcfg0, t0

  /* Permit S/U reads of cycle/time/instret counters. */

  li      t0, -1
  csrw    mcounteren, t0

  /* Do not inherit ROM/IDF CLIC enables into the S-mode kernel.  A pending
   * source could otherwise trap before NuttX has installed its handlers.
   * External CLIC inputs occupy IDs 16-47 and each byte IE register is four
   * bytes apart.  NuttX re-enables allocated sources through the M-call ABI.
   */

  li      t0, 0x10801041
  li      t1, 32
.Ldisable_external_clic:
  sb      zero, 0(t0)
  addi    t0, t0, 4
  addi    t1, t1, -1
  bnez    t1, .Ldisable_external_clic

  /* Delegate synchronous faults and U-mode ecall to the S-mode kernel.
   * S-mode ecall deliberately remains in M-mode for monitor diagnostics.
   */

  li      t0, ESP32S31_MEDELEG_NUTTX
  csrw    medeleg, t0
  li      t0, ESP32S31_MIDELEG_STANDARD
  csrw    mideleg, t0

  /* Configure the supervisor CLIC exception and hardware-vector tables. */

#ifdef CONFIG_ESP32S31_SMP
  csrr    t1, mhartid
  beqz    t1, 1f
  la      t0, esp32s31_cpu1_supervisor_trap
  j       2f
1:
#endif
  la      t0, _vector_table
#ifdef CONFIG_ESP32S31_SMP
2:
#endif
  ori     t0, t0, 3
  csrw    stvec, t0
  la      t0, _mtvt_table
  csrw    CSR_STVT, t0
  li      t0, ESP32S31_SINTTHRESH_ENABLE
  csrw    CSR_SINTTHRESH, t0

  /* ESP32-S31's CLIC does not implement the standard sie CSR (0x104).
   * Per-source enables live in clicintie and the global S-mode gate is
   * sstatus.SIE, so no sie write is needed here.
   */

  csrc    sstatus, SSTATUS_SIE
  li      t0, SSTATUS_SUM
  csrs    sstatus, t0

  /* Start with address translation disabled.  The later Kernel Build stage
   * installs the kernel root table and changes satp to Sv32 in S-mode.
   */

  csrw    satp, zero
  sfence.vma zero, zero

  /* mstatus.MPP = S, with M/S interrupts disabled until NuttX initializes
   * its interrupt subsystem.
   */

  csrr    t0, mstatus
  li      t1, 0xffffe777
  and     t0, t0, t1
  /* Official S31 wireless blobs use ilp32f.  Enable the F extension before
   * the first S-mode context frame is created; riscv_fpuconfig() will clear
   * fcsr and finish initialization later in normal NuttX startup. */
  li      t1, 0x00002800
  or      t0, t0, t1
  csrw    mstatus, t0
  csrw    mepc, t6
  mret

  .size esp32s31_start_smode, .-esp32s31_start_smode

/****************************************************************************
 * Name: esp32s31_supervisor_trap
 *
 * Description:
 *   Enter NuttX's normal S-mode exception vector after the M-mode monitor
 *   has recreated the architectural supervisor trap state.
 ****************************************************************************/

  .balign 4
esp32s31_supervisor_trap:
  j       _vector_table

  .size esp32s31_supervisor_trap, .-esp32s31_supervisor_trap

/****************************************************************************
 * Name: esp32s31_machine_trap
 *
 * Description:
 *   Provide three narrow M-mode services:
 *
 *   1. Proxy CLIC configuration accesses made by the S-mode HAL.
 *   2. Forward the M-only CLIC interrupt into NuttX as a synthetic S-mode
 *      trap.  ESP32-S31 hardwires clicintattr.mode to M, so an architectural
 *      mideleg write cannot move these interrupt causes to S-mode.
 *   3. Forward synchronous exceptions originating in U-mode when the S31's
 *      WARL medeleg implementation cannot delegate their cause directly.
 *
 *   Any other M-mode trap is diagnosed and stops the core.
 ****************************************************************************/

  .balign 0x40
esp32s31_machine_trap:
  /* Preserve every S/U-mode register touched by the shim.  mscratch holds
   * the private M-mode stack while normal execution uses the NuttX stack.
   */

  csrrw   sp, mscratch, sp
  addi    sp, sp, -16
  sw      t0, 0(sp)
  sw      t1, 4(sp)
  sw      t2, 8(sp)

  csrr    t0, mcause
  bltz    t0, .Lmachine_forward_interrupt

  /* S31 implements only a subset of medeleg (U-ecall and page faults on the
   * tested revision).  Forward any other synchronous exception taken from
   * U-mode so NuttX can apply its normal per-process fault policy.  Faults
   * from S/M-mode still indicate a monitor or kernel failure and are fatal.
   */

  csrr    t1, mstatus
  li      t2, MSTATUS_MPP_MASK
  and     t1, t1, t2
  beqz    t1, .Lmachine_forward_exception

  andi    t1, t0, 0x3f
  li      t2, 9
  bne     t1, t2, .Lmachine_trap_fault

  /* return_from_exception enters this service only after it has restored
   * every U-mode general register and installed the user EPC/status.  The
   * call therefore cannot use a0 as an ABI selector.  Recognize its unique
   * PC before dispatching the ordinary monitor services.
   */

  csrr    t1, mepc
  la      t2, esp32s31_user_return_ecall
  beq     t1, t2, .Lmachine_user_return
  la      t2, esp32s31_supervisor_return_ecall
  beq     t1, t2, .Lmachine_supervisor_return

  /* S-mode ecall is the narrow monitor ABI used for CLIC registers that are
   * physically M-mode-only on ESP32-S31:
   *
   *   a0 = 2: configure local CLIC source a1, clearing fields a2 and setting
   *           fields a3; return status in a0 and the register value in a1
   *   a0 = 3: hand one deferred input back to S-mode as a supervisor trap
   *   a0 = 4: forget deferred CLIC input a1, leaving it masked
   *   a0 = 5: mask external CLIC inputs and disable branch prediction while
   *           the Flash cache is off
   *   a0 = 6: re-enable branch prediction and restore the external CLIC mask
   *           after restoring the Flash cache
   *   a0 = 7: acknowledge and re-enable CLIC input a1 after its S-mode ISR
   *   a0 = 9: confirm that the caller entered the monitor from S-mode
   */

  li      t1, 3
  beq     a0, t1, .Lmachine_irq_reenable
  li      t1, 4
  beq     a0, t1, .Lmachine_irq_drop
  li      t1, 5
  beq     a0, t1, .Lmachine_branch_disable
  li      t1, 6
  beq     a0, t1, .Lmachine_branch_enable
  li      t1, 7
  beq     a0, t1, .Lmachine_irq_ack
  li      t1, 9
  beq     a0, t1, .Lmachine_confirm_smode
  li      t1, ESP32S31_MCALL_CLIC_CONFIG
  bne     a0, t1, .Lmachine_ecall_unknown

.Lmachine_clic_config:
  /* Only the 32 external inputs owned by the local hart may be addressed.
   * Reject attempts to modify the hardwired privilege mode or reserved bits.
   * A zero clear/set mask is a side-effect-free query for the HAL getters.
   */

  li      t0, ESP32S31_MCALL_CLIC_SOURCE_COUNT
  bgeu    a1, t0, .Lmachine_ecall_invalid
  or      t0, a2, a3
  li      t1, ESP32S31_MCALL_CLIC_RESERVED_MASK
  and     t0, t0, t1
  bnez    t0, .Lmachine_ecall_invalid

  slli    t0, a1, 2
  li      t1, ESP32S31_MCALL_CLIC_CTRL_BASE
  add     t0, t0, t1
  lw      t1, 0(t0)
  or      t2, a2, a3
  beqz    t2, 1f
  not     t2, a2
  and     t1, t1, t2
  or      t1, t1, a3
  sw      t1, 0(t0)
  lw      t1, 0(t0)
1:
  mv      a1, t1
  li      a0, 0
  j       .Lmachine_ecall_return

.Lmachine_confirm_smode:
  li      a0, 1
  j       .Lmachine_ecall_return

.Lmachine_irq_reenable:
  /* Re-enable CLIC inputs that were masked while their interrupt was being
   * serviced in S-mode.  Callers use this after the peripheral ISR has
   * cleared or re-armed its source.
   */

  esp32s31_percore_address t0, t2, g_esp32s31_machine_forward_mask
  lw      t1, 0(t0)
#ifdef CONFIG_ESP32S31_SMP
  /* SIE can be restored while riscv_doirq() is still dispatching the outer
   * interrupt.  NuttX does not support nested interrupts, so do not
   * synthesize another S trap until its per-CPU interrupt-context flag is
   * clear.  Reading the byte directly also keeps this M-mode path free from
   * scheduler/helper calls. */

  la      t0, g_interrupt_context
  csrr    t2, mhartid
  add     t0, t0, t2
  lbu     t0, 0(t0)
  bnez    t0, .Lmachine_irq_reenable_empty

  /* Do not expose an asserted M-mode CLIC input at an ecall-return boundary.
   * S31 can tail-chain it before mret has committed the S-mode return state,
   * replacing mepc with an intermediate monitor value.  Instead, consume
   * the ecall and recreate the architectural state of an S-mode interrupt.
   * The S-mode handler clears the peripheral source and service 7 re-enables
   * the hardware input afterwards.
   */

  beqz    t1, .Lmachine_irq_reenable_empty
  li      a1, 0
  mv      t2, t1
.Lmachine_irq_deferred_find:
  andi    t0, t2, 1
  bnez    t0, .Lmachine_irq_deferred_found
  srli    t2, t2, 1
  addi    a1, a1, 1
  j       .Lmachine_irq_deferred_find

.Lmachine_irq_deferred_found:
  /* Remove this input from the deferred mask.  Any other pending input stays
   * masked until a later task-context service-3 call. */

  li      t0, 1
  sll     t0, t0, a1
  not     t0, t0
  and     t1, t1, t0
  esp32s31_percore_address t0, t2, g_esp32s31_machine_forward_mask
  sw      t1, 0(t0)
  esp32s31_monstat_bump ESP32S31_MONSTAT_REPLAY, t0, t1

  addi    t2, a1, 16
  li      t0, 0x80000000
  or      t2, t2, t0
  csrw    scause, t2

  csrr    t0, mepc
  addi    t0, t0, 4
  csrw    sepc, t0
  csrw    stval, zero

  /* Model a supervisor trap taken from the currently executing S-mode
   * monitor caller.  Preserve its SIE state in SPIE and force SPP=S so sret
   * resumes the interrupted exception-exit or task-context code.
   */

  csrr    t1, sstatus
  andi    t1, t1, SSTATUS_SIE
  li      t0, (SSTATUS_SIE | SSTATUS_SPIE | SSTATUS_SPPS)
  csrc    sstatus, t0
  beqz    t1, 5f
  li      t0, SSTATUS_SPIE
  csrs    sstatus, t0
5:
  li      t0, SSTATUS_SPPS
  csrs    sstatus, t0

  li      t0, MSTATUS_MPP_MASK
  csrc    mstatus, t0
  li      t0, MSTATUS_MPPS
  csrs    mstatus, t0
#ifdef CONFIG_ESP32S31_SMP
  csrr    t0, mhartid
  bnez    t0, 6f
#endif
  la      t0, esp32s31_supervisor_trap
#ifdef CONFIG_ESP32S31_SMP
  j       7f
6:
  la      t0, esp32s31_cpu1_supervisor_trap
7:
#endif
  csrw    mepc, t0
  j       .Lmachine_trap_return

.Lmachine_irq_reenable_empty:
  li      a0, 0
  j       .Lmachine_ecall_return
#else
  /* The established single-core path can safely restore all deferred inputs
   * after clearing the mask.  Keep the SMP-only synthetic trap replay out of
   * production: it changes the ecall return state and can corrupt a U-mode
   * syscall frame even though no second scheduler CPU exists.
   */

  sw      zero, 0(t0)
  li      t0, 0x10801041
  li      t2, 32
.Lmachine_irq_reenable_loop:
  andi    a1, t1, 1
  beqz    a1, 1f
  li      a1, 1
  sb      a1, 0(t0)
1:
  srli    t1, t1, 1
  addi    t0, t0, 4
  addi    t2, t2, -1
  bnez    t2, .Lmachine_irq_reenable_loop
  li      a0, 0
  j       .Lmachine_ecall_return
#endif

.Lmachine_irq_drop:
  /* Keep the unclaimed/stale CLIC input in a1 disabled.  A later NuttX
   * interrupt allocation will explicitly enable the line if it becomes
   * valid.
   *
   * Only that one input may be forgotten.  Every other bit in the deferred
   * set names an input whose hardware enable this monitor has already
   * cleared, so dropping the whole word would leave those inputs masked
   * with nothing left to re-enable them -- and the set routinely holds the
   * scheduler tick, the cross-core IPI and the console.
   */

  li      t0, 32
  bgeu    a1, t0, .Lmachine_ecall_unknown
  li      t0, 1
  sll     t0, t0, a1
  not     t0, t0
  esp32s31_percore_address t1, t2, g_esp32s31_machine_forward_mask
  lw      t2, 0(t1)
  and     t2, t2, t0
  sw      t2, 0(t1)
  esp32s31_monstat_bump ESP32S31_MONSTAT_DROP, t0, t1
  li      a0, 0
  j       .Lmachine_ecall_return

.Lmachine_irq_ack:
  /* Remove one completed interrupt from the deferred set and re-enable only
   * that CLIC input.  Re-enabling all deferred inputs from inside an ISR can
   * immediately retrigger an unrelated level source while sstatus.SIE is
   * still clear.
   */

  li      t0, 32
  bgeu    a1, t0, .Lmachine_ecall_unknown
  li      t0, 1
  sll     t0, t0, a1
  esp32s31_percore_address t1, t2, g_esp32s31_machine_forward_mask
  lw      t2, 0(t1)
  not     t0, t0
  and     t2, t2, t0
  sw      t2, 0(t1)

  slli    t0, a1, 2
  li      t1, 0x10801041
  add     t0, t0, t1
  /* The S31 CLIC keeps the forwarded pending bit latched after its input
   * has been masked.  The S-mode peripheral handler has already cleared
   * the device source, so drop that stale pending state before re-enabling
   * the input.  A still-asserted level source will set it again.
   */
  sb      zero, -1(t0)

  /* t0 still carries the clicintie address for the single-core return path
   * below, so account this acknowledgement using t1/t2 only.
   */

  esp32s31_monstat_bump ESP32S31_MONSTAT_ACK, t1, t2

#ifdef CONFIG_ESP32S31_SMP
  /* As with deferred service 3, exposing the CLIC input before the M-call
   * frame is fully restored permits hardware tail-chaining to overwrite
   * mepc/mcause.  Complete the ecall return first and make the IE store the
   * final instruction before mret.  a0/a1 are caller-clobbered by the
   * service-7 ABI. */

  csrr    t0, mepc
  addi    t0, t0, 4
  csrw    mepc, t0

  lw      t0, 0(sp)
  lw      t1, 4(sp)
  lw      t2, 8(sp)
  addi    sp, sp, 16
  csrrw   sp, mscratch, sp

  slli    a1, a1, 2
  li      a0, 0x10801041
  add     a1, a1, a0
  li      a0, 1
  sb      a0, 0(a1)
  mret
#else
  /* Preserve the established single-core ecall ABI.  With no peer scheduler
   * CPU, the ordinary monitor return is the validated path and keeps the
   * caller's complete register frame intact.
   */

  li      t1, 1
  sb      t1, 0(t0)
  li      a0, 0
  j       .Lmachine_ecall_return
#endif

.Lmachine_branch_disable:
  /* CLIC inputs are fixed to M-mode on S31 and therefore are not blocked by
   * sstatus.SIE.  Save and clear every external source before cache-off code
   * begins so the monitor cannot forward an interrupt into cached S-mode
   * code.  Each byte-wide IE register is four bytes apart.
   */

  li      t0, 0x10801041
  li      t1, 32
  li      t2, 0
.Lmachine_flash_mask_loop:
  lbu     a1, 0(t0)
  andi    a1, a1, 1
  srli    t2, t2, 1
  beqz    a1, 1f
  li      a1, 0x80000000
  or      t2, t2, a1
1:
  sb      zero, 0(t0)
  addi    t0, t0, 4
  addi    t1, t1, -1
  bnez    t1, .Lmachine_flash_mask_loop
  esp32s31_percore_address t0, t1, g_esp32s31_machine_flash_mask
  sw      t2, 0(t0)
  li      t0, ESP32S31_MHCR_BRANCH_MASK
  csrrc   zero, 0x7c1, t0
  li      a0, 0
  j       .Lmachine_ecall_return

.Lmachine_branch_enable:
  li      t0, ESP32S31_MHCR_BRANCH_MASK
  csrrs   zero, 0x7c1, t0
  esp32s31_percore_address t0, t1, g_esp32s31_machine_flash_mask
  lw      t2, 0(t0)
  sw      zero, 0(t0)
  li      t0, 0x10801041
  li      t1, 32
.Lmachine_flash_restore_loop:
  andi    a1, t2, 1
  sb      a1, 0(t0)
  srli    t2, t2, 1
  addi    t0, t0, 4
  addi    t1, t1, -1
  bnez    t1, .Lmachine_flash_restore_loop
  li      a0, 0
  j       .Lmachine_ecall_return

.Lmachine_ecall_unknown:
.Lmachine_ecall_invalid:
  li      a0, -1

.Lmachine_ecall_return:
  csrr    t0, mepc
  addi    t0, t0, 4
  csrw    mepc, t0
  j       .Lmachine_trap_return

.Lmachine_supervisor_return:
  /* Complete an S-to-S exception return atomically.  If the selected frame
   * restores SIE, consume one deferred input and model an interrupt taken
   * immediately after sret entered S-mode.  If the frame intentionally keeps
   * SIE clear, leave the input deferred for its eventual up_irq_restore().
   */

  csrr    t1, sstatus
  andi    t1, t1, SSTATUS_SPIE
  beqz    t1, .Lmachine_supervisor_return_direct

  esp32s31_percore_address t0, t2, g_esp32s31_machine_forward_mask
  lw      t1, 0(t0)
  beqz    t1, .Lmachine_supervisor_return_direct

  li      t2, 0
.Lmachine_supervisor_return_find:
  andi    t0, t1, 1
  bnez    t0, .Lmachine_supervisor_return_found
  srli    t1, t1, 1
  addi    t2, t2, 1
  j       .Lmachine_supervisor_return_find

.Lmachine_supervisor_return_found:
  /* Publish the synthetic cause while t2 still holds the input index. */

  addi    t2, t2, 16
  li      t0, 0x80000000
  or      t2, t2, t0
  csrw    scause, t2
  csrw    stval, zero

  andi    t2, t2, 0x3f
  addi    t2, t2, -16
  li      t0, 1
  sll     t0, t0, t2
  not     t0, t0
  esp32s31_percore_address t1, t2, g_esp32s31_machine_forward_mask
  lw      t2, 0(t1)
  and     t2, t2, t0
  sw      t2, 0(t1)
  esp32s31_monstat_bump ESP32S31_MONSTAT_REPLAY, t0, t1
  esp32s31_monstat_bump ESP32S31_MONSTAT_SREPLAY, t0, t1

  /* sret would first copy SPIE to SIE.  The synthetic trap then copies that
   * value back to SPIE, clears SIE and records an S-mode origin in SPP.
   */

  csrr    t1, sstatus
  andi    t1, t1, SSTATUS_SPIE
  li      t0, (SSTATUS_SIE | SSTATUS_SPIE | SSTATUS_SPPS)
  csrc    sstatus, t0
  beqz    t1, 1f
  li      t0, SSTATUS_SPIE
  csrs    sstatus, t0
1:
  li      t0, SSTATUS_SPPS
  csrs    sstatus, t0

  li      t0, MSTATUS_MPP_MASK
  csrc    mstatus, t0
  li      t0, MSTATUS_MPPS
  csrs    mstatus, t0
  li      t0, MSTATUS_MPIE
  csrs    mstatus, t0
#ifdef CONFIG_ESP32S31_SMP
  csrr    t0, mhartid
  bnez    t0, 2f
#endif
  la      t0, esp32s31_supervisor_trap
#ifdef CONFIG_ESP32S31_SMP
  j       3f
2:
  la      t0, esp32s31_cpu1_supervisor_trap
3:
#endif
  csrw    mepc, t0
  j       .Lmachine_trap_return

.Lmachine_supervisor_return_direct:
  /* No eligible deferred input remains.  Reproduce sret's status effects and
   * use mret for the final transition so a tail-chained CLIC input observes a
   * complete S-mode return state rather than the monitor ecall frame.
   */

  csrr    t0, sepc
  csrw    mepc, t0

  csrr    t0, sstatus
  andi    t1, t0, SSTATUS_SPIE
  li      t0, (SSTATUS_SIE | SSTATUS_SPIE | SSTATUS_SPPS)
  csrc    sstatus, t0
  beqz    t1, 4f
  li      t0, SSTATUS_SIE
  csrs    sstatus, t0
4:
  li      t0, SSTATUS_SPIE
  csrs    sstatus, t0

  li      t0, MSTATUS_MPP_MASK
  csrc    mstatus, t0
  li      t0, MSTATUS_MPPS
  csrs    mstatus, t0
  li      t0, MSTATUS_MPIE
  csrs    mstatus, t0
  csrr    t0, mcause
  li      t1, 0xc7ffffff
  and     t0, t0, t1
  li      t1, 0x18000000
  or      t0, t0, t1
  csrw    mcause, t0
  j       .Lmachine_trap_return

.Lmachine_user_return:
  /* If an external source was deferred while S-mode had SIE clear, consume
   * exactly one source and enter the normal S-mode vector as a trap from U.
   * The ISR return reaches this gate again and drains the next source.  All
   * user registers remain untouched because the monitor uses only t0-t2,
   * which were saved on its private stack at entry.
   */

  esp32s31_percore_address t0, t2, g_esp32s31_machine_forward_mask
  lw      t1, 0(t0)
  beqz    t1, .Lmachine_user_return_direct

  li      t2, 0
.Lmachine_user_return_find:
  andi    t0, t1, 1
  bnez    t0, .Lmachine_user_return_found
  srli    t1, t1, 1
  addi    t2, t2, 1
  j       .Lmachine_user_return_find

.Lmachine_user_return_found:
  /* Publish the synthetic supervisor cause while t2 still contains the
   * selected external-input index.
   */

  addi    t2, t2, 16
  li      t0, 0x80000000
  or      t2, t2, t0
  csrw    scause, t2
  csrw    stval, zero

  andi    t2, t2, 0x3f
  addi    t2, t2, -16
  li      t0, 1
  sll     t0, t0, t2
  not     t0, t0
  esp32s31_percore_address t1, t2, g_esp32s31_machine_forward_mask
  lw      t2, 0(t1)
  and     t2, t2, t0
  sw      t2, 0(t1)
  esp32s31_monstat_bump ESP32S31_MONSTAT_REPLAY, t0, t1

  /* Model an interrupt immediately after the pending sret would have
   * entered U-mode.  sret first copies SPIE to SIE; the subsequent trap
   * copies that value back to SPIE and clears SIE.  Therefore the synthetic
   * trap's SPIE comes from the saved return state's SPIE, not its SIE.
   */

  csrr    t1, sstatus
  andi    t1, t1, SSTATUS_SPIE
  li      t0, (SSTATUS_SIE | SSTATUS_SPIE | SSTATUS_SPPS)
  csrc    sstatus, t0
  beqz    t1, 1f
  li      t0, SSTATUS_SPIE
  csrs    sstatus, t0
1:
  li      t0, MSTATUS_MPP_MASK
  csrc    mstatus, t0
  li      t0, MSTATUS_MPPS
  csrs    mstatus, t0
#ifdef CONFIG_ESP32S31_SMP
  csrr    t0, mhartid
  bnez    t0, 2f
#endif
  la      t0, esp32s31_supervisor_trap
#ifdef CONFIG_ESP32S31_SMP
  j       3f
2:
  la      t0, esp32s31_cpu1_supervisor_trap
3:
#endif
  csrw    mepc, t0
  j       .Lmachine_trap_return

.Lmachine_user_return_direct:
  /* sepc and sstatus already hold the selected user's architectural return
   * state.  Make the M-level return target U explicitly in both mstatus and
   * the S31 CLIC extension fields carried by mcause, then jump straight to
   * the user EPC.  A CLIC input tail-chained at this mret consequently sees
   * a complete U-mode frame and is safe to forward immediately.
   */

  csrr    t0, sepc
  csrw    mepc, t0

  /* This path replaces the final sret, so reproduce all of its supervisor
   * status effects before mret enters U-mode: SIE=SPIE, SPIE=1 and SPP=U.
   * Leaving SIE cleared slowly corrupts the saved return state under a high
   * rate of scheduler ticks even though higher-privilege interrupts can
   * still arrive while U-mode is running.
   */

  csrr    t0, sstatus
  andi    t1, t0, SSTATUS_SPIE
  li      t0, (SSTATUS_SIE | SSTATUS_SPIE | SSTATUS_SPPS)
  csrc    sstatus, t0
  beqz    t1, 4f
  li      t0, SSTATUS_SIE
  csrs    sstatus, t0
4:
  li      t0, SSTATUS_SPIE
  csrs    sstatus, t0

  li      t0, MSTATUS_MPP_MASK
  csrc    mstatus, t0
  li      t0, MSTATUS_MPIE
  csrs    mstatus, t0
  csrr    t0, mcause
  li      t1, 0xc7ffffff
  and     t0, t0, t1
  li      t1, 0x08000000
  or      t0, t0, t1
  csrw    mcause, t0
  j       .Lmachine_trap_return

.Lmachine_forward_exception:
  /* Recreate an S-mode trap whose previous privilege is U. */

  csrw    scause, t0
  csrr    t1, mepc
  csrw    sepc, t1
  csrr    t1, mtval
  csrw    stval, t1

  csrr    t1, sstatus
  andi    t1, t1, SSTATUS_SIE
  li      t0, (SSTATUS_SIE | SSTATUS_SPIE | SSTATUS_SPPS)
  csrc    sstatus, t0
  beqz    t1, 1f
  li      t0, SSTATUS_SPIE
  csrs    sstatus, t0
1:
  li      t0, MSTATUS_MPP_MASK
  csrc    mstatus, t0
  li      t0, MSTATUS_MPPS
  csrs    mstatus, t0
#ifdef CONFIG_ESP32S31_SMP
  csrr    t0, mhartid
  bnez    t0, 3f
#endif
  la      t0, esp32s31_supervisor_trap
#ifdef CONFIG_ESP32S31_SMP
  j       4f
3:
  la      t0, esp32s31_cpu1_supervisor_trap
4:
#endif
  csrw    mepc, t0
  j       .Lmachine_trap_return

.Lmachine_forward_interrupt:
  /* Recreate the architectural state of a trap taken by S-mode.  NuttX's
   * normal exception entry then saves the untouched interrupted context and
   * returns with sret.  Preserve whether the interrupted context was S or U
   * in sstatus.SPP so this also works once U-mode applications are enabled.
   */

  /* Mask this CLIC input before returning to S-mode.  Otherwise the still
   * asserted M-level input retraps before the first S-mode instruction can
   * execute.  Record it for service 3 at S-mode exception return.
   */

  andi    t1, t0, 0x3f
  li      t2, 16
  bltu    t1, t2, .Lmachine_trap_fault
  li      t2, 48
  bgeu    t1, t2, .Lmachine_trap_fault
  addi    t1, t1, -16

  slli    t2, t1, 2
  li      t0, 0x10801041
  add     t2, t2, t0
  sb      zero, 0(t2)

  li      t2, 1
  sll     t2, t2, t1
  esp32s31_percore_address t0, t1, g_esp32s31_machine_forward_mask
  lw      t1, 0(t0)
  or      t1, t1, t2
  sw      t1, 0(t0)

  /* M-level CLIC delivery is not automatically gated by sstatus.SIE.  If
   * the interrupted S-mode context has interrupts masked, defer rather than
   * synthesizing an unmaskable S trap.  Supervisor interrupts remain enabled
   * while executing below S privilege irrespective of SIE, so a U-mode
   * origin must be forwarded directly.
   *
   * Do not infer the interrupted privilege from mepc.  A CLIC input can
   * tail-chain while return_from_exception is executing its final monitor
   * ecall: mepc then points into the kernel even though the architectural
   * return target is U-mode.  CLIC snapshots MPP in mcause[29:28]; XPPS is
   * therefore the authoritative S-versus-U origin bit for this interrupt.
   */

  csrr    t1, sstatus
  andi    t1, t1, SSTATUS_SIE
  bnez    t1, .Lmachine_forward_direct
#ifdef CONFIG_ESP32S31_SMP
  csrr    t1, mcause
  li      t0, ESP32S31_MCAUSE_XPPS
  and     t1, t1, t0
  beqz    t1, .Lmachine_forward_direct
#endif
.Lmachine_forward_deferred:
  esp32s31_monstat_bump ESP32S31_MONSTAT_DEFER, t0, t1
  j       .Lmachine_trap_return
.Lmachine_forward_direct:
  csrr    t0, mcause
  csrw    scause, t0

  csrr    t1, mepc
  csrw    sepc, t1
  csrr    t1, mtval
  csrw    stval, t1

  csrr    t1, sstatus
  li      t0, SSTATUS_SIE
  and     t1, t1, t0

  li      t0, (SSTATUS_SIE | SSTATUS_SPIE | SSTATUS_SPPS)
  csrc    sstatus, t0
  beqz    t1, 1f
  li      t0, SSTATUS_SPIE
  csrs    sstatus, t0
1:
  /* Recreate sstatus.SPP from the privilege snapshot carried by this CLIC
   * mcause, not from live mstatus or mepc.  This remains correct both for an
   * ordinary U-mode interrupt and for tail-chaining at the final S-to-U
   * exception-return monitor call.
   */

  csrr    t1, mcause
  li      t0, ESP32S31_MCAUSE_XPPS
  and     t1, t1, t0
  beqz    t1, 2f
  li      t0, SSTATUS_SPPS
  csrs    sstatus, t0
2:

  /* mret must enter the S-mode exception vector, irrespective of whether
   * the interrupted context was S or U.
   */

  li      t0, MSTATUS_MPP_MASK
  csrc    mstatus, t0
  li      t0, MSTATUS_MPPS
  csrs    mstatus, t0
#ifdef CONFIG_ESP32S31_SMP
  csrr    t0, mhartid
  bnez    t0, 3f
#endif
  la      t0, esp32s31_supervisor_trap
#ifdef CONFIG_ESP32S31_SMP
  j       4f
3:
  la      t0, esp32s31_cpu1_supervisor_trap
4:
#endif
  csrw    mepc, t0

.Lmachine_trap_return:
  lw      t0, 0(sp)
  lw      t1, 4(sp)
  lw      t2, 8(sp)
  addi    sp, sp, 16
  csrrw   sp, mscratch, sp
  mret

.Lmachine_trap_fault:
  /* Preserve the live argument, return and supervisor stack registers before
   * preparing the C ABI.  A fatal S-mode access fault can otherwise report
   * only the instruction inside a low-level routine such as memset(), while
   * losing both its caller and destination.  This extra frame is used only
   * on the non-returning fatal path and keeps the normal monitor fast path
   * unchanged.
   */

  addi    sp, sp, -48
  sw      a0, 0(sp)
  sw      a1, 4(sp)
  sw      a2, 8(sp)
  sw      a3, 12(sp)
  sw      a4, 16(sp)
  sw      a5, 20(sp)
  sw      a6, 24(sp)
  sw      a7, 28(sp)
  sw      ra, 32(sp)
  csrr    t1, mscratch
  sw      t1, 36(sp)

  mv      a3, sp
  mv      a0, t0
  csrr    a1, mepc
  csrr    a2, mtval
  call    esp32s31_mmode_fault

.Lmachine_trap_hang:
  wfi
  j       .Lmachine_trap_hang

  .size esp32s31_machine_trap, .-esp32s31_machine_trap

  .section .note.GNU-stack, "", @progbits
