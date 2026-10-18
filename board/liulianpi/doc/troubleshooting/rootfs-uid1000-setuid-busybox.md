# 故障复盘:rootfs 属主错误导致系统以非 root 启动

- 日期:2026-10-18(问题出现于 2026-10-14 构建的镜像)
- 现象:系统能启动内核、挂载 squashfs,但 `/preinit` 之后所有需要特权的操作全部失败,
  系统以 uid 1000 运行,overlay 无法建立,根文件系统只读
- 影响范围:该构建环境下生成的全部 rootfs 镜像(squashfs / tar / ext2 …)

## 1. 现象

NOR 启动日志中的关键片段:

```
[    1.216235] Run /preinit as init process
mount: you must be root                                  ← preinit 第一条命令失败
cat: can't open '/proc/cmdline': No such file or directory
umount: can't unmount /proc: Operation not permitted
mount: you must be root
hostname: sethostname: Operation not permitted
Starting network: ip: RTNETLINK answers: Operation not permitted
Populating /dev using udev: ... could not create /run/udev: Read-only file system
Saving random seed: SKIP (read-only file system detected)
```

`mount: you must be root` 是 BusyBox `mount` 在 `geteuid() != 0` 时的固定报错,
说明 **PID 1 的 euid 不是 0**,后续 `sethostname`/`RTNETLINK` 的 EPERM 也印证了这一点。

## 2. 根因链

按因果顺序:

1. **BusyBox 被安装为 setuid**。`package/busybox/busybox.config` 打开了
   `CONFIG_FEATURE_SUID=y`,Buildroot 的权限表(`package/busybox/busybox.mk:136`)
   随之声明:

   ```
   /bin/busybox  f 4755 0 0
   ```

   makedevs 在生成 rootfs 时会把 `/bin/busybox` 的 mode 改成 4755。

2. **属主本应由 fakeroot 伪造为 root**。`fs/common.mk:163` 在 fakeroot 会话里执行
   `chown -h -R 0:0 $(TARGET_DIR)`,并用 `FAKEROOTDONTTRYCHOWN=1`
   (`fs/common.mk:177`)跳过真实 chown,只在 fakeroot 的内存数据库中记录属主。

3. **宿主 glibc ≥ 2.33 时,该伪造对打包工具无效**。宿主机为 Ubuntu 22.04
   (glibc 2.35);glibc 2.33 起 `stat/lstat/fstat/fstatat` 变为直接系统调用,
   不再经过旧的 `__xstat/__lxstat/__fxstat/__fxstatat` 兼容符号。
   本仓库的 fakeroot 1.20.2 只导出了这些旧符号:

   ```console
   $ nm -D output/host/lib/libfakeroot.so | grep -E "xstat|chown"
   ... T chown / fchown / fchownat / lchown
   ... T __fxstat / __fxstat64 / __fxstatat / __fxstatat64
   ... T __lxstat / __lxstat64 / __xstat / __xstat64
   # 没有 stat/lstat/fstat/fstatat -->
   ```

   而 `mksquashfs`/`tar`/`mke2fs` 等宿主工具都由 glibc 2.35 编译,直接调用
   `lstat@GLIBC_2.33`,**绕过 fakeroot**,读到的是磁盘上的真实属主
   (构建用户 queqt,uid 1000)。`chmod` 是真实系统调用,故 setuid 位属实生效。

4. **镜像被写成"setuid 指向 uid 1000"**:

   ```console
   $ output/host/bin/unsquashfs -ll output/images/rootfs.squashfs | grep busybox
   -rwsr-xr-x queqt/queqt 777168 ... squashfs-root/bin/busybox
   ```

5. **内核以 root 启动 PID 1,但 execve 会兑现 setuid**。`/preinit` 是脚本,
   `#!/bin/sh` → `/bin/sh -> /bin/busybox`(4755,owner=1000),
   进程 euid 变为 1000。

6. **连锁反应**:preinit 的 `mount proc /proc -t proc` 失败 → `/proc/cmdline`
   读不到 → `overlayfsdev` 分支被跳过 → JFFS2 overlay 未建立 →
   根文件系统保持只读 squashfs 且无 tmpfs → udev、随机种子、网络全部失败。

注意:**与镜像新旧无关**——10-17 重新生成的镜像有同样问题,必须修构建侧后重烧。

## 3. 代码与上游参考

- 上游修复:Buildroot 2020.02.x 的 commit `f45925a9513`
  (`package/fakeroot/0006-libfakeroot.c-add-wrappers-for-new-glibc-2.33-symbol.patch`),
  源自 fakeroot 上游 commit `feda578ca3608b7fc9a28a3a91293611c0ef47b7`
  (Ilya Lipnitskiy),"add wrappers for newly exported symbols in glibc 2.33"。
- 本仓库此前已有两个针对 glibc 2.33 的本地补丁(`0005` 定义 `_STAT_VER`、
  `0006` 增加 mknod/mknodat wrapper),**恰好漏掉了 stat 系列 wrapper**,
  因此能编译、能安装,却静默产生错误属主的镜像。

## 4. 修复

新增 `package/fakeroot/0007-libfakeroot.c-add-wrappers-for-new-glibc-2.33-symbol.patch`
(45 行),为 glibc ≥ 2.33 导出:

- `lstat` / `stat` / `fstat` / `fstatat`
- `lstat64` / `stat64` / `fstat64` / `fstatat64`(由 `STAT64_SUPPORT` 保护)

包装器复用现有宏(`WRAP_LSTAT LSTAT_ARG(...)`),转发到 fakeroot 既有的
`__lxstat` 等实现,mknod 部分不重复(已由 `0006` 提供)。
详细 diff/验证/回滚见变更记录 `../changes/2026-10-18-rootfs-ownership-splash-simplefb.md`。

## 5. 验证

```console
$ make host-fakeroot-dirclean host-fakeroot   # 重建 host-fakeroot
$ nm -D output/host/lib/libfakeroot.so | grep -cE " T (lstat|stat|fstat|fstatat)$"
4

# 端到端:在 fakeroot 里 chown 后用 mksquashfs 打包,检查属主
$ FAKEROOTDONTTRYCHOWN=1 output/host/bin/fakeroot -- /bin/sh -c \
    'chown -h -R 0:0 /tmp/opencode/fr-test/rootfs && \
     output/host/bin/mksquashfs /tmp/opencode/fr-test/rootfs /tmp/opencode/fr-test/test.squashfs -noappend'
$ output/host/bin/unsquashfs -ll /tmp/opencode/fr-test/test.squashfs | grep busybox
-rwsr-xr-x root/root ... busybox        ← setuid 位保留,属主为 root

# 正式镜像
$ output/host/bin/unsquashfs -ll output/images/rootfs.squashfs | grep -E "bin/busybox|preinit"
-rwsr-xr-x root/root ... bin/busybox
-rwxr-xr-x root/root ... preinit
```

重新烧写 `sysimage-nor.img` 后,预期启动日志不再出现
`mount: you must be root` / `Operation not permitted`,
preinit 能挂 `/proc`、建立 overlay 并进入可写系统。

## 6. 经验与检查项

1. **每次构建后检查镜像属主**(见 `../build/build-and-verify.md` 必查清单),
   尤其 `/bin/busybox` 必须为 `root root`。
2. `mount: you must be root` + `sethostname: Operation not permitted` 同时出现时,
   优先怀疑 **init 进程 euid 非 0**,而不是内核权限配置。
3. 在 glibc ≥ 2.33 的宿主上构建老 Buildroot(≤2020.02 + fakeroot 1.20.2)时,
   必须确认 fakeroot 补丁包含 stat 系列 wrapper;否则属主错误是**静默**的,
   编译日志毫无异常。
4. setuid 程序放大了属主错误的影响:如果属主为普通用户,setuid 会让进程
   **降权**到该用户,而不是提权到 root。这也是本故障最隐蔽的部分。
