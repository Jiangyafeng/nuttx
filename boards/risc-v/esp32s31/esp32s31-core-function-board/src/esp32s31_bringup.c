/****************************************************************************
 * boards/risc-v/esp32s31/esp32s31-core-function-board/src/esp32s31_bringup.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/mount.h>
#include <syslog.h>

#include <nuttx/fs/fs.h>

#ifdef CONFIG_ESP32S31_SMP_TEST
#  include <assert.h>
#  include <errno.h>
#  include <inttypes.h>
#  include <sched.h>
#  include <stdbool.h>
#  include <stdlib.h>
#  include <sys/wait.h>
#  include <nuttx/arch.h>
#  include <nuttx/clock.h>
#  include <nuttx/kthread.h>
#  include <nuttx/semaphore.h>
#endif

#include "esp32s31_monstat.h"
#include "esp32s31_tlb.h"

#ifdef CONFIG_ESP32S31_FLASH_APPFS
#  include <errno.h>
#  include <fcntl.h>
#  include <inttypes.h>
#  include <stdbool.h>
#  include <stdio.h>
#  include <string.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <nuttx/mtd/mtd.h>

#  include "espressif/esp_spiflash_mtd.h"
#elif defined(CONFIG_BUILD_KERNEL)
#  include <nuttx/drivers/ramdisk.h>
#  include <nuttx/fs/fs.h>
#endif

#include "esp32s31-core-function-board.h"

#ifdef CONFIG_RTC_DRIVER
#  include "espressif/esp_rtc.h"
#endif

#ifdef CONFIG_ESPRESSIF_WIFI
#  include "espressif/esp_wlan_netdev.h"
#endif

#ifdef CONFIG_ESP32S31_FLASH_APPFS

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_ESP32S31_SMP_TEST
#  define ESP32S31_SMP_TEST_PRIORITY 80

static sem_t g_esp32s31_smp_start;
static sem_t g_esp32s31_smp_ack;
static volatile int g_esp32s31_smp_test_status;

extern volatile uint32_t g_esp32s31_smp_ipi_send[CONFIG_SMP_NCPUS];
extern volatile uint32_t g_esp32s31_smp_ipi_receive[CONFIG_SMP_NCPUS];
extern volatile uint32_t g_esp32s31_smp_timer_count[CONFIG_SMP_NCPUS];
extern volatile uint32_t g_esp32s31_smp_cpu1_stage;
extern volatile uint32_t g_esp32s31_smp_cpu1_cpuint;
extern volatile uint32_t g_esp32s31_smp_cpu1_sstatus;
extern volatile uint32_t g_esp32s31_smp_cpu1_sintthresh;
extern uint32_t esp32s31_smp_cpu1_ipi_state(void);
extern uint32_t esp32s31_smp_cpu1_ipi_route(void);

static int esp32s31_smp_cpu1_worker(int argc, char *argv[])
{
  int semvalue;
  int wait;
  int round;

  for (round = 0; round < CONFIG_ESP32S31_SMP_TEST_ROUNDS; round++)
    {
      if (nxsem_wait_uninterruptible(&g_esp32s31_smp_start) < 0 ||
          sched_getcpu() != 1)
        {
          g_esp32s31_smp_test_status = -EXDEV;
          nxsem_post(&g_esp32s31_smp_ack);
          return EXIT_FAILURE;
        }

      /* Make this a deterministic cross-core wakeup test.  Without this
       * handshake CPU1 can post the acknowledgement before CPU0 has joined
       * the wait queue, in which case no reverse scheduling IPI is needed.
       */

      semvalue = 0;
      for (wait = 0; wait < 20000; wait++)
        {
          nxsem_get_value(&g_esp32s31_smp_ack, &semvalue);
          if (semvalue < 0)
            {
              break;
            }

          up_udelay(10);
        }

      if (semvalue >= 0)
        {
          g_esp32s31_smp_test_status = -ETIMEDOUT;
          nxsem_post(&g_esp32s31_smp_ack);
          return EXIT_FAILURE;
        }

      g_esp32s31_smp_test_status = round + 1;
      nxsem_post(&g_esp32s31_smp_ack);
    }

  return EXIT_SUCCESS;
}

static int esp32s31_smp_scheduler_test(void)
{
  uint32_t ipi0_before = g_esp32s31_smp_ipi_receive[0];
  uint32_t ipi1_before = g_esp32s31_smp_ipi_receive[1];
  uint32_t timer1_before = g_esp32s31_smp_timer_count[1];
  cpu_set_t cpuset;
  cpu_set_t original_cpuset;
  bool parent_pinned = false;
  pid_t pid;
  int round;
  int ret;

  if (sched_getaffinity(0, sizeof(original_cpuset), &original_cpuset) < 0)
    {
      return -errno;
    }

  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0)
    {
      return -errno;
    }

  parent_pinned = true;
  nxsem_init(&g_esp32s31_smp_start, 0, 0);
  nxsem_init(&g_esp32s31_smp_ack, 0, 0);
  g_esp32s31_smp_test_status = 0;
  syslog(LOG_INFO, "TEST:BEGIN stage=smp-scheduler cpu=%d\n",
         sched_getcpu());

  /* Keep the worker below the board bring-up thread until its affinity is
   * installed.  G.2.0 validates a task pinned before first execution; live
   * cross-core task migration belongs to the later address-environment
   * shootdown stage. */

  pid = kthread_create("s31-cpu1-test", ESP32S31_SMP_TEST_PRIORITY, 2048,
                       esp32s31_smp_cpu1_worker, NULL);
  if (pid < 0)
    {
      ret = (int)pid;
      goto out;
    }

  syslog(LOG_INFO, "SMP scheduler worker created: pid=%d\n", pid);

  CPU_ZERO(&cpuset);
  CPU_SET(1, &cpuset);
  ret = sched_setaffinity(pid, sizeof(cpuset), &cpuset);
  if (ret < 0)
    {
      ret = -errno;
      goto out;
    }

  syslog(LOG_INFO, "SMP scheduler worker pinned: pid=%d cpu=1\n", pid);

  for (round = 0; round < CONFIG_ESP32S31_SMP_TEST_ROUNDS; round++)
    {
      nxsem_post(&g_esp32s31_smp_start);
      ret = nxsem_tickwait_uninterruptible(&g_esp32s31_smp_ack,
                                            SEC2TICK(2));
      if (ret < 0 || g_esp32s31_smp_test_status != round + 1 ||
          sched_getcpu() != 0)
        {
          ret = ret < 0 ? ret : -EIO;
          goto out;
        }
    }

  if (g_esp32s31_smp_ipi_receive[0] <= ipi0_before ||
      g_esp32s31_smp_ipi_receive[1] <= ipi1_before)
    {
      ret = -ETIMEDOUT;
      goto out;
    }

  /* Target1 is a private CPU1 interrupt, not a second owner of the global
   * NuttX clock.  Require at least one fresh interrupt after the test began
   * so a stale boot-time count cannot satisfy the per-core timer proof. */

  for (round = 0; round < 20000 &&
       g_esp32s31_smp_timer_count[1] <= timer1_before; round++)
    {
      up_udelay(10);
    }

  if (g_esp32s31_smp_timer_count[1] <= timer1_before)
    {
      ret = -ETIMEDOUT;
      goto out;
    }

  /* Without CONFIG_SCHED_CHILD_STATUS a finished kernel thread is reaped
   * immediately.  The final acknowledgement already proves that it ran to
   * completion, and a later waitpid() would race with that automatic reap
   * and report ECHILD.
   */

#ifdef CONFIG_SCHED_CHILD_STATUS
  if (waitpid(pid, NULL, 0) != pid)
    {
      ret = -errno;
      goto out;
    }
#endif

  pid = -1;

  syslog(LOG_INFO,
         "TEST:PASS stage=smp-scheduler rounds=%d cpu0-ipi=%" PRIu32
         " cpu1-ipi=%" PRIu32 " timer1=%" PRIu32 "\n",
         CONFIG_ESP32S31_SMP_TEST_ROUNDS,
         g_esp32s31_smp_ipi_receive[0] - ipi0_before,
         g_esp32s31_smp_ipi_receive[1] - ipi1_before,
         g_esp32s31_smp_timer_count[1] - timer1_before);
  ret = OK;

out:
  if (pid > 0 && ret < 0)
    {
      kthread_delete(pid);
    }

  if (ret >= 0)
    {
      nxsem_destroy(&g_esp32s31_smp_ack);
      nxsem_destroy(&g_esp32s31_smp_start);
    }

  if (parent_pinned)
    {
      sched_setaffinity(0, sizeof(original_cpuset), &original_cpuset);
    }

  if (ret < 0)
    {
      syslog(LOG_ERR,
             "TEST:FAIL stage=smp-scheduler ret=%d status=%d cpu1-stage=%"
             PRIu32 " cpuint=%" PRIu32 " ipi-send=%" PRIu32 "/%"
             PRIu32 " ipi-recv=%" PRIu32 "/%" PRIu32
             " pending=%" PRIu32 " route=%08" PRIx32
             " sstatus=%08" PRIx32 " sintthresh=%08" PRIx32 "\n",
             ret, g_esp32s31_smp_test_status, g_esp32s31_smp_cpu1_stage,
             g_esp32s31_smp_cpu1_cpuint,
             g_esp32s31_smp_ipi_send[0], g_esp32s31_smp_ipi_send[1],
             g_esp32s31_smp_ipi_receive[0],
             g_esp32s31_smp_ipi_receive[1],
             esp32s31_smp_cpu1_ipi_state(),
             esp32s31_smp_cpu1_ipi_route(),
             g_esp32s31_smp_cpu1_sstatus,
             g_esp32s31_smp_cpu1_sintthresh);
    }

  return ret;
}
#endif

static bool esp32s31_mtd_is_erased(struct mtd_dev_s *mtd)
{
  uint8_t buffer[64];
  ssize_t nread;
  size_t i;

  nread = MTD_READ(mtd, 0, sizeof(buffer), buffer);
  if (nread != sizeof(buffer))
    {
      return false;
    }

  for (i = 0; i < sizeof(buffer); i++)
    {
      if (buffer[i] != 0xff)
        {
          return false;
        }
    }

  return true;
}

static int esp32s31_mtd_boundary_test(struct mtd_dev_s *mtd,
                                      size_t partition_size)
{
  struct mtd_geometry_s geometry;
  uint8_t buffer = 0xff;
  int ret;

  ret = MTD_IOCTL(mtd, MTDIOC_GEOMETRY,
                  (unsigned long)((uintptr_t)&geometry));
  if (ret < 0)
    {
      return ret;
    }

  if (MTD_READ(mtd, partition_size, 1, &buffer) >= 0 ||
      MTD_WRITE(mtd, partition_size, 1, &buffer) >= 0 ||
      MTD_ERASE(mtd, geometry.neraseblocks, 1) >= 0)
    {
      return -EIO;
    }

  syslog(LOG_INFO, "AppFS MTD boundary checks: PASS\n");
  return OK;
}

#ifdef CONFIG_ESP32S31_FLASH_APPFS_SEED_INSTALL
static int esp32s31_copy_file(const char *source, const char *target,
                              mode_t mode)
{
  struct file src;
  struct file dst;
  uint8_t buffer[512];
  ssize_t nread;
  ssize_t nwritten;
  size_t offset;
  int ret;

  ret = file_open(&src, source, O_RDONLY);
  if (ret < 0)
    {
      return ret;
    }

  ret = file_open(&dst, target, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (ret < 0)
    {
      file_close(&src);
      return ret;
    }

  for (;;)
    {
      nread = file_read(&src, buffer, sizeof(buffer));
      if (nread <= 0)
        {
          ret = nread < 0 ? (int)nread : OK;
          break;
        }

      offset = 0;
      while (offset < nread)
        {
          nwritten = file_write(&dst, buffer + offset, nread - offset);
          if (nwritten <= 0)
            {
              ret = nwritten < 0 ? (int)nwritten : -EIO;
              goto out;
            }

          offset += nwritten;
        }
    }

  if (ret == OK)
    {
      ret = file_fsync(&dst);
    }

out:
  file_close(&dst);
  file_close(&src);
  return ret;
}

static int esp32s31_create_seed_marker(const char *path)
{
  static const char contents[] = "S31-E0\n";
  struct file marker;
  ssize_t nwritten;
  int ret;

  ret = file_open(&marker, path, O_WRONLY | O_CREAT | O_TRUNC, 0444);
  if (ret < 0)
    {
      return ret;
    }

  nwritten = file_write(&marker, contents, sizeof(contents) - 1);
  if (nwritten != sizeof(contents) - 1)
    {
      ret = nwritten < 0 ? (int)nwritten : -EIO;
    }
  else
    {
      ret = file_fsync(&marker);
    }

  file_close(&marker);
  return ret;
}

static int esp32s31_install_seed_app(void)
{
  static const char source[] = ESP32S31_APPFS_SEED_DIR "/sv32test";
  static const char source_hash[] =
    ESP32S31_APPFS_SEED_DIR "/sv32test.sha256";
  static const char target[] = ESP32S31_APPS_DIR "/sv32test";
  static const char target_hash[] = ESP32S31_APPS_DIR "/sv32test.sha256";
  static const char temp[] = ESP32S31_APPS_DIR "/.sv32test.new";
  static const char temp_hash[] =
    ESP32S31_APPS_DIR "/.sv32test.sha256.new";
  static const char marker[] = ESP32S31_APPS_DIR "/.seeded";
  struct stat st;
  int ret;

  /* The marker makes a later user deletion persistent.  For filesystems
   * seeded by an older image, adopt the existing complete pair first.
   */

  if (nx_stat(marker, &st, 0) == OK)
    {
      return OK;
    }

  if (nx_stat(target, &st, 0) == OK &&
      nx_stat(target_hash, &st, 0) == OK)
    {
      ret = esp32s31_create_seed_marker(marker);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: AppFS seed marker failed: %d\n", ret);
        }

      return ret;
    }

  nx_unlink(temp);
  nx_unlink(temp_hash);

  ret = esp32s31_copy_file(source, temp, 0555);
  if (ret < 0)
    {
      goto errout;
    }

  ret = esp32s31_copy_file(source_hash, temp_hash, 0444);
  if (ret < 0)
    {
      goto errout;
    }

  if (rename(temp, target) < 0)
    {
      ret = -errno;
      goto errout;
    }

  if (rename(temp_hash, target_hash) < 0)
    {
      ret = -errno;
      goto errout;
    }

  ret = esp32s31_create_seed_marker(marker);
  if (ret < 0)
    {
      goto errout;
    }

  syslog(LOG_INFO, "AppFS installed seed application: %s\n", target);
  return OK;

errout:
  nx_unlink(temp);
  nx_unlink(temp_hash);
  syslog(LOG_ERR, "ERROR: AppFS seed install failed: %d\n", ret);
  return ret;
}
#endif
#endif

#ifdef CONFIG_ESPRESSIF_WIFI
static int start_wifi_station(void)
{
  return esp_wlan_sta_initialize();
}
#endif

int esp_bringup(void)
{
  int ret = OK;

  ret = esp32s31_tlb_register_stats_device();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: /dev/tlbshoot register failed: %d\n", ret);
      return ret;
    }

  ret = esp32s31_monstat_register_device();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: /dev/s31stat register failed: %d\n", ret);
      return ret;
    }

#ifdef CONFIG_ESP32S31_SMP_TEST
  ret = esp32s31_smp_scheduler_test();
  if (ret < 0)
    {
      return ret;
    }
#endif

#ifdef CONFIG_FS_PROCFS
  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount procfs at /proc: %d\n", ret);
    }
#endif

#ifdef CONFIG_FS_TMPFS
  /* Resolver state must be shared by separately linked U-mode processes.
   * Keep it in a volatile filesystem rather than in process-local libc data
   * or persistent application flash.
   */

  ret = nx_mount(NULL, CONFIG_LIBC_TMPDIR, "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: tmpfs mount at %s failed: %d\n",
             CONFIG_LIBC_TMPDIR, ret);
      return ret;
    }
#endif

#ifdef CONFIG_RTC_DRIVER
  /* Wi-Fi and PHY callbacks use the ESP high-resolution timer adapter. */

  ret = esp_rtc_driverinit();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: RTC/high-resolution timer init failed: %d\n",
             ret);
      return ret;
    }
#endif

#ifdef CONFIG_ESP32S31_FLASH_APPFS
  uint8_t header[8];
  struct mtd_dev_s *seed_mtd;
  struct mtd_dev_s *apps_mtd;
  size_t region_size = CONFIG_ESPRESSIF_STORAGE_MTD_SIZE;
  size_t seed_size = CONFIG_ESP32S31_FLASH_APPFS_SEED_SIZE;
  size_t apps_offset;
  size_t apps_size;
  ssize_t nread;
  if (seed_size == 0 || seed_size >= region_size)
    {
      syslog(LOG_ERR, "ERROR: invalid AppFS seed size: %zu/%zu\n",
             seed_size, region_size);
      return -EINVAL;
    }

  apps_offset = CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET + seed_size;
  apps_size = region_size - seed_size;

  seed_mtd = esp_spiflash_alloc_mtdpart(
    CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET, seed_size);
  if (seed_mtd == NULL)
    {
      syslog(LOG_ERR, "ERROR: failed to create AppFS seed MTD\n");
      return -EINVAL;
    }

  nread = MTD_READ(seed_mtd, 0, sizeof(header), header);
  if (nread != sizeof(header))
    {
      syslog(LOG_ERR, "ERROR: AppFS header read failed: %zd\n", nread);
      return nread < 0 ? (int)nread : -EIO;
    }

  syslog(LOG_INFO,
         "AppFS header: %02" PRIx8 "%02" PRIx8 "%02" PRIx8 "%02" PRIx8
         "%02" PRIx8 "%02" PRIx8 "%02" PRIx8 "%02" PRIx8 "\n",
         header[0], header[1], header[2], header[3],
         header[4], header[5], header[6], header[7]);

  ret = ftl_initialize_by_path(ESP32S31_APPFS_BLOCKDEV, seed_mtd, O_RDONLY);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: AppFS FTL registration failed: %d\n", ret);
      return ret;
    }

  ret = nx_mount(ESP32S31_APPFS_BLOCKDEV, ESP32S31_APPFS_SEED_DIR,
                 "romfs", MS_RDONLY, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: AppFS seed mount failed: %d\n", ret);
      return ret;
    }

  apps_mtd = esp_spiflash_alloc_mtdpart(apps_offset, apps_size);
  if (apps_mtd == NULL)
    {
      syslog(LOG_ERR, "ERROR: failed to create writable AppFS MTD\n");
      return -EINVAL;
    }

  ret = esp32s31_mtd_boundary_test(apps_mtd, apps_size);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: AppFS MTD boundary checks failed: %d\n", ret);
      return ret;
    }

  ret = register_mtddriver(ESP32S31_APPS_MTDDEV, apps_mtd, 0755, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: writable AppFS MTD registration failed: %d\n",
             ret);
      return ret;
    }

  ret = nx_mount(ESP32S31_APPS_MTDDEV, ESP32S31_APPS_DIR,
                 "littlefs", 0, NULL);
  if (ret < 0 && esp32s31_mtd_is_erased(apps_mtd))
    {
      syslog(LOG_INFO, "AppFS LittleFS is blank; formatting once\n");
      ret = nx_mount(ESP32S31_APPS_MTDDEV, ESP32S31_APPS_DIR,
                     "littlefs", 0, "forceformat");
    }

  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: writable AppFS mount failed without destructive "
             "reformat: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "AppFS writable LittleFS: offset=%#zx size=%#zx\n",
         apps_offset, apps_size);

#ifdef CONFIG_ESP32S31_FLASH_APPFS_SEED_INSTALL
  ret = esp32s31_install_seed_app();
  if (ret < 0)
    {
      return ret;
    }
#endif

#ifdef CONFIG_ESPRESSIF_WIFI
  ret = start_wifi_station();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Wi-Fi station init failed: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "E1 Wi-Fi station netdev ready\n");
    }
#endif

  return ret;
#elif defined(CONFIG_BUILD_KERNEL)
  extern const unsigned char romfs_img[];
  extern const unsigned int romfs_img_len;

#  define SECTORSIZE  512
#  define NSECTORS(n) (((n) + SECTORSIZE - 1) / SECTORSIZE)

  if (NSECTORS(romfs_img_len) > 1)
    {
      int ret = romdisk_register(0, romfs_img, NSECTORS(romfs_img_len),
                                 SECTORSIZE);
      if (ret < 0)
        {
          return ret;
        }
    }
#endif

#ifdef CONFIG_ESPRESSIF_WIFI
  return start_wifi_station();
#endif

  return 0;
}
