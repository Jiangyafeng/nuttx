/****************************************************************************
 * arch/risc-v/src/esp32s31/hal_backports/esp_wifi_wpa_ap_join_compat.c
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

/* ESP32-S31 libnet80211.a invokes wpa_cb->wpa_ap_join with a single pointer
 * to wpa_station_join_param_t, whereas the pinned esp-hal-3rdparty
 * esp_wpa_main.c registers the legacy 10-argument hostap_sta_join().
 * Wrap esp_wifi_register_wpa_cb_internal() so the SoftAP join callback
 * adapts the new struct-based ABI to the legacy supplicant callback without
 * modifying the pinned esp-hal-3rdparty submodule.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef struct
{
  void **sm;
  uint8_t *bssid;
  uint8_t *wpa_ie;
  uint8_t *rsnxe;
  bool *pmf_enable;
  uint8_t *pairwise_cipher;
  uint8_t *rsn_selection_ie;
  uint8_t *owe_dhie;
  int subtype;
  uint16_t rsnxe_len;
  uint8_t wpa_ie_len;
  uint8_t owe_dh_len;
} wpa_station_join_param_t;

typedef bool (*legacy_wpa_ap_join_t)(void **sm,
                                     uint8_t *bssid,
                                     uint8_t *wpa_ie,
                                     uint8_t wpa_ie_len,
                                     uint8_t *rsnxe,
                                     uint16_t rsnxe_len,
                                     bool *pmf_enable,
                                     int subtype,
                                     uint8_t *pairwise_cipher,
                                     uint8_t *rsn_selection_ie);

/* Prefix of struct wpa_funcs matching esp_wifi_driver.h through wpa_ap_join */

struct wpa_funcs_prefix_s
{
  void *wpa_sta_init;
  void *wpa_sta_deinit;
  void *wpa_sta_connect;
  void *wpa_sta_connected_cb;
  void *wpa_sta_disconnected_cb;
  void *wpa_sta_rx_eapol;
  void *wpa_sta_in_4way_handshake;
  void *wpa_ap_init;
  void *wpa_ap_deinit;
  void *wpa_ap_join;
};

_Static_assert(offsetof(struct wpa_funcs_prefix_s, wpa_ap_join) ==
               9 * sizeof(void *),
               "wpa_ap_join slot offset mismatch");

/****************************************************************************
 * Private Data
 ****************************************************************************/

static legacy_wpa_ap_join_t g_legacy_wpa_ap_join;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

extern int __real_esp_wifi_register_wpa_cb_internal(void *cb);

static bool esp32s31_wpa_ap_join_trampoline(wpa_station_join_param_t *join)
{
  if (join == NULL || g_legacy_wpa_ap_join == NULL)
    {
      return false;
    }

  return g_legacy_wpa_ap_join(join->sm,
                              join->bssid,
                              join->wpa_ie,
                              join->wpa_ie_len,
                              join->rsnxe,
                              join->rsnxe_len,
                              join->pmf_enable,
                              join->subtype,
                              join->pairwise_cipher,
                              join->rsn_selection_ie);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int __wrap_esp_wifi_register_wpa_cb_internal(void *cb)
{
  struct wpa_funcs_prefix_s *funcs = (struct wpa_funcs_prefix_s *)cb;

  if (funcs != NULL && funcs->wpa_ap_join != NULL)
    {
      g_legacy_wpa_ap_join = (legacy_wpa_ap_join_t)funcs->wpa_ap_join;
      funcs->wpa_ap_join = (void *)esp32s31_wpa_ap_join_trampoline;
    }

  return __real_esp_wifi_register_wpa_cb_internal(cb);
}
