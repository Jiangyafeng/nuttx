/****************************************************************************
 * boards/risc-v/esp32s31/esp32s31-core-function-board/src/esp32s31-core-function-board.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_RISCV_ESP32S31_CORE_FUNCTION_BOARD_SRC_ESP32S31_CORE_FUNCTION_BOARD_H
#define __BOARDS_RISCV_ESP32S31_CORE_FUNCTION_BOARD_SRC_ESP32S31_CORE_FUNCTION_BOARD_H

#define ESP32S31_APPFS_BLOCKDEV "/dev/appfs"
#define ESP32S31_APPS_MTDDEV    "/dev/apps"
#define ESP32S31_APPFS_SEED_DIR "/system/bin"
#define ESP32S31_APPS_DIR       "/apps"

int esp_bringup(void);

#endif
