# LiuLianPi (F1C100s) 板级文档

本目录收录 LiuLianPi(F1C100s)板级的技术说明、构建烧录指南、故障复盘与变更记录,
用于后续维护、复盘和问题定位。

## 目录结构

| 路径 | 内容 |
| ---- | ---- |
| `boot/`             | 启动链路、Flash 分区布局、bootargs、preinit/overlay 机制 |
| `build/`            | 构建流程、产物校验清单、烧录方法 |
| `troubleshooting/`  | 故障复盘(现象 → 根因 → 证据 → 修复 → 验证) |
| `changes/`          | 变更记录,文件名格式 `YYYY-MM-DD-<主题>.md` |

## 文档索引

- [启动流程与 NOR 分区布局](boot/boot-flow-and-flash-layout.md)
- [构建、校验与烧录](build/build-and-verify.md)
- [故障复盘:rootfs 属主错误导致系统以非 root 启动](troubleshooting/rootfs-uid1000-setuid-busybox.md)
- [故障复盘:sun4i-drm 不注册(panel compatible 无匹配)](troubleshooting/drm-not-binding-simple-panel-compatible.md)
- [变更记录 2026-10-18:rootfs 属主修复 / 开机图 / simplefb / DRM 修复](changes/2026-10-18-rootfs-ownership-splash-simplefb.md)

## 维护约定

1. 修改板级配置(defconfig / DTS / 补丁)后,必须在 `changes/` 下按日期新增一条记录,
   写明动机、改动文件、影响、验证方法与回滚方式。
2. 疑难问题解决后在 `troubleshooting/` 下补一篇完整复盘。
3. 每次重新构建后,按 `build/build-and-verify.md` 中的“产物必查清单”检查镜像,
   尤其是**镜像内文件的属主必须是 root** 这一项(见故障复盘)。
