# SPDX-License-Identifier: Apache-2.0

set(SRCS esp32s31_boot.c esp32s31_bringup.c)

if(CONFIG_ESP32S31_FLASH_APPFS)
  # Keep the U-mode application image outside the NuttX ELF.  The flash
  # helper writes this image at CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET.

  find_program(GENROMFS genromfs REQUIRED)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(APPFS_IMAGE ${NUTTX_BINARY_DIR}/appfs.img)
  set(APPFS_ROOT ${NUTTX_BINARY_DIR}/appfs-root)
  set(APPFS_PREPARE
      ${NUTTX_DIR}/tools/espressif/esp32s31_prepare_appfs.py)
  if(CONFIG_ESP32S31_FLASH_APPFS_E0_CORPUS)
    set(APPFS_CORPUS_ARG --e0-corpus)
  endif()
  add_custom_command(
    OUTPUT ${APPFS_IMAGE}
    COMMAND ${Python3_EXECUTABLE} ${APPFS_PREPARE}
            --source ${NUTTX_BINARY_DIR}/bin
            --output ${APPFS_ROOT}
            --max-size ${CONFIG_ELF_APP_MAX_FILESIZE}
            ${APPFS_CORPUS_ARG}
    COMMAND ${GENROMFS} -f ${APPFS_IMAGE} -d ${APPFS_ROOT}
            -V S31AppFS
    DEPENDS $<TARGET_PROPERTY:nuttx,NUTTX_LOADABLE_APPS> ${APPFS_PREPARE}
    COMMENT "Generating external ESP32-S31 application ROMFS"
    VERBATIM)
  add_custom_target(board_appfs DEPENDS ${APPFS_IMAGE})
  add_dependencies(board board_appfs)
elseif(CONFIG_BUILD_KERNEL)
  # Pack the separately linked U-mode applications after their targets have
  # been created.  A local target anchors the generated source in this board
  # directory without introducing an apps_post dependency cycle.

  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/romfs_boot.c
    COMMAND genromfs -f romfs.img -d ${NUTTX_BINARY_DIR}/bin -V NuttXBootVol
    COMMAND xxd -i romfs.img romfs_boot.c
    DEPENDS $<TARGET_PROPERTY:nuttx,NUTTX_LOADABLE_APPS>
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  add_custom_target(board_romfs
                    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/romfs_boot.c)
  add_dependencies(board board_romfs)
  list(APPEND SRCS ${CMAKE_CURRENT_BINARY_DIR}/romfs_boot.c)
endif()

target_sources(board PRIVATE ${SRCS})

string(REPLACE "\"" "" CHIP_SERIES "${CONFIG_ESPRESSIF_CHIP_SERIES}")
set(BOARD_COMMON_DIR "${NUTTX_BOARD_DIR}/../common")

set(LDSCRIPTS
    "${BOARD_COMMON_DIR}/scripts/${CHIP_SERIES}_aliases.ld"
    "${BOARD_COMMON_DIR}/scripts/${CHIP_SERIES}_flat_memory.ld")

if(CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT OR CONFIG_ESPRESSIF_SIMPLE_BOOT)
  list(APPEND LDSCRIPTS
       "${BOARD_COMMON_DIR}/scripts/${CHIP_SERIES}_sections.ld")
endif()

set_property(GLOBAL APPEND PROPERTY LD_SCRIPT ${LDSCRIPTS})
