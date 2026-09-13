# Buildroot `rootfs-overlay` 目录作用分析

> 分析 `rootfs/buildroot/board/qemu/aarch64-virt/rootfs-overlay` 目录的作用、机制与使用方法。

## 一、它是什么

`BR2_ROOTFS_OVERLAY` 是 Buildroot 提供的**根文件系统叠加层**机制。

配置项：

```makefile
BR2_ROOTFS_OVERLAY="board/qemu/aarch64-virt/rootfs-overlay"
```

Buildroot 在生成 rootfs 时，会把**这个目录里的所有内容直接覆盖到最终 rootfs 的根目录**。

## 二、映射关系

这是"**目录树镜像**"——overlay 里的路径 = 最终 rootfs 里的路径：

```
board/qemu/aarch64-virt/rootfs-overlay/          →   rootfs.ext4 里的
└── etc/                                              └── etc/
    └── init.d/                                           └── init.d/
        └── S99drv                                            └── S99drv
```

## 三、当前 overlay 内容

目录下**只有一个文件**：

```
rootfs-overlay/etc/init.d/S99drv     (690 字节, 权限 775)
```

作用：guest 启动时自动挂载 9p 共享目录。

```sh
#!/bin/sh
# 驱动开发环境启动脚本（ext4 rootfs 场景，由 busybox init 自动执行）
# 挂载 9p 共享目录 host_drv 到 /mnt，方便从 host 实时替换 .ko

case "$1" in
    start)
        mkdir -p /mnt /root/drv

        if mount -t 9p -o trans=virtio,version=9p2000.L host_drv /mnt 2>/dev/null; then
            echo "[S99drv] 9p host_drv 已挂载到 /mnt"
            cp -f /mnt/*.ko /root/drv/ 2>/dev/null
        else
            echo "[S99drv] 9p 挂载失败（检查 qemu 的 -fsdev/-device 参数）"
        fi
        ;;
    stop)
        umount /mnt 2>/dev/null
        ;;
    *)
        echo "Usage: $0 {start|stop}"
        exit 1
        ;;
esac

exit 0
```

## 四、为什么必须用 overlay（而不是别的办法）

| 方案 | 问题 |
|---|---|
| 直接改 Buildroot 的 `output/target/` | 每次 `make` 会被覆盖/重建，改动丢失 |
| 编译后手动挂载 ext4 改 | 每次都要手动操作，且不可复现 |
| **rootfs-overlay** ✅ | 声明式，每次构建自动应用，版本可追踪 |

overlay 是**源码的一部分**，跟着 Buildroot 配置走，可重复构建。

## 五、执行时机与顺序

```
Buildroot 构建流程:
  编译所有包 → 生成 output/target/ 基础 rootfs
       ↓
  应用 BR2_ROOTFS_OVERLAY      ← S99drv 在这里被拷入
       ↓
  执行 BR2_ROOTFS_POST_BUILD_SCRIPT（若有）
       ↓
  mke2fs 打包成 rootfs.ext4    ← S99drv 已固化进镜像
```

**关键点**：overlay 在打包**之前**应用，所以 `S99drv` 会**永久固化在 ext4 镜像里**，guest 每次启动都会执行它。

## 六、`S99drv` 的命名含义

BusyBox init 按**文件名字典序**执行 `/etc/init.d/S*`：

| 文件名 | 含义 |
|---|---|
| `S` 前缀 | Start（启动时执行） |
| `99` | 排序号，**数字越大越晚执行** |
| `drv` | 自定义名称（driver 缩写） |

用 `99` 是为了**确保在网络、devtmpfs 等基础服务之后执行**，此时 `/dev`、`/proc` 已就绪，9p 挂载才不会失败。

## 七、运行效果

guest 启动后你会看到：

```
[S99drv] 9p host_drv 已挂载到 /mnt
```

然后就可以直接：

```sh
insmod /mnt/scull.ko          # 从 host 共享目录直接加载
# 或
insmod /root/drv/scull.ko     # 从 kernel 侧拷过来的副本加载
```

## 八、overlay 的其他常见用途

这个机制很通用，后续可以往 overlay 里放任何东西：

| 想做的事 | 在 overlay 里放 |
|---|---|
| 加启动脚本 | `etc/init.d/SxxXxx` |
| 改配置 | `etc/xxx.conf` |
| 预置驱动 | `lib/modules/.../xxx.ko` |
| 加测试工具 | `usr/bin/mytool` |
| 改欢迎信息 | `etc/issue` |
| 预设 root 密码 | `etc/shadow` |

## 九、同目录其他文件说明

`board/qemu/aarch64-virt/` 下的另外两个文件**与 overlay 无关**，是 Buildroot 自带的 `qemu_aarch64_virt_defconfig` 配套文件：

| 文件 | 说明 |
|---|---|
| `linux.config` | Buildroot 编译内核时的默认配置。**本工程已关闭内核编译（`# BR2_LINUX_KERNEL is not set`），此文件不会被用到** |
| `readme.txt` | Buildroot 官方文档，说明如何启动该配置。同样不适用，因为本工程复用自己编译的内核 |

## 十、总结

`rootfs-overlay` 是**定制 ext4 rootfs 内容的入口**。

目前它只做一件事——把 9p 挂载逻辑塞进 guest 的启动流程。若后续想在 rootfs 里预置东西（驱动、工具、配置），直接按路径结构放进去即可，重新 `make` 就会生效。

### 相关文件路径

| 路径 | 作用 |
|---|---|
| `rootfs/buildroot/configs/my_qemu_aarch64_defconfig` | 声明 `BR2_ROOTFS_OVERLAY` |
| `rootfs/buildroot/board/qemu/aarch64-virt/rootfs-overlay/etc/init.d/S99drv` | 实际的 overlay 内容 |
| `rootfs/buildroot/output/images/rootfs.ext4` | 应用 overlay 后生成的最终镜像 |
