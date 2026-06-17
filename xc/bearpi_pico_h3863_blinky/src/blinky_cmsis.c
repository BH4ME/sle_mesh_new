#include <stdio.h>

#include "cmsis_os2.h"
#include "ohos_init.h"
#include "gpio.h"
#include "pinctrl.h"

#ifndef CONFIG_BLINKY_PIN
#define CONFIG_BLINKY_PIN 2
#endif

#ifndef CONFIG_BLINKY_DURATION_MS
#define CONFIG_BLINKY_DURATION_MS 500
#endif

#ifndef CONFIG_BLINKY_ACTIVE_LOW
#define CONFIG_BLINKY_ACTIVE_LOW 1
#endif

static void blinky_set_led(int on)
{
    if (CONFIG_BLINKY_ACTIVE_LOW) {
        uapi_gpio_set_val(CONFIG_BLINKY_PIN, on ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH);
    } else {
        uapi_gpio_set_val(CONFIG_BLINKY_PIN, on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
    }
}

static void *blinky_task(const char *arg)
{
    (void)arg;

    (void)uapi_pin_set_mode(CONFIG_BLINKY_PIN, HAL_PIO_FUNC_GPIO);
    (void)uapi_gpio_set_dir(CONFIG_BLINKY_PIN, GPIO_DIRECTION_OUTPUT);

    /* 先点亮一次，确认硬件连线正常。 */
    blinky_set_led(1);
    printf("[blinky] pin=%d active_low=%d\r\n", CONFIG_BLINKY_PIN, CONFIG_BLINKY_ACTIVE_LOW);

    while (1) {
        osDelay(CONFIG_BLINKY_DURATION_MS);
        (void)uapi_gpio_toggle(CONFIG_BLINKY_PIN);
    }

    return NULL;
}

static void blinky_entry(void)
{
    osThreadAttr_t attr;

    attr.name = "BlinkyTask";
    attr.stack_size = 1024;
    attr.priority = osPriorityNormal;
    attr.attr_bits = 0U;
    attr.cb_mem = NULL;
    attr.cb_size = 0U;
    attr.stack_mem = NULL;

    if (osThreadNew(blinky_task, NULL, &attr) == NULL) {
        printf("[blinky] failed to create task\r\n");
    }
}

SYS_RUN(blinky_entry);
