# 变更记录 2026-11-10：mp4play 播放器与 AAC 音频

- 日期：2026-11-10
- 主题：在已验证的 cedrus 视频链路上增加 AAC 音频与完整 MP4 播放器
- 关联包：`package/vdec-selftest`（同时安装 `vdec_selftest` 和 `mp4play`）

## 背景

`vdec_selftest` 已验证 H.264 解码、tiled NV12、DRM plane 和 suniv
DEFE/frontend 全部正常，但它是视频-only 的 bring-up 工具：播完文件就退出，
没有音轨，也没有 A/V 同步。

本阶段目标：

1. 确认 F1C100s 板载 codec 能出声；
2. 提供一个可持续播放 SD 卡 MP4 的命令行播放器；
3. 支持 MP4 中常见的 AAC 音轨，并以音频为主时钟做 A/V 同步。

## 音频硬件与内核

内核已经包含：

```text
CONFIG_SND_SUN4I_CODEC=y
sun4i-codec 1c23c00.codec: Codec <-> 1c23c00.codec mapping ok
ALSA device list:
  #0: F1C100s Audio Codec
```

设备树 `&codec` 已把 `Headphone` 路由到 `HP`/`HPCOM`。用户态已经有
`tinyalsa`（`tinyplay` / `tinymix` / `tinypcminfo`），因此本阶段不需要再引入
`alsa-utils`，避免额外占用 rootfs。

## 变更内容

### 1. 视频样本时间戳

`src/vdec/mp4_demux.c` 原来只取 `stts` 第一项的帧间隔（CFR 假设），现在：

- 展开完整 `stts` 表，得到每个样本的 DTS；
- 解析 `ctts`（version 0 无符号 / version 1 有符号），得到 PTS；
- 换算为相对于轨道起点的微秒值 `mp4_sample.pts_us`。

同时 `parse_trak` 先解析 `mdhd` timescale，再构建样本表。

### 2. AAC 音轨解析

新增 `src/vdec/mp4_audio_demux.[ch]`：

- 找第一条 `soun` 轨；
- 解析 `mp4a` AudioSampleEntry（声道数、采样率）；
- 解析 `esds` 描述符链，提取 `AudioSpecificConfig`；
- 展开 `stsz` / `stsc` / `stco(co64)`，得到每个 AAC 样本的文件偏移和长度；
- 样本读取使用 `pread()`，SD 卡拔出时返回错误而不是 SIGBUS。

### 3. mp4play

新增 `src/mp4play.c`：

- 视频：复用 `vdec_selftest` 的 cedrus + DPB + DRM 路径；
- 音频：faad2 解码 AAC -> tinyalsa 写 PCM；
- 音频线程在首帧上屏时才真正开始写，避免声音比画面提前一个解码预热时间；
- 以音频播放位置为主时钟，视频按 `pts_us` 等待；音频缺失/异常时回退到
  单调时钟；
- 自动设置 suniv codec mixer：
  - `Headphone Source Playback Route = DAC`
  - `Headphone Playback Switch = on`
  - `Headphone Playback Volume = 75%`
  - `DAC Playback Volume = 80%`
- 画面保持宽高比居中，剩余区域由 CRTC 黑底填充；
- 支持 `-m`（静音/只播视频）、`-l`（循环）、`-p <plane>`。

### 4. 音频自测脚本

新增 `board/allwinner/suniv-f1c100s/rootfs/usr/bin/audio-test`，用于不经过
播放器、直接用 `tinyplay` 验证 codec 和喇叭/耳机：

```sh
audio-test                       # 播放 rootfs 内置的 1.2 秒提示音
audio-test /mnt/sd/test-tone.wav # 播放 SD 卡上的 5 秒立体声测试音
```

不传参数时优先使用 `/mnt/sd/test-tone.wav`，没有则回退到
`/usr/share/audio/test-tone.wav`（8kHz 立体声，左 440Hz / 右 880Hz）。

脚本会打印设置前后的 mixer 控件、PCM 能力，然后播放 WAV。

## 构建

```sh
output/host/bin/host-make olddefconfig
output/host/bin/host-make -j12
```

`mp4play` 依赖：

```text
libdrm tinyalsa faad2
```

`faad2` 由 `BR2_PACKAGE_VDEC_SELFTEST` 自动 select，无需手工改 defconfig。

## 测试素材

主机侧放在 `/mnt/VMShare/VMShare/`：

| 文件 | 内容 |
| --- | --- |
| `test-tone.wav` | 5 秒，44.1kHz 立体声，左 440Hz / 右 880Hz |
| `test-sweep.wav` | 5 秒，200Hz -> 2kHz 扫频 |
| `test-48k.wav` | 3 秒，48kHz 1kHz |
| `test-av.mp4` | 1024x600 H.264 + AAC-LC 44.1kHz 立体声，3 秒 |
| `test-av-48k.mp4` | 同上，AAC 48kHz，用于验证 codec 采样率切换 |
| `test-bframes.mp4` | 320x240 H.264 High + B 帧 + AAC，用于验证 PTS/重排 |

把需要的文件拷到 SD 卡根目录，目标机：

```sh
mkdir -p /mnt/sd
mount -t vfat /dev/mmcblk0p1 /mnt/sd
```

注意是 `/dev/mmcblk0p1`，不是 `/dev/mmcblk0`。

## 目标机验证

### 1. 先验证 WAV

不需要 SD 卡，先播 rootfs 内置提示音：

```sh
audio-test
```

然后播 SD 卡上的完整测试音：

```sh
audio-test /mnt/sd/test-tone.wav
```

预期：左耳 440Hz、右耳 880Hz，持续 5 秒。

如果无声：

```sh
tinymix
tinypcminfo -D 0 -d 0
dmesg | tail -50
```

重点看 `Headphone Playback Switch`、`Headphone Source Playback Route` 和
`Headphone Playback Volume`。

### 2. 播放带音频的 MP4

```sh
mp4play /mnt/sd/test-av.mp4
```

预期：

- LCD 显示彩条；
- 同时听到 1kHz 声音；
- 播放结束回到控制台。

只播视频：

```sh
mp4play -m /mnt/sd/test-av.mp4
```

循环：

```sh
mp4play -l /mnt/sd/test-av.mp4
```

### 3. B 帧/重排测试

```sh
mp4play /mnt/sd/test-bframes.mp4
```

该文件是 H.264 High + 2 个 B 帧，用来验证 `ctts` PTS 和 DPB 显示顺序。

## 已知限制

- 视频仅支持 H.264；`hvc1/hev1` 会报 `not an H.264 MP4`。
- 音频仅支持 AAC（`mp4a` + `esds`）；MP3/AC-3 等音轨会被忽略，播放器退回
  视频-only。
- 暂不解析 `elst` 编辑列表：
  - 视频以“最早 PTS”为 0 归一化，恒定偏移会抵消；
  - AAC 编码器 priming 可能造成约 10~30ms 的音画偏移。
- 不支持 seek；`-l` 通过重新打开文件实现，循环点会有短暂黑屏/间隔。
- AAC 采样率必须在 codec 支持的 8~48kHz 范围内，暂不做重采样。
- 音频解码在独立线程中，极端高码率文件可能 underrun。

## 后续

- 解析 `elst`，精确对齐 AAC priming；
- SD 卡自动挂载、扫描 MP4、开机自启；
- 播放列表/循环衔接优化；
- 根据真实样本决定是否扩展 demux（fragmented MP4、多 slice 等）。
