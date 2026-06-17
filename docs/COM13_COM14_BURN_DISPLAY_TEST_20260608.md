# COM13/COM14 烧录与实体屏幕检测记录

记录时间：2026-06-08 00:52 CST

## 结论

- `COM13` 烧录测试通过，`v4.4.99` 完整包所有分区传输到 100%，最后输出 `Done. Reseting device...`。
- `COM14` 按要求使用硬件 RTS 复位烧录，硬件复位流程确实执行，但该路径未通过：一次进入 ROM 后在 `root_loaderboot_sign.bin` 传输 77% 失败，另外两次等待 ROM handshake timeout。
- `COM13` 和 `COM14` 的 `disp status` / `disp demo` 都返回 `disp ready=1` 和 demo `ret=0`。
- 当前硬件/固件不能仅靠串口判断哪块没有接入实体 ST7789 屏幕，因为屏幕接口只有 `SCL/SDA/CS/RS/RESET` 写入/控制线，没有 `MISO`、`TE`、`ID`、触摸或其他可读回检测线。

## 烧录证据

`COM13` 成功日志：

```text
logs/burn/v4.4.99_20260608_003332/COM13.log
ROM handshake ACK
Transferring ... 100%
Done. Reseting device...
```

`COM14` 硬件复位失败日志：

```text
logs/burn/v4.4.99_20260608_003809/COM14.log
Auto reset: RTS=1
Auto reset: RTS=0
Download control-line release: RTS=0 DTR=0
ROM handshake ACK
root_loaderboot_sign.bin 77%
Error transferring root_loaderboot_sign.bin
```

```text
logs/burn/v4.4.99_20260608_003935/COM14.log
Auto reset: RTS=1
Auto reset: RTS=0
Auto handshake timeout
```

```text
logs/burn/v4.4.99_20260608_004020/COM14.log
Auto reset: RTS=0
Auto reset: RTS=1
Auto handshake timeout
```

## 屏幕测试证据

`COM13`：

```text
logs/serial/com13_disp_demo_v4.4.99_20260608_005225.log
[cli] disp ready=1 spi=0 sclk=7 sda=9 cs=8 rs=10 rst=13 size=240x135 off=40,53
[display-event] event=LOST label=M63 member=99 ret=0
```

`COM14`：

```text
logs/serial/com14_disp_demo_v4.4.99_20260608_005225.log
[cli] disp ready=1 spi=0 sclk=7 sda=9 cs=8 rs=10 rst=13 size=240x135 off=40,53
[display-event] event=LOST label=M63 member=99 ret=0
```

## 为什么不能判断哪块没接实体屏幕

当前屏幕接口和固件配置只有：

```text
SCL/SCLK -> IO07
SDA/MOSI -> IO09
CS       -> IO08
RS/DC    -> IO10
RESET    -> IO13
```

没有可读回的 `MISO` 或屏幕 ID 线。固件里的 `disp ready=1` 表示 ST7789 驱动初始化流程完成、GPIO/SPI 写出成功；即使屏幕模组物理未接入，这类纯写接口也不会自动返回失败。

## 当前可执行的实体屏幕确认方式

已经对两块板下发 `disp demo`。现场肉眼看屏幕：

- 哪块显示 `LOST / M63` demo 画面，哪块就接了实体屏幕。
- 哪块没有任何显示，哪块就是未接实体屏幕、屏幕供电异常、排线/焊接异常或屏幕损坏。

如果要让固件以后自动判断实体屏幕，需要硬件增加至少一种可读回信号，例如 `MISO` 读 ST7789 ID、屏幕连接检测脚、背光电流检测、TE 脚、触摸控制器 I2C ID，或其他能被 MCU 读取的屏幕在位信号。
