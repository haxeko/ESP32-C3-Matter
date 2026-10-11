#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

/** One measurement window of the BL0937 */
typedef struct {
    float power_w;           /* active power, W */
    float voltage_v;         /* RMS voltage, V (last voltage window) */
    float current_a;         /* RMS current, A (last current window, or P/V when too low to measure) */
    uint64_t energy_mwh;     /* cumulative imported energy since first boot, mWh */
    bool voltage_valid;      /* at least one voltage window has completed */
} bl0937_reading_t;

/**
 * Configure CF / CF1 / SEL GPIOs and start counting pulses.
 *
 * @param energy_base_mwh energy restored from NVS, added to the pulse-counted energy.
 */
esp_err_t bl0937_init(uint64_t energy_base_mwh);

/**
 * Close the current measurement window and start the next one.
 *
 * Must be called periodically (every CONFIG_BL0937_UPDATE_INTERVAL_MS). Each call
 * evaluates CF (power, every window) and CF1 (alternating current / voltage windows).
 */
void bl0937_update(bl0937_reading_t *out);
