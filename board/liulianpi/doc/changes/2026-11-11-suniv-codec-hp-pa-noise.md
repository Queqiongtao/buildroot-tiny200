# 变更记录 2026-11-11：suniv codec 底噪、HP PA 控制与播放器修复

- 日期：2026-11-11
- 主题：定位并改善 F1C100s 耳机输出底噪，修复 mp4play 显示和 audio-test mixer 语法
- 关联补丁：`0024-suniv-codec-hp-pa-and-bias.patch`

## 现象

- `test-speech.wav`、`test-music.wav` 可以清楚听到，说明解码、DMA、16-bit 数据、
  DAC 数字通路都正常。
- 播放 `test-silence.wav` 时耳机里有明显的“沙沙 + 吱吱”底噪。
- 测试结果：
  - 耳机音量 20/40/63 变化，底噪几乎不变
  - `Headphone Playback Switch = 0 0`（HP PA 输入静音）底噪仍在
  - `DAC Playback Volume = 0`（DAC 数字静音）底噪仍在
  - 把 `HPPAEN`（`DAC_MIXER_CTRL` bit15）清零后底噪消失
  - 不播放任何音频时没有底噪

结论：噪声来自 **suniv codec 的耳机 PA 输出级**，不是 DAC 数据，也不是板级之后。
空闲时 DAPM 会断电，所以只有 PCM 活跃（包括播放数字静音）时才有底噪。

## 寄存器定位

F1C600 手册写的模拟 tuning 寄存器是 `0x28/0x2C/0x34`，但这颗 F1C100s/F1C200s
实际布局是：

| 地址 | 值 | 含义 |
| --- | --- | --- |
| `0x2C` | `0x44555556` | `ADDA_TUNE`，boot ROM 已设置 |
| `0x30` | `0x00000000` | `BIAS_DA16_CAL_CTRL0`，**缺少校准触发** |
| `0x34` | `0x10000000` | `BIAS_DA16_CAL_CTRL1`，boot ROM 已设置 |

即真正缺的是：

```text
0x30 = 0x00000004
```

上电时没有触发 BIAS/DA16 校准，耳放输出级底噪偏高。

## 变更内容

### 1. 内核 `0024` 补丁

- 新增 suniv tuning 寄存器定义（`0x2C/0x30/0x34`）。
- `suniv_codec_init()` 在 probe/reset 之后写入：
  - `0x2C = 0x44555556`
  - `0x30 = 0x00000004`（触发校准）
  - `0x34 = 0x10000000`
- 新增用户态控件：

```text
Headphone PA Enable
```

直接控制 `HPPAEN`（`DAC_MIXER_CTRL` bit15），用于静音段关闭耳放。

### 2. `mp4play`

- 修复 `1024x608` 视频在 `1024x600` 屏幕上的宽高比适配：
  - 旧逻辑按宽度适配得到 `dst_h=608`，`drmModeSetPlane` 每帧失败
  - 新逻辑选择较小的缩放比例，得到约 `1010x600`
- `drmModeSetPlane` 失败时打印一次 errno，不再每帧刷屏。
- 音频线程增加数字静音检测：
  - 峰值 ≤ 16（约 -66 dBFS）持续 500ms
  - 自动 `Headphone PA Enable = 0`
  - 有声音立即恢复 `Headphone PA Enable = 1`
  - 只对真正静音段生效，不gate正常音乐

### 3. `audio-test`

tinyalsa 1.1.1 的 `tinymix` 使用子命令语法。脚本改为：

```sh
tinymix contents
tinymix get  NAME
tinymix set  NAME VALUE...
```

默认音源顺序改为：

```text
/mnt/sd/test-music.wav
/mnt/sd/test-speech.wav
/mnt/sd/test-tone.wav
/usr/share/audio/test-tone.wav
```

并在播放结束后恢复 `Headphone PA Enable = 1`。

## 验证

重新构建并烧录：

```sh
output/host/bin/host-make linux-dirclean
output/host/bin/host-make -j12
```

目标机：

```sh
mount -t vfat /dev/mmcblk0p1 /mnt/sd

# 1) 确认新控件存在
tinymix contents | grep -E 'Headphone|DAC'

# 2) 播放音乐，确认正常
tinyplay -D 0 -d 0 /mnt/sd/test-music.wav

# 3) 静音段关闭 PA
tinyplay -D 0 -d 0 /mnt/sd/test-silence.wav &
sleep 0.5
tinymix set "Headphone PA Enable" 0
# 底噪应消失

# 4) 视频应正常显示（1010x600 居中）
mp4play /mnt/sd/test-av.mp4
```

预期：

- `mp4play` 不再打印 `display failed`
- 数字静音段自动关闭 HP PA，底噪消失
- 音乐/语音段正常

## 已知限制

- `Headphone PA Enable` 是普通 kcontrol，DAPM 在 PCM 启动时仍会把
  `HPPAEN` 置 1；播放器在流启动后按静音检测重新控制。
- 如果校准后音乐播放时仍有明显“吱吱”声，基本是 Class D 输出级载波或
  板级滤波问题。软件只能静音/关耳放，根治需要 LC/磁珠/耳机输出滤波。
- 静音检测阈值 16/保持 500ms 是保守值，可按实际素材调整。

## 回滚

删除 `0024-suniv-codec-hp-pa-and-bias.patch`，重新构建内核：

```sh
output/host/bin/host-make linux-dirclean
output/host/bin/host-make -j12
```
