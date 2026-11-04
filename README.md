# Allwinner F1C100s/F1C200s Buildroot BSP

面向 Allwinner F1C100s/F1C200s 的 Buildroot BSP，当前主要开发板为
**LiuLianPi（F1C100s）**。

- 本仓库：<https://github.com/Queqiongtao/buildroot-tiny200>
- 上游项目：<https://github.com/aodzip/buildroot-tiny200>
- Buildroot：2020.02.7
- U-Boot：2020.07
- Linux：5.4.99

> 本仓库 fork 自 [aodzip/buildroot-tiny200](https://github.com/aodzip/buildroot-tiny200)，
> 在其基础上增加了 LiuLianPi 板级支持、SPI NOR 烧录工具链、新宿主构建兼容修复
> 和板级文档。本仓库是独立维护的衍生版本，不是 Buildroot 官方项目。

## 支持的板卡

仓库中的 `configs/` 提供以下 defconfig：

| defconfig | 板卡 |
| --- | --- |
| `liulianpi_defconfig` | LiuLianPi（F1C100s），当前主要维护目标 |
| `sipeed_lichee_nano_defconfig` | Sipeed Lichee Nano |
| `widora_mangopi_r1_defconfig` | Widora MangoPi R1 |
| `widora_mangopi_r2_defconfig` | Widora MangoPi R2 |
| `widora_mangopi_r3_defconfig` | Widora MangoPi R3 |
| `hatlab_badge200_defconfig` | HatLab Badge200 |

除 LiuLianPi 外，其余配置来自上游项目或仓库历史，未在本仓库当前硬件上逐一验证。

## LiuLianPi 硬件概览

| 项目 | 参数 |
| --- | --- |
| SoC | Allwinner F1C100s（ARM926EJ-S，ARMv5） |
| DRAM | 64MiB |
| 存储 | 16MiB SPI NOR（Winbond W25Q128，`spi0`，50MHz） |
| 显示 | 1024×600 RGB LCD（PD0~PD21），背光 GPIO134 |
| 串口 | ttyS0，115200 |
| 启动介质 | SPI NOR（也支持 MMC0/MMC1、SPI NAND） |

## 快速开始

### 1. 安装宿主依赖

推荐使用 Linux x86_64 宿主：

```shell
sudo apt update
sudo apt install -y wget unzip build-essential git bc swig \
    libncurses-dev libpython3-dev libssl-dev libusb-1.0-0

# 可选：使用 DFU 烧录方式时需要
sudo apt install -y dfu-util

# 旧版 Ubuntu/Debian 可能还需要 python3-distutils；新版发行版已移除该包，可忽略
# sudo apt install -y python3-distutils
```

`libusb-1.0-0` 是仓库自带 FEL 工具（`sunxi-fel`/`xfel`）的运行时依赖。

### 2. 应用板级配置

```shell
git clone git@github.com:Queqiongtao/buildroot-tiny200.git
cd buildroot-tiny200

# 只需执行一次；会重置当前 Buildroot 配置
make liulianpi_defconfig
```

### 3. 构建

```shell
make -j$(nproc)
```

首次构建会下载源码到 `dl/`，并把工具链、内核、U-Boot 和 rootfs 等产物写入
`output/`。

> 如果宿主使用 GNU Make 4.4 及以上，且构建卡在 `glibc ... Building`
> （`MAKE_RESTARTS` 持续增大），先构建 Buildroot 自带的 Make 4.2.1，
> 再用它继续：
>
> ```shell
> make host-make
> output/host/bin/host-make -j$(nproc)
> ```
>
> 详细说明见 [构建、校验与烧录](board/liulianpi/doc/build/build-and-verify.md)。

### 4. 构建产物

构建完成后，主要文件位于 `output/images/`：

| 文件 | 说明 |
| --- | --- |
| `u-boot-sunxi-with-spl.bin` | SPL + U-Boot，烧写到 NOR 偏移 `0x0` |
| `splash.bmp` | U-Boot 开机图，必须是 24bpp BMP |
| `zImage` / `devicetree.dtb` | 内核与设备树 |
| `kernel.itb` | FIT 镜像（zImage + devicetree.dtb） |
| `rootfs.squashfs` | 只读根文件系统 |
| `sysimage-nor.img` | 16MiB SPI NOR 整包镜像，实际烧写对象 |

### 5. 构建后校验

每次构建后至少检查以下几项：

```shell
# 1) rootfs 内文件属主必须是 root（不能是构建用户 uid 1000）
output/host/bin/unsquashfs -ll output/images/rootfs.squashfs \
    | grep -E "bin/busybox|preinit"
# 期望：-rwsr-xr-x root/root ... bin/busybox
#       -rwxr-xr-x root/root ... preinit

# 2) fakeroot 的 stat 系列拦截符号存在（新版 glibc 宿主必需）
nm -D output/host/lib/libfakeroot.so \
    | grep -E " T (lstat|stat|fstat|fstatat)$"
# 期望：4 个符号全部输出

# 3) squashfs 必须小于 7MiB，否则会写穿 overlay 分区
ls -l output/images/rootfs.squashfs

# 4) 开机图必须是 24bpp BMP
file output/images/splash.bmp
# 期望包含：24-bit

# 5) bootargs 中包含 console=tty0 和 console=ttyS0
output/host/bin/linux-dtc -I dtb -O dts output/images/devicetree.dtb \
    | grep bootargs

# 6) 内核 panel-simple 补丁已应用
grep -c 0017 output/build/linux-5.4.99/.applied_patches_list
grep -c '"simple-panel"' \
    output/build/linux-5.4.99/drivers/gpu/drm/panel/panel-simple.c
# 期望：各为 1
```

完整说明见 [构建、校验与烧录](board/liulianpi/doc/build/build-and-verify.md)。

### 6. 烧录

#### 方式一：FEL 全量烧写（推荐）

进入 FEL 的常见方式：

- 板内没有有效 bootloader：上电自动进入 FEL（USB ID `1f3a:efe8`）。
- 板内有可用 U-Boot：在串口打断自动启动，执行
  `sf probe; sf erase 0x0 0x80000; reset`，擦除启动区后复位即进入 FEL。
- 硬件方式：上电瞬间短接 SPI flash 引脚。

板子进入 FEL 模式后，在仓库根目录执行：

```shell
./flashutils/linux/fel-nor-all.sh
```

脚本会等待 FEL 设备出现，使用仓库自带的 `sunxi-fel` 写入
`output/images/sysimage-nor.img`，最后用 `xfel reset` 复位。也可以手动执行：

```shell
./flashutils/linux/bin/sunxi-fel -p spiflash-write 0 output/images/sysimage-nor.img
./flashutils/linux/bin/xfel reset
```

Linux 下建议先安装 udev 规则：

```shell
sudo cp flashutils/linux/99-allwinnerdl.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

#### 方式二：U-Boot DFU

U-Boot 启动时会保留约 3 秒的 DFU 窗口（串口打印
`DFU waiting on SPI-NOR...`），可只更新内核或整片更新：

```shell
./flashutils/linux/dfu-nor-all.sh   # 整片更新
./flashutils/linux/dfu-kernel.sh    # 只更新 kernel
```

烧录细节、Windows 工具和常见问题见：

- [flashutils/README.zh.md](flashutils/README.zh.md)（中文）
- [flashutils/README.md](flashutils/README.md)（English）

## 启动流程

```text
BROM
 └─ SPL（sunxi spl，从 MMC0/MMC1 尝试，失败后回落 SPI NOR）
     └─ U-Boot 2020.07
         ├─ 扫描启动槽（mmc0 / mmc1 / spi-nor / spi-nand）
         ├─ 显示 splash 并点亮背光
         ├─ 保留约 3 秒 DFU 窗口
         └─ NOR: sf read kernel.itb 到 0x81000000 → bootm
             └─ Linux 5.4.99（从 FIT 中取 zImage + devicetree.dtb）
                 └─ /preinit → 挂载 JFFS2 overlay → overlayfs → chroot /sbin/init
```

`/preinit` 负责在只读 squashfs 根之上建立可写的 overlayfs，因此它必须由
PID 1 以 root 身份运行。若 rootfs 镜像属主错误，`mount` 会失败，系统会停留在
只读根文件系统。

## SPI NOR 分区布局

16MiB NOR 的分区定义在内核 DTS 的 `spi-nor@0/partitions` 中：

| 分区 | 偏移 | 大小 | MTD 设备 | 内容 |
| --- | --- | --- | --- | --- |
| `u-boot` | `0x000000` | 512K | `/dev/mtdblock0` | `u-boot-sunxi-with-spl.bin` |
| `splash` | `0x080000` | 512K | `/dev/mtdblock1` | `splash.bmp`（24bpp） |
| `kernel` | `0x100000` | 5M | `/dev/mtdblock2` | `kernel.itb` |
| `rom` | `0x600000` | 7M | `/dev/mtdblock3` | `rootfs.squashfs` |
| `overlay` | `0xD00000` | 3M | `/dev/mtdblock4` | JFFS2 可写层 |

注意：`board/allwinner/generic/genimage-nor.cfg` 中 `rom` 分区没有写死大小，
会填满剩余空间；而内核 DTS 中 `rom` 只到 `0xD00000`。因此
**`rootfs.squashfs` 必须小于 7MiB**，否则会写穿到 overlay 分区。

详细说明见 [启动流程与 NOR 分区布局](board/liulianpi/doc/boot/boot-flow-and-flash-layout.md)。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `board/liulianpi/f1c100s/` | LiuLianPi 的 Buildroot defconfig、U-Boot defconfig、U-Boot/内核 DTS |
| `board/liulianpi/doc/` | 板级中文文档：启动、构建、故障复盘、变更记录 |
| `board/allwinner/generic/` | 共用 U-Boot 环境变量、genimage 布局、`kernel.its`、`preinit`、splash、post-image 脚本 |
| `board/allwinner/suniv-f1c100s/` | 共用 dtsi、内核 defconfig、内核补丁 `0001`~`0017`、U-Boot 补丁 |
| `flashutils/` | 随仓库分发的 FEL/DFU 工具与烧写脚本 |
| `configs/` | 各板卡的 defconfig 符号链接 |
| `package/` | Buildroot 软件包及本地兼容性补丁 |
| `output/images/` | 构建产物（构建后生成） |

## 相对上游的主要改动

| 类别 | 内容 |
| --- | --- |
| 板级支持 | 新增 LiuLianPi（F1C100s）defconfig、U-Boot DTS、内核 DTS |
| NOR 布局 | 对齐 16MiB SPI NOR 分区，U-Boot 从 `kernel.itb` 启动 |
| 开机显示 | 启用 U-Boot `bmp` 命令，splash 转为 24bpp；内核增加 simplefb 节点 |
| DRM 修复 | 新增 panel-simple 通用 `"simple-panel"` 支持补丁 `0017` |
| 内核日志 | bootargs 增加 `console=tty0`，内核日志同时输出到 LCD |
| 构建兼容 | 修复 fakeroot 在 glibc ≥2.33 下的 stat 系列 wrapper；补充新版 GCC/CMake 兼容补丁 |
| 烧录工具 | 随仓库附带 `xfel`、`sunxi-fel`、`mksunxi` 及 FEL/DFU 脚本 |
| 文档 | 新增 `board/liulianpi/doc/` 中文板级文档与故障复盘 |

## 板级文档

- [板级文档索引](board/liulianpi/doc/README.md)
- [启动流程与 NOR 分区布局](board/liulianpi/doc/boot/boot-flow-and-flash-layout.md)
- [构建、校验与烧录](board/liulianpi/doc/build/build-and-verify.md)
- [故障复盘：rootfs 属主错误导致系统以非 root 启动](board/liulianpi/doc/troubleshooting/rootfs-uid1000-setuid-busybox.md)
- [故障复盘：sun4i-drm 不注册（panel compatible 无匹配）](board/liulianpi/doc/troubleshooting/drm-not-binding-simple-panel-compatible.md)
- [变更记录 2026-10-18](board/liulianpi/doc/changes/2026-10-18-rootfs-ownership-splash-simplefb.md)

驱动支持进度见：

- [F1C100s/F1C200s 驱动支持进度](PROGRESS-SUNIV.md)
- [V3/V3s/S3/S3L 驱动支持进度](PROGRESS-V3.md)

## 增量重建

修改板级文件后，按改动类型选择重建命令：

| 改动 | 命令 |
| --- | --- |
| U-Boot defconfig（`uboot.defconfig`） | `make uboot-dirclean uboot` |
| 内核 DTS / 内核配置 | `make linux-rebuild` |
| 内核补丁（`board/allwinner/suniv-f1c100s/patch/linux/`） | `make linux-dirclean linux` |
| rootfs 内容 / fakeroot / BusyBox | `rm -f output/images/rootfs.* output/images/sysimage-*.img && make` |

内核补丁只在解包阶段应用，`linux-rebuild` 不会重新打补丁；rootfs 镜像被删除后
才会重新生成。

## 已知注意事项

1. **rootfs 属主必须是 root**。宿主 glibc ≥2.33 时，旧版 fakeroot 可能让镜像
   属主变成构建用户，导致系统以 uid 1000 启动、`/preinit` 无法挂载 overlay。
   本仓库已包含修复补丁，构建后仍建议按上面的校验步骤检查。
2. **`rootfs.squashfs` 必须小于 7MiB**，否则会覆盖 overlay 分区。
3. **开机图必须是 24bpp BMP**。U-Boot legacy video 路径不支持 32bpp。
4. **`output/host/bin` 中不要手工放置无 RPATH 的二进制**（例如手工安装的
   `sunxi-fel`），否则 Buildroot 的 `check-host-rpath` 会中断构建。烧录脚本使用
   `flashutils/linux/bin/` 下的自带工具，不依赖 `output/host/bin`。
5. 修改内核补丁后必须执行 `make linux-dirclean linux`，只执行 `linux-rebuild`
   不会重新应用补丁。

## 上游与许可

- 上游项目：[aodzip/buildroot-tiny200](https://github.com/aodzip/buildroot-tiny200)
- Buildroot 主体使用 GPL-2.0-or-later，详见 [COPYING](COPYING)。
- 各软件包、补丁和第三方工具可能有各自的许可证，见对应目录中的声明。
