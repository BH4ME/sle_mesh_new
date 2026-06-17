# WS63 Mac 编译烧录踩坑记录

本文记录一次在 Apple Silicon Mac 上，把小熊派 `BearPi-Pico H3863 / WS63` 的 LiteOS blinky 固件编译、烧录并点灯成功的过程。

这不是官方最短流程，而是实测可跑通流程，重点记录踩坑点。

## 本次结果

- 主机：Mac，Apple Silicon
- 板子：小熊派 `BearPi-Pico H3863 / WS63`
- 系统：LiteOS
- 示例：`peripheral/blinky`
- 点灯引脚：`GPIO_2`
- 翻转周期：`500 ms`
- 编译目标：`ws63-liteos-app`
- 烧录包：`ws63-liteos-app_all.fwpkg`
- 烧录串口：`/dev/tty.usbserial-10`
- 烧录波特率：`115200`

最终现象：

- 烧录工具输出 `Done. Reseting device...`
- 串口能看到 `APP|[SYS INFO] ...`
- 板载 LED 按约 `500 ms` 周期闪烁

## 整体流程

```text
Mac
  |
  | QEMU 启动 x86_64 Debian VM
  v
Debian VM 编译 WS63 SDK
  |
  | 生成 .fwpkg
  v
Mac 拷回固件包
  |
  | /dev/tty.usbserial-10 串口烧录
  v
WS63 LiteOS blinky 运行
```
## 为什么要用 x86_64 Debian VM

WS63 SDK 里的工具链和构建脚本更适合 Linux x86_64 环境。

Mac 直接编译容易遇到：

- 工具链架构不匹配
- Python 版本兼容问题
- 旧 `output` 缓存路径污染
- HiSpark 插件在 Mac 上不可用或不完整

所以本次采用：

- Mac 负责 QEMU、串口、烧录
- Debian x86_64 VM 负责编译

## VM 连接信息

本次 VM 通过 QEMU user network 暴露 SSH：

```sh
ssh -p 2222 builder@127.0.0.1
```

VM 用户：

```text
user: builder
password: builder
```

## VM 安装依赖

在 Debian VM 内安装：

```sh
sudo apt update
sudo apt install -y rsync file build-essential cmake ninja-build python3-pip python3-venv git make
```

安装 SDK Python 依赖：

```sh
python3 -m pip install --user -r requirements.txt
```

如果提示用户脚本目录不在 `PATH`，加上：

```sh
export PATH="$HOME/.local/bin:$PATH"
```

## Python 3.13 的 distutils 坑

如果 Debian/Python 版本较新，`build.py` 里可能有：

```python
from distutils.spawn import find_executable
```

Python 3.12+ 后 `distutils` 被移除，构建会失败。

本次修法：

```python
from shutil import which
```

并把：

```python
find_executable(...)
```

替换为：

```python
which(...)
```

## output 缓存路径污染坑

第一次把 Mac 上的 SDK 整目录同步到 VM 后，原有 `output/` 里残留了 Mac 路径，例如：

```text
<sdk-root>/output/...
```

VM 构建时会报类似 CMakeCache 路径不一致的问题。

正确做法是同步源码时排除仓库根目录的 `output`：

```sh
rsync -a --exclude=/output \
  /home/builder/workspace/bearpi-pico_h3863/ \
  /home/builder/workspace/bearpi-pico_h3863_fresh/
```

注意只排除根目录 `output`，不要排除所有叫 `output` 的目录。

错误做法：

```sh
rsync -a --exclude=output ...
```

这个会把 SDK 内部必要文件也排掉，例如：

```text
drivers/chips/ws63/rom_config/acore/output/rom_callback_wrap.cmake
```

结果 LiteOS app 配置阶段会失败。

## 编译命令

在 VM 的干净 SDK 目录中执行：

```sh
export PATH="$HOME/.local/bin:$PATH"
cd /home/builder/workspace/bearpi-pico_h3863_fresh
python3 build.py ws63-liteos-app -j4
```

成功标志：

```text
######### Build target:ws63_liteos_app success
packet success!
```

本次编译耗时约 20 分钟。

## 生成的关键文件

完整烧录包：

```text
/home/builder/workspace/bearpi-pico_h3863_fresh/output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg
```

只更新应用的包：

```text
/home/builder/workspace/bearpi-pico_h3863_fresh/output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_load_only.fwpkg
```

第一次上板建议使用完整包 `ws63-liteos-app_all.fwpkg`，避免 bootloader、分区、NV 状态不一致。

## 拷回 Mac

```sh
mkdir -p <sdk-root>/output_from_vm

scp -P 2222 \
  builder@127.0.0.1:/home/builder/workspace/bearpi-pico_h3863_fresh/output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg \
  <sdk-root>/output_from_vm/
```

## Mac 串口识别

查看串口：

```sh
find /dev -maxdepth 1 \( -name 'cu.*' -o -name 'tty.*' \) 2>/dev/null | sort
```

本次识别到：

```text
/dev/cu.usbserial-10
/dev/tty.usbserial-10
```

USB 信息显示是 CH340/CH341 类串口：

```text
Vendor ID: 0x1a86
Product ID: 0x7523
```

## 安装烧录工具

本次使用社区 Python 工具 `xf-burn-tools`：

```sh
python3 -m pip install --user xf-burn-tools rich
```

工具命令路径：

```text
<python-user-bin>/burn
```

查看帮助：

```sh
<python-user-bin>/burn --help
```

查看固件包内容：

```sh
<python-user-bin>/burn -s \
  <sdk-root>/output_from_vm/ws63-liteos-app_all.fwpkg
```

## 烧录命令

`sle_mesh` 工程当前推荐走项目脚本，它会先尝试自动复位：

```sh
printf 'flash leader\n' | \
  <repo-root>/scripts/flash/ws63_flash_team.sh \
  leader /dev/tty.usbserial-10
```

自动复位机制：

- 新版 `xc/ws63_team_network` 固件支持串口 CLI `reboot/reset`，烧录脚本会先发 `reboot`。
- 脚本也会尝试 DTR/RTS 脉冲；是否有效取决于当前 USB 转串口/烧录器是否把控制线接到板子的复位控制脚。
- 第一次从老固件升级时，老固件没有 `reboot` 命令，仍可能需要手按一次 `RESET/RST`。小熊派 WS63 没有 BOOT 键时按 RESET 即可；带 BOOT 下载键的板子才需要按住 BOOT 再点 RESET。
- 关闭自动复位可用：`AUTO_RESET=0 scripts/flash/ws63_flash_team.sh leader /dev/tty.usbserial-10`。

底层 `xf-burn-tools` 命令仍是：

最终成功命令：

```sh
<python-user-bin>/burn \
  -p /dev/tty.usbserial-10 \
  -b 115200 \
  <sdk-root>/output_from_vm/ws63-liteos-app_all.fwpkg
```

烧录过程中如果工具停在：

```text
Waiting for device reset...
```

需要按一下板子的 `RESET/RST`。

如果板子有 `BOOT` 下载键，操作方式通常是：

```text
按住 BOOT -> 点一下 RESET -> 松开 BOOT
```

本次实际操作中，按复位后工具进入：

```text
Establishing ymodem session...
Transferring root_loaderboot_sign.bin...
```

## /dev/cu 和 /dev/tty 的坑

Mac 下同一个 USB 串口会有两个设备：

```text
/dev/cu.usbserial-10
/dev/tty.usbserial-10
```

本次实测：

- `/dev/cu.usbserial-10`：能打开，但烧录等待复位超时
- `/dev/tty.usbserial-10`：可以成功握手并烧录

所以 WS63 烧录时优先用：

```text
/dev/tty.usbserial-10
```

## 921600 波特率的坑

SDK 配置里默认 upload 波特率是 `921600`：

```json
"baud": "921600"
```

本次实测在 Mac + CH340 串口链路上：

- `921600` 能握手
- 但 YModem 传 `root_loaderboot_sign.bin` 会失败

失败现象：

```text
Establishing ymodem session...
Transferring root_loaderboot_sign.bin...
ERROR Error transferring root_loaderboot_sign.bin
```

改为 `115200` 后成功烧录完整包。

结论：

- 稳定优先：用 `115200`
- 速度优先：可以再试 `460800` 或 `921600`
- 第一次上板点灯：不要和高速串口较劲

## 等待复位超时的坑

`xf-burn-tools` 默认等待复位时间较短。

如果你来不及按复位，可以临时把安装包里的：

```text
<python-site-packages>/xf_burn_tools/ws63flash.py
```

从：

```python
RESET_TIMEOUT = 10
```

改成：

```python
RESET_TIMEOUT = 60
```

这只是本机工具修改，不是项目源码修改。

## 烧录成功标志

烧录工具末尾输出：

```text
Done. Reseting device...
```

串口读取能看到：

```text
APP|[SYS INFO] mem: used:..., free:...
```

读取串口示例：

```sh
python3 - <<'PY'
import serial, time
p = '/dev/tty.usbserial-10'
ser = serial.Serial(p, 115200, timeout=0.2)
ser.reset_input_buffer()
t = time.time()
while time.time() - t < 12:
    data = ser.read(512)
    if data:
        print(data.decode('utf-8', 'replace'), end='')
ser.close()
PY
```

## 点灯配置确认

本次编译的 blinky 配置来自：

```text
build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config
```

关键配置：

```text
CONFIG_SAMPLE_ENABLE=y
CONFIG_ENABLE_PERIPHERAL_SAMPLE=y
CONFIG_SAMPLE_SUPPORT_BLINKY=y
CONFIG_BLINKY_PIN=2
CONFIG_BLINKY_DURATION_MS=500
```

代码逻辑：

```c
uapi_pin_set_mode(CONFIG_BLINKY_PIN, PIN_MODE_0);
uapi_gpio_set_dir(CONFIG_BLINKY_PIN, GPIO_DIRECTION_OUTPUT);
uapi_gpio_set_val(CONFIG_BLINKY_PIN, GPIO_LEVEL_LOW);

while (1) {
    osal_msleep(CONFIG_BLINKY_DURATION_MS);
    uapi_gpio_toggle(CONFIG_BLINKY_PIN);
}
```

板级头文件里：

```c
#define BSP_LED_1  GPIO_02
```

所以当前点的是 `GPIO_2 / BSP_LED_1`。

## 常见问题速查

### 编译报 CMakeCache 路径不一致

原因：从 Mac 同步过去的根目录 `output/` 带了旧绝对路径。

解决：重新同步，排除根目录 `output`。

```sh
rsync -a --exclude=/output source/ fresh/
```

### 报 `distutils` 不存在

原因：Python 3.12+ 移除了 `distutils`。

解决：把 `find_executable` 改为 `shutil.which`。

### 找不到 `rom_callback_wrap.cmake`

原因：错误使用 `--exclude=output`，把 SDK 内部必要 output 目录也排除了。

解决：只排除根目录 output。

```sh
--exclude=/output
```

### `/dev/cu.usbserial-10` 烧录超时

解决：换 `/dev/tty.usbserial-10`。

### 921600 传输失败

解决：换 `115200`。

### 工具一直等待 reset

优先解决：升级到支持串口 CLI `reboot/reset` 的 `xc/ws63_team_network` 固件，然后用 `scripts/flash/ws63_flash_team.sh` 默认自动复位烧录。

如果是第一次从老固件升级，或当前烧录器没有接复位控制线，按板子复位；如果有 BOOT 键，按住 BOOT 再点 RESET。

### 烧录成功但灯不闪

检查：

- 当前是否真的编译了 `CONFIG_SAMPLE_SUPPORT_BLINKY=y`
- LED 是否接在 `GPIO_2`
- LED 是否低电平点亮
- 是否烧了完整包 `ws63-liteos-app_all.fwpkg`

## 本次推荐固定命令

编译：

```sh
cd /home/builder/workspace/bearpi-pico_h3863_fresh
export PATH="$HOME/.local/bin:$PATH"
python3 build.py ws63-liteos-app -j4
```

烧录：

```sh
<python-user-bin>/burn \
  -p /dev/tty.usbserial-10 \
  -b 115200 \
  <sdk-root>/output_from_vm/ws63-liteos-app_all.fwpkg
```

串口查看：

```sh
python3 - <<'PY'
import serial, time
p = '/dev/tty.usbserial-10'
ser = serial.Serial(p, 115200, timeout=0.2)
t = time.time()
while time.time() - t < 12:
    data = ser.read(512)
    if data:
        print(data.decode('utf-8', 'replace'), end='')
ser.close()
PY
```
