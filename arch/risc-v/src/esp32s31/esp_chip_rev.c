/****************************************************************************
 * arch/risc-v/src/esp32s31/esp_chip_rev.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Revision policy is intentionally left open during initial silicon bring-up.
 * Once Espressif publishes the supported production revision range, this hook
 * should reject unsupported revisions as the ESP32-P4 port does.
 */

void esp_chip_revision_check(void)
{
}
