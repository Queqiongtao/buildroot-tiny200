# 变更记录 2026-11-14：mp4play 按原始显示尺寸计算几何

- 日期：2026-11-14
- 主题：按 MP4 容器显示尺寸显示，保持宽高比并让黑边对称
- 关联工具：`package/vdec-selftest/src/mp4play.c`

## 现象

播放器之前用解码后的**编码对齐尺寸**计算宽高比和黑边：

```text
容器显示尺寸        解码对齐尺寸
1024x600      ->    1024x608
1280x720      ->    1280x736
1920x1080     ->    1920x1088
320x240       ->    320x256
```

结果：

- 1024x600 视频按 1024x608 算，变成约 1010x600，左右各 7px 黑边
- 1280x720 / 1920x1080 按编码高度算，上下黑边不对称
- 4:3 视频内容偏小

## 根因

`mp4_demux` 已经从 `tkhd`/SampleEntry 解析出原始显示尺寸
`m->width` / `m->height`，但 `display_open()` 和 `display_show()` 只用了
`vdec.cap_width` / `vdec.cap_height`（即 32 对齐后的编码尺寸）。

编码尺寸包含底部/右侧的 macroblock padding，不能代表真实画面比例。

## 方案

### 1. 几何用容器显示尺寸

`device_play()` 计算：

```c
disp_w = m->width  ? m->width  : width;
disp_h = m->height ? m->height : height;
```

并限制不超过编码尺寸。`display_show()` 用 `disp_w/disp_h` 计算：

- 内容目标矩形 `dst_w x dst_h`
- 居中坐标 `dst_x/dst_y`
- 左右/上下黑边

### 2. 默认使用 visible FB

之前出现过“整屏偏红”，组合是：

```text
framebuffer 高度 = 608（完整编码高度）
source 高度      = 600（裁到面板高度）
```

这时 frontend 按 600 行读一个按 608 行组织的 tiled chroma 平面，颜色错位。

本次改为按显示尺寸创建 FB 后，FB 和 source 的高度一致：

```text
source      = 显示尺寸（如 1024x600）
framebuffer = 显示尺寸（如 1024x600）
```

DRM tiled NV12 的 chroma 高度按 300 计算，和 source 匹配。实机验证
chroma 正常，没有红屏。因此默认使用 visible FB。

### 3. 显示模式

默认：`-V`（visible FB）

```text
source  = 容器显示尺寸
content = 容器显示尺寸按比例适配屏幕
plane   = content 矩形
```

效果：

- 1024x600 铺满 1024x600
- 720p 显示 1024x576，上下各 12px
- 4:3 显示 800x600，左右各 112px
- 不依赖 DEFE 裁剪 padding

安全回退：`-C`（coded shrink）

```text
source = 完整编码尺寸
plane 整体缩到屏幕内，padding 落进底部/右侧黑边
```

代价：1024x600 内容约 1003x586，4:3 内容会明显变小，但完全不依赖裁剪。

回退对比：`-F`（coded fill）

```text
source  = 完整编码尺寸
plane   = content 矩形按 coding/display 比例放大
padding 超出屏幕的部分由 DEFE 裁掉
```

visible FB 创建失败时（例如 AddFB2 拒绝非 32 对齐高度），自动重试
coded fill。

### 4. 调试输出

启动时打印：

```text
video: 1280x736 display 1280x720 max_ref=... cap_bufs=...
display: plane ... screen 1024x600 fb 1280x720 src 1280x720 (visible FB)
display: src 1280x720 -> content 1024x576+0+12, plane 1024x576 (visible FB, screen 1024x600)
```

## 预期几何

默认 `-V`：

| 显示尺寸 | 编码尺寸 | 内容矩形 | 黑边 |
| --- | --- | --- | --- |
| 1024x600 | 1024x608 | 1024x600+0+0 | 无 |
| 1280x720 | 1280x736 | 1024x576+0+12 | 上下各 12 |
| 1920x1080 | 1920x1088 | 1024x576+0+12 | 上下各 12 |
| 854x480 | 864x480 | 1024x576+0+12 | 上下各 12 |
| 320x240 | 320x256 | 800x600+112+0 | 左右各 112 |
| 640x480 | 640x480 | 800x600+112+0 | 左右各 112 |
| 1920x816 (2.35:1) | 1920x832 | 1024x435+0+82 | 上下约 82 |

目标尺寸按四舍五入取整。例如 854x480：

```text
480 * 1024 / 854 = 575.55 -> 576
```

因此上下黑边各 12px，而不是 12/13px。

`-C` 安全模式：

| 显示尺寸 | 内容矩形 | 说明 |
| --- | --- | --- |
| 1024x600 | 1003x586+10+6 | 约 2% 缩小，黑边 10/11、6/8 |
| 1280x720 | 1024x575+0+12 | 黑边 12/13 |
| 320x240 | 709x530+157+34 | 4:3 内容缩小，黑边对称 |

## 测试

烧录后：

```sh
# 默认 visible FB
mp4play /mnt/sd/geometry-tests/test-1024x600.mp4
mp4play /mnt/sd/geometry-tests/test-1280x720.mp4
mp4play /mnt/sd/geometry-tests/test-320x240.mp4

# 宽度 padding：854 显示宽，864 编码宽
mp4play /mnt/sd/geometry-tests/test-854x480.mp4

# 安全缩小对比
mp4play -C /mnt/sd/geometry-tests/test-1024x600.mp4

# coded fill 对比
mp4play -F /mnt/sd/geometry-tests/test-1024x600.mp4
```

重点检查：

1. 1024x600 是否铺满，没有左右黑边
2. 720p 上下黑边是否各 12px
3. 320x240 是否 800x600 居中，左右各 112px
4. 854x480 右侧编码 padding 是否被裁掉，内容是否为 1024x576+0+12
5. `-C` 是否仍能作为安全回退

## 实机验证

2026-11-15，1024x600 面板：

- 默认 visible FB 显示正常
- `test-1024x600.mp4` 铺满，红框四边贴边
- `test-1280x720.mp4` 上下各 12px，无左右黑边
- `test-320x240.mp4` 800x600 居中，左右各 112px
- visible FB 的 tiled NV12 chroma 正常，没有红屏
- coded fill / coded shrink 模式也正常
- `test-1024x600-embedded-16x9.mp4` 的上下 12px 黑边来自源文件
- 不需要修改 DISSIZE 的内核补丁

## 已知限制

- `-F` 回退依赖 DEFE 裁剪超出屏幕的 plane；`-C` 不需要裁剪
- 不处理旋转元数据；转码脚本会把旋转烘进画面
- 1080p 仍不是实时支持格式

## 源文件黑边与测试片

从 1080p（16:9）转成 1024x600 时，默认转码策略保持比例，输出画面实际是
1024x576，上下各有 12px 黑边。这是源文件自带的黑边，不是播放器额外加的；
播放器仍然把整个 1024x600 画布铺满屏幕。

如果希望内容铺满 1024x600，需要裁切：

```sh
tools/convert-for-f1c100s.sh --fill input.mp4
```

显示几何测试片生成脚本：

```text
tools/generate-geometry-tests.sh
```

测试片特征：

- 红色边框在文件显示矩形的最外圈
- 绿色边框向内缩 8px
- 网格、CENTER、L/R、BOTTOM 标记
- 6 秒 H.264 Baseline + AAC-LC

建议用 `test-1024x600.mp4`、`test-1024x600-embedded-16x9.mp4`、
`test-1280x720.mp4`、`test-320x240.mp4` 区分：

- 播放器几何错误
- DEFE 裁剪问题
- 源文件自带黑边

## 构建

只改 `mp4play`，内核不变：

```sh
output/host/bin/host-make vdec-selftest-rebuild
rm -f output/images/rootfs.* output/images/sysimage-*.img
output/host/bin/host-make -j12
```
