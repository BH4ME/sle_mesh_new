# COM14 CH340 RTS/DTR 复位问题记录与解决方案

记录日期：2026-06-07

## 结论

COM14 是带 CH340E 和 MOS 管硬件复位电路的自制板。这个问题表面看起来像“下载到一半又被 RTS/DTR 复位”，但现有日志验证后，结论更准确地分成两层：

1. COM14 的硬件复位电路确实对 CH340E 的 `RTS#` 状态敏感，因为 `RTS#` 是低有效信号。
2. 这次已经复现并验证的“下载到一半失败”，直接原因不是烧录脚本中途再次切 RTS/DTR，而是 Windows/CH340 在 1029 字节 YMODEM 阻塞写入时发生 timeout。

2026-06-08 最新统一策略：

- COM14 不再把 `-HardwareReset` 作为可用烧录方案。
- `scripts/flash/ws63_flash_multi.ps1` 检测到 `COM14` 后启用 `COM14 safe profile`。
- `COM14 safe profile` 默认直接拒绝 `-HardwareReset`、拒绝 `blocking` 写入、拒绝 `SerialWriteChunkSize` 分包，并把 `WaitTimeout` 提升到 30 秒。
- 以后 COM14 固定使用 `software-reset-only + reboot + RTS=0 DTR=0 + no post-open control assertion + 1024-byte YMODEM + fresh receiver C + nonblocking-drain + full flash-session retry`。
- 不再复用 ROM handshake 读包里混带的 `C`；每个 YMODEM 文件传输前都必须重新等待一个新的接收端 `C`。
- COM14 不做同一 ROM 会话内的 YMODEM 文件级重试；失败后重新打开串口、重新软件复位、重新 handshake，再从头烧录。
- COM14 safe profile 先做 ROM 预探测：如果 ROM handshake 已经 active，就跳过 `reboot` 直接烧录；如果 ROM 不 active，才发送软件 `reboot`。
- 如果 COM14 已经进入“串口可打开，但 `cfg status` 无回复、静默 ROM handshake 也无 ACK”的状态，说明已经没有串口软件控制入口；此时不要继续尝试 RTS 硬件烧录，必须先按一下复位、断电重上电、拔插 USB，或修硬件复位电路，让板子回到 CLI 或 ROM 后再执行 safe profile。

之前“又失败”的原因是后来按要求强制 COM14 使用了硬件 RTS 复位；实测证明这条硬件复位入口本身不稳定，所以新的软件解决方案是脚本层面防止 COM14 再走这条路径。

所以最终软件方案不是分包，而是：

- 不走 RTS 硬件复位进入下载。
- 串口打开前就预置空闲状态：`RTS=0`、`DTR=0`。
- 使用软件复位命令 `reboot` 进入 ROM 下载模式。
- YMODEM 仍保持 1024 字节包。
- 串口写入模式改为 `--serial-write-mode nonblocking-drain`。
- 每个 YMODEM 传输前等待 fresh `C`，不能复用 handshake 读到的 `C`。
- `--ymodem-transfer-retries` 固定为 `1`；COM14 safe profile 使用 `--flash-attempts 2` 做完整会话重试。
- `--skip-reset-if-rom-active` 固定启用；板子已在 ROM 时不再额外发送 CLI `reboot`。
- 当前烧录工具默认就是 `nonblocking-drain`，不需要再用 `--serial-write-chunk-size`。
- 下载期间启用 RTS/DTR guard，后续如果有任何 RTS/DTR 变化，直接报显式错误。

成功验证过的 COM14 烧录没有使用 `--serial-write-chunk-size`，也就是没有走分包方案。

## 板卡区分

- `COM14`：自制 CH340E 板，带 MOS 管硬件复位电路。
- `COM13`：对照板，没有 MOS 管硬件复位电路。
- `COM23`、`COM24`、`COM25`、`COM26`：小熊派后续测试用板，其中 `COM26` 配置为 leader。

当前优先解决对象是 `COM14`。

## 现象

COM14 焊上 MOS 管后：

- 手动复位可以进入下载，但烧录到一半可能失败。
- RTS 复位也可以进入下载，但烧录到一半也可能失败。
- 失败多表现为 YMODEM 写串口时 timeout。
- 典型失败点是一次 1029 字节写入，1024 字节数据包加 YMODEM 头和校验。

COM14 拆掉 MOS 管后：

- 仅手动复位时，不再触发同样的“下载一半失败”问题。

分包后：

- 烧录可以正常。
- 但分包只是降低单次串口写入压力，不是首选方案。

## 为什么 MOS 管会放大这个问题

原理图中的信号是 CH340E `RTS#`，不是普通意义上的高有效 `RTS`。`#` 表示低有效。

当前电路图确实有硬件复位：`SCH_Schematic1_2_2026-06-07.pdf` 第 3 页中，CH340E 的 `RTS#` 网标接到 Q1/MOS 复位电路，Q1 再影响 WS63 模块的 `EN`；旁边还有 SW1 手动把 `EN` 拉到 GND 的复位按键。

但这条硬件复位只能说明“PC 可能通过 CH340 的 RTS# 改变 EN”，不能直接等价为“软件烧录时可以可靠进入 ROM 下载”。原因有三个：

- CH340E 暴露的是 `RTS#` 低有效脚，pyserial 里的 `ser.rts=True/False` 和引脚电平/EN 行为容易被反相误解。
- 这条电路是 RTS# 到 EN 的电平控制路径，不是边沿限宽的一次性复位脉冲；如果空闲态、打开串口瞬间、或下载期间的保持态不对，可能变成保持复位、误复位、或复位时机错误。
- `BOOT` 在图中是 WS63 的 IO3，没有接到 CH340 的 RTS/DTR 控制线；所以软件只能尝试复位并等待 ROM handshake，不能同时保证 BOOT/下载条件。

在 pyserial/Windows 语义里，设置 RTS 的布尔状态和 CH340 实际 `RTS#` 引脚电平之间容易产生误解。结果就是：如果把 `RTS#` 直接接进 MOS 管复位路径，串口打开瞬间、驱动默认状态、复位脉冲极性、或者下载期间保持的控制线状态，都可能影响 EN/RESET。

因此 COM14 不能依赖这种旧流程：

```text
先 open 串口，再 setRTS(False)
```

更安全的流程是：

```text
先创建未打开的 serial.Serial()
预置 ser.rts = 0、ser.dtr = 0
再 open()
```

这样可以减少打开串口瞬间由 RTS/DTR 默认状态造成的误复位风险。

## 已验证的证据

烧录脚本已经加过保护和日志：

- `open_serial_for_burn()`：在打开串口前预置 `RTS=0`、`DTR=0`。
- `ControlLineGuard`：下载阶段拦截 `setRTS()`、`setDTR()`、`ser.rts = ...`、`ser.dtr = ...`。
- 串口写失败时会记录 RTS/DTR/CTS/DSR 和写入队列状态。

关键证据是：

- 阻塞写模式下，日志显示 `RTS=0 DTR=0`，没有 control-line guard violation，但仍可能在 1029 字节写入时 timeout。
- 只把写入模式改为 `nonblocking-drain` 后，COM14 使用 1024 字节 YMODEM 包完整烧录成功。
- 成功命令没有包含 `--serial-write-chunk-size`。
- 2026-06-08 按要求强制 `COM14` 使用 `-HardwareReset` 后，日志证明硬件复位流程确实执行了，但该路径不稳定：
  - `logs/burn/v4.4.99_20260608_003809/COM14.log`：执行 `Auto reset: RTS=1` 后 `Auto reset: RTS=0`，随后 `Download control-line release: RTS=0 DTR=0`，guard 期间保持 `rts=0 dtr=0`，ROM handshake 成功，但 `root_loaderboot_sign.bin` 传输到 77% 后 ACK timeout。
  - `logs/burn/v4.4.99_20260608_003935/COM14.log`：同样硬件 RTS 复位序列后，等待 ROM handshake timeout。
  - `logs/burn/v4.4.99_20260608_004020/COM14.log`：反向 RTS 序列 `rts=0:0.25;rts=1:0.5` 后，等待 ROM handshake timeout。
- 上述硬件复位日志没有出现 `Unexpected RTS change` 或 `Unexpected DTR change`，所以没有证据表明烧录脚本在 YMODEM 中途再次触发 CH340 的 RTS/DTR。
- 2026-06-08 09:38/09:40 的 safe profile 已经使用软件 `reboot`、`RTS=0 DTR=0`、`nonblocking-drain` 进入 ROM，但第一个 `root_loaderboot_sign.bin` 的 YMODEM 失败。
- 这次新的软件原因是烧录器把 ROM handshake 读包中混带的 `C` 记录成 `_ymodem_receiver_ready`，随后跳过 YMODEM 前的 fresh `C` 等待。这个行为和供应商原流程不同，也和 2026-06-07 成功日志不同。
- 最新修正已经改为：handshake 读到 `C` 只记录日志，不作为可复用状态；YMODEM 每次都重新等待 fresh `C`。
- 09:38/09:40 的日志还证明同一 ROM 会话内重试整个 YMODEM 文件不能有效恢复，所以 COM14 safe profile 改为文件级重试 `1` 次、完整 flash session 默认 `2` 次。
- 2026-06-08 10:57 使用最新 safe profile 做真实验证，脚本参数已经正确生效：`ymodem_transfer_retries: 1`、`flash_attempts: 2`、`serial_write_mode: nonblocking-drain`、`RTS=0 DTR=0`。
- 但该次验证两次完整会话都在 ROM handshake timeout：`logs/burn/codex_com14_verify_20260608_105719/v4.4.99_20260608_105720/COM14.log`。
- 验证后，`cfg status` 无回复，静默 ROM handshake 无 ACK，Windows PnP 禁用/启用 COM14 CH340 因权限失败，两种 RTS handshake-only 脉冲也没有 ACK。
- 因此当前 COM14 已处在无串口软件入口状态。软件方案能防止再次走错误的硬件 RTS 烧录路径，也能修复 YMODEM fresh-C 同步问题；但不能在 MCU 完全无 CLI/无 ROM 响应时凭空复位芯片。

成功日志：

```text
logs/burn/com14_software_reset_nonblock_drain_1024_20260607_171545/stdout_stderr.log
logs/burn/com14_software_reset_nonblock_drain_1024_20260607_171545/exit.txt
```

成功日志中的关键行包括：

```text
Serial opened with idle RTS=0 DTR=0
Serial write mode: nonblocking-drain
Download control-line guard active
ROM handshake ACK: ... rts=0 dtr=0 ...
Transferring ... 100%
Done. Reseting device...
exit=0
```

## 根因判断

根因不是单一的“脚本中途又复位了”。

更准确的判断是：

1. 硬件层面：COM14 的 MOS 管复位电路对 CH340E `RTS#` 状态敏感。`RTS#` 是低有效，如果极性、初始状态或打开串口瞬间处理不好，确实可能导致误复位或复位保持。
2. 硬件复位路径：强制 `COM14` 走 RTS 硬件复位时，ROM 入口本身不稳定；即使偶尔进入 ROM，也可能在 loader YMODEM 阶段 ACK timeout。该失败不是由脚本中途再次切 RTS/DTR 造成的。
3. 软件传输层面：已经验证到的中途失败，是 Windows/CH340 在 1029 字节阻塞写入时 timeout。这个 timeout 在带 MOS 管的 COM14 上更容易暴露，但 guard 日志没有证明下载过程中脚本又切了 RTS/DTR。

分包能成功，是因为它把一次 1029 字节写入拆成更小写入，降低了 CH340/Windows 写队列压力。但这改变了串口写入节奏，只适合作为备用 workaround。

首选方案是 `nonblocking-drain`：仍按 1024 字节 YMODEM 包发送，只是不再用一次阻塞写死等，而是非阻塞写入后等待 TX 队列排空。

## 推荐的 COM14 单板烧录命令

推荐使用这个不分包命令：

```powershell
E:\codex_documents\sle\.tooling\py311\python.exe `
  E:\codex_documents\sle\automation\ws63\tools\ws63_auto_burn.py `
  -p COM14 `
  -b 115200 `
  --software-reset-only `
  --reset-command reboot `
  --no-reset-command-fallback `
  --no-compat-reset-command `
  --reset-command-delay 0.05 `
  --reset-command-retries 1 `
  --reset-command-retry-gap 0 `
  --idle-rts 0 `
  --idle-dtr 0 `
  --no-assert-control-after-open `
  --wait-timeout 30 `
  --manual-retry-timeout 0 `
  --ymodem-packet-size 1024 `
  --ymodem-transfer-retries 1 `
  --serial-write-mode nonblocking-drain `
  --serial-write-drain-timeout 3.0 `
  --skip-reset-if-rom-active `
  --rom-preflight-timeout 1.0 `
  --flash-attempts 2 `
  --expected-version v4.4.99 `
  E:\codex_documents\sle\output_from_vm\team_network_v4_unified_runtime_role\ws63-liteos-app_v4_unified_all.fwpkg
```

期望看到：

```text
ROM handshake ACK
Transferring ws63-liteos-app-sign.bin...
Done. Reseting device...
exit=0
```

## 批量脚本用法

`scripts/flash/ws63_flash_multi.ps1` 已经默认使用同样的不分包写入模式。

COM14 推荐：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File E:\codex_documents\sle\scripts\flash\ws63_flash_multi.ps1 `
  -Ports COM14 `
  -ExpectedVersion v4.4.99 `
  -WaitTimeout 30 `
  -ManualRetryTimeout 0 `
  -SerialWriteDrainTimeout 3.0 `
  -YmodemPacketSize 1024 `
  -FlashAttempts 2
```

脚本检测到 `COM14` 时会自动启用安全策略，所以上面的命令会生成如下关键参数：

```text
--software-reset-only
--reset-command reboot
--no-reset-command-fallback
--no-compat-reset-command
--idle-rts 0
--idle-dtr 0
--no-assert-control-after-open
--ymodem-packet-size 1024
--ymodem-transfer-retries 1
--serial-write-mode nonblocking-drain
--skip-reset-if-rom-active
--flash-attempts 2
```

正常使用时不要加：

```text
-SerialWriteChunkSize
```

也不要给 COM14 加：

```text
-HardwareReset
```

如果误加，脚本会直接报错并拒绝烧录。只有做诊断时才允许显式加 `-AllowUnsafeCom14HardwareReset`，但这不是常规方案。

## 不推荐作为 COM14 常规方案

不要把下面两个作为 COM14 的常规烧录路径：

```text
-HardwareReset
--control-sequence rts=0:0.25;rts=1:0.5
--serial-write-chunk-size 16 --serial-write-gap 0.001
```

原因：

- `-HardwareReset` 和 `--control-sequence ...` 都依赖 RTS 硬件复位路径，实测 COM14 ROM 入口不稳定。
- 分包只对已经进入 ROM/YMODEM 的传输阶段有意义；2026-06-08 13:48 复测时 COM14 失败在 ROM handshake 之前，分包没有机会生效。

## 硬件修改建议

如果后续仍希望保留 RTS 自动复位，不建议让 CH340E `RTS#` 通过 MOS 管形成一个能长期控制 EN/RESET 的直流通路。

更推荐的硬件行为是：RTS 只能产生一个短复位脉冲，不能长期保持复位，也不能在 YMODEM 传输期间再次触发复位。

可选方向：

- 用 RC/AC 耦合或单稳态电路，把 RTS 转成边沿触发的短脉冲。
- 加跳帽或门控，让烧录时可以断开 RTS 复位路径。
- 给 EN/RESET 做更明确的上拉和滤波，保证 CH340 默认状态或串口打开瞬间不会把板子保持在复位。
- 重新核对 MOS 管方向、上拉/下拉阻值、`RTS#` 极性，确保低有效逻辑被正确反相或隔离。

## 后续验收清单

以后检查 COM14 烧录时，确认这些点：

- 烧录前如果 `cfg status` 无回复且静默 ROM handshake 无 ACK，先物理复位/断电/拔插 USB；不要继续烧录。
- 命令包含 `--ymodem-packet-size 1024`。
- 日志或命令显示 `nonblocking-drain`。
- 日志或命令显示 `--flash-attempts 2` 或 `flash_attempts: 2`。
- 日志或命令显示 `--skip-reset-if-rom-active`。
- 日志显示 `ROM handshake read also contained YMODEM receiver C; waiting for fresh C` 时，后续仍必须正常等待 fresh `C`，不能跳过。
- 命令不包含 `--serial-write-chunk-size`。
- 日志包含 `Serial opened with idle RTS=0 DTR=0`。
- 日志包含 `Download control-line guard active`。
- 日志不包含 `Unexpected RTS change`。
- 日志不包含 `Unexpected DTR change`。
- 日志不包含 `Serial write failed`。
- 日志最后有 `Done. Reseting device...`。
- 进程退出码是 `0`。

## 一句话版

COM14 的 MOS 管复位电路确实让 CH340E `RTS#` 变成高风险控制线；按要求强制硬件复位时，实测 ROM 入口不稳定，且没有证据表明脚本在下载中途又切 RTS/DTR。当前主方案仍是固定 `RTS=0 DTR=0`、软件复位、YMODEM 1024 包不变，并使用 `nonblocking-drain` 写入模式。

最新执行规则：以后 COM14 一律使用 `ws63_flash_multi.ps1` 的 `COM14 safe profile`；不要加 `-HardwareReset`，不要分包。脚本已经把这条规则固化，误用硬件复位会被拒绝。

## 2026-06-08 13:48 强制硬件复位和分包复测

按现场要求，对 COM14 强制绕过 safe profile，启用 `-HardwareReset -AllowUnsafeCom14HardwareReset`，并启用串口写入分包。

- 固件：`output_from_vm/team_network_v4_unified_runtime_role/ws63-liteos-app_v4_unified_all.fwpkg`
- 版本：`v4.4.100`
- 分包参数：`SerialWriteChunkSize=256`、`SerialWriteGap=0.005`、`SerialWriteMode=nonblocking-drain`
- 尝试过的复位/握手组合：RTS 正反向、DTR 单独切换、RTS+DTR 同向/交叉切换、RTS 1s/2s 长脉冲、3 分钟人工复位等待窗口。
- Windows PnP disable/enable COM14 CH340 失败，系统返回“常规故障”。
- 结果：全部失败在 ROM handshake 阶段，未进入 YMODEM 文件传输，所以本轮失败不是分包写入失败。
- 相关日志：
  - `logs/burn/v4.4.100_20260608_134818/COM14.log`
  - `logs/burn/v4.4.100_20260608_135011/COM14.log`
  - `logs/burn/v4.4.100_20260608_135341/COM14.log`

同期同一固件已成功烧录到 `COM13`、`COM23`、`COM24`、`COM25`、`COM26`。COM14 当前阻塞点是板端没有被 PC 侧 RTS/DTR 控制带入 WS63 ROM 下载模式；需要板端实际进入 ROM 后，256B 串口分包参数才会生效。
