# 变更记录 2026-11-04：cedrus 视频解码 bring-up

- 日期：2026-11-04
- 主题：从 `F1C100s_UartScreen` 工程移植 suniv cedrus V4L2 M2M 视频解码支持，
  并加入独立的 H.264/MP4 解码验证工具
- 关联工程：`/home/queqt/project/f1cx00s/F1C100s_UartScreen`
- 上游播放器：<https://github.com/rhodesepass/drm_app_neo>
  （`da86dacbb492e3df4c5e10d601cf140d8cf622a9`）

## 背景

当前 LiuLianPi 镜像的内核虽然编译了 Allwinner CedarX staging 驱动和 ION，
但设备树中的 VE 节点是 disabled，用户态也没有启用 `libcedarc`，因此默认
镜像不能播放 MP4。

`F1C100s_UartScreen` 与当前仓库同源（初始提交均为 `c1d5cdb`），
Buildroot 2020.02.7、U-Boot 2020.07、Linux 5.4.99 完全一致。该工程已经用
主线 cedrus V4L2 M2M 驱动在 F1C200s 上验证了 H.264/MPEG2 解码，并通过
DRM 直接显示 tiled NV12 帧。

本次先移植视频解码链路，音频和任意 MP4 兼容性后续分阶段处理。

## 变更内容

### 1. 内核补丁

从 `F1C100s_UartScreen` 移植并在本仓库重新编号：

| 本仓库 | 来源 | 作用 |
| --- | --- | --- |
| `0018-cedrus-suniv-f1c-support.patch` | `0013-cedrus-suniv-f1c-support.patch` | suniv VE variant、H.264/MPEG2 解码修正、tiled NV12 |
| `0019-cedrus-h264-per-buffer-mv-col.patch` | `0015-cedrus-h264-per-buffer-mv-col.patch` | 按帧分配 H.264 mv col buffer，降低 CMA 占用 |
| `0020-cedrus-suniv-new-die-engine-select-bias.patch` | `0019-cedrus-suniv-new-die-engine-select-bias.patch` | 兼容新批次 suniv die 的引擎选择编码 |
| `0021-cedrus-request-cancel-complete-once.patch` | `0035-cedrus-request-cancel-complete-once.patch` | 修复 request 取消时重复完成 control 的 WARN |

### 2. 设备树

- `board/liulianpi/f1c100s/devicetree/linux/devicetree.dts`
  - 启用 `&cedar_ve`。
  - 在板级节点中把 compatible 覆盖为
    `allwinner,suniv-f1c100s-video-engine`，使 cedrus 驱动可以匹配。
  - 不修改共享的 `suniv-f1c100s.dtsi`，避免影响其他仍使用
    `allwinner,suniv-f1c100s-cedar` + CedarX 的板卡。

### 3. 内核配置

`board/allwinner/suniv-f1c100s/linux.defconfig`：

```text
CONFIG_MEDIA_CONTROLLER_REQUEST_API=y
CONFIG_VIDEO_SUNXI_CEDRUS=y
```

当前配置已包含 `MEDIA_SUPPORT`、`MEDIA_CONTROLLER`、`VIDEO_DEV`、
`VIDEO_V4L2`、`VIDEOBUF2_DMA_CONTIG`、`DMA_CMA`（16MiB）。

### 4. 验证工具

新增 `package/vdec-selftest/`，源码来自上表上游工程的：

```text
tools/vdec_selftest.c
src/vdec/mp4_demux.c
src/vdec/bitreader.c
src/vdec/nalu.c
src/vdec/h264_parser.c
src/vdec/h264_dpb.c
src/vdec/vdec_v4l2.c
```

安装为 `/usr/bin/vdec_selftest`。该工具：

- 输入一个 MP4 文件路径
- 使用 cedrus V4L2 M2M 解码 H.264
- 通过 DRM plane 直接显示 tiled NV12
- 不依赖 LVGL、UI 或串口屏协议

构建配置：

```text
BR2_PACKAGE_LIBDRM=y
BR2_PACKAGE_LIBDRM_INSTALL_TESTS=y
BR2_PACKAGE_VDEC_SELFTEST=y
```

### 5. 宿主构建兼容

为在本机 GCC 15 / glibc 2.43 环境完成构建，补充：

- `package/gawk/0002-fix-readfunc-prototype.patch`
  - 修复 `host-gawk 5.0.1` 在 gnu23 模式下的旧式函数指针声明。
- `package/gawk/gawk.mk`
  - host 构建增加 `-std=gnu17`，避免 GCC 15 对 `gawkapi.h` 中
    `ssize_t (*read_func)()` 的 gnu23 语义变化。
- `package/gmp/gmp.mk`、`package/mpfr/mpfr.mk`、`package/mpc/mpc.mk`
  - host 构建增加 `-std=gnu17`，兼容 GCC 15 默认 gnu23 对 configure
    探测中旧式空参数列表的影响。
- `package/dtc/dtc.mk`
  - host 构建增加 `-Wno-error=discarded-qualifiers`，兼容 GCC 15 对
    `dtc 1.6.0` 的告警升级。
- `package/make/make.mk`
  - 修复 `host-make` 在缺少 `makeinfo` 时的安装失败；完整构建改用
    `host-make 4.2.1`，规避宿主 GNU Make 4.4 与 glibc 2.30 的
    `MAKE_RESTARTS` 循环。
- `package/fakeroot/`
  - 新增 `0008-libfakeroot-cast-fts-statp-to-stat64.patch`，适配 glibc
    2.43 下 FTS 的 `struct stat` / `struct stat64` 类型差异。
  - `package/fakeroot/fakeroot.mk` 的 host 构建参数加
    `-std=gnu17 -Wno-error=implicit-function-declaration`，修复 setgroups
    configure 探测。

## 验证方法

### 构建

```shell
make liulianpi_defconfig
make -j$(nproc)
```

### 目标机检查

```sh
ls /dev/video*
dmesg | grep -i cedrus
ls /dev/dri/
```

### SD 卡测试

```sh
mkdir -p /mnt/sd
mount /dev/mmcblk0p1 /mnt/sd

# 注意：U-Boot 会先扫描 SD 卡；作为数据盘时不要在 SD 上放 kernel.itb。

# 查看 DRM plane
modetest -M sun4i-drm

# 播放测试视频
vdec_selftest /mnt/sd/test.mp4

# 如果 video plane 不是 0，指定 plane 序号
vdec_selftest -p 1 /mnt/sd/test.mp4

# 如果 fbcon 控制台盖住了视频层，可临时解绑 fbcon：
echo 0 > /sys/class/vtconsole/vtcon1/bind
```

## 已知限制

- 本阶段只验证视频，不含音频。
- `mp4_demux` 是精简实现，要求 H.264 单 slice/帧；任意 MP4、AAC 音轨、
  fragmented MP4 等尚未支持。
- 当前 rootfs 分区只有 7MiB，媒体文件应放 SD 卡；播放器和后续音频库的
  体积需要单独评估。
- 尚未加入开机自动播放、SD 自动挂载和 UI。

## 回滚

1. 删除 `0018`~`0021` 四个内核补丁。
2. 删除 LiuLianPi DTS 中的 `&cedar_ve` 覆盖节点（恢复共享 dtsi 中的
   `allwinner,suniv-f1c100s-cedar`）。
3. 删除内核配置中的 `CONFIG_VIDEO_SUNXI_CEDRUS=y` 和
   `CONFIG_MEDIA_CONTROLLER_REQUEST_API=y`。
4. 删除 `package/vdec-selftest/`、`package/Config.in` 中的 source 行以及
   defconfig 中的相关选项。
5. 重新构建内核和 rootfs。
