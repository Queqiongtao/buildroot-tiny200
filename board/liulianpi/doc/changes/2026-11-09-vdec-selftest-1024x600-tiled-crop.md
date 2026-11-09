# 变更记录 2026-11-09：vdec_selftest 1024×600 tiled 裁剪修复

- 日期：2026-11-09
- 主题：修复 1024×600 H.264 测试视频运行期间整屏偏红的问题
- 关联工具：`package/vdec-selftest/`

## 现象

目标机执行：

```sh
vdec_selftest /mnt/sd/test-1024x600.mp4
```

工具日志显示解码和提交都成功：

```text
h264: 1024x608 max_ref=1 cap_bufs=6 frame_dur=33333us
capture: 1024x608 bpl=1024 sizeimage=950272 uv_off=622592
display: plane 31 crtc 47 screen 1024x600
decoded 90, displayed 90 frames
```

但 LCD 上看到的是红色画面，工具退出后恢复 fbcon 日志。

## 根因

H.264 的编码高度按宏块对齐：

```text
视频高度 600 -> 编码高度 608
```

VE 输出的是 tiled NV12，chroma 平面按 32 行 tile 组织。
`vdec_selftest` 原来的 `display_show()` 把 source 高度从 608 裁成面板高度 600：

```c
if (d->screen_h && src_h > d->screen_h)
    src_h = d->screen_h;
```

这样 frontend 按 600 行去读一个按 608 行组织的 tiled chroma 平面，颜色数据错位，
表现为整屏偏红。

旁支的 800×480 素材高度是 32 的整数倍，不会触发这个问题。

## 修复

`package/vdec-selftest/src/vdec_selftest.c` 的 `display_show()` 改为：

- source 保持完整编码尺寸（1024×608）
- destination 使用面板尺寸（1024×600）
- 由 frontend 做 608→600 的缩放

关键代码：

```c
unsigned int src_w = v->cap_width, src_h = v->cap_height;
unsigned int dst_w = src_w, dst_h = src_h;

if (d->screen_w && dst_w > d->screen_w)
    dst_w = d->screen_w;
if (d->screen_h && dst_h > d->screen_h)
    dst_h = d->screen_h;

return drmModeSetPlane(d->fd, d->plane_id, d->crtc_id, d->fb_ids[slot],
                       0, 0, 0, dst_w, dst_h, 0, 0,
                       src_w << 16, src_h << 16);
```

800×480 素材的 source/destination 都是 800×480，不会触发缩放。

## 验证

重新构建：

```shell
output/host/bin/host-make vdec-selftest-dirclean vdec-selftest
rm -f output/images/rootfs.* output/images/sysimage-*.img
output/host/bin/host-make -j12
```

目标机重新烧录后执行：

```sh
vdec_selftest /mnt/sd/test-1024x600.mp4
```

预期：

- 运行期间显示彩条测试画面
- 不再整屏偏红
- 工具退出后恢复 fbcon 日志（正常行为）

如果仍然红屏，再检查 DRM plane 选择（`-p 1`/`-p 2`）和 fbcon 占用。

## 回滚

恢复 `display_show()` 中原来的 source 裁剪逻辑，重新构建
`vdec-selftest` 和 rootfs 即可。
