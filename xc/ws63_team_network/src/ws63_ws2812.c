#include "ws63_ws2812.h"

#ifndef WS63_WS2812_HOST_TEST
#include "common_def.h"
#include "errcode.h"
#include "gpio.h"
#include "platform_core.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "tcxo.h"

#define WS63_WS2812_MAX_PIN 18U
#define WS63_WS2812_GPIO_GROUP_WIDTH 8U
#define WS63_WS2812_GPIO_GROUP_STRIDE 0x40U
#define WS63_WS2812_GPIO_DATA_SET_OFFSET 0x30U
#define WS63_WS2812_GPIO_DATA_CLR_OFFSET 0x34U
#define WS63_WS2812_RESET_US 320U
#define WS63_WS2812_CALIBRATE_US 1000U
#define WS63_WS2812_T0H_NS 350U
#define WS63_WS2812_T1H_NS 700U
#define WS63_WS2812_SLOT_NS 1250U
#define WS63_WS2812_PIN_DRIVE PIN_DS_7

/*
 * Raw WS2812 bit-banged driver.
 *
 * WS2812 LEDs are timing-sensitive. We bypass slow GPIO APIs during the data
 * waveform by writing GPIO set/clear registers directly and keeping
 * interrupts locked for the 24-bit frame.
 */
typedef struct {
    volatile uint32_t *set_reg;
    volatile uint32_t *clr_reg;
    uint32_t mask;
    uint32_t t0h_cycles;
    uint32_t t1h_cycles;
    uint32_t slot_cycles;
    uint8_t pin;
    uint8_t ready;
} ws63_ws2812_ctx_t;

static ws63_ws2812_ctx_t g_ws2812;

/* Cycle counter gives sub-microsecond waits for the LED waveform. */
static uint32_t ws63_ws2812_now_cycles(void)
{
    uint32_t cycles;

    __asm__ volatile("rdcycle %0" : "=r"(cycles));
    return cycles;
}

/* Busy-wait until the absolute cycle deadline, wrap-safe for short intervals. */
static void ws63_ws2812_wait_until(uint32_t target)
{
    while ((int32_t)(target - ws63_ws2812_now_cycles()) > 0) {
    }
}

static uint32_t ws63_ws2812_cycles_from_ns(uint32_t cycles_per_us, uint32_t ns)
{
    uint32_t cycles = (cycles_per_us * ns + 500U) / 1000U;
    return cycles == 0U ? 1U : cycles;
}

/* Measure CPU cycles/us at boot so the timing survives clock changes. */
static void ws63_ws2812_calibrate_timing(void)
{
    uint32_t start = ws63_ws2812_now_cycles();
    uint32_t cycles_per_us;

    (void)uapi_tcxo_delay_us(WS63_WS2812_CALIBRATE_US);
    cycles_per_us = (uint32_t)(ws63_ws2812_now_cycles() - start) / WS63_WS2812_CALIBRATE_US;
    if (cycles_per_us == 0U) {
        cycles_per_us = 24U;
    }
    g_ws2812.t0h_cycles = ws63_ws2812_cycles_from_ns(cycles_per_us, WS63_WS2812_T0H_NS);
    g_ws2812.t1h_cycles = ws63_ws2812_cycles_from_ns(cycles_per_us, WS63_WS2812_T1H_NS);
    g_ws2812.slot_cycles = ws63_ws2812_cycles_from_ns(cycles_per_us, WS63_WS2812_SLOT_NS);
    if (g_ws2812.slot_cycles <= g_ws2812.t1h_cycles) {
        g_ws2812.slot_cycles = g_ws2812.t1h_cycles + 1U;
    }
}

/* WS63 GPIOs are split into groups; this maps a pin to its register base. */
static uintptr_t ws63_ws2812_gpio_base(uint8_t pin)
{
    if (pin <= 7U) {
        return (uintptr_t)0x44028000U;
    }
    if (pin <= 15U) {
        return (uintptr_t)0x44029000U;
    }
    return (uintptr_t)0x4402A000U;
}

static void ws63_ws2812_pin_high(void)
{
    *g_ws2812.set_reg = g_ws2812.mask;
}

static void ws63_ws2812_pin_low(void)
{
    *g_ws2812.clr_reg = g_ws2812.mask;
}

/* Write one WS2812 bit: high time encodes 0 or 1, total slot stays fixed. */
static void ws63_ws2812_write_bit(uint8_t bit)
{
    uint32_t start;
    uint32_t high_cycles = bit != 0U ? g_ws2812.t1h_cycles : g_ws2812.t0h_cycles;

    start = ws63_ws2812_now_cycles();
    ws63_ws2812_pin_high();
    ws63_ws2812_wait_until(start + high_cycles);
    ws63_ws2812_pin_low();
    ws63_ws2812_wait_until(start + g_ws2812.slot_cycles);
}

static void ws63_ws2812_write_byte(uint8_t value)
{
    uint8_t mask;

    for (mask = 0x80U; mask != 0U; mask >>= 1U) {
        ws63_ws2812_write_bit((value & mask) != 0U ? 1U : 0U);
    }
}
#endif

void ws63_ws2812_encode_frame(uint8_t red, uint8_t green, uint8_t blue, uint8_t out[3])
{
    if (out == 0) {
        return;
    }
    /* WS2812B-XF01/W uses the common WS2812 GRB wire order. Keep this isolated
     * so hardware-order changes cannot leak into status colors. */
    out[0] = green;
    out[1] = red;
    out[2] = blue;
}

#ifndef WS63_WS2812_HOST_TEST
int ws63_ws2812_init(uint8_t pin)
{
    uintptr_t base;
    uintptr_t group_offset;
    uint8_t group_pin;

    if (pin > WS63_WS2812_MAX_PIN) {
        return -1;
    }

    (void)uapi_pin_set_mode(pin, HAL_PIO_FUNC_GPIO);
    (void)uapi_pin_set_ds(pin, WS63_WS2812_PIN_DRIVE);
    (void)uapi_pin_set_pull(pin, PIN_PULL_TYPE_DISABLE);
    if (uapi_gpio_set_dir(pin, GPIO_DIRECTION_OUTPUT) != ERRCODE_SUCC) {
        return -1;
    }

    base = ws63_ws2812_gpio_base(pin);
    group_pin = (uint8_t)(pin % WS63_WS2812_GPIO_GROUP_WIDTH);
    group_offset = (uintptr_t)((pin / WS63_WS2812_GPIO_GROUP_WIDTH) * WS63_WS2812_GPIO_GROUP_STRIDE);
    g_ws2812.set_reg = (volatile uint32_t *)(base + group_offset + WS63_WS2812_GPIO_DATA_SET_OFFSET);
    g_ws2812.clr_reg = (volatile uint32_t *)(base + group_offset + WS63_WS2812_GPIO_DATA_CLR_OFFSET);
    g_ws2812.mask = (uint32_t)1U << group_pin;
    g_ws2812.pin = pin;
    ws63_ws2812_calibrate_timing();
    g_ws2812.ready = 1U;
    /* Start from a dark latch; the status layer owns all visible animation. */
    ws63_ws2812_pin_low();
    (void)uapi_tcxo_delay_us(WS63_WS2812_RESET_US);
    (void)ws63_ws2812_clear();
    return 0;
}

int ws63_ws2812_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    uint8_t encoded[3];
    uint32_t irq_sts;

    if (g_ws2812.ready == 0U) {
        return -1;
    }

    ws63_ws2812_encode_frame(red, green, blue, encoded);
    /*
     * Keep the bus quiet: one short, locked waveform per visible color change.
     * The rejected retry path reconfigured IO0, recalibrated, and repeated frames
     * on every refresh; hardware showed that extra activity made green flicker
     * faster. If green appears with semantic green=0, the safest software
     * response is to minimize DIN transitions, not add more retries.
     */
    irq_sts = osal_irq_lock();
    ws63_ws2812_write_byte(encoded[0]);
    ws63_ws2812_write_byte(encoded[1]);
    ws63_ws2812_write_byte(encoded[2]);
    ws63_ws2812_pin_low();
    osal_irq_restore(irq_sts);
    (void)uapi_tcxo_delay_us(WS63_WS2812_RESET_US);
    return 0;
}

int ws63_ws2812_clear(void)
{
    return ws63_ws2812_set_rgb(0U, 0U, 0U);
}

int ws63_ws2812_hold_low(void)
{
    uint32_t irq_sts;

    if (g_ws2812.ready == 0U) {
        return -1;
    }

    irq_sts = osal_irq_lock();
    ws63_ws2812_pin_low();
    osal_irq_restore(irq_sts);
    (void)uapi_tcxo_delay_us(WS63_WS2812_RESET_US);
    return 0;
}

uint8_t ws63_ws2812_is_ready(void)
{
    return g_ws2812.ready;
}

uint8_t ws63_ws2812_pin(void)
{
    return g_ws2812.pin;
}
#endif
