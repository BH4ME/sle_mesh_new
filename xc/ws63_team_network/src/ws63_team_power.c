#include "ws63_team_power.h"

#include <string.h>
#include "adc.h"
#include "adc_porting.h"
#include "common_def.h"
#include "errcode.h"
#include "gpio.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "tcxo.h"

#ifndef CONFIG_SLE_TEAM_ADC_ENABLE
#define CONFIG_SLE_TEAM_ADC_ENABLE 1
#endif
#ifndef CONFIG_SLE_TEAM_ADC_CTRL_PIN
#define CONFIG_SLE_TEAM_ADC_CTRL_PIN 5
#endif
#ifndef CONFIG_SLE_TEAM_ADC_VBAT_PIN
#define CONFIG_SLE_TEAM_ADC_VBAT_PIN 12
#endif
#ifndef CONFIG_SLE_TEAM_ADC_VBAT_CHANNEL
#define CONFIG_SLE_TEAM_ADC_VBAT_CHANNEL 5
#endif
#ifndef CONFIG_SLE_TEAM_ADC_CTRL_ACTIVE_HIGH
#define CONFIG_SLE_TEAM_ADC_CTRL_ACTIVE_HIGH 1
#endif
#ifndef CONFIG_SLE_TEAM_ADC_SAMPLE_SETTLE_MS
#define CONFIG_SLE_TEAM_ADC_SAMPLE_SETTLE_MS 50
#endif
#ifndef CONFIG_SLE_TEAM_ADC_SAMPLE_INTERVAL_S
#define CONFIG_SLE_TEAM_ADC_SAMPLE_INTERVAL_S 30
#endif
#ifndef CONFIG_SLE_TEAM_CHRG_ENABLE
#define CONFIG_SLE_TEAM_CHRG_ENABLE 1
#endif
#ifndef CONFIG_SLE_TEAM_CHRG_PIN
#define CONFIG_SLE_TEAM_CHRG_PIN 2
#endif
#ifndef CONFIG_SLE_TEAM_CHRG_ACTIVE_LOW
#define CONFIG_SLE_TEAM_CHRG_ACTIVE_LOW 1
#endif
#ifndef CONFIG_SLE_TEAM_CHRG_EXTERNAL_PULLUP
#define CONFIG_SLE_TEAM_CHRG_EXTERNAL_PULLUP 1
#endif

#define SLE_TEAM_ADC_DIVIDER_TOP_KOHM 390U
#define SLE_TEAM_ADC_DIVIDER_BOTTOM_KOHM 100U
#define SLE_TEAM_BATTERY_EMPTY_MV 3300U
#define SLE_TEAM_BATTERY_FULL_MV 4200U

typedef struct {
    uint8_t adc_ready;
    uint8_t adc_ctrl_pin;
    uint8_t adc_vbat_pin;
    uint8_t adc_vbat_channel;
    uint8_t adc_ctrl_active_high;
    uint8_t battery_valid;
    uint8_t battery_percent;
    uint16_t adc_sample_mv;
    uint16_t battery_mv;
    uint32_t battery_sample_last_ms;
    int32_t battery_sample_last_ret;
    uint8_t chrg_ready;
    uint8_t chrg_pin;
    uint8_t chrg_active_low;
    uint8_t chrg_raw;
    uint8_t charging;
} ws63_team_power_state_t;

static ws63_team_power_state_t g_power;

static void power_gpio_config_output_level(uint8_t pin, uint8_t level)
{
    if (pin > 31U) {
        return;
    }
    (void)uapi_pin_set_mode(pin, HAL_PIO_FUNC_GPIO);
    (void)uapi_gpio_set_dir(pin, GPIO_DIRECTION_OUTPUT);
    (void)uapi_gpio_set_val(pin, level);
}

static void power_chrg_sample(void)
{
#if CONFIG_SLE_TEAM_CHRG_ENABLE
    if (g_power.chrg_ready == 0U || g_power.chrg_pin > 31U) {
        return;
    }
    g_power.chrg_raw = (uint8_t)uapi_gpio_get_val(g_power.chrg_pin);
    if (g_power.chrg_active_low != 0U) {
        g_power.charging = g_power.chrg_raw == (uint8_t)GPIO_LEVEL_LOW ? 1U : 0U;
    } else {
        g_power.charging = g_power.chrg_raw == (uint8_t)GPIO_LEVEL_HIGH ? 1U : 0U;
    }
#endif
}

static const char *power_source_name(void)
{
    if (g_power.chrg_ready == 0U) {
        return "unknown";
    }
    return g_power.charging != 0U ? "pwr-charging" : "battery-or-full";
}

static uint8_t power_source_certain(void)
{
    return (uint8_t)(g_power.chrg_ready != 0U && g_power.charging != 0U);
}

static uint8_t adc_ctrl_off_level(void)
{
    return CONFIG_SLE_TEAM_ADC_CTRL_ACTIVE_HIGH ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH;
}

static uint8_t adc_ctrl_on_level(void)
{
    return CONFIG_SLE_TEAM_ADC_CTRL_ACTIVE_HIGH ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW;
}

static void adc_ctrl_set(uint8_t enabled)
{
    if (g_power.adc_ctrl_pin > 31U) {
        return;
    }
    (void)uapi_gpio_set_val(g_power.adc_ctrl_pin,
        enabled != 0U ? adc_ctrl_on_level() : adc_ctrl_off_level());
}

static uint16_t battery_vbat_mv_from_adc_mv(uint16_t adc_mv)
{
    uint32_t vbat_mv = ((uint32_t)adc_mv *
        (SLE_TEAM_ADC_DIVIDER_TOP_KOHM + SLE_TEAM_ADC_DIVIDER_BOTTOM_KOHM) +
        (SLE_TEAM_ADC_DIVIDER_BOTTOM_KOHM / 2U)) / SLE_TEAM_ADC_DIVIDER_BOTTOM_KOHM;

    return vbat_mv > 65535U ? 65535U : (uint16_t)vbat_mv;
}

static uint8_t battery_percent_from_vbat_mv(uint16_t vbat_mv)
{
    uint32_t range_mv = SLE_TEAM_BATTERY_FULL_MV - SLE_TEAM_BATTERY_EMPTY_MV;
    uint32_t above_empty_mv;

    if (vbat_mv <= SLE_TEAM_BATTERY_EMPTY_MV) {
        return 0U;
    }
    if (vbat_mv >= SLE_TEAM_BATTERY_FULL_MV) {
        return 100U;
    }
    above_empty_mv = (uint32_t)vbat_mv - SLE_TEAM_BATTERY_EMPTY_MV;
    return (uint8_t)((above_empty_mv * 100U + (range_mv / 2U)) / range_mv);
}

static int battery_sample_once(uint8_t log_result)
{
    int ret = -1;

    power_chrg_sample();
#if CONFIG_SLE_TEAM_ADC_ENABLE
    uint16_t adc_mv = 0U;

    if (g_power.adc_ready == 0U) {
        g_power.battery_sample_last_ret = ret;
        return ret;
    }
    adc_ctrl_set(1U);
    osal_msleep((uint32_t)CONFIG_SLE_TEAM_ADC_SAMPLE_SETTLE_MS);
    ret = (int)adc_port_read(g_power.adc_vbat_channel, &adc_mv);
    uapi_adc_power_en(AFE_GADC_MODE, false);
    adc_ctrl_set(0U);
    g_power.battery_sample_last_ms = uapi_tcxo_get_ms();
    g_power.battery_sample_last_ret = ret;
    if (ret == (int)ERRCODE_SUCC) {
        g_power.adc_sample_mv = adc_mv;
        g_power.battery_mv = battery_vbat_mv_from_adc_mv(adc_mv);
        g_power.battery_percent = battery_percent_from_vbat_mv(g_power.battery_mv);
        g_power.battery_valid = 1U;
    }
#else
    (void)log_result;
#endif
    if (log_result != 0U) {
        osal_printk("[battery] sample valid=%u adc_mv=%u vbat_mv=%u percent=%u ctrl=%u vbat=%u channel=%u ratio=%u/%u ret=%ld source=%s source_certain=%u charging=%u chrg_raw=%u chrg_pin=%u\r\n",
            g_power.battery_valid, g_power.adc_sample_mv, g_power.battery_mv,
            g_power.battery_percent, g_power.adc_ctrl_pin, g_power.adc_vbat_pin,
            g_power.adc_vbat_channel, SLE_TEAM_ADC_DIVIDER_TOP_KOHM,
            SLE_TEAM_ADC_DIVIDER_BOTTOM_KOHM, (long)g_power.battery_sample_last_ret,
            power_source_name(), power_source_certain(), g_power.charging,
            g_power.chrg_raw, g_power.chrg_pin);
    }
    return ret;
}

void ws63_team_power_init(void)
{
    (void)memset_s(&g_power, sizeof(g_power), 0, sizeof(g_power));
    g_power.adc_ctrl_pin = (uint8_t)CONFIG_SLE_TEAM_ADC_CTRL_PIN;
    g_power.adc_vbat_pin = (uint8_t)CONFIG_SLE_TEAM_ADC_VBAT_PIN;
    g_power.adc_vbat_channel = (uint8_t)CONFIG_SLE_TEAM_ADC_VBAT_CHANNEL;
    g_power.adc_ctrl_active_high = (uint8_t)CONFIG_SLE_TEAM_ADC_CTRL_ACTIVE_HIGH;
    g_power.battery_percent = 100U;
    g_power.battery_sample_last_ret = -1;
    g_power.chrg_pin = (uint8_t)CONFIG_SLE_TEAM_CHRG_PIN;
    g_power.chrg_active_low = (uint8_t)CONFIG_SLE_TEAM_CHRG_ACTIVE_LOW;
    g_power.chrg_raw = GPIO_LEVEL_HIGH;

#if CONFIG_SLE_TEAM_CHRG_ENABLE
    if (g_power.chrg_pin <= 31U) {
        (void)uapi_pin_set_mode(g_power.chrg_pin, HAL_PIO_FUNC_GPIO);
        (void)uapi_gpio_set_dir(g_power.chrg_pin, GPIO_DIRECTION_INPUT);
        g_power.chrg_ready = 1U;
        power_chrg_sample();
    }
#endif
    osal_printk("[hw] chrg present=%u ready=%u pin=%u active_low=%u external_pullup=%u raw=%u charging=%u source=%s source_certain=%u\r\n",
        (uint8_t)CONFIG_SLE_TEAM_CHRG_ENABLE, g_power.chrg_ready, g_power.chrg_pin,
        g_power.chrg_active_low, (uint8_t)CONFIG_SLE_TEAM_CHRG_EXTERNAL_PULLUP, g_power.chrg_raw,
        g_power.charging, power_source_name(), power_source_certain());

#if CONFIG_SLE_TEAM_ADC_ENABLE
    if (g_power.adc_ctrl_pin <= 31U && g_power.adc_vbat_pin <= 31U) {
        errcode_t init_ret;

        power_gpio_config_output_level(g_power.adc_ctrl_pin, adc_ctrl_off_level());
        init_ret = uapi_adc_init(ADC_CLOCK_500KHZ);
        g_power.battery_sample_last_ret = (int32_t)init_ret;
        g_power.adc_ready = init_ret == ERRCODE_SUCC ? 1U : 0U;
        if (g_power.adc_ready != 0U) {
            (void)battery_sample_once(0U);
        }
    }
#endif
    osal_printk("[hw] adc present=%u ready=%u ctrl=%u vbat=%u channel=%u ctrl_active_high=%u ctrl_level=off valid=%u adc_mv=%u vbat_mv=%u battery=%u ret=%ld\r\n",
        (uint8_t)CONFIG_SLE_TEAM_ADC_ENABLE, g_power.adc_ready, g_power.adc_ctrl_pin,
        g_power.adc_vbat_pin, g_power.adc_vbat_channel, g_power.adc_ctrl_active_high,
        g_power.battery_valid, g_power.adc_sample_mv, g_power.battery_mv,
        g_power.battery_percent, (long)g_power.battery_sample_last_ret);
}

void ws63_team_power_tick(uint8_t force_now)
{
    uint32_t now_ms;

    if (g_power.adc_ready == 0U) {
        return;
    }
    now_ms = (uint32_t)uapi_tcxo_get_ms();
    if (force_now == 0U && g_power.battery_sample_last_ms != 0U &&
        (uint32_t)(now_ms - g_power.battery_sample_last_ms) <
            (uint32_t)CONFIG_SLE_TEAM_ADC_SAMPLE_INTERVAL_S * 1000U) {
        return;
    }
    (void)battery_sample_once(0U);
}

uint8_t ws63_team_power_battery_percent(void)
{
    return g_power.battery_valid != 0U ? g_power.battery_percent : 100U;
}

static uint8_t cli_match2(const char *line, const char *first, const char *second)
{
    return (uint8_t)(strcmp(line, first) == 0 || strcmp(line, second) == 0);
}

static void cli_status(void)
{
    power_chrg_sample();
    osal_printk("[cli] bat ready=%u valid=%u adc_mv=%u vbat_mv=%u percent=%u ctrl=%u vbat=%u channel=%u ratio=%u/%u empty_mv=%u full_mv=%u settle_ms=%u interval_s=%u ret=%ld source=%s source_certain=%u charging=%u chrg_ready=%u chrg_pin=%u chrg_raw=%u chrg_active_low=%u\r\n",
        g_power.adc_ready, g_power.battery_valid, g_power.adc_sample_mv, g_power.battery_mv,
        g_power.battery_percent, g_power.adc_ctrl_pin, g_power.adc_vbat_pin, g_power.adc_vbat_channel,
        SLE_TEAM_ADC_DIVIDER_TOP_KOHM, SLE_TEAM_ADC_DIVIDER_BOTTOM_KOHM,
        SLE_TEAM_BATTERY_EMPTY_MV, SLE_TEAM_BATTERY_FULL_MV,
        (uint32_t)CONFIG_SLE_TEAM_ADC_SAMPLE_SETTLE_MS,
        (uint32_t)CONFIG_SLE_TEAM_ADC_SAMPLE_INTERVAL_S,
        (long)g_power.battery_sample_last_ret, power_source_name(), power_source_certain(),
        g_power.charging, g_power.chrg_ready, g_power.chrg_pin, g_power.chrg_raw, g_power.chrg_active_low);
}

int ws63_team_power_cli_handle(const char *line)
{
    if (line == NULL) {
        return 0;
    }
    if (cli_match2(line, "bat", "bat status") != 0U ||
        cli_match2(line, "adc", "adc status") != 0U ||
        cli_match2(line, "power", "power status") != 0U ||
        cli_match2(line, "pwr", "pwr status") != 0U) {
        cli_status();
        return 1;
    }
    if (cli_match2(line, "bat sample", "adc sample") != 0U ||
        cli_match2(line, "power sample", "pwr sample") != 0U) {
        (void)battery_sample_once(1U);
        cli_status();
        return 1;
    }
    if (cli_match2(line, "bat help", "adc help") != 0U ||
        cli_match2(line, "power help", "pwr help") != 0U) {
        osal_printk("[cli] bat commands: status|sample; adc aliases: adc status|adc sample; power aliases: power status|power sample|pwr status|pwr sample\r\n");
        return 1;
    }
    return 0;
}
