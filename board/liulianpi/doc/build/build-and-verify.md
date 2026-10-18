# 构建、校验与烧录

## 宿主环境要求

- Linux(x86_64),已安装 Buildroot 所需依赖(见仓库根 `README.md`)。
- **宿主 glibc ≥ 2.33 时注意 fakeroot 兼容性**:本仓库使用的 fakeroot 1.20.2
  需要额外补丁才能在新 glibc 上正确伪造文件属主,否则 **所有 rootfs 镜像的属主都会
  变成构建用户(如 uid 1000)**,引发启动失败。详见
  `../troubleshooting/rootfs-uid1000-setuid-busybox.md`。
  构建完成后务必执行下面的“产物必查清单”确认属主为 root。

## 配置与构建

```shell
# 一次性应用板级 defconfig(会重置 buildroot 配置)
make liulianpi_defconfig

# 常规构建
make -j$(nproc)
```

修改板级配置后的增量重建方式:

| 改动 | 命令 |
| ---- | ---- |
| U-Boot defconfig(`uboot.defconfig`) | `make uboot-dirclean uboot` |
| 内核 DTS / 内核配置 | `make linux-rebuild`(DTS 在 build 阶段重新拷贝并编译) |
| **内核补丁**(`board/allwinner/suniv-f1c100s/patch/linux/`) | `make linux-dirclean linux`(补丁只在解包阶段应用,rebuild 不会重新打补丁) |
| rootfs 内容 / fakeroot / busybox | 删除镜像后 `make`:`rm -f output/images/rootfs.* output/images/sysimage-*.img && make` |

> rootfs 镜像文件被删除后才会重新生成;只改板级源文件不删镜像时,`make` 不会重建 rootfs。

## 构建产物(output/images)

| 文件 | 说明 |
| ---- | ---- |
| `u-boot-sunxi-with-spl.bin`      | SPL + U-Boot(烧在 0x0) |
| `splash.bmp`                     | 开机图(必须是 24bpp,见下) |
| `zImage` / `devicetree.dtb`      | 内核镜像与设备树 |
| `kernel.itb`                     | FIT 镜像(zImage + devicetree.dtb),由 post-image 脚本生成 |
| `rootfs.squashfs`                | 只读根文件系统(rom 分区) |
| `sysimage-nor.img`               | **NOR 整包镜像**(16MiB,实际烧写对象) |

`sysimage-nor.img` 布局自检:

```shell
for off in 0x0 0x80000 0x100000 0x600000 0xd00000; do
  printf "%s: " $off
  dd if=output/images/sysimage-nor.img bs=1 skip=$((off)) count=8 2>/dev/null | od -A n -t x1z
done
```

预期:

| 偏移 | 期望魔数/内容 |
| ---- | ------------- |
| 0x000000 | `16 00 00 ea 65 47 4f 4e`(`eGON.BT0`,sunxi SPL 头) |
| 0x080000 | `42 4d`(`BM`,BMP 文件头);`bit_count` 必须为 24(0x18) |
| 0x100000 | `d0 0d fe ed`(FDT/FIT 魔数) |
| 0x600000 | `68 73 71 73`(`hsqs`,squashfs 魔数) |
| 0xD00000 | 全 `ff`(overlay 分区擦除态,未烧过数据) |

## 产物必查清单(每次构建后)

```shell
# 1) 镜像内文件属主必须是 root/root
output/host/bin/unsquashfs -ll output/images/rootfs.squashfs | grep -E "bin/busybox|preinit"
#   期望:-rwsr-xr-x root/root ... bin/busybox
#         -rwxr-xr-x root/root ... preinit
tar tvf output/images/rootfs.tar | grep -E "\./bin/busybox"
#   期望:-rwsr-xr-x 0/0 ... ./bin/busybox

# 2) fakeroot 的 stat 拦截符号存在(glibc >= 2.33 宿主必需)
nm -D output/host/lib/libfakeroot.so | grep -E " T (lstat|stat|fstat|fstatat)$"
#   期望:4 个符号全部输出

# 3) U-Boot 开机图命令已编入
grep -c "There is no valid bmp file" output/images/u-boot-sunxi-with-spl.bin
#   期望:1

# 4) squashfs 大小必须 < 7MiB(rom 分区大小),否则会写穿 overlay
ls -l output/images/rootfs.squashfs

# 5) bootargs 含 console=tty0(FIT 内 DTB 与 devicetree.dtb 逐字节一致)
output/host/bin/linux-dtc -I dtb -O dts output/images/devicetree.dtb | grep bootargs

# 6) 内核补丁 0017 已应用(panel-simple 支持 "simple-panel")
grep -c 0017 output/build/linux-5.4.99/.applied_patches_list
grep -c '"simple-panel"' output/build/linux-5.4.99/drivers/gpu/drm/panel/panel-simple.c
#   期望:各为 1
```

烧录后运行时的显示/DRM 验收(目标机):

```sh
ls /sys/class/drm/                    # 应有 card0
cat /sys/class/graphics/fb0/name      # DRM 就绪后为 sun4i-drmdrmfb(启动早期为 simple)
dmesg | grep -iE "sun4i-drm|fb0"      # 有组件 bound 记录
fb-test -r                            # LCD 全屏红
```

## 烧录

### 方法一:FEL 全量烧写(推荐,NOR 无需可用 bootloader)

```shell
./flashutils/linux/fel-nor-all.sh
```

详细说明见 `flashutils/README.md` / `flashutils/README.zh.md`。

### 方法二:U-Boot DFU 窗口

上电后 U-Boot 会停留 3 秒(`DFU waiting on SPI-NOR...`),可用 `dfu-util`
更新各分区;分区别名定义在 `board/allwinner/generic/uboot.env` 的 `nor_dfu_info`。

## 已知构建环境坑

1. **`output/host/bin` 中手工放置的无 RPATH 二进制会打断构建**
   (例如手工放入的 `sunxi-fel`)。Buildroot 在任意 host 包安装后会执行
   `support/scripts/check-host-rpath`,扫描 `output/host/bin`、`sbin`,发现
   依赖 `output/host/lib` 中库却没有 RPATH 的 ELF 就报错:
   `ERROR: package ... installs executables without proper RPATH`。
   处理:临时移走该二进制,或为其补 RPATH:
   `patchelf --set-rpath '$ORIGIN/../lib' output/host/bin/sunxi-fel`。
   注意 `flashutils/linux/fel-nor-all.sh` 使用的是自带的
   `flashutils/linux/bin/sunxi-fel`,不依赖 `output/host/bin`。

2. **开机图必须为 24bpp BMP**。U-Boot legacy video 路径
   (`drivers/video/cfb_console.c` 的 `video_display_bitmap`)只支持
   4/8/24bpp;32bpp 会打印 `Error: 32 bit/pixel bitmaps not supported`。
   当前 `board/allwinner/generic/splash.bmp` 已转换为 24bpp(像素与原始一致)。
   转换脚本思路见变更记录。
