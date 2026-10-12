#include <esp_log.h>

#include <app_priv.h>
#include <purifier.h>

#include <button_gpio.h>
#include <device.h>
#include <iot_button.h>

static const char *TAG = "app_driver";

static void app_driver_button_toggle_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Toggle button pressed");
    /* Same path as a Matter command: presses the purifier's power button and reports the result */
    purifier_toggle();
}

app_driver_handle_t app_driver_button_init()
{
    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = button_driver_get_config();

    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button device");
        return NULL;
    }

    iot_button_register_cb(handle, BUTTON_SINGLE_CLICK, NULL, app_driver_button_toggle_cb, NULL);
    return (app_driver_handle_t)handle;
}
