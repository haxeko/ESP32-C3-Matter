#pragma once

#include <esp_err.h>
#include <esp_matter.h>

/** Relay state applied at first boot (before anything is stored in NVS) */
#define DEFAULT_POWER false

typedef void *app_driver_handle_t;

/** Configure the relay GPIO (CONFIG_RELAY_GPIO) as output, relay OFF. */
app_driver_handle_t app_driver_relay_init();

/** Initialize the BOOT button: short press toggles the relay. */
app_driver_handle_t app_driver_button_init();

/** Apply an attribute change coming from Matter to the hardware. */
esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val);

/** Drive the relay from the OnOff attribute value stored in the data model (restores state after reboot). */
esp_err_t app_driver_relay_set_defaults(uint16_t endpoint_id);
