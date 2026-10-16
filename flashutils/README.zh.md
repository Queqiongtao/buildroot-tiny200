# 烧录指南（中文）

本工程固件烧录的完整说明。下载工具已随仓库分发（`linux/bin/`、`windows/bin/`，
来源 `QF1cx00s_Project/tools`，xfel v1.3.2），克隆仓库后无需另行编译即可使用。

## 工具一览

| 工具 | 平台 | 用途 |
|---|---|---|
| `sunxi-fel` | Linux | SPI NOR 烧写（主用）：`spiflash-write`，自带擦除 |
| `xfel` | Linux | SPI NOR/NAND 检测读写、复位（`reset`） |
| `mksunxi` | Linux | 给裸固件 bin 加 eGON.BT0 启动头 |
| `sunxi-fel.exe` / `xfel.exe` / `mksunxi.exe` | Windows | 同上 |

Linux 工具依赖 libusb-1.0 运行时（一般系统已带，`dfu-util` 也依赖它）。

## 方式一：FEL 直写 SPI NOR（推荐，无需板内 bootloader）

用法与 QF1 工程 `build.sh` 完全一致：`sunxi-fel -p spiflash-write 0` + `xfel reset`。

### 1. 让板子进入 FEL 模式

- **板内有可用 U-Boot**：串口按住任意键复位打断 autoboot，进入命令行后执行
  （先拔掉 TF 卡）：
  ```
  => sf probe
  => sf erase 0x0 0x80000
  => reset
  ```
  擦掉启动区后 BootROM 会落到 FEL。此状态不会变砖：BootROM FEL 在芯片 ROM 里，
  随时可以重新进入。
- **板内无有效 bootloader**：上电自动进入 FEL（USB 设备 `1f3a:efe8`）。
- **硬件法**：上电瞬间短接 SPI flash 引脚。

### 2. 烧录

Linux（工程根目录下，脚本会等待 FEL 设备出现）：

```shell
./flashutils/linux/fel-nor-all.sh          # 可选参数：镜像路径
```

手动等价命令：

```shell
./flashutils/linux/bin/sunxi-fel -p spiflash-write 0 output/images/sysimage-nor.img
./flashutils/linux/bin/xfel reset
```

等价 xfel 写法（先整片擦除再写）：

```shell
./flashutils/linux/bin/xfel spinor erase 0x0 0x1000000
./flashutils/linux/bin/xfel spinor write 0x0 output/images/sysimage-nor.img
./flashutils/linux/bin/xfel reset
```

Windows：

```bat
flashutils\windows\fel-nor-all.bat
REM 或手动
flashutils\windows\bin\sunxi-fel.exe -p spiflash-write 0 output\images\sysimage-nor.img
flashutils\windows\bin\xfel.exe reset
```

16MB 镜像约 1~3 分钟，完成后自动复位（`xfel reset`）即从新固件启动。

## 方式二：U-Boot DFU 烧录

板内 U-Boot 带 DFU 时可用 `flashutils/linux/dfu-*.sh`（或 Windows 的 `.bat`）：

- `dfu-nor-all.sh`：整片写 `sysimage-nor.img`
- `dfu-nand-all.sh`：整片写 `sysimage-nand.img`
- `dfu-mmc-all.sh`：整片写 `sysimage-sdcard.img`
- `dfu-kernel.sh`：只更新内核 `kernel.itb`

先运行脚本（等待设备），再复位板子让 U-Boot 进入 DFU。

## NAND / 裸固件（QF1 工程场景）

```shell
# SPI NAND 裸程序烧写（与 QF1 ./build.sh flash nand 相同）
./flashutils/linux/bin/xfel spinand splwrite 1024 0x40000 firmware.bin
./flashutils/linux/bin/xfel reset

# 给裸固件加 eGON.BT0 启动头（就地修改，之后可直接写 NOR 0x0）
./flashutils/linux/bin/mksunxi firmware.bin
```

## udev 规则（Linux）

```shell
sudo cp flashutils/linux/99-allwinnerdl.rules /etc/udev/rules.d/
```

规则同时放行 FEL（`1f3a:efe8`）与 U-Boot DFU（`1f3a:1010`）设备。

## 常见问题

- **提示 `Resource busy`**：同一时间只能有一个工具占用 FEL 设备，等另一个进程退出。
- **烧录中途断电/拔线**：板子会停在 FEL 模式，重新执行烧录即可，不会变砖。
- **`Waiting for FEL Device` 一直不出现**：检查 USB 线接的是 OTG 口、驱动/udev 规则
  （Windows 用 zadig 装 FEL 驱动），以及板子确实被擦除启动区或为空片。
- **验证镜像**：`./flashutils/linux/bin/xfel spinor read 0x0 <长度> dump.bin` 可与原镜像比对，
  或重新上电看串口启动日志。
