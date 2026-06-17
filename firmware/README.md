# Firmware

固件代码按 SDK 工程边界管理。

## 当前固件工程

- [../xc/ws63_team_network/](../xc/ws63_team_network/)：WS63 SLE team network 固件工程，包含板端 WebUI、ST7789 显示、SLE leader/member/relay 运行时配置逻辑。
- [../xc/bearpi_pico_h3863_blinky/](../xc/bearpi_pico_h3863_blinky/)：BearPi Pico H3863 blinky / flash notes 辅助工程。

## 协议共享代码

- [../include/](../include/)：公共头文件。
- [../src/](../src/)：协议、组网状态机、CLI/Web API 序列化。
- [../examples/](../examples/)：本地 C 回归和接入示例。

## 版本边界

- 当前固件版本：`v4.4.129`，定义在 `xc/ws63_team_network/src/ws63_team_network_app.c`。
- 当前仓库整理记录：`v4.4.129`，见 [../versions/v4.4.129/VERSION.md](../versions/v4.4.129/VERSION.md)。
- 构建产物不提交到 Git；远程构建输出默认写入 `output_from_vm/`。
