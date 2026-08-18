/****************************************************************************
 * sched/addrenv/addrenv.c
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

#include <assert.h>
#include <errno.h>
#include <nuttx/debug.h>

#include <nuttx/addrenv.h>
#include <nuttx/atomic.h>
#include <nuttx/irq.h>
#include <nuttx/sched.h>
#include <nuttx/wqueue.h>

#include "sched/sched.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* This variable holds the address environment instantiated on each CPU, so
 * that a switch to a thread of the same task group can be skipped.  An entry
 * is NULL only before the CPU has run any user code, and -- where the
 * architecture can deselect an address environment -- after the environment
 * it named has been destroyed.
 *
 * Where the architecture cannot deselect, an entry is also a reference:  the
 * mappings stay live in the MMU while a CPU runs kernel threads, so the
 * environment must outlive them.  It is then freed at the next switch on
 * that CPU rather than when its owner dies.
 *
 * This must only be accessed with interrupts disabled.
 *
 * REVISIT: Try to get rid of this, global bookkeeping for this is dangerous.
 */

static FAR struct addrenv_s *g_addrenv[CONFIG_SMP_NCPUS];
static spinlock_t g_addrenv_lock = SP_UNLOCKED;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: addrenv_destroy
 *
 * Description:
 *   Deferred service routine for destroying an address environment. This is
 *   so that the heavy lifting is not done when the context is switching, or
 *   from ISR.
 *
 * Input Parameters:
 *   arg - Contains pointer to the address environment that is freed.
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_HAVE_ADDRENV_DESELECT
/****************************************************************************
 * Name: addrenv_uninstall
 *
 * Description:
 *   Stop translating through an address environment that is about to be
 *   destroyed, on the CPU this runs on.
 *
 *   Runs either directly or from the SMP call IPI, so it may execute in
 *   interrupt context:  it only touches the bookkeeping and satp.
 *
 * Input Parameters:
 *   arg - The address environment being destroyed.
 *
 * Returned Value:
 *   Zero (OK); the SMP call machinery reads a negative value as failure.
 *
 ****************************************************************************/

static int addrenv_uninstall(FAR void *arg)
{
  FAR struct addrenv_s *addrenv = (FAR struct addrenv_s *)arg;
  irqstate_t flags;
  int cpu;

  flags = spin_lock_irqsave_nopreempt(&g_addrenv_lock);

  cpu = this_cpu();

  if (g_addrenv[cpu] == addrenv)
    {
      /* Nothing may hold a reference at this point, so no thread running
       * here can still need these mappings.
       */

      DEBUGASSERT(this_task()->addrenv_curr != addrenv);

      up_addrenv_coherent(&addrenv->addrenv);
      up_addrenv_deselect();
      g_addrenv[cpu] = NULL;
    }

  spin_unlock_irqrestore_nopreempt(&g_addrenv_lock, flags);
  return OK;
}

/****************************************************************************
 * Name: addrenv_uninstall_all
 *
 * Description:
 *   Make every CPU stop translating through an address environment, so that
 *   its page tables can be freed.
 *
 *   A CPU keeps the last address environment it selected installed for as
 *   long as it only runs kernel threads:  kernel code needs no user mappings
 *   and reloading satp on every switch would be wasted work.  The mappings
 *   are still live in the MMU, though, so they must be replaced by the
 *   kernel page tables before the tables they name are freed.
 *
 * Input Parameters:
 *   addrenv - The address environment being destroyed.
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

static void addrenv_uninstall_all(FAR struct addrenv_s *addrenv)
{
#ifdef CONFIG_SMP
  cpu_set_t cpuset;
#endif
  irqstate_t flags;
  bool installed;
  int cpu;

  flags = spin_lock_irqsave_nopreempt(&g_addrenv_lock);

  for (cpu = 0, installed = false; cpu < CONFIG_SMP_NCPUS; cpu++)
    {
      if (g_addrenv[cpu] == addrenv)
        {
          installed = true;
        }
    }

  spin_unlock_irqrestore_nopreempt(&g_addrenv_lock, flags);

  /* An environment that was never instantiated -- one that failed while
   * being created, say -- needs no cross-CPU work at all.  A CPU can drop
   * the environment on its own after this check, but no CPU can newly
   * install it:  that would take a reference, and there are none left.
   */

  if (!installed)
    {
      return;
    }

#ifdef CONFIG_SMP
  CPU_ZERO(&cpuset);
  for (cpu = 0; cpu < CONFIG_SMP_NCPUS; cpu++)
    {
      CPU_SET(cpu, &cpuset);
    }

  /* Waiting for the answer is what makes this safe:  a CPU that is switching
   * away from this environment right now dereferences it under
   * g_addrenv_lock with interrupts disabled, so it cannot be servicing the
   * IPI, and the tables stay valid until it is.
   */

  nxsched_smp_call(cpuset, addrenv_uninstall, addrenv);
#else
  addrenv_uninstall(addrenv);
#endif
}
#endif /* CONFIG_ARCH_HAVE_ADDRENV_DESELECT */

static void addrenv_destroy(FAR void *arg)
{
  FAR struct addrenv_s *addrenv = (FAR struct addrenv_s *)arg;

#ifdef CONFIG_ARCH_HAVE_ADDRENV_DESELECT
  /* Take the mappings out of every MMU before freeing what they point to */

  addrenv_uninstall_all(addrenv);
#endif

  /* Destroy the address environment */

  up_addrenv_destroy(&addrenv->addrenv);

  /* Then finally release the memory */

  kmm_free(addrenv);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: addrenv_switch
 *
 * Description:
 *   Instantiate the group address environment for the current thread at the
 *   the head of the ready to run list.
 *
 *   This function is called from platform-specific code after any context
 *   switch (i.e., after any change in the thread at the head of the
 *   ready-to-run list).  This function will change the address environment
 *   if the new thread is part of a different task group.
 *
 * Input Parameters:
 *   tcb - The TCB of thread that needs an address environment.  This should
 *         be the TCB at the head of the ready-to-run list, but that is not
 *         enough.
 *
 * Returned Value:
 *   Zero (OK) is returned on success.  A negated errno value is returned on
 *   any failure.
 *
 * Assumptions:
 *   This function should only be called within critical OS sections with
 *   interrupts disabled.  Interrupts are disabled internally just to be
 *   certain, however.
 *
 ****************************************************************************/

int addrenv_switch(FAR struct tcb_s *tcb)
{
  FAR struct addrenv_s *curr;
  FAR struct addrenv_s *next;
  irqstate_t flags;
  int cpu;
  int ret;

  next = tcb->addrenv_curr;

  /* Does the group have an address environment? */

  if (!next)
    {
      /* No... just return perhaps leaving a different address environment
       * intact.
       */

      return OK;
    }

  flags = spin_lock_irqsave_nopreempt(&g_addrenv_lock);

  cpu = this_cpu();
  curr = g_addrenv[cpu];

  /* Are we going to change address environments? */

  if (curr != next)
    {
      /* Yes.. Is there a current address environment in place? */

      if (curr)
        {
          /* We need to flush the D-Cache and Invalidate the I-Cache for
           * the group whose environment is disappearing.
           */

          up_addrenv_coherent(&curr->addrenv);
        }

#ifndef CONFIG_ARCH_HAVE_ADDRENV_DESELECT
      /* While the address environment is instantiated, it cannot be freed */

      addrenv_take(next);
#endif

      /* Instantiate the new address environment (removing the old
       * environment in the process).  For the case of kernel threads,
       * the old mappings will be removed and no new mappings will be
       * instantiated.
       */

      ret = up_addrenv_select(&next->addrenv);
      if (ret < 0)
        {
          berr("ERROR: up_addrenv_select failed: %d\n", ret);
        }

#ifndef CONFIG_ARCH_HAVE_ADDRENV_DESELECT
      /* This is a safe spot to drop the current address environment */

      if (curr)
        {
          addrenv_drop(curr, true);
        }
#endif

      /* Save the new, current address environment group */

      g_addrenv[cpu] = next;
    }

  spin_unlock_irqrestore_nopreempt(&g_addrenv_lock, flags);
  return OK;
}

/****************************************************************************
 * Name: addrenv_allocate
 *
 * Description:
 *   Allocate an address environment for a new process.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Pointer to the new address environment, or NULL if out of memory.
 *
 ****************************************************************************/

FAR struct addrenv_s *addrenv_allocate(void)
{
  FAR struct addrenv_s *addrenv;

  addrenv = kmm_zalloc(sizeof(struct addrenv_s));
  if (addrenv)
    {
      /* Take reference so this won't get freed */

      atomic_set(&addrenv->refs, 1);
    }

  return addrenv;
}

/****************************************************************************
 * Name: addrenv_attach
 *
 * Description:
 *   Attach address environment to a newly created group. Called by exec()
 *   right before injecting the new process into the system.
 *
 * Input Parameters:
 *   tcb     - The tcb of the newly loaded task.
 *   addrenv - The address environment that is attached.
 *
 * Returned Value:
 *   This is a NuttX internal function so it follows the convention that
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

int addrenv_attach(FAR struct tcb_s *tcb, FAR struct addrenv_s *addrenv)
{
  /* Attach the address environment */

  tcb->addrenv_own = addrenv;
  tcb->addrenv_curr = tcb->addrenv_own;

  return OK;
}

/****************************************************************************
 * Name: addrenv_join
 *
 * Description:
 *   Join the parent process's address environment.
 *
 * Input Parameters:
 *   ptcb - The tcb of the parent process
 *   tcb  - The tcb of the child process
 *
 * Returned Value:
 *   This is a NuttX internal function so it follows the convention that
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

int addrenv_join(FAR struct tcb_s *ptcb, FAR struct tcb_s *tcb)
{
  int ret;

  ret = up_addrenv_attach(ptcb, tcb);
  if (ret < 0)
    {
      berr("ERROR: up_addrenv_attach failed: %d\n", ret);
      return ret;
    }

  /* Take a reference to the address environment */

  addrenv_take(ptcb->addrenv_own);

  /* Share the parent's address environment */

  tcb->addrenv_own = ptcb->addrenv_own;
  tcb->addrenv_curr = tcb->addrenv_own;

  return OK;
}

#ifdef CONFIG_ARCH_HAVE_FORK
/****************************************************************************
 * Name: addrenv_fork
 *
 * Description:
 *   Duplicate the parent process's address environment for a POSIX fork()
 *   child, and attach the duplicate to the child.
 *
 *   This is the counterpart of addrenv_join():  where join gives the child
 *   the parent's memory, fork gives it a copy -- its own pages, holding a
 *   snapshot of the parent's contents, mapped at the same virtual addresses.
 *   Mapping at the same addresses is what lets the copy be exact: every
 *   pointer the parent held into its own memory remains valid in the child,
 *   including the pointers inside the copied heap's own metadata.
 *
 *   The copy is eager -- there is no copy-on-write, because NuttX has no
 *   demand paging to build it on -- so forking a large process needs as much
 *   free memory as the process occupies, and fails with -ENOMEM if that is
 *   not available.  That is the nature of the primitive on this class of
 *   system, not a defect of this implementation; spawn-heavy code should
 *   prefer posix_spawn() or vfork().
 *
 * Input Parameters:
 *   ptcb - The tcb of the parent process
 *   tcb  - The tcb of the child process
 *
 * Returned Value:
 *   This is a NuttX internal function so it follows the convention that
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

int addrenv_fork(FAR struct tcb_s *ptcb, FAR struct tcb_s *tcb)
{
  FAR struct addrenv_s *addrenv;
  int ret;

  DEBUGASSERT(ptcb->addrenv_own != NULL);

  addrenv = addrenv_allocate();
  if (addrenv == NULL)
    {
      return -ENOMEM;
    }

  /* Duplicate the parent's regions into freshly allocated pages, mapped at
   * the same virtual addresses.
   */

  ret = up_addrenv_fork(&ptcb->addrenv_own->addrenv, &addrenv->addrenv);
  if (ret < 0)
    {
      berr("ERROR: up_addrenv_fork failed: %d\n", ret);
      addrenv_drop(addrenv, false);
      return ret;
    }

  /* Hand the reference taken by addrenv_allocate() to the child */

  return addrenv_attach(tcb, addrenv);
}
#endif /* CONFIG_ARCH_HAVE_FORK */

/****************************************************************************
 * Name: addrenv_leave
 *
 * Description:
 *   Leave a process's address environment.
 *
 * Input Parameters:
 *   tcb  - The tcb of the process
 *
 * Returned Value:
 *   This is a NuttX internal function so it follows the convention that
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

int addrenv_leave(FAR struct tcb_s *tcb)
{
  int ret;

  /* Detach from the address environment */

  ret = up_addrenv_detach(tcb);

  /* This thread is on its way out and must never be switched back into the
   * environment it is leaving:  it may well be gone before this thread is
   * scheduled again to finish its own teardown.
   */

  tcb->addrenv_curr = NULL;

  /* Then drop the address environment.  This runs inside the exiting task's
   * final critical section, so the destruction it may trigger has to be
   * deferred to the work queue.
   */

  addrenv_drop(tcb->addrenv_own, true);
  tcb->addrenv_own = NULL;

  return ret;
}

/****************************************************************************
 * Name: addrenv_select
 *
 * Description:
 *   Temporarily select a different address environment for the currently
 *   running process.
 *
 * Input Parameters:
 *   addrenv - The address environment to instantiate.
 *   oldenv  - The old active address environment is placed here.
 *
 * Returned Value:
 *   This is a NuttX internal function so it follows the convention that
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 * Note:
 *   This API is not safe to use from interrupt.
 *
 ****************************************************************************/

int addrenv_select(FAR struct addrenv_s *addrenv,
                   FAR struct addrenv_s **oldenv)
{
  FAR struct tcb_s *tcb = this_task();
  addrenv_take(addrenv);
  *oldenv = tcb->addrenv_curr;
  tcb->addrenv_curr = addrenv;
  return addrenv_switch(tcb);
}

/****************************************************************************
 * Name: addrenv_restore
 *
 * Description:
 *   Switch back to the procces's previous address environment.
 *
 * Input Parameters:
 *   addrenv - The address environment to restore.
 *
 * Returned Value:
 *   This is a NuttX internal function so it follows the convention that
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

int addrenv_restore(FAR struct addrenv_s *addrenv)
{
  FAR struct tcb_s *tcb = this_task();
  addrenv_give(tcb->addrenv_curr);
  tcb->addrenv_curr = addrenv;
  return addrenv_switch(tcb);
}

/****************************************************************************
 * Name: addrenv_take
 *
 * Description:
 *   Take a reference to an address environment.
 *
 * Input Parameters:
 *   addrenv - The address environment.
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

void addrenv_take(FAR struct addrenv_s *addrenv)
{
  /* A task can legitimately have no address environment -- addrenv_switch()
   * and addrenv_drop() both treat that as "nothing to do".  A kernel thread
   * never owns one, and in a protected build no task does:  there is a
   * single address space for the whole system and the architecture's
   * up_addrenv_*() are stubs.  There is then nothing to reference count.
   */

  if (addrenv != NULL)
    {
      atomic_fetch_add(&addrenv->refs, 1);
    }
}

/****************************************************************************
 * Name: addrenv_give
 *
 * Description:
 *   Give back a reference to an address environment, obtaining the resulting
 *   reference counter as returned value.
 *
 * Input Parameters:
 *   addrenv - The address environment.
 *
 * Returned Value:
 *   Remaining reference count.
 *
 ****************************************************************************/

int addrenv_give(FAR struct addrenv_s *addrenv)
{
  /* See addrenv_take():  nothing was counted, so nothing is given back.  A
   * non-zero count is returned so that callers never conclude the (absent)
   * address environment has become unreferenced and should be destroyed.
   */

  return addrenv ? atomic_fetch_sub(&addrenv->refs, 1) - 1 : 1;
}

/****************************************************************************
 * Name: addrenv_drop
 *
 * Description:
 *   Drop an address environment.
 *
 * Input Parameters:
 *   addrenv - The address environment.
 *   deferred - yes: The address environment should be dropped by the worker
 *              no:  The address environment can be dropped at once
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

void addrenv_drop(FAR struct addrenv_s *addrenv, bool deferred)
{
  if (addrenv == NULL)
    {
      /* No address environment, get out */

      return;
    }

  /* If no more users, the address environment can be dropped */

  if (addrenv_give(addrenv) == 0)
    {
      /* Defer dropping if requested to do so, otherwise drop at once */

      if (deferred)
        {
          /* Let the DSR do the heavy lifting */

          work_queue(LPWORK, &addrenv->work, addrenv_destroy, addrenv, 0);
        }
      else
        {
          addrenv_destroy(addrenv);
        }
    }
}
