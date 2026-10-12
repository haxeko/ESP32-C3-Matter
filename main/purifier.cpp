/*
 * Air purifier driven through its power button.
 *
 *   HomeKit / Matter  --FanMode / PercentSetting-->  Fan Control cluster  --delegate-->  purifier task
 *   purifier task     --0.3 s relay pulse-->         purifier power button (relay in parallel)
 *   BL0937 power      --P > 3 W ?-->                 purifier task  --FanMode / PercentCurrent-->  Matter
 *
 * The power button toggles the purifier, so a press is only sent when the requested state differs
 * from the state the purifier is in (or is about to be in after a press still waiting for
 * confirmation). Everything runs in one task, the Matter side only posts events to it.
 */
#include "purifier.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_matter.h>

#include <app/clusters/fan-control-server/CodegenIntegration.h>

using namespace chip::app::Clusters;
using chip::Protocols::InteractionModel::Status;

static const char *TAG = "purifier";

static constexpr gpio_num_t k_relay_gpio = (gpio_num_t)CONFIG_RELAY_GPIO;
static constexpr int64_t k_pulse_gap_us = CONFIG_PURIFIER_PULSE_GAP_MS * 1000LL;
static constexpr int64_t k_confirm_timeout_us = CONFIG_PURIFIER_CONFIRM_TIMEOUT_MS * 1000LL;

namespace {

enum event_type_t { EVT_POWER, EVT_SET, EVT_TOGGLE };

struct event_t {
    event_type_t type;
    union {
        float power_w; /* EVT_POWER */
        bool on;       /* EVT_SET */
    };
};

QueueHandle_t s_queue;
uint16_t s_endpoint_id;

/* Measured state (debounced) */
bool s_measured_valid;
bool s_measured_on;
bool s_candidate_on;
int s_debounce;
float s_power_w;

/* A press was sent, waiting for the power draw to confirm the new state */
bool s_pending;
bool s_pending_on;
int64_t s_pending_deadline_us;

int64_t s_last_release_us;

/* Set (with the Matter stack lock held) while this module writes the Fan Control cluster itself */
bool s_syncing;

void post_event(const event_t &evt)
{
    if (s_queue && xQueueSend(s_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, dropping event %d", (int)evt.type);
    }
}

class PurifierFanDelegate : public FanControl::Delegate {
public:
    Status HandleStep(FanControl::StepDirectionEnum, bool, bool) override { return Status::UnsupportedCommand; }

    /* Runs in the Matter thread after a controller changed FanMode / PercentSetting */
    void OnFanDriveStateChanged(const FanControl::FanDriveState &state) override
    {
        if (s_syncing) {
            return; /* our own state report, not a command */
        }
        event_t evt = {};
        evt.type = EVT_SET;
        evt.on = state.mode != FanControl::FanModeEnum::kOff;
        post_event(evt);
    }
};

PurifierFanDelegate s_fan_delegate;

void relay_set(bool pressed)
{
    gpio_set_level(k_relay_gpio, pressed ? CONFIG_RELAY_ACTIVE_LEVEL : !CONFIG_RELAY_ACTIVE_LEVEL);
}

void press_power_button()
{
    /* Leave the purifier's own button logic time to see two presses as two presses */
    int64_t since_release = esp_timer_get_time() - s_last_release_us;
    if (s_last_release_us > 0 && since_release < k_pulse_gap_us) {
        vTaskDelay(pdMS_TO_TICKS((k_pulse_gap_us - since_release) / 1000) + 1);
    }

    ESP_LOGI(TAG, "Pressing power button (relay GPIO%d, %d ms)", (int)k_relay_gpio, CONFIG_PURIFIER_PULSE_MS);
    relay_set(true);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_PURIFIER_PULSE_MS));
    relay_set(false);
    s_last_release_us = esp_timer_get_time();
}

void request_state(bool on)
{
    if (!s_measured_valid) {
        ESP_LOGW(TAG, "Purifier state not measured yet, ignoring %s request", on ? "ON" : "OFF");
        return;
    }

    bool expected = s_pending ? s_pending_on : s_measured_on;
    if (on == expected) {
        ESP_LOGI(TAG, "Request %s: already %s", on ? "ON" : "OFF", s_pending ? "switching" : "there");
        return;
    }

    ESP_LOGI(TAG, "Request %s (purifier %s, %.1f W)", on ? "ON" : "OFF", s_measured_on ? "ON" : "OFF", s_power_w);
    press_power_button();
    s_pending = true;
    s_pending_on = on;
    s_pending_deadline_us = esp_timer_get_time() + k_confirm_timeout_us;
}

void update_measured(float power_w)
{
    s_power_w = power_w;
    bool raw_on = power_w * 1000.0f > CONFIG_PURIFIER_ON_THRESHOLD_MW;

    if (s_measured_valid && raw_on == s_measured_on) {
        s_debounce = 0;
    } else {
        if (s_debounce > 0 && raw_on != s_candidate_on) {
            s_debounce = 0;
        }
        s_candidate_on = raw_on;
        if (++s_debounce >= CONFIG_PURIFIER_DEBOUNCE_SAMPLES) {
            s_debounce = 0;
            s_measured_on = raw_on;
            s_measured_valid = true;
            ESP_LOGI(TAG, "Purifier is %s (%.1f W)", raw_on ? "ON" : "OFF", power_w);
        }
    }

    if (s_pending && s_measured_valid && s_measured_on == s_pending_on) {
        s_pending = false;
        ESP_LOGI(TAG, "Purifier switched %s", s_measured_on ? "ON" : "OFF");
    }
}

/* Make the Fan Control cluster show the purifier state */
void sync_matter()
{
    if (!s_measured_valid) {
        return;
    }
    bool report_on = s_pending ? s_pending_on : s_measured_on;

    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    FanControlCluster *fan = FanControl::FindClusterOnEndpoint(s_endpoint_id);
    if (!fan) {
        return;
    }

    s_syncing = true;
    if ((fan->GetFanMode() != FanControl::FanModeEnum::kOff) != report_on) {
        ESP_LOGI(TAG, "Reporting %s", report_on ? "ON" : "OFF");
        fan->SetFanMode(report_on ? FanControl::FanModeEnum::kHigh : FanControl::FanModeEnum::kOff);
    }
    /* On/off only: a speed picked on a slider is snapped back to full */
    if (report_on && fan->GetPercentSetting() != chip::app::DataModel::MakeNullable<chip::Percent>(100)) {
        fan->SetPercentSetting(chip::app::DataModel::MakeNullable<chip::Percent>(100));
    }
    /* PercentCurrent / SpeedCurrent are the actual fan output: always the measured state */
    fan->SetPercentCurrent(s_measured_on ? 100 : 0);
    fan->SetSpeedCurrent(s_measured_on ? 1 : 0);
    s_syncing = false;
}

void attach_delegate()
{
    while (true) {
        {
            esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
            if (FanControl::FindClusterOnEndpoint(s_endpoint_id)) {
                FanControl::SetDefaultDelegate(s_endpoint_id, &s_fan_delegate);
                return;
            }
        }
        ESP_LOGW(TAG, "Fan Control cluster on endpoint %u not ready yet", s_endpoint_id);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void purifier_task(void *arg)
{
    attach_delegate();
    ESP_LOGI(TAG, "Controlling the purifier on endpoint %u (ON above %d mW)", s_endpoint_id,
             CONFIG_PURIFIER_ON_THRESHOLD_MW);

    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (s_pending) {
            int64_t left_us = s_pending_deadline_us - esp_timer_get_time();
            wait = left_us > 0 ? pdMS_TO_TICKS(left_us / 1000) + 1 : 0;
        }

        event_t evt;
        if (xQueueReceive(s_queue, &evt, wait) == pdTRUE) {
            switch (evt.type) {
            case EVT_POWER:
                update_measured(evt.power_w);
                break;
            case EVT_SET:
                request_state(evt.on);
                break;
            case EVT_TOGGLE:
                request_state(!(s_pending ? s_pending_on : s_measured_on));
                break;
            }
        }

        if (s_pending && esp_timer_get_time() >= s_pending_deadline_us) {
            s_pending = false;
            ESP_LOGW(TAG, "Purifier did not switch %s (%.1f W), reporting %s", s_pending_on ? "ON" : "OFF", s_power_w,
                     s_measured_on ? "ON" : "OFF");
        }

        sync_matter();
    }
}

} // namespace

esp_err_t purifier_init()
{
    /* Released level first, so enabling the output never glitches the button */
    relay_set(false);
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << k_relay_gpio;
    cfg.mode = GPIO_MODE_OUTPUT;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    relay_set(false);

    s_queue = xQueueCreate(8, sizeof(event_t));
    return s_queue ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t purifier_start(uint16_t endpoint_id)
{
    s_endpoint_id = endpoint_id;
    if (xTaskCreate(purifier_task, "purifier", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void purifier_report_power(float power_w)
{
    event_t evt = {};
    evt.type = EVT_POWER;
    evt.power_w = power_w;
    post_event(evt);
}

void purifier_toggle()
{
    event_t evt = {};
    evt.type = EVT_TOGGLE;
    post_event(evt);
}
