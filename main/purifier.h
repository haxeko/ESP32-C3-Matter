#pragma once

#include <stdint.h>
#include <esp_err.h>

/**
 * Air purifier controlled through its power button.
 *
 * The relay contacts are wired in parallel with the purifier's power button: switching it on or off
 * is a CONFIG_PURIFIER_PULSE_MS "press" of the relay. The real state is not remembered but measured:
 * the purifier is ON while its power draw is above CONFIG_PURIFIER_ON_THRESHOLD_MW. The Fan Control
 * cluster of the Air Purifier endpoint always reports that measured state, except for a short window
 * after a press while waiting for the purifier to react.
 */

/** Configure the relay GPIO, released. Call first thing at boot so the button is never held. */
esp_err_t purifier_init();

/** Attach to the Fan Control cluster of `endpoint_id` and start the control task. Call after esp_matter::start(). */
esp_err_t purifier_start(uint16_t endpoint_id);

/** Feed one power measurement (W). Called by the power meter for every measurement window. */
void purifier_report_power(float power_w);

/** Local button: switch the purifier to the opposite of its current state. */
void purifier_toggle();
