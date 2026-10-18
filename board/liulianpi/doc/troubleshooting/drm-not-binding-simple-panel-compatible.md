# 故障复盘:sun4i-drm 不注册(card0 缺失)/ panel compatible 无匹配

- 日期:2026-10-18
- 现象:LCD 能显示 U-Boot 开机图,系统以 root 正常运行,但内核没有任何 DRM 设备;
  `fb-test -r` 无法改变(或看不到)画面
- 根因:DTS 的 panel 节点使用通用 compatible `"simple-panel"`,而内核 5.4 的
  panel-simple 驱动没有该匹配项 → panel 驱动永不绑定 → sun4i-drm 组件链
  **静默**地永不完成

## 1. 现象与首次定位

目标机诊断输出:

```console
# id
uid=0(root) ...                       ← rootfs 属主问题已修复
# ls -l /dev/fb*
crw-rw---- 1 root video 29, 0 /dev/fb0
# cat /sys/class/graphics/fb0/name
simple                                ← 不是 sun4i-drmdrmfb
# dmesg | grep -iE "sun4i|tcon|drm|fb0|simple-framebuffer|panel"
simple-framebuffer 83da8000.framebuffer: framebuffer at 0x83da8000, 0x258000 bytes
simple-framebuffer 83da8000.framebuffer: format=x8r8g8b8, mode=1024x600x32, linelength=4096
simple-framebuffer 83da8000.framebuffer: fb0: simplefb registered!
sun4i-codec 1c23c00.codec: Codec <-> 1c23c00.codec mapping ok
# ls /sys/class/drm/
version                               ← 没有任何 card0/connector
```

结论:显示画面完全来自 **U-Boot 配置好的 TCON 持续扫描 + 内核 simplefb**;
`sun4i-drm` 从头到尾没有注册(kernel 5.4 `sun4i_drv` 不成功时不会打印错误,
只会反复 deferred probe)。

## 2. 根因链

1. liulianpi DTS 的 panel 节点:

   ```dts
   panel: panel {
       compatible = "simple-panel";      /* 只有通用字符串 */
       panel-timing { ... };
   };
   ```

2. 内核 5.4 的 `drivers/gpu/drm/panel/panel-simple.c` **不存在
   `"simple-panel"` 匹配项**(该内核只有具体面板型号;`panel-dpi` 通用
   支持也是更晚的版本才有):

   ```console
   $ grep -rn '"simple-panel"' output/build/linux-5.4.99/drivers/gpu/drm/
   (无输出)
   ```

3. 面板无驱动 → `sun4i_tcon` 探测时
   `drm_of_find_panel_or_bridge()`(`sun4i_tcon.c:1327`)返回 `-EPROBE_DEFER`,
   tcon 组件永远不 available → 组件框架里 DRM 主设备的
   `component_bind_all()` 永远等待 → `sun4i_drv` 永不注册 card0。

4. 该失败模式**没有任何 dmesg 报错**:platform 驱动只打印 probe 失败(硬错误),
   而 `-EPROBE_DEFER` 是静默重试。所以判断驱动是否起来必须看运行时对象:
   `/sys/class/drm/`、`/sys/bus/platform/drivers/...`。

参考:同仓库能正常工作的板子都使用**具体面板型号 + `simple-panel` 兜底**:

```dts
/* board/sipeed/lichee/nano/devicetree/linux/devicetree.dts:29 */
compatible = "qiaodian,qd43003c0-40", "simple-panel";
```

## 3. 修复

新增内核补丁 `board/allwinner/suniv-f1c100s/patch/linux/0017-drm-panel-simple-add-generic-simple-panel-support.patch`:

- 给 panel-simple 注册 `{ .compatible = "simple-panel", .data = &simple_panel_desc }`;
- 用 `.timings`(而非固定模式)描述时序**边界**。5.4 的 panel-simple 已有
  DT 覆盖机制:`panel_simple_probe()` 读取 `panel-timing` 并调用
  `panel_simple_parse_panel_timing_node()`(panel-simple.c:370-410),
  在边界内校验通过后生成 `override_mode`,由
  `panel_simple_get_non_edid_modes()`(panel-simple.c:190-208)优先使用。
- flags 必须与 DT 解析结果精确相等:DT 只有 `hsync-active=1` / `vsync-active=1`
  (`drivers/video/of_display_timing.c`),因此补丁的 flags 为
  `DISPLAY_FLAGS_HSYNC_HIGH | DISPLAY_FLAGS_VSYNC_HIGH`。

这样 DRM 点屏使用的时序与 U-Boot 现网时序(40MHz、150/150/20、15/15/5)完全一致。

## 4. 验证

构建侧:

```console
$ patch --dry-run -p1 -i .../0017-*.patch            # 干净应用
$ grep -c 0017 output/build/linux-5.4.99/.applied_patches_list
1
```

目标机(烧录后):

```console
# ls /sys/class/drm/
card0  card0-DPI-1  version     ← 出现 card0(connector 名称以实际为准)
# dmesg | grep sun4i
sun4i-drm display-engine: bound 1e00000.display-frontend (ops ...)
sun4i-drm display-engine: bound 1e60000.display-backend (ops ...)
sun4i-drm display-engine: bound 1c0c000.lcd-controller (ops ...)
...
# cat /sys/class/graphics/fb0/name
sun4i-drmdrmfb
```

行为变化(预期):

- DRM 就绪后接管显示,U-Boot 开机图被替换;配合 `console=tty0`,
  LCD 会显示(回放)内核日志;
- 串口仍是 `/dev/console`(bootargs 里 ttyS0 放最后)。

若点屏后出现画面偏移或颜色异常,调整顺序:先核对补丁时序边界与 DT
`panel-timing`,再考虑 `bus_flags`(PIXDATA 边沿等)。

## 5. 经验与检查项

1. **"驱动没起来"不一定会报错**:`-EPROBE_DEFER` 链是静默的;排查时先看
   `/sys/class/drm/`、`/sys/bus/platform/drivers/*/`、`dmesg` 中 component
   绑定信息。
2. 板级 DTS 的 panel 节点必须使用**驱动能匹配**的具体型号 compatible;
   通用字符串(`simple-panel`/`panel-dpi`)只有在对应内核版本支持时才可用。
3. 新增/修改内核 patch 后必须 `make linux-dirclean linux`(补丁只在解包阶段
   应用),只 `linux-rebuild` 不会重新打补丁。
4. 内核 cmdline 中多个 `console=` 的**最后一个**决定 `/dev/console`
   (`kernel/printk/printk.c`),涉及 getty 位置时要留意顺序。
