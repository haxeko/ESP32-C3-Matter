#include <esp_log.h>
#include <driver/gpio.h>

#include <app_priv.h>
#include <common_macros.h>
#include <esp_matter.h>

#include <button_gpio.h>
#include <device.h>
#include <iot_button.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";
extern uint16_t relay_endpoint_id;

static constexpr gpio_num_t k_relay_gpio = (gpio_num_t)CONFIG_RELAY_GPIO;

static esp_err_t app_driver_relay_set_power(bool on)
{
    int level = on ? CONFIG_RELAY_ACTIVE_LEVEL : !CONFIG_RELAY_ACTIVE_LEVEL;
    ESP_LOGI(TAG, "Relay (GPIO%d) -> %s", (int)k_relay_gpio, on ? "ON" : "OFF");
    return gpio_set_level(k_relay_gpio, level);
}

static void app_driver_button_toggle_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Toggle button pressed");
    uint16_t endpoint_id = relay_endpoint_id;
    uint32_t cluster_id = OnOff::Id;
    uint32_t attribute_id = OnOff::Attributes::OnOff::Id;

    attribute_t *attribute = attribute::get(endpoint_id, cluster_id, attribute_id);

    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    attribute::get_val(attribute, &val);
    val.val.b = !val.val.b;
    /* update() runs the attribute callback (drives the relay) and reports the new state to controllers */
    attribute::update(endpoint_id, cluster_id, attribute_id, &val);
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    esp_err_t err = ESP_OK;
    if (endpoint_id == relay_endpoint_id && cluster_id == OnOff::Id &&
        attribute_id == OnOff::Attributes::OnOff::Id) {
        err = app_driver_relay_set_power(val->val.b);
    }
    return err;
}

esp_err_t app_driver_relay_set_defaults(uint16_t endpoint_id)
{
    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    attribute_t *attribute = attribute::get(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
    esp_err_t err = attribute::get_val(attribute, &val);
    if (err != ESP_OK) {
        return err;
    }
    return app_driver_relay_set_power(val.val.b);
}

app_driver_handle_t app_driver_relay_init()
{
    gpio_reset_pin(k_relay_gpio);
    gpio_set_direction(k_relay_gpio, GPIO_MODE_OUTPUT);
    app_driver_relay_set_power(false);
    return (app_driver_handle_t)k_relay_gpio;
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
