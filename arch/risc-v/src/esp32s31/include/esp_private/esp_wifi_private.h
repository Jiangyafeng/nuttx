/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX compatibility overlay for ESP-IDF's private Wi-Fi umbrella header.
 */

#ifndef __ARCH_RISCV_SRC_ESP32S31_INCLUDE_ESP_PRIVATE_ESP_WIFI_PRIVATE_H
#define __ARCH_RISCV_SRC_ESP32S31_INCLUDE_ESP_PRIVATE_ESP_WIFI_PRIVATE_H

#include "platform/os.h"
#include <sys/queue.h>

#include "sdkconfig.h"
#include "esp_wifi_crypto_types.h"
#include "esp_private/wifi_os_adapter.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WIFI_OSI_FUNCS_INITIALIZER() &g_wifi_osi_funcs

#ifdef __cplusplus
}
#endif

#endif
