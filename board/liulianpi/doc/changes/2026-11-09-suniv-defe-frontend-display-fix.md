# 变更记录 2026-11-09：suniv DEFE/frontend 显示修复

- 日期：2026-11-09
- 主题：修复 `vdec_selftest` 解码成功但 LCD 整屏偏红的问题
- 关联工程：`F1C100s_UartScreen`
- 关联补丁：
  - `0022-f1c100s-defe-debe-fix.patch`
  - `0023-sun4i-frontend-suniv-defe-mod-clock.patch`

## 现象

`vdec_selftest` 日志显示：

```text
decoded 90, displayed 90 frames
```

但 LCD 上看到的是红色画面，工具退出后恢复 fbcon 日志。

为了区分解码和显示，目标机执行：

```sh
vdec_selftest -n 1 -w 1 /mnt/sd/test-1024x600.mp4
```

把 `/tmp/vdec_frame_0.raw` 拷到主机后按 tiled NV12 解析，得到的是**标准彩条测试图**：

- 黑、红、青、品红、蓝、黄、绿、白彩条
- 底部彩条渐变
- 右侧测试方块

因此 VE 解码、H.264 parser、tiled NV12 输出都是正确的，问题在 DRM/frontend 显示环节。

## 根因

当前内核是 Linux 5.4.99 的 mainline sun4i DRM，缺少旁支工程针对
suniv/F1C100s 的 DEFE frontend 修复：

1. **frontend scaler coefficient RAM 访问握手缺失**
   - mainline 在写 scaler 系数前没有等待 `COEF_ACCESS_STATUS`；
   - 在 F1C100s 上会与 scaler 竞争，输出雪花/颜色异常。
   - 旁支补丁同时补充了 F1C100s 所需的 polyphase FIR 系数表。

2. **suniv DEFE mod clock 只有 300MHz**
   - 旁支实测 suniv DEFE 大约需要 6 个 mod-clk 周期/NV12 像素；
   - 300MHz 下宽画面的 frontend 行输出可能跟不上扫描线；
   - 旁支改为 384MHz（pll-video/1），增加宽度余量。

3. DEBE packed-YUV 地址寄存器在 F1C100s 上按 bit 地址处理
   - 旁支补丁同时修正了 `SUN4I_BACKEND_IYUVADD_REG` 的 `paddr << 3`。

## 变更内容

从 `F1C100s_UartScreen` 移植并重新编号：

| 本仓库 | 来源 | 作用 |
| --- | --- | --- |
| `0022-f1c100s-defe-debe-fix.patch` | `0003-f1c100s-defe-debe-fix.patch` | frontend scaler 系数/握手、F1C100s 系数表、DEBE packed-YUV 地址修正 |
| `0023-sun4i-frontend-suniv-defe-mod-clock.patch` | `0024-sun4i-frontend-suniv-defe-mod-clock.patch` | suniv frontend mod clock 提高到 384MHz |

补丁应用顺序为 0022 → 0023，均已通过 `linux-patch` 验证。

## 构建验证

```shell
output/host/bin/host-make linux-dirclean
output/host/bin/host-make -j12
```

构建成功，产物：

| 产物 | 大小 |
| --- | --- |
| `kernel.itb` | 4,341,592 B |
| `rootfs.squashfs` | 6,119,424 B（5.84 MiB） |
| `sysimage-nor.img` | 16,777,216 B |

内核中已确认：

```text
sun4i_frontend.o
sun4i_frontend_coef.h
.mod_rate = 384000000   (suniv_f1c100s_frontend)
```

## 目标机验证

重新烧录后：

```sh
mount -t vfat /dev/mmcblk0p1 /mnt/sd
vdec_selftest /mnt/sd/test-1024x600.mp4
```

预期：

- LCD 显示彩条测试画面，不再整屏偏红
- 工具退出后恢复 fbcon 日志（正常行为）

如果仍然红屏，下一步检查：

```sh
modetest -M sun4i-drm
vdec_selftest -p 1 /mnt/sd/test-1024x600.mp4
dmesg | tail -80
```

并考虑继续移植旁支的 `0014-sun4i-backend-suniv-quirks.patch`。

## 回滚

删除 `0022`、`0023` 两个补丁，重新执行：

```shell
output/host/bin/host-make linux-dirclean
output/host/bin/host-make -j12
```
