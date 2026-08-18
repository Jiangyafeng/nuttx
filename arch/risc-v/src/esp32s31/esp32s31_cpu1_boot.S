/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_cpu1_boot.S
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <arch/irq.h>

#include "esp32s31_cpu1_boot.h"

/****************************************************************************
 * Public Symbols
 ****************************************************************************/

  .global esp32s31_cpu1_boot_entry
  .type   esp32s31_cpu1_boot_entry, function

  .extern g_esp32s31_cpu1_boot_state
  .extern g_esp32s31_cpu1_hartid
  .extern esp32s31_start_smode
  .extern esp32s31_prepare_hart_smode
  .extern esp32s31_cpu1_smp_entry
  .extern g_esp32s31_cpu1_smp_release
  .extern g_idle_topstack

  .global esp32s31_cpu1_smode_entry
  .type   esp32s31_cpu1_smode_entry, function

  .global esp32s31_cpu1_supervisor_trap
  .type   esp32s31_cpu1_supervisor_trap, function

/****************************************************************************
 * CPU1 bring-up stack
 *
 * Used only until esp32s31_cpu1_smp_entry switches to the idle task stack.
 ****************************************************************************/

  .section .bss.esp32s31_cpu1_boot_stack, "aw", @nobits
  .balign 16
.Lcpu1_boot_stack:
  .space CONFIG_ESP32S31_CPU1_BOOT_STACKSIZE
.Lcpu1_boot_stack_top:

/****************************************************************************
 * CPU1 entry point
 ****************************************************************************/

  .section .iram1, "ax", @progbits
  .balign 0x40

esp32s31_cpu1_boot_entry:
  .option push
  .option norelax

  /* Establish all ABI state before using C-visible shared storage. */

  la      gp, __global_pointer$
  la      sp, .Lcpu1_boot_stack_top

  /* Keep this hart out of NuttX interrupt delivery until it reaches S-mode.
   * A local trap sink records any unexpected exception without touching CPU0.
   */

  csrc    mstatus, MSTATUS_MIE
  csrw    mie, zero
  la      t0, .Lcpu1_boot_trap
  ori     t0, t0, 3
  csrw    mtvec, t0

  csrr    t0, mhartid
  la      t1, g_esp32s31_cpu1_hartid
  sw      t0, 0(t1)
  li      t1, 1
  bne     t0, t1, .Lcpu1_bad_hart

  /* esp32s31_start_smode installs CPU1's private machine monitor state and
   * enters the supplied address with mstatus.MPP=S.
   *
   * The ROM PMA setup is private to this hart and leaves cached PSRAM RX.
   * Install the same writable PSRAM aperture as CPU0 before U-mode work can
   * be scheduled here.  The helper preserves no caller-saved registers, so
   * load the supervisor entry after it returns.
   */

  call    esp32s31_prepare_hart_smode
  la      a0, esp32s31_cpu1_smode_entry
  j       esp32s31_start_smode

.Lcpu1_bad_hart:
  la      t0, g_esp32s31_cpu1_boot_state
  li      t1, ESP32S31_CPU1_BOOT_BAD_HART
  sw      t1, 0(t0)
  fence   rw, rw
  j       .Lcpu1_fault_park

  .balign 0x40
.Lcpu1_boot_trap:
  la      t0, g_esp32s31_cpu1_boot_state
  li      t1, ESP32S31_CPU1_BOOT_MMODE_FAULT
  sw      t1, 0(t0)
  fence   rw, rw

.Lcpu1_fault_park:
  j       .Lcpu1_fault_park

  .option pop
  .size esp32s31_cpu1_boot_entry, .-esp32s31_cpu1_boot_entry

/****************************************************************************
 * CPU1 S-mode entry and trap sink
 ****************************************************************************/

  .balign 0x40
esp32s31_cpu1_smode_entry:
  /* The trap shim below distinguishes "per-CPU area installed" from "still
   * bringing up" by reading sscratch, so give it a defined starting value
   * before anything can trap.
   */

  csrw    sscratch, zero

  /* Service 9 returns one only for an S-mode ecall handled by the machine
   * monitor.  Reaching this point and completing that round trip proves both
   * sides of the CPU1 privilege transition before publishing READY.
   */

  li      a0, 9
  ecall
  li      t0, 1
  bne     a0, t0, .Lcpu1_smode_fault

  csrc    sstatus, SSTATUS_SIE
  la      t0, g_esp32s31_cpu1_boot_state
  li      t1, ESP32S31_CPU1_SMP_READY
  sw      t1, 0(t0)
  fence   rw, rw

.Lcpu1_smp_wait:
  la      t0, g_esp32s31_cpu1_smp_release
  lw      t1, 0(t0)
  beqz    t1, .Lcpu1_smp_wait
  fence   rw, rw

  /* CPU1 owns the upper reserved idle stack.  nx_start has initialized its
   * TCB and stack metadata before up_cpu_start releases this loop. */

  la      t0, g_idle_topstack
  lw      sp, 0(t0)
  andi    sp, sp, -16
  call    esp32s31_cpu1_smp_entry
  j       .Lcpu1_smode_fault

  .size esp32s31_cpu1_smode_entry, .-esp32s31_cpu1_smode_entry

  .balign 0x40
esp32s31_cpu1_supervisor_trap:
  /* Use the complete NuttX trap machinery only after sscratch has acquired
   * CPU1's per-CPU area.  SIE is clear before that point, but a synchronous
   * exception must still fail cleanly instead of recursively faulting in
   * _interrupt_handler while dereferencing an empty sscratch.
   *
   * The machine monitor forwards every CPU1 trap here, interrupts included,
   * with the interrupted context still live in the integer registers.  Swap
   * the probe through sscratch itself rather than loading a flag into a
   * temporary: a plain "la/lw t0" would hand the resumed task a corrupted
   * t0 on every trap it takes.
   */

  csrrw   t0, sscratch, t0
  beqz    t0, .Lcpu1_trap_unready
  csrrw   t0, sscratch, t0
  j       _vector_table

.Lcpu1_trap_unready:
  csrrw   t0, sscratch, t0

.Lcpu1_smode_fault:
  la      t0, g_esp32s31_cpu1_boot_state
  li      t1, ESP32S31_CPU1_BOOT_SMODE_FAULT
  sw      t1, 0(t0)
  fence   rw, rw

.Lcpu1_smode_fault_park:
  j       .Lcpu1_smode_fault_park

  .size esp32s31_cpu1_supervisor_trap, \
        .-esp32s31_cpu1_supervisor_trap
