/****************************************************************************
 * arch/risc-v/src/esp32s31/esp32s31_monstat.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Observation surface for the M-mode monitor's interrupt deferral protocol
 * and for address environment lifetime.  Both answer questions that no NSH
 * command can: a replayed CLIC input looks exactly like a directly forwarded
 * one from S-mode, and `free` deltas cannot separate a leaked address
 * environment from allocator fragmentation.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include <nuttx/fs/fs.h>
#include <nuttx/mutex.h>

#include "esp32s31_monstat.h"

#ifdef CONFIG_ARCH_USE_S_MODE

#ifdef CONFIG_ARCH_ADDRENV
#  include "addrenv.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Must match the layout allocated by esp32s31_supervisor.S. */

#define MONSTAT_CORES       2
#define MONSTAT_WORDS       8
#define MONSTAT_DEFER       0
#define MONSTAT_REPLAY      1
#define MONSTAT_ACK         2
#define MONSTAT_DROP        3
#define MONSTAT_SREPLAY     4

/* The text is built in a shared buffer under a mutex rather than on the
 * stack of a user-mode reader, which is too small for the full report.
 */

#define MONSTAT_TEXT_LEN    512

/****************************************************************************
 * External Data
 ****************************************************************************/

extern volatile uint32_t
  g_esp32s31_machine_monstat[MONSTAT_CORES][MONSTAT_WORDS];
extern volatile uint32_t g_esp32s31_machine_forward_mask[MONSTAT_CORES];

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static ssize_t esp32s31_monstat_read(FAR struct file *filep,
                                     FAR char *buffer, size_t buflen);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static char g_esp32s31_monstat_text[MONSTAT_TEXT_LEN];
static mutex_t g_esp32s31_monstat_lock = NXMUTEX_INITIALIZER;

static const struct file_operations g_esp32s31_monstat_ops =
{
  NULL,                     /* open */
  NULL,                     /* close */
  esp32s31_monstat_read,    /* read */
  NULL,                     /* write */
  NULL,                     /* seek */
  NULL,                     /* ioctl */
  NULL,                     /* mmap */
  NULL,                     /* truncate */
  NULL,                     /* poll */
  NULL,                     /* readv */
  NULL                      /* writev */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static size_t esp32s31_monstat_format(FAR char *text, size_t textlen)
{
  size_t len = 0;
  int cpu;

  for (cpu = 0; cpu < MONSTAT_CORES && len < textlen; cpu++)
    {
      len += snprintf(text + len, textlen - len,
                      "MONIRQ: cpu=%d defer=%" PRIu32 " replay=%" PRIu32
                      " ack=%" PRIu32 " drop=%" PRIu32
                      " pending=0x%" PRIx32 " sreplay=%" PRIu32 "\n",
                      cpu,
                      g_esp32s31_machine_monstat[cpu][MONSTAT_DEFER],
                      g_esp32s31_machine_monstat[cpu][MONSTAT_REPLAY],
                      g_esp32s31_machine_monstat[cpu][MONSTAT_ACK],
                      g_esp32s31_machine_monstat[cpu][MONSTAT_DROP],
                      g_esp32s31_machine_forward_mask[cpu],
                      g_esp32s31_machine_monstat[cpu][MONSTAT_SREPLAY]);
    }

#ifdef CONFIG_ARCH_ADDRENV
  if (len < textlen)
    {
      len += snprintf(text + len, textlen - len,
                      "ADDRENV: created=%" PRIu32 " fail=%" PRIu32
                      " destroyed=%" PRIu32 " live=%" PRIu32
                      " livemax=%" PRIu32 " pages=%" PRIu32
                      " pgt=%" PRIu32 "\n",
                      g_riscv_addrenv_stats.created,
                      g_riscv_addrenv_stats.create_fail,
                      g_riscv_addrenv_stats.destroyed,
                      g_riscv_addrenv_stats.live,
                      g_riscv_addrenv_stats.live_max,
                      g_riscv_addrenv_stats.pages_freed,
                      g_riscv_addrenv_stats.pgt_freed);
    }
#endif

  if (len >= textlen)
    {
      len = textlen - 1;
    }

  return len;
}

static ssize_t esp32s31_monstat_read(FAR struct file *filep,
                                     FAR char *buffer, size_t buflen)
{
  FAR char *text = g_esp32s31_monstat_text;
  size_t len;
  size_t ncopy;
  int ret;

  ret = nxmutex_lock(&g_esp32s31_monstat_lock);
  if (ret < 0)
    {
      return ret;
    }

  len = esp32s31_monstat_format(text, MONSTAT_TEXT_LEN);

  if (filep->f_pos >= (off_t)len)
    {
      nxmutex_unlock(&g_esp32s31_monstat_lock);
      return 0;
    }

  ncopy = len - (size_t)filep->f_pos;
  if (ncopy > buflen)
    {
      ncopy = buflen;
    }

  memcpy(buffer, text + filep->f_pos, ncopy);
  filep->f_pos += (off_t)ncopy;
  nxmutex_unlock(&g_esp32s31_monstat_lock);
  return (ssize_t)ncopy;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp32s31_monstat_register_device(void)
{
  return register_driver("/dev/s31stat", &g_esp32s31_monstat_ops, 0444,
                         NULL);
}

#endif /* CONFIG_ARCH_USE_S_MODE */
