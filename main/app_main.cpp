#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <common_macros.h>

#include <app_priv.h>
#include <app_reset.h>
#include <power_meter.h>
#include <purifier.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <clusters/FanControl/Enums.h>

/* GPIO9 is the BOOT button (esp32c3_devkit_m device_hal), GPIO11..17 belong to the SPI flash */
#define APP_PIN_USABLE(p) ((p) != 9 && ((p) < 11 || (p) > 17))
static_assert(APP_PIN_USABLE(CONFIG_RELAY_GPIO) && APP_PIN_USABLE(CONFIG_BL0937_CF_GPIO) &&
                  APP_PIN_USABLE(CONFIG_BL0937_CF1_GPIO) && APP_PIN_USABLE(CONFIG_BL0937_SEL_GPIO),
              "relay / BL0937 pin set to the BOOT button or a flash pin");
static_assert(CONFIG_RELAY_GPIO != CONFIG_BL0937_CF_GPIO && CONFIG_RELAY_GPIO != CONFIG_BL0937_CF1_GPIO &&
                  CONFIG_RELAY_GPIO != CONFIG_BL0937_SEL_GPIO && CONFIG_BL0937_CF_GPIO != CONFIG_BL0937_CF1_GPIO &&
                  CONFIG_BL0937_CF_GPIO != CONFIG_BL0937_SEL_GPIO && CONFIG_BL0937_CF1_GPIO != CONFIG_BL0937_SEL_GPIO,
              "relay and BL0937 pins must all be different");

static const char *TAG = "app_main";
static uint16_t purifier_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

constexpr auto k_timeout_seconds = 300;

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "Fabric removed successfully");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            chip::CommissioningWindowManager &commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
            constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
            if (!commissionMgr.IsCommissioningWindowOpen()) {
                /* Wi-Fi credentials are kept, so only advertise over DNS-SD */
                CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(kTimeoutSeconds,
                                                                            chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
                }
            }
        }
        break;
    }

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        break;

    default:
        break;
    }
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    /* Log only: the relay can only toggle the purifier, it must not be used as an identify indicator */
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

/* Purifier commands arrive through the Fan Control delegate (see purifier.cpp), nothing to do here */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    return ESP_OK;
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Release the relay (purifier power button) before anything else */
    err = purifier_init();
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to init relay, err:%d", err));

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Unreadable by this firmware anyway: start over, the device has to be commissioned again */
        ESP_LOGW(TAG, "NVS unusable (%s), erasing it", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to init NVS, err:%d", err));

    /* BOOT button: short press switches the purifier, long press = factory reset */
    app_driver_handle_t button_handle = app_driver_button_init();
    if (app_reset_button_register(button_handle) != ESP_OK) {
        ESP_LOGW(TAG, "BOOT button unavailable: no local on/off and no factory reset by button");
    }

    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    /* Air Purifier device type, on/off only: FanModeSequence Off/High, one speed */
    air_purifier::config_t purifier_config;
    purifier_config.fan_control.fan_mode = chip::to_underlying(FanControl::FanModeEnum::kOff);
    purifier_config.fan_control.fan_mode_sequence = chip::to_underlying(FanControl::FanModeSequenceEnum::kOffHigh);
    endpoint_t *endpoint = air_purifier::create(node, &purifier_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create air purifier endpoint"));

    /* A single speed: controllers that show a speed slider (Apple Home) can only snap it to off / full */
    cluster::fan_control::feature::multi_speed::config_t one_speed;
    one_speed.speed_max = 1;
    err = cluster::fan_control::feature::multi_speed::add(cluster::get(endpoint, FanControl::Id), &one_speed);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to add fan speed feature, err:%d", err));

    purifier_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Air purifier created with endpoint_id %d", purifier_endpoint_id);

#if CONFIG_POWER_METER_MATTER
    /* Same endpoint also becomes an Electrical Sensor: power / voltage / current / energy */
    err = power_meter_add_to_endpoint(endpoint);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to add electrical sensor, err:%d", err));
#endif

    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    err = purifier_start(purifier_endpoint_id);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start purifier control, err:%d", err));

    /* The power draw tells whether the purifier is running */
    err = power_meter_start(purifier_endpoint_id, purifier_report_power);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Power meter not started, purifier state unknown, err:%d", err);
    }

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::attribute_register_commands();
    esp_matter::console::init();
#endif
}
