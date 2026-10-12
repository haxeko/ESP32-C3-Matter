#pragma once

#include <esp_err.h>
#include <esp_matter.h>

typedef void *app_driver_handle_t;

/** Initialize the BOOT button: short press switches the purifier on / off. */
app_driver_handle_t app_driver_button_init();
