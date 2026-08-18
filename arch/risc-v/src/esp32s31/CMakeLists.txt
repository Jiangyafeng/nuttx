# ##############################################################################
# arch/risc-v/src/esp32s31/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/espressif espressif)

target_compile_definitions(arch PRIVATE _RETARGETABLE_LOCKING)
target_sources(arch PRIVATE esp_chip_rev.c)

if(CONFIG_ARCH_RISCV_INTXCPT_EXTENSIONS)
  target_sources(arch PRIVATE esp32s31_intctx.c)
endif()

if(CONFIG_ESPRESSIF_WIFI)
  # The NuttX Wi-Fi OS adapter is shared by the RV32 Espressif targets.  S31
  # uses ABI v9; target-specific fields are selected in the adapter with
  # CONFIG_IDF_TARGET_ESP32S31.
  target_sources(arch PRIVATE ../esp32c6/esp_coex_adapter.c
                              ../esp32c6/esp_wifi_adapter.c)
endif()

if(CONFIG_ARCH_USE_S_MODE)
  target_sources(arch PRIVATE esp32s31_supervisor.S esp32s31_monstat.c)
endif()

if(CONFIG_ESP32S31_SMP)
  target_sources(arch PRIVATE esp32s31_cpu1_boot.S esp32s31_cpu1_boot.c)
endif()

if(CONFIG_ESP32S31_SMP)
  target_sources(arch PRIVATE esp32s31_smp.c)
endif()

if(CONFIG_ESP32S31_SMP_TLB_SHOOTDOWN)
  target_sources(arch PRIVATE esp32s31_tlb.c)
endif()

if(CONFIG_BUILD_KERNEL)
  target_sources(arch PRIVATE esp32s31_mm_init.c esp32s31_pgalloc.c
                              esp32s31_smode_libc.c)
  target_link_options(nuttx PRIVATE
                      -Wl,-u,esp32s31_smode_libc_keep)
endif()

# Flash instructions and read-only data use the same unified cache virtual
# address window.  The linker script intentionally overlays a NOLOAD rodata
# cursor on .flash.text.

target_link_options(nuttx PRIVATE -Wl,--no-check-sections)
