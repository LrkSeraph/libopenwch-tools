# libopenwch-tools

[English](README.md)

WCH RISC-V 单片机的宿主侧工具集。当前主要工具是 **wchlink**：面向
**WCH-LinkE** 的命令行烧录器和调试工具。

本仓库与
[libopenwch](https://github.com/LrkSeraph/libopenwch) 分离：驱动库是运行在
RISC-V 目标上的代码，本仓库则是使用 libusb 构建的宿主程序。模板仓库以
submodule 方式挂载本仓库，并把 `make flash` 委派给 `wchlink`。

> **状态：CH32V00x 的 M1–M5 已实现；硬件验证仍不完整。**
> `info`、halt/resume、调试寄存器访问、内存读回、CH32V00x 擦除/写入/校验、
> `reset`、`unbrick`、SDI/DMDATA 终端，以及一个精简 GDB server 都已实现。
> 其中 GDB halt/resume、寄存器和内存访问、continue、软件断点（`Z0`）已在
> 真实 CH32V003 上验证；独立的 flash/unbrick/terminal 路径仍需要完整硬件验证。

## 构建

```sh
make
```

产物：`build/wchlink`。`make install` 会安装可执行文件和 udev 规则。
libusb 通过 `pkg-config` 查找；如果系统只有运行库：

```sh
make LIBUSB_CFLAGS=-I/path/include LIBUSB_LIBS=-l:libusb-1.0.so.0
```

## 权限

```sh
sudo make install-udev-rules
# 重新插拔编程器
```

规则使用 `TAG+="uaccess"` 和 `MODE="0660", GROUP="plugdev"`。如果使用
group 方式，请把自己加入 `plugdev` 并重新登录。没有 udev 规则时，
`wchlink` 会报告“设备存在但无法打开”，而不是“找不到设备”。

## 用法

```text
wchlink info                 报告目标芯片（--chip 可选）
wchlink chips                列出已知型号
wchlink flash <file.bin>     擦除、写入、校验
wchlink read  <file.bin>     读内存；默认导出整片 flash
wchlink reset                复位并运行
wchlink unbrick              拉低复位并做电源循环
wchlink programmer <sub>     info | list | rv | iap
wchlink target <sub>         halt | resume | reset | pc | regs | read32 | write32
wchlink terminal             SDI/DMDATA 调试输出；Ctrl-C 停止
```

省略 `--chip` 时，`flash` 和 `read` 会自动探测芯片。在线调试命令
`target pc`、`regs`、`read32`、`write32` 需要 `--chip`：自动探测会释放并
重启目标，从而破坏要检查的现场。`target halt`、`resume`、`reset` 不需要
`--chip`。这些调试命令会让内核保持 halt；请用 `target resume` 恢复运行。

选项：`--serial`、`--chip`、`--address`、`--size`、`--verify`、
`--no-verify`、`--verbose`、`--quiet`、`--version`、`--help`。
退出码：`0` 成功，`1` USB/目标错误，`2` 用法错误，`3` 未找到编程器。

```sh
wchlink flash firmware.bin --chip ch32v003
wchlink flash config.bin --chip ch32v003 --address 0x08003f00
wchlink read dump.bin --chip ch32v003 --address 0x08000000 --size 1024
wchlink read whole.bin
wchlink programmer info
wchlink terminal
```

## GDB server

`wchlink gdbserver` 为单个目标提供精简 GDB Remote Serial Protocol server：

```sh
wchlink gdbserver --chip ch32v003
# 另一个终端：
gdb-multiarch firmware.elf
(gdb) target extended-remote :3333
(gdb) info registers
(gdb) continue
```

`--chip` 是必需的：初始 attach 会 halt 目标，而自动探测会重启它。需要已知
复位状态时请使用 `monitor reset`。连接之后，halt/resume 和 single-step 使用
RISC-V Debug Module 的 `DMCONTROL` 位，因此 server 继续或停止运行时不会
重启正在运行的程序。

目前已实现：寄存器、内存读写、continue、single-step、软件断点（`Z0`），以及
若干 `monitor` 子命令。软件断点会设置 `DCSR.ebreakm`，使 `ebreak` 进入
debug mode；这条路径已在真实 CH32V003 上验证，包括多个 flash 断点以及从
断点继续运行。flash 中的断点通过“读回、修改、擦除、重写所在页”插入，因此
速度较慢，并且只应在目标 halt 时使用。硬件断点和 GDB `load`（`vFlash*`）
仍未实现。

常用的 monitor 命令：

```text
(gdb) monitor info
(gdb) monitor regs
(gdb) monitor read32 0x40021018
(gdb) monitor write32 0x20000000 0x12345678
(gdb) monitor reset
(gdb) monitor halt
```

## 编程器与芯片探测

WCH-LinkE 可能以 RISC-V 调试设备（`1a86:8010`）、ARM/SWD 设备
（`1a86:8012`）或 USB ISP/IAP bootloader（`4348:55e0`、`1a86:55e0`）出现。
`wchlink` 会把可访问的 ARM/SWD 或 IAP 设备自动切换到 RISC-V debug mode。

`info` 关注目标芯片；`programmer info` 报告编程器本身。未知或未上电的目标
会被当作错误处理；工具不会猜测。

## 烧录

`flash` 按页执行擦除、写入和校验。部分覆盖的页会先读回并合并。操作期间目标
保持 halt，结束后复位。

烧录目前只支持 **CH32V00x**（CH32V003/002/004/005/006/007）。CH5xx 会被
拒绝，因为其 ROM 擦写接口没有公开文档。

## 测试

```sh
make test
```

三套无硬件测试：

* `target_test` — 芯片表和匹配逻辑；
* `cli_test` — 命令行处理和退出码；
* `flash_test` — 通过模拟 WCH-LinkE 和模拟 CH32V003 跑完整烧录流程。

模拟器不能替代真实芯片：它无法验证厂商协议常量或 debug module 的真实行为。

## 路线图

| | |
|---|---|
| **M1** | USB 发现、`info`、芯片表、CLI — 完成 |
| **M2** | halt/resume、调试寄存器、内存读回 — 完成 |
| **M3** | CH32V00x 擦除/写入/校验、`reset`、`unbrick` — 完成；硬件待验 |
| **M3b** | CH5xx 烧录 |
| **M4** | SDI/DMDATA 终端 — 完成；硬件待验 |
| **M5** | GDB server — 寄存器/内存/continue/step、软件断点、`monitor` — 完成；CH32V003 软件断点已实机验证 |

硬件断点和 GDB `load`（`vFlash*`）仍不在当前范围内。

## 许可证

LGPL-3.0-or-later；见 `LICENSE` 和 `NOTICE`。实现为 clean-room：
minichlink、wlink、riscv-openocd-wch 仅用于查阅协议事实。该协议没有官方
文档，来自社区逆向；WCH 未对其背书。
