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

### 2. 默认保留完整编码 source

H.264 的编码高度按宏块对齐，VE 输出的是 tiled NV12。之前实测把 source
高度从 608 裁到 600 会破坏 tiled chroma 布局，出现整屏偏红。因此默认：

```text
source = 完整编码尺寸（如 1024x608）
framebuffer = 完整编码尺寸
```

不用 source 裁剪。

### 3. 三种显示模式

默认模式：`-F`（coded fill）

```text
source  = 完整编码尺寸
content = 按显示尺寸算出的内容矩形
plane   = 内容矩形按 coding/display 比例放大
padding 超出屏幕的部分由 DEFE 裁掉
```

效果：

- 可见内容比例正确
- padding 不参与可见区域
- 黑边对称
- 1024x600 可以铺满 1024x600

安全回退：`-C`（coded shrink）

```text
plane 整体缩到屏幕内，padding 落进底部/右侧黑边
```

代价：1024x600 内容约 1003x586，4:3 内容会明显变小，但完全不依赖裁剪。

实验模式：`-V`（visible FB）

```text
按显示尺寸创建 FB，source 也裁到显示尺寸
```

这是最直接的方案，但对非 32 对齐高度（600、240）可能再次触发 tiled
chroma 异常，仅用于实机验证。

如果 `-V` 创建 FB 失败，会自动重试 `-C`。

### 4. 调试输出

启动时打印：

```text
video: 1280x736 display 1280x720 max_ref=... cap_bufs=...
display: plane ... screen 1024x600 fb 1280x736 src 1280x736 (coded-fill FB)
display: src 1280x736 -> content 1024x576+0+12, plane 1024x589 (coded-fill FB, screen 1024x600)
```

## 预期几何

默认 `-F`：

| 显示尺寸 | 编码尺寸 | 内容矩形 | 黑边 |
| --- | --- | --- | --- |
| 1024x600 | 1024x608 | 1024x600+0+0 | 无，底部 8 行 padding 被裁 |
| 1280x720 | 1280x736 | 1024x576+0+12 | 上下各 12 |
| 1920x1080 | 1920x1088 | 1024x576+0+12 | 上下各 12 |
| 320x240 | 320x256 | 800x600+112+0 | 左右各 112 |
| 640x480 | 640x480 | 800x600+112+0 | 左右各 112 |
| 1920x816 (2.35:1) | 1920x1088 | 1024x435+0+82 | 上下约 82 |

`-C` 安全模式：

| 显示尺寸 | 内容矩形 | 说明 |
| --- | --- | --- |
| 1024x600 | 1003x586+10+6 | 约 2% 缩小，黑边 10/11、6/8 |
| 1280x720 | 1024x575+0+12 | 黑边 12/13 |
| 320x240 | 709x530+157+34 | 4:3 内容缩小，黑边对称 |

## 测试

烧录后：

```sh
# 默认 coded fill
mp4play /mnt/sd/VID_20261102_223149_f1c100s.mp4

# 安全缩小
mp4play -C /mnt/sd/VID_20261102_223149_f1c100s.mp4

# 720p
mp4play /mnt/sd/VID_20261102_223149_720p.mp4

# 4:3 / 编码高度 256
mp4play /mnt/sd/test-bframes.mp4

# 实验 visible FB
mp4play -V /mnt/sd/VID_20261102_223149_f1c100s.mp4
```

重点检查：

1. 1024x600 是否铺满，没有左右黑边
2. 720p 上下黑边是否各 12px
3. 320x240 是否 800x600 居中，左右各 112px
4. 默认模式底部是否有 padding 残留/错位（验证 DEFE 是否裁剪超出屏幕的 plane）
5. `-V` 是否出现红屏/彩屏（预期非 32 对齐高度可能异常）

如果默认模式底部出现错位，改用 `-C` 作为默认；如果 `-V` 正常，可以再评估
是否用 visible FB。

## 已知限制

- 依赖 DEFE 裁剪超出屏幕的 plane；`-C` 不需要裁剪，作为回退
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
