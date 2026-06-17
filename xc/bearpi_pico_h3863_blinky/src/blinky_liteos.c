#include <stdio.h>

#include "ohos_init.h"
#include "gpio.h"
#include "los_task.h"
#include "securec.h"
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

#ifndef BLINKY_TASK_STACK_SIZE
#define BLINKY_TASK_STACK_SIZE 0x1000
#endif

#ifndef BLINKY_TASK_PRIO
#define BLINKY_TASK_PRIO 20
#endif

static void blinky_set_led(int on)
{
    if (CONFIG_BLINKY_ACTIVE_LOW) {
        uapi_gpio_set_val(CONFIG_BLINKY_PIN, on ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH);
    } else {
        uapi_gpio_set_val(CONFIG_BLINKY_PIN, on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
    }
}

static UINT32 blinky_task_entry(VOID)
{
    (void)uapi_pin_set_mode(CONFIG_BLINKY_PIN, HAL_PIO_FUNC_GPIO);
    (void)uapi_gpio_set_dir(CONFIG_BLINKY_PIN, GPIO_DIRECTION_OUTPUT);

    /* 先点亮一次，确认硬件连线正常。 */
    blinky_set_led(1);
    printf("[blinky-liteos] pin=%d active_low=%d\r\n", CONFIG_BLINKY_PIN, CONFIG_BLINKY_ACTIVE_LOW);

    while (1) {
        LOS_Msleep(CONFIG_BLINKY_DURATION_MS);
        (void)uapi_gpio_toggle(CONFIG_BLINKY_PIN);
    }

    return LOS_OK;
}

static void blinky_entry(void)
{
    UINT32 task_id;
    TSK_INIT_PARAM_S task;

    (void)memset_s(&task, sizeof(task), 0, sizeof(task));
    task.pfnTaskEntry = blinky_task_entry;
    task.uwStackSize = BLINKY_TASK_STACK_SIZE;
    task.pcName = "BlinkyTask";
    task.usTaskPrio = BLINKY_TASK_PRIO;
    task.uwResved = LOS_TASK_STATUS_DETACHED;

    if (LOS_TaskCreate(&task_id, &task) != LOS_OK) {
        printf("[blinky-liteos] failed to create task\r\n");
    }
}

SYS_RUN(blinky_entry);
