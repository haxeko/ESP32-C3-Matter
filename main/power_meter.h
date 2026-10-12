#pragma once

#include <esp_err.h>
#include <esp_matter.h>

/** Called from the meter task with the active power (W) of every measurement window. */
typedef void (*power_meter_cb_t)(float power_w);

#if CONFIG_POWER_METER_MATTER
/**
 * Add the Electrical Sensor device type (Power Topology + Electrical Power Measurement +
 * Electrical Energy Measurement) to the endpoint. Call before esp_matter::start().
 */
esp_err_t power_meter_add_to_endpoint(esp_matter::endpoint_t *endpoint);
#endif

/**
 * Start the BL0937 and the meter task. Every CONFIG_BL0937_UPDATE_INTERVAL_MS the power is passed to
 * `power_cb` and, with CONFIG_POWER_METER_MATTER, published on `endpoint_id`. Call after esp_matter::start().
 */
esp_err_t power_meter_start(uint16_t endpoint_id, power_meter_cb_t power_cb);
