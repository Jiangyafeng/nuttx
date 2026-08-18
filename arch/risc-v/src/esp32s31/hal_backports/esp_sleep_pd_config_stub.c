/* SPDX-License-Identifier: Apache-2.0 */

/* E.1 deliberately excludes power management.  The S31 modem clock component
 * records LP-clock power-domain preferences here for a future sleep
 * transition; only a transition acts on them.  Wi-Fi modem sleep does not
 * take that path -- the blob parks the modem itself -- so with CONFIG_PM off
 * there is no transition and accepting the preference is the complete and
 * safe implementation, matching what IDF does in the same configuration. */

#include <nuttx/config.h>

#include "esp_sleep.h"

#ifdef CONFIG_PM
#  error "esp_sleep_pd_config stub is only valid without power management"
#endif

esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t domain,
                              esp_sleep_pd_option_t option)
{
  (void)domain;
  (void)option;
  return ESP_OK;
}
