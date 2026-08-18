/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_smp.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <strings.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/compiler.h>
#include <nuttx/irq.h>
#include <nuttx/sched_note.h>

#include "sched/sched.h"
#include "init/init.h"
#include "riscv_internal.h"
#include "riscv_mmu.h"
#include "riscv_percpu.h"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_ipc.h"
#include "esp_irq.h"
#include "esp_private/esp_ipc.h"
#include "esp_private/systimer.h"
#include "hal/crosscore_int_ll.h"
#include "hal/systimer_ll.h"
#include "riscv/csr_clic.h"
#include "soc/clic_reg.h"
#include "soc/interrupts.h"
#include "soc/soc.h"

#include "esp32s31_cpu1_boot.h"
#include "esp32s31_monitor.h"
#include "esp32s31_tlb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP32S31_SMP_CPU0              0
#define ESP32S31_SMP_CPU1              1
#define ESP32S31_SMP_CPU1_TIMER_ALARM  SYSTIMER_ALARM_OS_TICK_CORE1
#define ESP32S31_SMP_RV_INT_MASK       63
#define ESP32S31_SMP_CLIC_PRIORITY     2

static_assert(CLIC_INT_CTRL_REG(CLIC_EXT_INTR_NUM_OFFSET) ==
              ESP32S31_MCALL_CLIC_CTRL_BASE,
              "monitor CLIC base does not match the HAL");
static_assert((CLIC_INT_IP_M | CLIC_INT_IE_M | CLIC_INT_ATTR_TRIG_M |
               CLIC_INT_ATTR_SHV_M | CLIC_INT_CTL_M) ==
              ESP32S31_MCALL_CLIC_WRITABLE_MASK,
              "monitor CLIC mask does not match the HAL");

/****************************************************************************
 * Public Data
 ****************************************************************************/

volatile uint32_t g_esp32s31_smp_ipi_send[CONFIG_SMP_NCPUS];
volatile uint32_t g_esp32s31_smp_ipi_receive[CONFIG_SMP_NCPUS];
volatile uint32_t g_esp32s31_smp_timer_count[CONFIG_SMP_NCPUS];
volatile uint32_t g_esp32s31_smp_cpu1_stage;
volatile uint32_t g_esp32s31_smp_cpu1_cpuint;
volatile uint32_t g_esp32s31_smp_cpu1_timer_cpuint;
volatile uint32_t g_esp32s31_smp_cpu1_sstatus;
volatile uint32_t g_esp32s31_smp_cpu1_sintthresh;

static void IRAM_ATTR esp32s31_smp_process_pending(int irq, void *context);

uint32_t esp32s31_smp_cpu1_ipi_state(void)
{
  return crosscore_int_ll_get_state(ESP32S31_SMP_CPU1);
}

uint32_t esp32s31_smp_cpu1_ipi_route(void)
{
  return REG_READ(DR_REG_INTERRUPT_CORE1_BASE +
                  4 * ETS_CPU_INTR_FROM_CPU_1_SOURCE);
}

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp32s31_ipc_call_s
{
  struct smp_call_data_s call;
  esp_ipc_func_t         func;
  FAR void              *arg;
  volatile bool          busy;
};

struct esp32s31_ipc_blocking_s
{
  esp_ipc_func_t func;
  FAR void      *arg;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct esp32s31_ipc_call_s
  g_esp32s31_ipc_call[CONFIG_SMP_NCPUS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline void esp32s31_smp_fence(void)
{
  __asm__ volatile ("fence rw, rw" ::: "memory");
}

static int IRAM_ATTR esp32s31_smp_configure_cpuint(uint32_t cpuint)
{
  const uintptr_t config_mask =
    CLIC_INT_IE_M | CLIC_INT_ATTR_TRIG_M |
    CLIC_INT_ATTR_SHV_M | CLIC_INT_CTL_M;
  register uintptr_t error __asm__("a0") =
    ESP32S31_MCALL_CLIC_CONFIG;
  register uintptr_t result __asm__("a1") = cpuint;
  register uintptr_t clear __asm__("a2") =
    CLIC_INT_IP_M | config_mask;
  register uintptr_t value __asm__("a3") =
    CLIC_INT_IE_M |
    (NLBITS_TO_BYTE(ESP32S31_SMP_CLIC_PRIORITY) << CLIC_INT_CTL_S);

  __asm__ volatile
    (
      "ecall\n"
      : "+r"(error), "+r"(result), "+r"(clear), "+r"(value)
      :
      : "a4", "a5", "a6", "a7", "t0", "t1", "t2", "t3", "t4",
        "t5", "t6", "memory"
    );

  if ((intptr_t)error != 0 || (result & config_mask) != value)
    {
      return -EIO;
    }

  return OK;
}

static int IRAM_ATTR esp32s31_ipc_async_handler(FAR void *arg)
{
  FAR struct esp32s31_ipc_call_s *ipc = arg;

  ipc->func(ipc->arg);
  __atomic_store_n(&ipc->busy, false, __ATOMIC_RELEASE);
  return OK;
}

static int esp32s31_ipc_blocking_handler(FAR void *arg)
{
  FAR struct esp32s31_ipc_blocking_s *ipc = arg;

  ipc->func(ipc->arg);
  return OK;
}

static int IRAM_ATTR esp32s31_smp_ipi_handler(int irq, void *context,
                                               void *arg)
{
  int cpu = this_cpu();

  (void)arg;
  crosscore_int_ll_clear_interrupt(cpu);
  g_esp32s31_smp_ipi_receive[cpu]++;
  esp32s31_smp_fence();

  /* Complete TLB shootdowns before scheduler work so a remote hart waiting
   * with interrupts masked observes the ack promptly.
   */

  esp32s31_tlb_shootdown_process();

  esp32s31_smp_process_pending(irq, context);

  return OK;
}

static int IRAM_ATTR esp32s31_smp_timer1_handler(int irq, void *context,
                                                 void *arg)
{
  systimer_ll_clear_alarm_int(&SYSTIMER,
                              ESP32S31_SMP_CPU1_TIMER_ALARM);
  g_esp32s31_smp_timer_count[ESP32S31_SMP_CPU1]++;
  esp32s31_smp_fence();

  /* CPU0 owns the NuttX global tick.  nxsched_process_timer() accounts for
   * every configured CPU, so calling it here would double wall time and all
   * timeout accounting.  Target1 is a private local interrupt source; task
   * delivery remains owned exclusively by the cross-core scheduler IPI.
   */

  return OK;
}

static int esp32s31_smp_setup_ipi(int cpu)
{
  int source = cpu == ESP32S31_SMP_CPU0 ?
               ETS_CPU_INTR_FROM_CPU_0_SOURCE :
               ETS_CPU_INTR_FROM_CPU_1_SOURCE;
  int cpuint;

  crosscore_int_ll_clear_interrupt(cpu);
  cpuint = esp_setup_irq(source, ESP_IRQ_PRIORITY_2,
                         ESP_IRQ_TRIGGER_LEVEL,
                         esp32s31_smp_ipi_handler, NULL);
  if (cpuint < 0)
    {
      return cpuint;
    }

  if (cpu == ESP32S31_SMP_CPU1)
    {
      /* The IDF allocator derives the interrupt-matrix core from its own
       * runtime core-id helper.  S31 runs NuttX in S-mode, where the port's
       * sscratch hart mapping is authoritative.  Program the proven G.1
       * core1 matrix path explicitly so the scheduler IPI cannot be left on
       * core0 by an M-mode-centric helper. */

      uintptr_t reg = DR_REG_INTERRUPT_CORE1_BASE + 4 * source;

      REG_SET_BITS(reg, cpuint + RV_EXTERNAL_INT_OFFSET,
                   ESP32S31_SMP_RV_INT_MASK);
      REG_SET_BITS(reg, 0, 3U << 8);
    }

  up_enable_irq(ESP_SOURCE2IRQ(source));
  if (cpu == ESP32S31_SMP_CPU1)
    {
      g_esp32s31_smp_cpu1_cpuint = cpuint;
      if (esp32s31_smp_configure_cpuint(cpuint) < 0)
        {
          return -EIO;
        }
    }

  return OK;
}

static int esp32s31_smp_setup_timer1(void)
{
  uintptr_t reg;
  int cpuint;

  cpuint = esp_setup_irq_with_flags(
    ETS_SYSTIMER_TARGET1_INTR_SOURCE,
    (1 << ESP_IRQ_PRIORITY_2) | ESP_INTR_FLAG_IRAM |
    ESP_INTR_FLAG_INTRDISABLED,
    esp32s31_smp_timer1_handler, NULL);
  if (cpuint < 0)
    {
      return cpuint;
    }

  /* Keep the target1 route and local CLIC input explicit.  The allocator
   * already sees the NuttX per-CPU hart ID, but these two writes preserve
   * the G.1.2-proven ownership rule and ensure the S-mode monitor can
   * forward this input before the periodic alarm is enabled. */

  reg = DR_REG_INTERRUPT_CORE1_BASE +
        4 * ETS_SYSTIMER_TARGET1_INTR_SOURCE;
  REG_SET_BITS(reg, cpuint + RV_EXTERNAL_INT_OFFSET,
               ESP32S31_SMP_RV_INT_MASK);
  REG_SET_BITS(reg, 0, 3U << 8);
  g_esp32s31_smp_cpu1_timer_cpuint = cpuint;
  if (esp32s31_smp_configure_cpuint(cpuint) < 0)
    {
      return -EIO;
    }

  systimer_ll_enable_alarm(&SYSTIMER,
                           ESP32S31_SMP_CPU1_TIMER_ALARM, false);
  systimer_ll_enable_alarm_int(&SYSTIMER,
                               ESP32S31_SMP_CPU1_TIMER_ALARM, false);
  systimer_ll_clear_alarm_int(&SYSTIMER,
                              ESP32S31_SMP_CPU1_TIMER_ALARM);
  systimer_ll_connect_alarm_counter(&SYSTIMER,
                                    ESP32S31_SMP_CPU1_TIMER_ALARM,
                                    SYSTIMER_COUNTER_OS_TICK);
  systimer_ll_set_alarm_period(&SYSTIMER,
                               ESP32S31_SMP_CPU1_TIMER_ALARM,
                               systimer_us_to_ticks(CONFIG_USEC_PER_TICK));
  systimer_ll_enable_alarm_period(&SYSTIMER,
                                  ESP32S31_SMP_CPU1_TIMER_ALARM);
  systimer_ll_apply_alarm_value(&SYSTIMER,
                                ESP32S31_SMP_CPU1_TIMER_ALARM);
  systimer_ll_enable_alarm_int(&SYSTIMER,
                               ESP32S31_SMP_CPU1_TIMER_ALARM, true);
  systimer_ll_enable_alarm(&SYSTIMER,
                           ESP32S31_SMP_CPU1_TIMER_ALARM, true);
  up_enable_irq(ESP_SOURCE2IRQ(ETS_SYSTIMER_TARGET1_INTR_SOURCE));
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

static void IRAM_ATTR esp32s31_smp_process_pending(int irq, void *context)
{
  struct tcb_s *tcb;
  int cpu = this_cpu();

  nxsched_smp_call_handler(irq, context, NULL);

  tcb = current_task(cpu);
  riscv_savecontext(tcb);
  nxsched_process_delivered(cpu);
  tcb = current_task(cpu);
  riscv_restorecontext(tcb);
}

int riscv_smp_call_handler(int irq, void *context, void *arg)
{
  return esp32s31_smp_ipi_handler(irq, context, arg);
}

esp_err_t esp_ipc_call_blocking(uint32_t cpu, esp_ipc_func_t func,
                                FAR void *arg)
{
  struct esp32s31_ipc_blocking_s ipc;
  int ret;

  if (cpu >= CONFIG_SMP_NCPUS || func == NULL)
    {
      return ESP_ERR_INVALID_ARG;
    }

  if (cpu == ESP32S31_SMP_CPU1 &&
      g_esp32s31_cpu1_boot_state != ESP32S31_CPU1_SMP_ONLINE)
    {
      return ESP_ERR_INVALID_STATE;
    }

  ipc.func = func;
  ipc.arg  = arg;
  ret = nxsched_smp_call_single(cpu, esp32s31_ipc_blocking_handler, &ipc);
  return ret < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_ipc_call(uint32_t cpu, esp_ipc_func_t func, FAR void *arg)
{
  /* NuttX's synchronous SMP call has the stronger completion guarantee.
   * None of the S31 HAL call sites require the weaker "callback started"
   * return point used by the IDF IPC task implementation. */

  return esp_ipc_call_blocking(cpu, func, arg);
}

esp_err_t IRAM_ATTR esp_ipc_call_nonblocking(uint32_t cpu,
                                             esp_ipc_func_t func,
                                             FAR void *arg)
{
  FAR struct esp32s31_ipc_call_s *ipc;
  bool expected = false;
  int ret;

  if (cpu >= CONFIG_SMP_NCPUS || func == NULL)
    {
      return ESP_ERR_INVALID_ARG;
    }

  if (cpu == ESP32S31_SMP_CPU1 &&
      g_esp32s31_cpu1_boot_state != ESP32S31_CPU1_SMP_ONLINE)
    {
      return ESP_ERR_INVALID_STATE;
    }

  ipc = &g_esp32s31_ipc_call[cpu];
  if (!__atomic_compare_exchange_n(&ipc->busy, &expected, true, false,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
      return ESP_FAIL;
    }

  ipc->func = func;
  ipc->arg  = arg;
  nxsched_smp_call_init(&ipc->call, esp32s31_ipc_async_handler, ipc);
  ret = nxsched_smp_call_single_async(cpu, &ipc->call);
  if (ret < 0)
    {
      __atomic_store_n(&ipc->busy, false, __ATOMIC_RELEASE);
      return ESP_FAIL;
    }

  return ESP_OK;
}

noreturn_function void IRAM_ATTR esp32s31_cpu1_smp_entry(void)
{
  struct tcb_s *tcb;
  int ret;

  /* Installing the per-CPU area also arms the trap shim: it treats a
   * non-zero sscratch as "the full NuttX trap path is usable now".
   */

  riscv_percpu_add_hart(ESP32S31_SMP_CPU1);
  esp32s31_smp_fence();
  tcb = current_task(ESP32S31_SMP_CPU1);
  riscv_percpu_set_thread(tcb);
  __asm__ volatile ("mv tp, %0" : : "r"(tcb) : "memory");

  mmu_enable(g_kernel_pgt_pbase, 0);
  riscv_fpuconfig();

  ret = esp32s31_smp_setup_ipi(ESP32S31_SMP_CPU1);
  g_esp32s31_smp_cpu1_stage = 1;
  esp32s31_smp_fence();

  if (ret < 0)
    {
      g_esp32s31_cpu1_boot_state = ESP32S31_CPU1_BOOT_SMODE_FAULT;
      esp32s31_smp_fence();
      for (; ; )
        {
          __asm__ volatile ("wfi");
        }
    }

  ret = esp32s31_smp_setup_timer1();
  g_esp32s31_smp_cpu1_stage = 2;
  esp32s31_smp_fence();

  if (ret < 0)
    {
      g_esp32s31_cpu1_boot_state = ESP32S31_CPU1_BOOT_SMODE_FAULT;
      esp32s31_smp_fence();
      for (; ; )
        {
          __asm__ volatile ("wfi");
        }
    }

#ifdef CONFIG_STACK_COLORATION
  riscv_stack_color(tcb->stack_alloc_ptr, 0);
#endif

#ifdef CONFIG_SCHED_INSTRUMENTATION
  sched_note_cpu_started(tcb);
#endif

  g_esp32s31_cpu1_boot_state = ESP32S31_CPU1_SMP_ONLINE;
  g_esp32s31_smp_cpu1_stage = 3;
  esp32s31_smp_fence();

  up_irq_enable();
  g_esp32s31_smp_cpu1_sstatus = READ_CSR(CSR_STATUS);
  g_esp32s31_smp_cpu1_sintthresh = READ_CSR(CSR_SINTTHRESH);
  g_esp32s31_smp_cpu1_stage = 4;
  esp32s31_smp_fence();
  nx_idle_trampoline();

  for (; ; )
    {
      __asm__ volatile ("wfi");
    }
}

int up_cpu_start(int cpu)
{
  uint32_t timeout;
  int ret;

  DEBUGASSERT(cpu == ESP32S31_SMP_CPU1 && this_cpu() == ESP32S31_SMP_CPU0);

#ifdef CONFIG_SCHED_INSTRUMENTATION
  sched_note_cpu_start(this_task(), cpu);
#endif

  if (g_esp32s31_cpu1_boot_state != ESP32S31_CPU1_SMP_READY)
    {
      return -EHOSTDOWN;
    }

  ret = esp32s31_smp_setup_ipi(ESP32S31_SMP_CPU0);
  if (ret < 0)
    {
      return ret;
    }

  g_esp32s31_cpu1_smp_release = 1;
  esp32s31_smp_fence();

  for (timeout = CONFIG_ESP32S31_CPU1_BOOT_TIMEOUT_US;
       timeout > 0; timeout--)
    {
      esp32s31_smp_fence();
      if (g_esp32s31_cpu1_boot_state == ESP32S31_CPU1_SMP_ONLINE)
        {
          syslog(LOG_INFO,
                 "CPU1 SMP online: ipi0=%" PRIu32 " timer1=%" PRIu32 "\n",
                 g_esp32s31_smp_ipi_receive[0],
                 g_esp32s31_smp_timer_count[1]);
          return OK;
        }

      if (g_esp32s31_cpu1_boot_state ==
          ESP32S31_CPU1_BOOT_SMODE_FAULT)
        {
          return -EIO;
        }

      up_udelay(1);
    }

  g_esp32s31_cpu1_boot_state = ESP32S31_CPU1_SMP_TIMEOUT;
  esp32s31_smp_fence();
  return -ETIMEDOUT;
}

int up_send_smp_sched(int cpu)
{
  if (cpu < 0 || cpu >= CONFIG_SMP_NCPUS)
    {
      return -EINVAL;
    }

  if (cpu == ESP32S31_SMP_CPU1 &&
      g_esp32s31_cpu1_boot_state != ESP32S31_CPU1_SMP_ONLINE)
    {
      return -EHOSTDOWN;
    }

  g_esp32s31_smp_ipi_send[cpu]++;
  esp32s31_smp_fence();
  crosscore_int_ll_trigger_interrupt(cpu);
  return OK;
}

void up_send_smp_call(cpu_set_t cpuset)
{
  int cpu;

  for (; cpuset != 0; cpuset &= ~(1 << cpu))
    {
      cpu = ffs(cpuset) - 1;
      up_send_smp_sched(cpu);
    }
}
