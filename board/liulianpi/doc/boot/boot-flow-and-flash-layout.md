# 启动流程与 NOR 分区布局

本文描述 LiuLianPi(F1C100s + 16MiB SPI NOR)从上电到用户态 `/sbin/init` 的完整启动链路,
以及各分区的作用和 bootargs 的关键参数。

## 板级配置位置

| 文件 | 作用 |
| ---- | ---- |
| `board/liulianpi/f1c100s/liulianpi_defconfig`        | Buildroot 板级 defconfig(内核/U-Boot/rootfs/sysimage 全部开关) |
| `board/liulianpi/f1c100s/uboot.defconfig`            | U-Boot 板级 defconfig(LCD、SPI、DFU、启动命令等) |
| `board/liulianpi/f1c100s/devicetree/linux/devicetree.dts` | 内核设备树(分区表、bootargs、LCD、编解码器等) |
| `board/allwinner/generic/uboot.env`                  | U-Boot 默认环境变量(bootcmd、启动槽扫描、DFU、splash) |
| `board/allwinner/generic/rootfs/preinit`             | rootfs 的 `init=/preinit` 脚本(挂 overlay 后 chroot) |
| `board/allwinner/generic/genimage-nor.cfg`           | NOR 整包镜像布局定义 |

## 硬件概览

- SoC:Allwinner F1C100s(ARM926EJ-S,64MiB DRAM)
- 存储:Winbond W25Q128 16MiB SPI NOR(`spi0`,50MHz)
- 显示:1024x600 RGB LCD(PD0~PD21),背光 GPIO134;U-Boot/内核都用该屏
- 串口:ttyS0(PE 口),115200

## 启动链路

```
BROM
 └─ SPL(sunxi spl,从 MMC0/MMC1 尝试,失败回落 SPI NOR)
     └─ U-Boot 2020.07
         ├─ distro_bootcmd:
         │   1. scan_boot_slot     扫描启动槽(mmC0/mmc1/spi-nor/spi-nand)
         │   2. show_splash        从 splash 分区读 BMP 并显示,点亮背光
         │   3. boot targets       按 fel → dfu → mmc0 → mmc1 → spi 顺序
         └─ spi → nor_boot:sf read kernel.itb 到 0x81000000,bootm
             └─ Linux 5.4.99(从 FIT 中取 zImage + devicetree.dtb)
                 └─ /preinit(rootfs 内,见下)→ chroot /tmp /sbin/init
```

关键点:

- SPL 阶段打印 `Card did not respond to voltage select!` / `spl: mmc init failed` 属于
  **未插 SD 卡时的正常回落过程**,不是故障。
- U-Boot 的 `dfu_wait_timeout=3`(`uboot.env`)使启动前会出现约 3 秒的
  `DFU waiting on SPI-NOR...` 窗口,用于通过 USB DFU 更新固件(见 `flashutils/`)。
- U-Boot 显示 splash 依赖两个条件:`CONFIG_CMD_BMP=y`(bmp 命令)和
  splash.bmp 为 **24bpp**(legacy video 路径只支持 4/8/24bpp),详见
  `changes/2026-10-18-rootfs-ownership-splash-simplefb.md`。

## NOR 分区布局(16MiB)

分区定义在内核 DTS(`devicetree.dts` 的 `spi-nor@0/partitions`),同时与
`genimage-nor.cfg` 的整包布局保持一致:

| 分区 | 偏移 | 大小 | MTD 设备 | 内容 |
| ---- | ---- | ---- | -------- | ---- |
| u-boot  | 0x000000 | 512K   | `/dev/mtdblock0` | `u-boot-sunxi-with-spl.bin`(SPL + U-Boot) |
| splash  | 0x080000 | 512K   | `/dev/mtdblock1` | `splash.bmp`(251x94,24bpp) |
| kernel  | 0x100000 | 5M     | `/dev/mtdblock2` | `kernel.itb`(FIT:zImage + devicetree.dtb) |
| rom     | 0x600000 | 7M     | `/dev/mtdblock3` | `rootfs.squashfs`(只读根文件系统,当前约 5.7MiB) |
| overlay | 0xD00000 | 3M     | `/dev/mtdblock4` | JFFS2 读写层(初始为全 0xFF 擦除态) |

> 注意:`genimage-nor.cfg` 中 rom 分区未写死大小(填满剩余空间),
> 而内核 DTS 里 rom 只到 0xD00000。**必须保证 `rootfs.squashfs` < 7MiB**,
> 否则会写穿到 overlay 分区。当前约 5.7MiB,余量约 1.3MiB。

## bootargs 说明

来源:`devicetree.dts` 的 `chosen/bootargs`:

```
console=ttyS0,115200 earlyprintk            # 串口控制台
rootwait                                    # 等待根设备就绪
init=/preinit                               # 先跑 preinit 而不是 busybox init
root=/dev/mtdblock3 rootfstype=squashfs     # 只读根文件系统 = rom 分区
overlayfsdev=/dev/mtdblock4                 # preinit 用它挂 overlay 分区
net.ifnames=0 vt.global_cursor_default=0    # 网络接口名固定;关内核光标
```

## preinit / overlay 机制

`board/allwinner/generic/rootfs/preinit`(rootfs 内的 `/preinit`)逻辑:

1. `mount proc /proc -t proc`
2. 解析 `/proc/cmdline`,取 `overlayfsdev=`
3. `mount -t jffs2 /dev/mtdblock4 /overlay`(首次启动为空 JFFS2,直接挂载)
4. 在 `/overlay/rom/{lower,upper,work}` 上组装 overlayfs:
   - `lower` = 只读 squashfs 根(`--rbind /`)
   - `upper/work` = JFFS2 分区
5. overlayfs 挂到 `/tmp`,`--rbind` 挂 `/dev` 和 `/overlay` 进去
6. `exec chroot /tmp /sbin/init`(BusyBox init → `/etc/init.d/rcS`)

**该脚本必须由 PID 1 以 root(euid 0)身份运行**,否则第 1 步 `mount` 就会失败,
后续解析 cmdline、挂 overlay 全部跳过,系统将运行在只读 squashfs 上无法持久化。
2026-10 曾因 rootfs 镜像属主错误导致此问题,详见故障复盘文档。

## U-Boot 关键环境变量(uboot.env 摘要)

| 变量 | 作用 |
| ---- | ---- |
| `bootcmd_spi` / `nor_boot` | 从 NOR 读 `kernel.itb` 至 `0x81000000` 并 `bootm` |
| `scan_boot_slot*`          | 探测 mmc0/mmc1/spi-nor/spi-nand,打印 Boot Slot |
| `show_splash`              | `bmp display` + `gpio set ${lcd_bl_pin}`(134 为背光) |
| `dfu_boot` / `nor_dfu_info`| 3 秒 DFU 窗口,`dfu 0 sf 0:0` 更新 NOR |
| `boot_targets`             | `fel dfu mmc0 mmc1 spi loopdfu` |

U-Boot 还负责把 LCD framebuffer 通过 `simple-framebuffer` 节点传给内核
(`sunxi_display.c` 的 simplefb 支持,pipeline 为 `de_be0-lcd0`),
对应设备树中的预填节点的说明见变更记录。

显示交接顺序:U-Boot 配置 TCON 并显示开机图 → 内核 simplefb 接住同一块
framebuffer(启动早期画面不变)→ `sun4i-drm` 就绪后接管显示(开机图被替换)。
DRM 能否注册取决于 panel 节点能否匹配到驱动(见
`../troubleshooting/drm-not-binding-simple-panel-compatible.md`);bootargs 中的
`console=tty0` 会让内核日志同时输出到 LCD。
