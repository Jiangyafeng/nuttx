/****************************************************************************
 * arch/risc-v/src/common/riscv_exception.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <assert.h>

#include <nuttx/debug.h>
#include <nuttx/irq.h>
#include <nuttx/arch.h>
#if defined(CONFIG_PAGING) || defined(CONFIG_ARCH_ADDRENV)
#  include <nuttx/pgalloc.h>
#endif

#if defined(CONFIG_PAGING) || defined(CONFIG_ARCH_ADDRENV)
#  include "pgalloc.h"
#  include "riscv_mmu.h"
#endif

#include "sched/sched.h"
#include "riscv_internal.h"
#include "chip.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *g_reasons_str[RISCV_MAX_EXCEPTION + 1] =
{
  "Instruction address misaligned",
  "Instruction access fault",
  "Illegal instruction",
  "Breakpoint",
  "Load address misaligned",
  "Load access fault",
  "Store/AMO address misaligned",
  "Store/AMO access fault",
  "Environment call from U-mode",
  "Environment call from S-mode",
  "Environment call from H-mode",
  "Environment call from M-mode",
  "Instruction page fault",
  "Load page fault",
  "Reserved",
  "Store/AMO page fault",
#ifdef CONFIG_ARCH_RV_MACHINE_ISA_1_13
  "Reserved",
  "Reserved",
  "Software check",
  "Hardware error",
#endif
#ifdef RISCV_CUSTOM_EXCEPTION_REASONS
  RISCV_CUSTOM_EXCEPTION_REASONS
#endif
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_ARCH_ADDRENV
/****************************************************************************
 * Name: riscv_dump_translation
 *
 * Description:
 *   Report the translation state behind a page fault.  Without demand
 *   paging every region of an address environment is mapped before its first
 *   user instruction runs, so a page fault means either that this hart has
 *   the wrong root table installed or that the leaf entry is not visible to
 *   its page-table walker.  Those two causes need different fixes, and the
 *   privilege state decides whether the fault is even recoverable.
 *
 * Input Parameters:
 *   vaddr - The faulting virtual address.
 *   regs  - A pointer to the register state at the time of the exception.
 *
 ****************************************************************************/

static void riscv_dump_translation(uintreg_t vaddr, void *regs)
{
  struct tcb_s *tcb    = this_task();
  uintptr_t     satp   = READ_CSR(CSR_SATP);
  uintptr_t     want   = 0;
  uintptr_t     l1pte  = 0;
  uintptr_t     l2pte  = 0;
  uintptr_t     lnvaddr;

  if (tcb->addrenv_curr != NULL)
    {
      want = tcb->addrenv_curr->addrenv.satp;
    }

  lnvaddr = riscv_pgvaddr(mmu_satp_to_paddr(satp));
  if (lnvaddr != 0)
    {
      l1pte   = mmu_ln_getentry(1, lnvaddr, vaddr);
      lnvaddr = (l1pte & PTE_VALID) != 0 ?
                riscv_pgvaddr(mmu_pte_to_paddr(l1pte)) : 0;
      if (lnvaddr != 0)
        {
          l2pte = mmu_ln_getentry(ARCH_PGT_MAX_LEVELS, lnvaddr, vaddr);
        }
    }

  _alert("ADDRENV: cpu=%d pid=%d flags=%04x status=%" PRIxREG
         " satp=%" PRIxPTR " want=%" PRIxPTR " va=%" PRIxREG
         " l1=%" PRIxPTR " l2=%" PRIxPTR "\n",
         this_cpu(), tcb->pid, tcb->flags,
         ((uintreg_t *)regs)[REG_INT_CTX], satp, want, vaddr, l1pte, l2pte);

#ifdef CONFIG_ARCH_RISCV_INTXCPT_EXTENSIONS
  /* On a CLIC implementation the privilege that xret restored comes from the
   * cause register's extension fields, not only from status.SPP.
   */

  _alert("ADDRENV: cause=%" PRIxREG " xcause=%" PRIxREG "\n",
         READ_CSR(CSR_CAUSE), ((uintreg_t *)regs)[REG_INT_CTX + 1]);
#endif
}
#endif /* CONFIG_ARCH_ADDRENV */

/****************************************************************************
 * Name: riscv_fault_handler
 *
 * Description:
 *   Handle a fault caused by the running task. If the task is a user task
 *   not currently in a syscall or interrupt context, kill it with SIGSEGV
 *   instead of taking down the whole system. Otherwise (kernel thread, a
 *   fault while already in kernel context on behalf of a syscall, or a
 *   fault while handling an interrupt) there is no safe task to kill, so
 *   panic.
 *
 * Input Parameters:
 *   cause - The (masked) machine cause of the exception, used for the
 *     panic message only.
 *   regs  - A pointer to the register state at the time of the exception.
 *
 ****************************************************************************/

static void riscv_fault_handler(uintreg_t cause, void *regs)
{
#ifdef CONFIG_ARCH_KERNEL_STACK
  struct tcb_s *tcb = this_task();

  /* The STATUS_PPP check alone is enough: any kernel-mode fault (kernel
   * thread, syscall body, or interrupt) has STATUS_PPP != 0.  The other
   * two checks are kept as defensive redundancy.
   */

  if (((tcb->flags & TCB_FLAG_TTYPE_MASK) != TCB_FLAG_TTYPE_KERNEL) &&
      ((tcb->flags & TCB_FLAG_SYSCALL) == false) &&
      !(((uintreg_t *)regs)[REG_INT_CTX] & STATUS_PPP))
    {
      struct tcb_s *ptcb = nxsched_get_tcb(tcb->group->tg_pid);

      _alert("Segmentation fault in %s (PID %d: %s)\n", get_task_name(ptcb),
             tcb->pid, get_task_name(tcb));

#ifdef CONFIG_SCHED_BACKTRACE
      sched_dumpstack(tcb->pid);
#endif

      tcb->flags |= TCB_FLAG_FORCED_CANCEL;

      /* Return to _exit function in privileged mode with argument SIGSEGV */

      running_regs()[REG_EPC] = _exit;
      running_regs()[REG_A0] = (void *)SIGSEGV;
      ((uintreg_t *)running_regs())[REG_INT_CTX] |= STATUS_PPP;

#if defined(CONFIG_ARCH_CHIP_ESP32S31) && \
    defined(CONFIG_ARCH_RISCV_INTXCPT_EXTENSIONS)
      /* S31 CLIC xret consumes the previous-privilege state saved in the
       * first integer-extension slot as well as sstatus.SPP.  The exception
       * frame still describes the original U-mode fault, so synchronize its
       * CLIC XPPS bit before redirecting execution to privileged _exit().
       */

      ((uintreg_t *)running_regs())[REG_INT_CTX + 1] |= UINT32_C(0x10000000);
#endif

      /* Continue with kernel stack in use. The frame(s) in kernel stack
       * are no longer needed, so just set it to top
       */

      running_regs()[REG_SP] = tcb->xcp.ktopstk;
      return;
    }
#endif

  _alert("PANIC!!! Exception = %" PRIxREG "\n", cause);
  up_irq_save();
  up_set_interrupt_context(true);
  PANIC_WITH_REGS("panic", regs);
}

/****************************************************************************
 * Name: riscv_exception
 *
 * Description:
 *   This is the exception handler.
 *
 ****************************************************************************/

int riscv_exception(int mcause, void *regs, void *args)
{
  uintreg_t cause = mcause & RISCV_IRQ_MASK;

  _alert("EXCEPTION: %s. MCAUSE: %" PRIxREG ", EPC: %" PRIxREG
         ", MTVAL: %" PRIxREG "\n",
         mcause > RISCV_MAX_EXCEPTION ? "Unknown" : g_reasons_str[cause],
         cause, READ_CSR(CSR_EPC), READ_CSR(CSR_TVAL));

#ifdef CONFIG_ARCH_ADDRENV
  if (cause == RISCV_IRQ_INSTRUCTIONPF || cause == RISCV_IRQ_LOADPF ||
      cause == RISCV_IRQ_STOREPF)
    {
      riscv_dump_translation(READ_CSR(CSR_TVAL), regs);
    }
#endif

  riscv_fault_handler(cause, regs);

  return 0;
}

/****************************************************************************
 * Name: riscv_fillpage
 *
 * Description:
 *   This function is an exception handler for page faults in a RISC-V.
 *   It is invoked when a page fault exception occurs, which is typically
 *   when a process tries to access a page that is not currently in memory.
 *
 *   The function takes as arguments the machine cause (mcause) which
 *   indicates the cause of the exception, a pointer to the register state
 *   at the time of the exception (regs), and a pointer to any additional
 *   arguments (args).
 *
 *   The function should handle the exception appropriately, typically by
 *   loading the required page into memory and updating the page table.
 *
 * Input Parameters:
 *   mcause - The machine cause of the exception.
 *   regs   - A pointer to the register state at the time of the exception.
 *   args   - A pointer to any additional arguments.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

#ifdef CONFIG_PAGING
int riscv_fillpage(int mcause, void *regs, void *args)
{
  uintptr_t cause = mcause & RISCV_IRQ_MASK;
  uintptr_t ptlast;
  uintptr_t ptprev;
  uintptr_t paddr;
  uintptr_t vaddr;
  uint32_t  ptlevel;
  uintptr_t satp;
  uint64_t  mmuflags;

  _info("EXCEPTION: %s. MCAUSE: %" PRIxREG ", EPC: %" PRIxREG
        ", MTVAL: %" PRIxREG "\n",
        mcause > RISCV_MAX_EXCEPTION ? "Unknown" : g_reasons_str[cause],
        cause, READ_CSR(CSR_EPC), READ_CSR(CSR_TVAL));
  vaddr = MM_PGALIGNDOWN(READ_CSR(CSR_TVAL));
  if (vaddr >= CONFIG_ARCH_TEXT_VBASE && vaddr <= ARCH_TEXT_VEND)
    {
      mmuflags = MMU_UTEXT_FLAGS;

      /* Write access to .text region needs to be set according to
       * https://github.com/apache/nuttx/pull/6193.
       */

      mmuflags |= PTE_W;
    }
  else if (vaddr >= CONFIG_ARCH_DATA_VBASE && vaddr <= ARCH_DATA_VEND)
    {
      mmuflags = MMU_UDATA_FLAGS;
    }
  else if (vaddr >= CONFIG_ARCH_HEAP_VBASE && vaddr <= ARCH_HEAP_VEND)
    {
      mmuflags = MMU_UDATA_FLAGS;
    }
  else
    {
      _alert("Virtual address not mappable: %" PRIxPTR "\n", vaddr);
      goto access_fault;
    }

  satp    = READ_CSR(CSR_SATP);
  paddr   = mmu_satp_to_paddr(satp);

  for (ptlevel = 1; ptlevel <= ARCH_SPGTS; ptlevel++)
    {
      ptprev  = riscv_pgvaddr(paddr);
      paddr   = mmu_pte_to_paddr(mmu_ln_getentry(ptlevel, ptprev, vaddr));
      if (!paddr)
        {
          /* Nothing yet, allocate one page for next level page table */

          paddr = riscv_pgtable_alloc();
          if (!paddr)
            {
              return -ENOMEM;
            }

          /* Map the page table to the prior level */

          mmu_ln_setentry(ptlevel, ptprev, paddr, vaddr, MMU_UPGT_FLAGS);

          /* This is then used to map the next level */

          riscv_pgwipe(paddr);
        }
    }

  ptlast = riscv_pgvaddr(paddr);

  /* LOADPF/STOREPF is also raised when the leaf PTE already exists but its
   * permission bits don't satisfy the access (e.g. a store to a .text page
   * whose write access was revoked).  That is not a fault this function
   * should handle: allocating a fresh page here would silently discard the
   * existing mapping's page.
   */

  if (mmu_ln_getentry(ARCH_PGT_MAX_LEVELS, ptlast, vaddr) & PTE_VALID)
    {
      _alert("Page already mapped, permission violation: %"
             PRIxPTR "\n", vaddr);
      goto access_fault;
    }

  paddr = mm_pgalloc(1);
  if (!paddr)
    {
      return -ENOMEM;
    }

  /* Wipe the physical page memory */

  riscv_pgwipe(paddr);

  /* Then map the virtual address to the physical address */

  mmu_ln_setentry(ARCH_PGT_MAX_LEVELS, ptlast, paddr, vaddr, mmuflags);

  return 0;

access_fault:
  riscv_fault_handler(cause, regs);
  return 0;
}
#endif /* CONFIG_PAGING */

/****************************************************************************
 * Name: riscv_exception_attach
 *
 * Description:
 *   Attach standard exception with suitable handler
 *
 ****************************************************************************/

void riscv_exception_attach(void)
{
  irq_attach(RISCV_IRQ_IAMISALIGNED, riscv_exception, NULL);
  irq_attach(RISCV_IRQ_IAFAULT, riscv_exception, NULL);
  irq_attach(RISCV_IRQ_IINSTRUCTION, riscv_exception, NULL);
  irq_attach(RISCV_IRQ_BPOINT, riscv_exception, NULL);
  irq_attach(RISCV_IRQ_LAFAULT, riscv_exception, NULL);
  irq_attach(RISCV_IRQ_SAFAULT, riscv_exception, NULL);

#ifdef CONFIG_RISCV_MISALIGNED_HANDLER
  irq_attach(RISCV_IRQ_LAMISALIGNED, riscv_misaligned, NULL);
  irq_attach(RISCV_IRQ_SAMISALIGNED, riscv_misaligned, NULL);
#else
  irq_attach(RISCV_IRQ_LAMISALIGNED, riscv_exception, NULL);
  irq_attach(RISCV_IRQ_SAMISALIGNED, riscv_exception, NULL);
#endif

  /* Attach the ecall interrupt handler */

#ifndef CONFIG_BUILD_FLAT
  irq_attach(RISCV_IRQ_ECALLU, riscv_swint, NULL);
#else
  irq_attach(RISCV_IRQ_ECALLU, riscv_exception, NULL);
#endif

  irq_attach(RISCV_IRQ_ECALLS, riscv_exception, NULL);
  irq_attach(RISCV_IRQ_ECALLH, riscv_exception, NULL);

#ifndef CONFIG_ARCH_USE_S_MODE
  irq_attach(RISCV_IRQ_ECALLM, riscv_swint, NULL);
#else
  irq_attach(RISCV_IRQ_ECALLM, riscv_exception, NULL);
#endif

  irq_attach(RISCV_IRQ_INSTRUCTIONPF, riscv_exception, NULL);

#ifdef CONFIG_PAGING
  irq_attach(RISCV_IRQ_LOADPF, riscv_fillpage, NULL);
  irq_attach(RISCV_IRQ_STOREPF, riscv_fillpage, NULL);
#else
  irq_attach(RISCV_IRQ_LOADPF, riscv_exception, NULL);
  irq_attach(RISCV_IRQ_STOREPF, riscv_exception, NULL);
#endif

  irq_attach(RISCV_IRQ_RESERVED14, riscv_exception, NULL);

#ifdef CONFIG_SMP
  irq_attach(RISCV_IRQ_SOFT, riscv_smp_call_handler, NULL);
#else
  irq_attach(RISCV_IRQ_SOFT, riscv_exception, NULL);
#endif
}
