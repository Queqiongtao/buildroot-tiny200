# 变更记录 2026-11-14：SD 卡自动挂载

- 日期：2026-11-14
- 主题：插入/开机自动挂载 SD 卡到 `/mnt/sd`，支持热插拔和 UTF-8 中文文件名
- 挂载策略：读写、`noatime`、`iocharset=utf8`

## 目标

- 开机时 SD 卡已插入：自动挂载
- 开机后再插卡：自动挂载
- 拔卡：自动卸载
- 重新插卡：自动重新挂载
- 支持中文文件名
- 不做自动播放，播放仍由用户执行 `mp4play`

## 实现

### 1. udev 规则

新增：

```text
board/allwinner/suniv-f1c100s/rootfs/etc/udev/rules.d/99-sdcard.rules
```

```udev
KERNEL=="mmcblk0p1", SUBSYSTEM=="block", ACTION=="add", RUN+="/usr/bin/sdcard-mount"
KERNEL=="mmcblk0p1", SUBSYSTEM=="block", ACTION=="remove", RUN+="/usr/bin/sdcard-umount"
```

`S10udev` 启动时会执行 `udevadm trigger --action=add`，所以开机时已经存在的
`mmcblk0p1` 也会触发挂载。

### 2. 挂载 helper

新增：

```text
board/allwinner/suniv-f1c100s/rootfs/usr/bin/sdcard-mount
```

行为：

1. `mkdir -p /mnt/sd`
2. 检查 `/dev/mmcblk0p1` 是否存在
3. 已从同一设备挂载则跳过
4. 如果有旧挂载，先 `umount -l`
5. 优先：

```sh
mount -t vfat -o rw,noatime,iocharset=utf8 /dev/mmcblk0p1 /mnt/sd
```

6. 内核没有 `CONFIG_NLS_UTF8` 时回退到 `rw,noatime`
7. 卡被写保护时回退到 `ro,noatime,iocharset=utf8`
8. 通过 `logger -t sdcard` 记录结果

### 3. 卸载 helper

新增：

```text
board/allwinner/suniv-f1c100s/rootfs/usr/bin/sdcard-umount
```

行为：

1. 先尝试正常 `umount /mnt/sd`，flush FAT 数据
2. 如果播放器还占用文件，正常 umount 失败
3. `sync` 后 `umount -l /mnt/sd`，lazy detach，允许拔卡
4. 记录日志

### 4. 启动/关机兜底

新增：

```text
board/allwinner/suniv-f1c100s/rootfs/etc/init.d/S99sdcard
```

- `start`：调用 `sdcard-mount`（udev 已经挂载则跳过）
- `stop`：调用 `sdcard-umount`
- `restart`：先卸载再挂载

### 5. UTF-8 支持

`board/allwinner/suniv-f1c100s/linux.defconfig`：

```text
CONFIG_NLS_UTF8=y
```

vfat 使用 `iocharset=utf8` 后，中文文件名可以正常显示和打开。

## 读写挂载注意事项

当前是读写挂载：

```text
rw,noatime,iocharset=utf8
```

- 播放器只读文件，正常播放不会写卡
- 如果从板子往 SD 卡写文件，拔卡前先：

```sh
sync
sdcard-umount
```

- 直接拔卡且正在写文件，FAT 可能脏
- 下次插入如果挂载异常，可以在主机或目标机上跑：

```sh
fsck.fat -a /dev/mmcblk0p1
```

## 构建

因为加了 `CONFIG_NLS_UTF8=y`，需要重新构建内核：

```sh
output/host/bin/host-make linux-dirclean
output/host/bin/host-make -j12
```

rootfs 只增加几个脚本，大小影响很小。

## 验证

### 开机时卡已插入

```sh
mount | grep /mnt/sd
ls /mnt/sd
```

### 中文文件名

```sh
ls /mnt/sd
```

应能看到中文名，而不是乱码。

### 开机后插卡

```sh
# 先拔卡，再插入
mount | grep /mnt/sd
```

### 拔卡

```sh
# 播放中直接拔卡
mount | grep /mnt/sd
```

预期：`/mnt/sd` 被 lazy unmount，播放器读文件失败并退出。

### 重新插卡

```sh
mount | grep /mnt/sd
mp4play /mnt/sd/xxx.mp4
```

### 日志

```sh
logread | grep sdcard
```

## 已知限制

- 设备名固定为 `/dev/mmcblk0p1`，对应本板的 SD 卡
- 如果以后增加 eMMC 或其他 MMC 设备，需要改成按 UUID/LABEL 匹配
- 读写挂载下直接拔卡仍可能造成 FAT 脏位
- 不做自动播放
