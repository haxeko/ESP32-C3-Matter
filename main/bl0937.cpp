/*
 * BL0937 single-phase energy meter driver (pulse outputs).
 *
 *   CF  : pulse frequency proportional to active power
 *   CF1 : pulse frequency proportional to RMS current (SEL = current level)
 *         or RMS voltage (other SEL level)
 *
 * ESP32-C3 has no PCNT peripheral, so pulses are counted with GPIO interrupts
 * and timestamped; frequency = pulses / time between pulses, which stays accurate
 * at low frequencies (1 Hz on CF ~ 2 W with default 1 mOhm / 2351:1 hardware).
 *
 * Formulas (BL0937 datasheet, Vref = 1.218 V):
 *   F_CF  = 1721506 * V(V) * V(I) / Vref^2
 *   F_CFI =   94638 * V(I) / Vref
 *   F_CFU =   15397 * V(V) / Vref
 * with V(I) = I * R_shunt and V(V) = U / divider.
 */
#include <sdkconfig.h>
#if CONFIG_BL0937_ENABLE

#include "bl0937.h"

#include <math.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

static const char *TAG = "bl0937";

static constexpr gpio_num_t k_cf_gpio = (gpio_num_t)CONFIG_BL0937_CF_GPIO;
static constexpr gpio_num_t k_cf1_gpio = (gpio_num_t)CONFIG_BL0937_CF1_GPIO;
static constexpr gpio_num_t k_sel_gpio = (gpio_num_t)CONFIG_BL0937_SEL_GPIO;

static constexpr double k_vref = 1.218;
static constexpr double k_shunt_ohm = CONFIG_BL0937_SHUNT_MICROOHM / 1e6;
static constexpr double k_divider = CONFIG_BL0937_VOLTAGE_DIVIDER;

/* Units per Hz, including user calibration */
static constexpr double k_w_per_hz = k_vref * k_vref * k_divider / (1721506.0 * k_shunt_ohm) *
                                     (CONFIG_BL0937_POWER_CAL_PERMILLE / 1000.0);
static constexpr double k_v_per_hz = k_vref * k_divider / 15397.0 * (CONFIG_BL0937_VOLTAGE_CAL_PERMILLE / 1000.0);
static constexpr double k_a_per_hz = k_vref / (94638.0 * k_shunt_ohm) * (CONFIG_BL0937_CURRENT_CAL_PERMILLE / 1000.0);

/* Below this the reading is treated as noise / no load */
static constexpr double k_min_power_w = 0.5;
/* No CF pulse for this long -> power is 0 */
static constexpr int64_t k_cf_timeout_us = 20 * 1000 * 1000;
/* CF1 output needs time to settle after SEL is switched */
static constexpr int64_t k_cf1_settle_us = 100 * 1000;

typedef struct {
    uint32_t count;
    int64_t first_us;
    int64_t last_us;
} pulse_window_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static pulse_window_t s_cf;         /* CF pulses since the last bl0937_update() */
static pulse_window_t s_cf1;        /* CF1 pulses in the current SEL window */
static uint64_t s_cf_total;         /* all CF pulses since boot -> energy */
static int64_t s_cf1_ignore_until;  /* drop CF1 pulses while the output settles */

static bool s_cf1_current_mode;
static int64_t s_cf_prev_last_us;   /* last CF pulse of the previous window, 0 = none yet */
static double s_cf_freq;
static uint64_t s_energy_base_mwh;
static bl0937_reading_t s_reading;
static bool s_current_valid;

static void IRAM_ATTR cf_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_lock);
    if (s_cf.count++ == 0) {
        s_cf.first_us = now;
    }
    s_cf.last_us = now;
    s_cf_total++;
    portEXIT_CRITICAL_ISR(&s_lock);
}

static void IRAM_ATTR cf1_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_lock);
    if (now >= s_cf1_ignore_until) {
        if (s_cf1.count++ == 0) {
            s_cf1.first_us = now;
        }
        s_cf1.last_us = now;
    }
    portEXIT_CRITICAL_ISR(&s_lock);
}

static void select_cf1_mode(bool current_mode)
{
    s_cf1_current_mode = current_mode;
    gpio_set_level(k_sel_gpio, current_mode ? CONFIG_BL0937_SEL_CURRENT_LEVEL : !CONFIG_BL0937_SEL_CURRENT_LEVEL);
    portENTER_CRITICAL(&s_lock);
    s_cf1 = {};
    s_cf1_ignore_until = esp_timer_get_time() + k_cf1_settle_us;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t bl0937_init(uint64_t energy_base_mwh)
{
    s_energy_base_mwh = energy_base_mwh;

    gpio_config_t out_cfg = {};
    out_cfg.pin_bit_mask = 1ULL << k_sel_gpio;
    out_cfg.mode = GPIO_MODE_OUTPUT;
    esp_err_t err = gpio_config(&out_cfg);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t in_cfg = {};
    in_cfg.pin_bit_mask = (1ULL << k_cf_gpio) | (1ULL << k_cf1_gpio);
    in_cfg.mode = GPIO_MODE_INPUT;
    in_cfg.intr_type = GPIO_INTR_POSEDGE;
    err = gpio_config(&in_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* INVALID_STATE: already installed */
        return err;
    }
    gpio_isr_handler_add(k_cf_gpio, cf_isr, NULL);
    gpio_isr_handler_add(k_cf1_gpio, cf1_isr, NULL);

    select_cf1_mode(false); /* start with voltage */

    ESP_LOGI(TAG, "BL0937 on CF=GPIO%d CF1=GPIO%d SEL=GPIO%d, %.4f W/Hz, %.4f V/Hz, %.5f A/Hz",
             (int)k_cf_gpio, (int)k_cf1_gpio, (int)k_sel_gpio, k_w_per_hz, k_v_per_hz, k_a_per_hz);
    return ESP_OK;
}

void bl0937_update(bl0937_reading_t *out)
{
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);
    pulse_window_t cf = s_cf;
    pulse_window_t cf1 = s_cf1;
    uint64_t cf_total = s_cf_total;
    s_cf = {};
    portEXIT_CRITICAL(&s_lock);

    /* --- Active power from CF: pulses / time since the previous pulse --- */
    if (cf.count > 0) {
        if (s_cf_prev_last_us > 0 && cf.last_us > s_cf_prev_last_us) {
            s_cf_freq = cf.count * 1e6 / (double)(cf.last_us - s_cf_prev_last_us);
        } else if (cf.count >= 2) {
            s_cf_freq = (cf.count - 1) * 1e6 / (double)(cf.last_us - cf.first_us);
        }
        s_cf_prev_last_us = cf.last_us;
    } else if (s_cf_prev_last_us > 0) {
        int64_t since = now - s_cf_prev_last_us;
        if (since > k_cf_timeout_us) {
            s_cf_freq = 0;
        } else {
            /* No pulse yet: the real frequency is at most 1 / time-since-last-pulse */
            s_cf_freq = fmin(s_cf_freq, 1e6 / (double)since);
        }
    }

    double power = s_cf_freq * k_w_per_hz;
    if (power < k_min_power_w) {
        power = 0;
    }
    s_reading.power_w = (float)power;
    s_reading.energy_mwh = s_energy_base_mwh + (uint64_t)llround(cf_total * k_w_per_hz / 3.6);

    /* --- CF1: current or voltage, depending on the window that just ended --- */
    double cf1_freq = 0;
    if (cf1.count >= 2) {
        cf1_freq = (cf1.count - 1) * 1e6 / (double)(cf1.last_us - cf1.first_us);
    }
    if (s_cf1_current_mode) {
        s_reading.current_a = (float)(cf1_freq * k_a_per_hz);
        s_current_valid = cf1.count >= 2;
    } else if (cf1.count >= 2) {
        s_reading.voltage_v = (float)(cf1_freq * k_v_per_hz);
        s_reading.voltage_valid = true;
    }
    select_cf1_mode(!s_cf1_current_mode);

    /* Small loads produce too few CF1 pulses per window: derive I = P / U (PF ~ 1) */
    if (power == 0) {
        s_reading.current_a = 0;
    } else if (!s_current_valid && s_reading.voltage_valid && s_reading.voltage_v > 1.0f) {
        s_reading.current_a = (float)(power / s_reading.voltage_v);
    }

    *out = s_reading;
}

#endif // CONFIG_BL0937_ENABLE
