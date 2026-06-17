# BearPi Pico H3863 Blinky

这是一个给小熊派 `BearPi-Pico H3863 / WS63` 用的最小点灯样例。

默认参考小熊派官方教程：

- 用户灯为蓝色 LED
- 教程里的默认点灯引脚是 `GPIO_2`
- 示例先把引脚拉低，再周期翻转电平

## 代码行为

- 初始化 `CONFIG_BLINKY_PIN`
- 配置为 GPIO 输出
- 默认先点亮一次
- 之后每隔 `CONFIG_BLINKY_DURATION_MS` 翻转一次

## 版本

- `CMSIS` 线程版：`src/blinky_cmsis.c`
- `LiteOS` 任务版：`src/blinky_liteos.c`

## 默认参数

```c
CONFIG_BLINKY_PIN = 2
CONFIG_BLINKY_DURATION_MS = 500
CONFIG_BLINKY_ACTIVE_LOW = 1
```

## 接入方式

把这两个源文件按你的工程风格选一个接入即可。

如果你已经有 WS63/OpenHarmony 工程，一般还需要：

1. 在 `Kconfig` 里打开对应 sample
2. 在 `CMakeLists.txt` 里把这个源文件加进去
3. 用 HiSpark/DevEco 选择 `ws63`、烧录到板子

## Mac 实机烧录记录

这次已经在 Mac + QEMU Debian VM + 小熊派 WS63 上跑通 LiteOS blinky。

完整踩坑记录见：

- [WS63_MAC_FLASH_NOTES.md](./WS63_MAC_FLASH_NOTES.md)

最关键的结论：

- Mac 烧录串口优先用 `/dev/tty.usbserial-10`，不要用 `/dev/cu.usbserial-10`
- 首次烧录建议用 `115200`，不要先用 `921600`
- 第一次上板建议烧 `ws63-liteos-app_all.fwpkg`
- 如果烧录工具等待 reset，按一下板子的 `RESET/RST`

## 注意

如果你的板子 LED 实际不是 `GPIO_2`，只需要改 `CONFIG_BLINKY_PIN`。

如果发现“代码跑了但灯不亮”，通常是这两种情况之一：

1. LED 是高电平点亮，不是低电平点亮
2. 你的板子用户灯并不接在 `GPIO_2`

## LiteOS 说明

LiteOS 版本使用 `LOS_TaskCreate` + `LOS_Msleep`，逻辑和 CMSIS 版一致，只是任务创建 API 不同。
