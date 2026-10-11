#pragma once

#include <esp_err.h>
#include <esp_matter.h>

/**
 * Add the Electrical Sensor device type (Power Topology + Electrical Power Measurement +
 * Electrical Energy Measurement) to the relay endpoint. Call before esp_matter::start().
 */
esp_err_t power_meter_add_to_endpoint(esp_matter::endpoint_t *endpoint);

/** Start the BL0937 and the task that publishes readings to Matter. Call after esp_matter::start(). */
esp_err_t power_meter_start(uint16_t endpoint_id);
