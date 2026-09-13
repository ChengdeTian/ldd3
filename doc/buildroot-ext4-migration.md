# QEMU ARM64 驱动开发环境：从 BusyBox initramfs 迁移到 Buildroot ext4

> 本文档记录将根文件系统从 BusyBox initramfs 切换为 Buildroot 生成的 ext4 镜像的完整过程，包括设计决策、修改的文件、关键配置项和踩坑记录。

## 一、目标与背景

| 项目 | 原始状态 | 目标状态 |
|---|---|---|
| 根文件系统 | BusyBox 生成的 `initramfs-arm64.cpio.gz` | Buildroot 生成的 `rootfs.ext4` |
| 启动方式 | `-initrd ... rdinit=/init` | `-drive if=virtio` + `root=/dev/vda` |
| 持久化 | 无（内存文件系统，重启即丢） | 有（可写回镜像） |
| 驱动共享 | 9p 挂载 `driver/mydriver` 到 `/mnt` | **保持不变** |

**核心设计决策**：内核不重新编译，复用已有的 `kernel/linux-6.12.10/arch/arm64/boot/Image`，Buildroot 只负责生成 rootfs。

原因：内核版本为 6.12.10，而 Buildroot 2025.02.4 自带 6.12.27 内核，版本不一致会带来 risk。复用现有内核可保证：

- `.config` 中的 9p / virtio / ext4 配置完全保留
- 免去内核重新编译（20-30 分钟）
- 驱动模块与内核 ABI 严格一致

### 环境信息

| 项 | 值 |
|---|---|
| 主机架构 | x86_64 |
| 目标架构 | aarch64 (ARM64) |
| 交叉工具链 | aarch64-linux-gnu-gcc（原有的）；Buildroot 自建 glibc 工具链 |
| 内核 | linux-6.12.10（复用） |
| Buildroot | 2025.02.4 |
| 主机核心/内存 | 16 核 / 13 GB |
| 代理 | `http://127.0.0.1:7897`（Clash 混合端口） |

---

## 二、修改的文件清单

### 2.1 修改：`kernel/linux-6.12.10/qemu_start.sh`

**改动性质**：从 initramfs 启动改为 ext4 启动，并新增 BusyBox 回退能力。

**原内容**：

```sh
qemu-system-aarch64 \
	-M virt \
	-cpu cortex-a72 \
	-smp 2 \
	-m 2G \
	-kernel ./arch/arm64/boot/Image \
	-initrd ../../rootfs/initramfs-arm64.cpio.gz \
	-append "console=ttyAMA0 rdinit=/init" \
	-fsdev local,security_model=passthrough,id=fsdev0,path=../../driver/mydriver \
	-device virtio-9p-device,fsdev=fsdev0,mount_tag=host_drv \
	-nographic
```

**当前内容**：

```sh
#!/bin/sh
# ARM64 QEMU 驱动开发环境启动脚本
#
# 默认：Buildroot 生成的 ext4 根文件系统（持久化）
# 驱动共享：9p 把 host 的 driver/mydriver 挂到 guest 的 /mnt
#
# 回退到旧的 BusyBox initramfs：
#   BUSYBOX=1 ./qemu_start.sh

ROOTFS_EXT4=../../rootfs/buildroot/output/images/rootfs.ext4
INITRAMFS=../../rootfs/initramfs-arm64.cpio.gz

if [ "$BUSYBOX" = "1" ]; then
    echo "[qemu] 使用 BusyBox initramfs 启动"
    exec qemu-system-aarch64 \
        -M virt \
        -cpu cortex-a72 \
        -smp 2 \
        -m 2G \
        -kernel ./arch/arm64/boot/Image \
        -initrd "$INITRAMFS" \
        -append "console=ttyAMA0 rdinit=/init" \
        -fsdev local,security_model=passthrough,id=fsdev0,path=../../driver/mydriver \
        -device virtio-9p-device,fsdev=fsdev0,mount_tag=host_drv \
        -nographic
else
    if [ ! -f "$ROOTFS_EXT4" ]; then
        echo "[qemu] 错误：未找到 ext4 镜像 $ROOTFS_EXT4"
        echo "[qemu] 请先构建：cd ../../rootfs/buildroot && make"
        echo "[qemu] 或回退到 BusyBox：BUSYBOX=1 $0"
        exit 1
    fi
    echo "[qemu] 使用 Buildroot ext4 镜像启动"
    exec qemu-system-aarch64 \
        -M virt \
        -cpu cortex-a72 \
        -smp 2 \
        -m 2G \
        -kernel ./arch/arm64/boot/Image \
        -drive file="$ROOTFS_EXT4",format=raw,if=virtio \
        -append "console=ttyAMA0 root=/dev/vda rw rootwait" \
        -fsdev local,security_model=passthrough,id=fsdev0,path=../../driver/mydriver \
        -device virtio-9p-device,fsdev=fsdev0,mount_tag=host_drv \
        -nographic
fi
```

**关键参数变化**：

| 参数 | 原值 | 新值 | 说明 |
|---|---|---|---|
| 根文件系统来源 | `-initrd xxx.cpio.gz` | `-drive file=xxx.ext4,format=raw,if=virtio` | cpio 是内存镜像，ext4 是块设备镜像 |
| 内核启动参数 | `rdinit=/init` | `root=/dev/vda rw rootwait` | 指定 ext4 块设备为根；`rw` 可写；`rootwait` 等待设备就绪 |
| 9p 共享 | 保留 | 保留 | 驱动目录依然可从 host 实时替换 |

> **踩坑记录**：该文件原属主为 `root:root`（权限 755），当前用户 `tcd` 无法写入，写入操作会被静默拒绝。需先执行 `sudo chown tcd:tcd qemu_start.sh`。

### 2.2 新增：`rootfs/buildroot/configs/my_qemu_aarch64_defconfig`

这是整个迁移的核心配置文件。基于 `qemu_aarch64_virt_defconfig` 改造。

```makefile
# ===== 架构：aarch64 (ARM64) =====
BR2_aarch64=y

# ===== 工具链：Buildroot 自建 =====
BR2_TOOLCHAIN_BUILDROOT_GLIBC=y

# ===== kernel headers：直接用本地内核 tarball，免下载 =====
BR2_KERNEL_HEADERS_CUSTOM_TARBALL=y
BR2_KERNEL_HEADERS_CUSTOM_TARBALL_LOCATION="file:///home/tcd/Desktop/qemu_driver/kernel/linux-6.12.10.tar.xz"
BR2_PACKAGE_HOST_LINUX_HEADERS_CUSTOM_6_12=y

# ===== 国内镜像加速 =====
# GNU 系源码包（gcc/binutils/glibc/m4/gmp/mpfr/mpc...）统一走阿里云 gnu 镜像
BR2_GNU_MIRROR="https://mirrors.aliyun.com/gnu"
# buildroot 官方托管的包走主站（BR2_PRIMARY_SITE 若指向不存在的路径会导致全部失败）
BR2_PRIMARY_SITE=""

# ===== 不编译内核，复用 kernel/linux-6.12.10/arch/arm64/boot/Image =====
# BR2_LINUX_KERNEL is not set

# ===== 目标 rootfs：ext4 =====
BR2_TARGET_ROOTFS_EXT2=y
BR2_TARGET_ROOTFS_EXT2_4=y
BR2_TARGET_ROOTFS_EXT2_SIZE="256M"
# BR2_TARGET_ROOTFS_TAR is not set

# ===== 根文件系统覆盖层（放 /init 挂 9p 等）=====
BR2_ROOTFS_OVERLAY="board/qemu/aarch64-virt/rootfs-overlay"

# ===== 系统选项 =====
BR2_TARGET_GENERIC_HOSTNAME="qemu-arm64"
BR2_TARGET_GENERIC_ISSUE="ARM64 QEMU Driver Test Environment"
BR2_TARGET_GENERIC_ROOT_PASSWD=""
BR2_TARGET_GENERIC_GETTY_PORT="ttyAMA0"
BR2_TARGET_GENERIC_GETTY_BAUDRATE_115200=y

# ===== init 系统：BusyBox =====
BR2_INIT_BUSYBOX=y
BR2_SYSTEM_BIN_SH_BUSYBOX=y

# ===== 驱动开发核心工具 =====
BR2_PACKAGE_BUSYBOX_SHOW_OTHERS=y
BR2_PACKAGE_KMOD=y
BR2_PACKAGE_KMOD_TOOLS=y
BR2_PACKAGE_STRACE=y

# ===== 解压工具（内核模块/固件包常用）=====
BR2_PACKAGE_GZIP=y
BR2_PACKAGE_XZ=y
BR2_PACKAGE_ZLIB=y

# ===== 网络工具（保留 iproute2，不做 SSH）=====
BR2_PACKAGE_IPROUTE2=y

# ===== 轻量编辑器 =====
BR2_PACKAGE_NANO=y
```

### 2.3 新增：`rootfs/buildroot/board/qemu/aarch64-virt/rootfs-overlay/etc/init.d/S99drv`

**作用**：ext4 启动场景下，由 BusyBox init 自动执行，负责挂载 9p 共享目录。

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

权限须为可执行（`chmod +x`，实际为 775）。

> **重要踩坑**：最初把这段逻辑写成了 rootfs 根目录的 `/init`，但那是 **initramfs 的 `rdinit` 机制**，ext4 启动时**根本不会执行**。
>
> ext4 启动流程是：内核 → `root=/dev/vda` → BusyBox init 读 `/etc/inittab` → 执行 `/etc/init.d/S*`。
>
> 因此必须用 **`/etc/init.d/S99drv`** 形式，否则 9p 永远不会被挂载。

### 2.4 未修改但需要了解的文件

| 文件 | 说明 |
|---|---|
| `rootfs/busybox-1.36.1.tar.bz2` | 未被使用（Buildroot 需要 busybox 1.37.0，版本不匹配） |
| `rootfs/initramfs-arm64.cpio.gz` | 保留，`BUSYBOX=1` 时可回退使用 |
| `kernel/linux-6.12.10.tar.xz` | **被复用为 Buildroot 的 kernel headers**（省 140MB 下载） |
| `driver/mydriver/*.ko` | 通过 9p 共享给 guest，无需改动 |

---

## 三、构建步骤

### 3.1 获取 Buildroot 源码

```bash
cd /home/tcd/Desktop/qemu_driver/rootfs
# 用 tar 管道复制，排除编译产物和下载缓存（源码约 87MB）
mkdir -p buildroot
cd /home/tcd/alientek/linux/tool/buildroot-2025.02.4
tar cf - --exclude=output --exclude=dl . | (cd /home/tcd/Desktop/qemu_driver/rootfs/buildroot && tar xf -)
```

> 注：源码取自本机已有的 `/home/tcd/alientek/linux/tool/buildroot-2025.02.4`。
> `cp -al`（硬链接）会因跨文件系统/权限问题失败。

### 3.2 加载配置并构建

```bash
cd /home/tcd/Desktop/qemu_driver/rootfs/buildroot
make my_qemu_aarch64_defconfig       # 加载自定义配置
make -j16                            # 完整构建（工具链 + rootfs）
```

**必须带代理**（否则下载卡死）：

```bash
cd /home/tcd/Desktop/qemu_driver/rootfs/buildroot
make my_qemu_aarch64_defconfig
setsid env http_proxy=http://127.0.0.1:7897 https_proxy=http://127.0.0.1:7897 \
       nohup make -j16 > /tmp/br_build.log 2>&1 &
```

### 3.3 产物

```
rootfs/buildroot/output/images/rootfs.ext4
```

### 3.4 启动

```bash
cd /home/tcd/Desktop/qemu_driver/kernel/linux-6.12.10
./qemu_start.sh              # ext4 启动
BUSYBOX=1 ./qemu_start.sh    # 回退到旧 initramfs
```

---

## 四、关键配置项说明

### 4.1 为什么用本地内核 tarball 作为 kernel headers

配置 `BR2_KERNEL_HEADERS_6_12` 时，Buildroot 会去 `cdn.kernel.org` 下载**完整内核源码包**（约 140MB）作为 headers。实测该源速度仅 **20-30 KB/s**，预计耗时 **94 分钟**。

改用 `BR2_KERNEL_HEADERS_CUSTOM_TARBALL` 指向本地的 `kernel/linux-6.12.10.tar.xz`：

```makefile
BR2_KERNEL_HEADERS_CUSTOM_TARBALL=y
BR2_KERNEL_HEADERS_CUSTOM_TARBALL_LOCATION="file:///home/tcd/Desktop/qemu_driver/kernel/linux-6.12.10.tar.xz"
```

**优势**：
- 零下载，秒级完成
- headers 版本精确匹配目标内核（6.12.10，优于 6.12.33）
- **自动跳过 hash 校验**（见 `package/linux-headers/linux-headers.mk` 的 `BR_NO_CHECK_HASH_FOR` 逻辑），无需额外 hash 文件

### 4.2 镜像源配置

| 配置项 | 值 | 覆盖范围 |
|---|---|---|
| `BR2_GNU_MIRROR` | `https://mirrors.aliyun.com/gnu` | GCC、binutils、glibc、m4、gmp、mpfr、mpc、gettext 等所有 GNU 包 |
| `BR2_PRIMARY_SITE` | `""`（空） | 不设，避免指向不存在路径导致全部失败 |

**镜像可用性实测**（用范围 GET 测试）：

| 镜像 | 结果 |
|---|---|
| 阿里云 `mirrors.aliyun.com/gnu` | **206 可用** ✅ |
| 清华 `mirrors.tuna.tsinghua.edu.cn/gnu` | 403 ❌ |
| 中科大 `mirrors.ustc.edu.cn/gnu` | 403 ❌ |

> **重要**：`BR2_GCC_SITE`、`BR2_BINUTILS_SITE` 这类变量**不存在**于 Buildroot 的 Kconfig 中，写入 defconfig 会被静默忽略。正确做法是用统一的 `BR2_GNU_MIRROR`。

### 4.3 为什么关闭内核编译

```makefile
# BR2_LINUX_KERNEL is not set
```

Buildroot 一旦开启 `BR2_LINUX_KERNEL`，就会下载并编译它自己的内核（6.12.27），与你手调的 6.12.10 内核冲突。关闭后复用现有 `Image`，保留全部 9p/virtio/ext4 配置。

### 4.4 包精简决策

按"**字符设备/内核模块驱动开发**"场景精简：

| 保留 | 用途 |
|---|---|
| `kmod` + `kmod-tools` | `insmod` / `rmmod` / `lsmod` / `modinfo` / `modprobe` |
| `strace` | 跟踪驱动的 syscall / ioctl / read / write |
| `iproute2` | 网络配置工具 |
| `nano` | 轻量编辑器 |
| `gzip` / `xz` / `zlib` | 处理 `.ko`、固件压缩包 |

| 移除 | 原因 |
|---|---|
| `dropbear`（SSH） | 有串口 console + 9p 共享，足够调试；无需 SSH |
| `dhcpcd` / `ethtool` | 不涉及网络驱动开发 |
| `libcap` | 是 dropbear 的依赖，一并移除 |
| `vim` | 换成体积小、编译快的 `nano` |
| `BR2_PACKAGE_BUSYBOX_WATCHDOG` | 与驱动调试无关 |

---

## 五、环境验证清单

构建前已确认以下内核能力（`kernel/linux-6.12.10/.config`）：

| 检查项 | 结果 | 作用 |
|---|---|---|
| `CONFIG_EXT4_FS=y` | ✅ | 支持 ext4 根文件系统 |
| `CONFIG_VIRTIO_BLK=y` | ✅ | 支持 virtio 块设备（`/dev/vda`） |
| `CONFIG_BLK_DEV_INITRD=y` | ✅ | 保留 initramfs 回退能力 |
| `CONFIG_DEVTMPFS=y` + `CONFIG_DEVTMPFS_MOUNT=y` | ✅ | `/dev` 自动挂载，`mknod` 可正常工作 |
| `CONFIG_NET_9P=y` / `NET_9P_VIRTIO=y` / `9P_FS=y` | ✅ | 9p 共享目录 |
| `CONFIG_SERIAL_AMBA_PL011_CONSOLE=y` | ✅ | 串口 console |

**结论**：内核无需重新编译，直接复用现有 `Image` 即可。

---

## 六、踩坑记录汇总

| # | 问题 | 现象 | 解决方案 |
|---|---|---|---|
| 1 | **文件属主为 root** | `qemu_start.sh` 属主 `root:root`，写入被静默拒绝 | `sudo chown tcd:tcd qemu_start.sh` |
| 2 | **镜像源 403** | 清华/中科大 GNU 镜像返回 403 | 改用**阿里云**（实测 206） |
| 3 | **无效配置项** | `BR2_GCC_SITE` 等变量不存在，被静默忽略 | 使用正确的 **`BR2_GNU_MIRROR`** |
| 4 | **PRIMARY_SITE 陷阱** | 指向不存在的路径导致所有包下载失败 | 设 `BR2_PRIMARY_SITE=""` |
| 5 | **内核源码 140MB 慢下载** | 6.12.33 头文件包需 94 分钟 | 改用**本地 tarball**（`BR2_KERNEL_HEADERS_CUSTOM_TARBALL`） |
| 6 | **hash 校验失败** | 并发写临时目录导致文件被中途删除 | 停止旧构建后干净重启，勿在构建中改配置 |
| 7 | **init 脚本不执行** | ext4 下根目录 `/init` 无效 | 改用 **`/etc/init.d/S99drv`** |
| 8 | **代理未生效** | 构建进程无 `proxy` 环境变量 | 用 `setsid env http_proxy=... https_proxy=...` 启动 |
| 9 | **busybox 版本不匹配** | 本地为 1.36.1，Buildroot 需 1.37.0 | 用 Buildroot 自行下载，本地包不用 |
| 10 | **硬链接复制失败** | `cp -al` 报 `Operation not permitted` | 改用 `tar` 管道复制 |

---

## 七、下载包清单（构建过程中已获取）

约 31 个源码包，分类如下：

**工具链核心**：gcc-13.3.0、binutils-2.43.1、glibc-2.41、gmp-6.3.0、mpfr-4.1.1、mpc-1.3.1

**构建工具**：m4-1.4.20、bison-3.8.2、flex-2.6.4、autoconf-2.72、automake-1.16.5、libtool-2.4.6、pkgconf-2.3.0、gawk-5.3.1、gettext-0.22.4

**镜像/权限工具**：e2fsprogs-1.47.2、util-linux-2.40.2、fakeroot-1.36、acl-2.3.2、attr-2.5.2

**目标软件**：busybox-1.37.0、kmod-33、iproute2-6.14.0、nano-8.2、ncurses-6.4、gzip-1.13、xz-5.6.4、zlib-1.3.1、strace

**内核**：linux-6.12.10（本地 tarball，未下载）

---

## 八、时间与性能参考

| 阶段 | 耗时 | 备注 |
|---|---|---|
| 配置准备 | 约 10 分钟 | 含镜像测试、踩坑 |
| 源码下载 | 约 40 分钟 | 未用代理时极慢（5-30 KB/s）；**代理后 100-350 KB/s** |
| 完整编译 | 25-40 分钟 | GCC + glibc 为串行瓶颈 |

**代理效果对比**：

| 来源 | 无代理 | 有代理 |
|---|---|---|
| kernel.org | 20-30 KB/s | 100-350 KB/s（峰值 175 MB/s 缓存命中） |
| GitHub | 超时失败 | 正常连接 |
| 阿里云 GNU | 正常 | 正常 |

---

## 九、后续待办

- [ ] 完成 Buildroot 完整编译（`make -j16`）
- [ ] 验证 `rootfs.ext4` 生成成功
- [ ] 用 `./qemu_start.sh` 启动，确认 9p 挂载正常
- [ ] 在 guest 中 `insmod /mnt/scull.ko` 验证驱动加载
- [ ] 确认 ext4 持久化生效（重启后文件保留）
