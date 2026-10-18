# 变更记录 2026-10-18:rootfs 属主修复 / 开机图 / simplefb / DRM 修复

- 日期:2026-10-18
- 主题:修复 NOR 启动时系统以非 root 运行的问题;恢复 U-Boot 开机图;内核接管
  U-Boot framebuffer(simplefb);修复 sun4i-drm 因 panel 无匹配驱动而不注册的问题
- 关联文档:
  - 故障复盘(rootfs 属主):`../troubleshooting/rootfs-uid1000-setuid-busybox.md`
  - 故障复盘(DRM 不注册):`../troubleshooting/drm-not-binding-simple-panel-compatible.md`
  - 构建与校验:`../build/build-and-verify.md`
  - 启动与分区:`../boot/boot-flow-and-flash-layout.md`

## 变更总览

| # | 文件 | 类型 | 说明 |
| - | ---- | ---- | ---- |
| 1 | `package/fakeroot/0007-libfakeroot.c-add-wrappers-for-new-glibc-2.33-symbol.patch` | 新增 | 补 glibc ≥ 2.33 的 `stat/lstat/fstat/fstatat` 拦截,修复镜像属主为 uid 1000 的问题 |
| 2 | `board/liulianpi/f1c100s/uboot.defconfig` | 修改 | 新增 `CONFIG_CMD_BMP=y`,提供 `bmp display` 命令 |
| 3 | `board/allwinner/generic/splash.bmp` | 修改 | 32bpp → 24bpp(像素完全一致),适配 U-Boot legacy video 解码限制 |
| 4 | `board/liulianpi/f1c100s/devicetree/linux/devicetree.dts` | 修改 | 新增 `framebuffer`(simple-framebuffer)预填节点 |
| 5 | `board/allwinner/suniv-f1c100s/patch/linux/0017-drm-panel-simple-add-generic-simple-panel-support.patch` | 新增 | panel-simple 驱动支持通用 `"simple-panel"`,使 sun4i-drm 能注册(见第 5 节) |
| 6 | `board/liulianpi/f1c100s/devicetree/linux/devicetree.dts` | 修改 | bootargs 增加 `console=tty0`,内核日志同时输出到 LCD(见第 6 节) |

---

## 1. fakeroot:新增 glibc 2.33+ stat wrapper 补丁

**动机**:宿主 glibc 2.35 下,打包工具(mksquashfs/tar/mke2fs)的 stat 系列调用
绕过 fakeroot 老式 `__xstat` 包装,rootfs 镜像属主被写成构建用户 uid 1000;
叠加 BusyBox 的 setuid 权限表后,系统启动即降权,preinit 无法挂载 proc/overlay。
完整分析见故障复盘文档。

**改动**:新增补丁,导出 `lstat/stat/fstat/fstatat`(+ `stat64` 系列,由
`STAT64_SUPPORT` 保护),插在 `libfakeroot.c` 的 `#endif /* HAVE_FSTATAT */`
之后;调用既有 `WRAP_LSTAT LSTAT_ARG(...)` 宏转发,`mknod/mknodat` 不重复定义
(已由本仓库既有的 `0006-add-mknod-mknodat-wrappers-for-glibc-2.33.patch` 提供)。
上游参照:Buildroot 2020.02.x commit `f45925a9513`、fakeroot 上游 `feda578`。

**影响范围**:所有 rootfs 格式(squashfs/tar/cpio/ext2)的属主恢复为 root;
`/bin/busybox` 变为 `-rwsr-xr-x root root`,setuid 语义恢复正常。

**验证**:

```console
$ make host-fakeroot-dirclean host-fakeroot
$ nm -D output/host/lib/libfakeroot.so | grep -E " T (lstat|stat|fstat|fstatat)$"
0000000000007a90 T lstat
0000000000007ab0 T stat
0000000000007ad0 T fstat
0000000000007ae0 T fstatat
$ output/host/bin/unsquashfs -ll output/images/rootfs.squashfs | grep busybox
-rwsr-xr-x root/root 777168 ... bin/busybox
```

**回滚**:删除 `0007-*.patch` 后 `make host-fakeroot-dirclean host-fakeroot`,
并重建 rootfs。注意回滚后必须同时关闭 BusyBox SUID(方案 C),否则启动问题复现。

---

## 2. U-Boot:启用 `bmp` 命令(开机图)

**动机**:`uboot.env` 的 `show_splash` 会执行 `bmp display ${splash_addr}`,
但 U-Boot 未开启 `CONFIG_CMD_BMP`,启动日志报 `Unknown command 'bmp'`,开机图不显示。

**改动**:`board/liulianpi/f1c100s/uboot.defconfig` 增加:

```
# Splash screen: boot script runs "bmp display", needs the bmp command
CONFIG_CMD_BMP=y
```

(BMP 16/24/32bpp 支持已由 U-Boot 的 `include/configs/suniv.h` 定义。)

**验证**:

```console
$ grep -c "There is no valid bmp file" output/images/u-boot-sunxi-with-spl.bin
1        # bmp 命令代码已编入
```

**回滚**:删除该行后 `make uboot-dirclean uboot`。

---

## 3. splash.bmp:32bpp → 24bpp

**动机**:U-Boot legacy video 路径 `drivers/video/cfb_console.c`
(`video_display_bitmap`)只支持 4/8/24bpp 的 BMP;原 `splash.bmp` 为 32bpp
(BITMAPV5HEADER,masks 为 R=0xff000000/G=0x00ff0000/B=0x0000ff00/A=0x000000ff),
即使开启 `CONFIG_CMD_BMP` 也会报 `Error: 32 bit/pixel bitmaps not supported`。

**改动**:将 `board/allwinner/generic/splash.bmp` 转换为标准 24bpp BI_RGB
(251x94,小端字节序 B,G,R,行自底向上),**像素数据与原始逐点一致**
(ImageMagick `compare -metric AE` = 0)。

**验证**:

```console
$ identify board/allwinner/generic/splash.bmp
... 24-bit ... 251x94
$ head -c 30 board/allwinner/generic/splash.bmp | od -A x -t x1z
000000 42 4d ... 28 00 00 00 fb 00 00 00 5e 00 00 00 01 00 18 00
#                    ^DIB=40  ^w=251      ^h=94       ^bpp=0x18(24)
$ compare -metric AE orig.png new.png null:
0
```

**回滚**:`git checkout board/allwinner/generic/splash.bmp`(需同时回滚变更 2,
否则开机图无法显示)。

---

## 4. 内核 DTS:simple-framebuffer 预填节点

**动机**:U-Boot 在 `bootm` 时会通过 `ft_board_setup` → `sunxi_simplefb_setup()`
(`drivers/video/sunxi/sunxi_display.c`,匹配 `allwinner,pipeline = "de_be0-lcd0"`)
把 LCD framebuffer 传给内核;内核 DTS 中缺少对应节点时会打印
`Cannot setup simplefb: node not found`,内核在 DRM 驱动加载前没有 framebuffer
(串口启动阶段屏幕无输出,开机图与内核显示之间会断开)。

**改动**:`board/liulianpi/f1c100s/devicetree/linux/devicetree.dts` 根节点下新增:

```dts
	/* Filled in by U-Boot (sunxi_display.c simplefb support) when the
	 * 1024x600 LCD console is set up, so the kernel gets a framebuffer
	 * before the sun4i DRM driver takes over */
	framebuffer {
		compatible = "allwinner,simple-framebuffer",
			     "simple-framebuffer";
		allwinner,pipeline = "de_be0-lcd0";
		status = "disabled";
	};
```

U-Boot 命中后会把 `reg/width/height/stride/format("x8r8g8b8")` 填好、
`status` 置 `okay`,并相应缩减 `/memory` 容量(约 1024*600*4 ≈ 2.4MiB);
内核 `CONFIG_FB_SIMPLE=y` 接管,待 `sun4i-drm`(tcon0)驱动加载后自动切换。

**验证**:编译后检查 DTB:

```console
$ output/host/bin/linux-dtc -I dtb -O dts output/images/devicetree.dtb | grep -A4 framebuffer
	framebuffer {
		compatible = "allwinner,simple-framebuffer\0simple-framebuffer";
		allwinner,pipeline = "de_be0-lcd0";
		status = "disabled";
	};
```

启动日志中不再出现 `Cannot setup simplefb: node not found`。

**回滚**:删除该节点后 `make linux-rebuild`。

---

## 5. 内核补丁:panel-simple 支持通用 `"simple-panel"`(修复 DRM 不注册)

**动机**:启动后 `/sys/class/drm/` 只有 `version`、dmesg 无任何 sun4i-drm 记录,
系统一直停留在 simplefb。根因是 DTS 的 panel 节点只写
`compatible = "simple-panel"`,而内核 5.4 的 panel-simple 驱动没有该匹配项
(只认具体面板型号;参考 LicheePi Nano 用的是
`"qiaodian,qd43003c0-40", "simple-panel"`),导致 panel 驱动永不绑定 →
`sun4i_tcon` 的 `drm_of_find_panel_or_bridge()` 永久 `-EPROBE_DEFER` →
DRM 组件链静默地永不完成。完整分析见
`../troubleshooting/drm-not-binding-simple-panel-compatible.md`。

**改动**:新增 `board/allwinner/suniv-f1c100s/patch/linux/0017-*.patch`:

- 在 panel-simple.c 中注册 `{ .compatible = "simple-panel", .data = &simple_panel_desc }`;
- `simple_panel_desc` 使用 `.timings` 时序边界(而非固定模式),使既有
  `panel_simple_parse_panel_timing_node()` 覆盖机制生效——DT 中的
  `panel-timing`(40MHz、150/150/20、15/15/5、HS/VS 高有效)经校验后作为
  `override_mode` 使用,与 U-Boot 当前正在用的时序完全一致;
- 边界 flags 必须与 DT 解析结果精确相等(DT 只有 `hsync-active`/`vsync-active`),
  故写成 `DISPLAY_FLAGS_HSYNC_HIGH | DISPLAY_FLAGS_VSYNC_HIGH`。

**影响**:sun4i-drm 正常注册(`/sys/class/drm/card0`、`fb0` 名称变为
`sun4i-drmdrmfb`);启动早期仍由 U-Boot+simplefb 显示,DRM 就绪后接管
(开机图会被替换,属预期行为)。若出现画面偏移/颜色异常,可再调
`bus_flags`/时序边界。

**验证**:

```console
$ patch --dry-run ... 0017-*.patch          # 干净应用
$ grep -c '"simple-panel"' output/build/linux-5.4.99/drivers/gpu/drm/panel/panel-simple.c
1
$ grep -c 0017 output/build/linux-5.4.99/.applied_patches_list
1
```

目标机(烧录后):`ls /sys/class/drm/` 出现 `card0`;
`dmesg | grep sun4i` 有绑定记录;`cat /sys/class/graphics/fb0/name` 为
`sun4i-drmdrmfb`。

**回滚**:删除 0017 补丁后 `make linux-dirclean linux`(补丁只在解包阶段应用)。

---

## 6. bootargs 增加 `console=tty0`(内核日志上屏)

**动机**:DRM 修复后 LCD 可以显示内核信息,但原 cmdline 只有 `console=ttyS0`。

**改动**:`devicetree.dts` 的 bootargs 改为:

```
console=tty0 console=ttyS0,115200 earlyprintk rootwait init=/preinit root=/dev/mtdblock3 rootfstype=squashfs overlayfsdev=/dev/mtdblock4
```

**顺序说明**(已核实 `kernel/printk/printk.c:2145`):所有 `console=` 都会收到
printk,但**最后一个** `console=` 作为 `/dev/console`。把 ttyS0 放在最后,
可保证 `/dev/console` 仍是串口,inittab 的串口 getty 不受影响;LCD(tty0)
从 fbcon 接管时起显示日志(fbcon 注册带 CON_PRINTBUFFER,会回放此前日志)。

**回滚**:去掉 `console=tty0` 后 `make linux-rebuild`。

---

## 构建验证记录

### 第一轮(2026-10-18 17:41,变更 1~4)

| 产物 | 大小 | 说明 |
| ---- | ---- | ---- |
| `rootfs.squashfs`   | 6,017,024 B | 属主全部 root,`/bin/busybox` 为 `-rwsr-xr-x root root` |
| `kernel.itb`        | 4,325,948 B | 含新 `devicetree.dtb`(14,615 B,带 framebuffer 节点) |
| `sysimage-nor.img`  | 16,777,216 B | 0x0=`eGON.BT0`;0x80000=`BM` 24bpp;0x100000=FIT;0x600000=`hsqs`;0xD00000=0xFF |

```console
$ md5sum output/images/rootfs.squashfs output/images/sysimage-nor.img output/images/kernel.itb
8830a62441036d44b78c2c8fe1bef2d5  output/images/rootfs.squashfs
996593e5be972076049d3aa899a1ebd2  output/images/sysimage-nor.img
9f00b845d46ebef26902a6567b1ab49e  output/images/kernel.itb
```

### 第二轮(2026-10-18 20:14,追加变更 5~6)

| 产物 | 大小 | 说明 |
| ---- | ---- | ---- |
| `rootfs.squashfs`   | 6,017,024 B | 内容同第一轮(重建后 md5 因压缩时间戳变化) |
| `devicetree.dtb`    | 14,631 B | bootargs 含 `console=tty0 console=ttyS0,115200` |
| `kernel.itb`        | 4,326,140 B | FIT 内 DTB 与 `devicetree.dtb` 逐字节一致(fdt hash `528b1704`) |
| `sysimage-nor.img`  | 16,777,216 B | 分区结构与第一轮一致;0x100000 处为 FIT 魔数 |

```console
$ md5sum output/images/rootfs.squashfs output/images/sysimage-nor.img output/images/kernel.itb output/images/devicetree.dtb
5ba2a53482b43c9443cde1d79d609c4d  output/images/rootfs.squashfs
f8ca67cba75d669773dd4fd40a808647  output/images/sysimage-nor.img
db9b508d3d3154a364b416285bf2918b  output/images/kernel.itb
dc73be718fb8dd84a16b3d3972f48c6b  output/images/devicetree.dtb
```

## 烧录与预期结果

```shell
./flashutils/linux/fel-nor-all.sh
```

重启后预期:

1. U-Boot 显示开机图(不再有 `Unknown command 'bmp'`),约 3 秒 DFU 窗口后启动内核;
2. 不再出现 `Cannot setup simplefb: node not found`;
3. `/preinit` 无权限错误:`/proc` 挂载成功、JFFS2 overlay 建立、`chroot` 进入
   可写系统(可用 `id` 验证为 `uid=0(root)`,可写 `/tmp`、`/var`);
4. 首次启动会在 overlay 分区(3MiB)上建立 JFFS2;
5. **DRM 生效**:`ls /sys/class/drm/` 有 `card0`;`dmesg | grep sun4i` 显示
   DRM 绑定;`cat /sys/class/graphics/fb0/name` 最终为 `sun4i-drmdrmfb`;
   LCD 上能看到内核日志(`console=tty0`),DRM 接管时 U-Boot 开机图被替换;
6. 串口登录(`/dev/console` = ttyS0)不受影响。
