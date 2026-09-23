# LDD3 学习路线 A 实施计划（边学边适配）

> **路线 A 定义**：从 `lld3-example/` 取原始代码，在自己的 `chapter-XX/` 目录中逐步移植到 kernel 6.12.10，
> 一边修复编译错误一边理解新版内核 API 的演进，最终在 QEMU 环境中实测通过。
>
> 配套目录：`/home/tcd/Desktop/qemu_driver/lld3-example/`（LDD3 官方示例，2004-2005，目标内核 2.6.x）

---

## 一、背景与现状

### 1.1 参考代码的性质

| 项 | 情况 |
|---|---|
| 来源 | 《Linux Device Drivers》第 3 版（O'Reilly, 2005）官方示例 |
| 代码年代 | 2004-2005（源码内可见 `$Id: ... 2004/10/26`） |
| 目标内核 | **2.6.x** |
| 当前内核 | **6.12.10** |
| 时间跨度 | 约 20 年 |
| 规模 | 17 个模块目录，约 11900 行 C 代码 |
| 兼容层 | ❌ **无**（仅有 `include/lddbus.h`，没有 `ldd3/` 兼容头） |

### 1.2 当前进度

| 项 | 状态 |
|---|---|
| `chapter-02/` | ✅ 完成（`01_hello.c`、`02_hellop.c`、`Makefile`） |
| `chapter-03/` | 🔄 起步（`scull.c` 仅 40 行，是 `scullc/main.c` 的拷贝，**含 `config.h` 无法编译**） |
| `doc/chapter-02.md` | 已有（2 行，偏简略） |
| 基础设施 | ✅ QEMU + ext4 根文件系统 + 9p 共享目录 + `mydriver/` |
| git | 2 次提交（`4cd1d94` 初始化、`d9792dc` 第一章节代码） |

### 1.3 可用环境

| 组件 | 说明 |
|---|---|
| 内核 | `kernel/linux-6.12.10`（已编译 `arch/arm64/boot/Image`） |
| 根文件系统 | `rootfs/buildroot/output/images/rootfs.ext4`（256M ext4） |
| 交叉工具链 | `aarch64-linux-gnu-gcc` 13.3.0（**必须用这个**） |
| 共享通道 | 9p：host `driver/mydriver/` ↔ guest `/mnt` |
| 启动脚本 | `kernel/linux-6.12.10/qemu_start.sh` |

> ⚠️ **工具链陷阱**：`~/.zshrc` 第 121-123 行导出了 `CROSS_COMPILE=aarch64-none-linux-gnu-`（ARM 官方 GCC 10.3.1），
> 该工具链**不支持** 6.12 内核使用的 `-ftrivial-auto-var-init=zero` 选项。
> **每个 Makefile 必须显式写 `CROSS_COMPILE := aarch64-linux-gnu-`**（用 `:=` 而非 `?=`）。

---

## 二、总路线图

| 阶段 | 章节 | 主题 | 代码来源 | 难度 | 预估 |
|---|---|---|---|---|---|
| 0 | — | 基建与工具 | 自建 | ⭐ | 0.5 天 |
| 1 | 2 | 模块基础 | `misc-modules/hello.c` `hellop.c` | ⭐ | ✅ 已完成 |
| 2 | 3-6 | **字符设备（核心）** | `scull/` `simple/` | ⭐⭐⭐ | 2-3 周 |
| 3 | 7 | 时间与延迟 | `misc-modules/jit.c` | ⭐⭐ | 3 天 |
| 4 | 8 | 内存分配 | `scullc/` `scullp/` `scullv/` | ⭐⭐ | 1 周 |
| 5 | 9-10 | 硬件与中断 | `short/` `shortprint/` | ⭐⭐⭐ | 1-2 周 |
| 6 | 11 | 内核数据类型 | `misc-modules/kdatasize.c` | ⭐ | 2 天 |
| 7 | 14 | 设备模型与总线 | `lddbus/` `sculld/` | ⭐⭐⭐ | 1-2 周 |
| 8 | 12-13 | PCI / USB | `pci/` `usb/` | ⭐⭐ | 1 周 |
| 9 | 15-16 | 块设备 | `sbull/` | ⭐⭐⭐⭐ | 2 周 |
| 10 | 17 | 网络设备 | `snull/` | ⭐⭐⭐ | 1-2 周 |
| 11 | 18 | TTY 驱动 | `tty/` | ⭐⭐⭐⭐ | 2 周 |

> **阶段 9-11（块/网络/TTY）改动最大**，这三个子系统在 20 年间被彻底重写，适配成本远高于其他部分。

---

## 三、阶段 0：基建（先做，省后续时间）

### 3.1 建立可复用的 Makefile 模板

路径：`driver/template/Makefile`，每个新 chapter 复制后只改两行：

```makefile
MODULE_NAME := scull        # ← 改这里（决定 .ko 名字）
SRC_OBJS    := scull.o      # ← 改这里（多文件用空格分隔）
```

模板内容：

```makefile
# ================================================================
# 模块基名 —— 单一来源，决定 .ko 的名字
# ================================================================
MODULE_NAME := scull

# 实际的中间对象（与模块名不同名时必须写，如 02_hellop.o）
SRC_OBJS    := scull.o

ifeq ($(KERNELRELEASE),)

KERNELDIR     ?= ../../kernel/linux-6.12.10
KODIR         := ../mydriver/
PWD           := $(shell pwd)

# 必须显式指定，覆盖 ~/.zshrc 中导出的错误 CROSS_COMPILE
ARCH          := arm64
CROSS_COMPILE := aarch64-linux-gnu-

modules:
	$(MAKE) -C $(KERNELDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) modules

install: modules
	@mkdir -p $(KODIR)
	@cp -f $(MODULE_NAME).ko $(KODIR)
	@echo "[install] $(MODULE_NAME).ko -> $(KODIR)"

clean:
	$(MAKE) -C $(KERNELDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) clean

.PHONY: modules install clean

else

# obj-m 的基名决定 .ko 名字；-objs 指定实际的中间对象
obj-m += $(MODULE_NAME).o
$(MODULE_NAME)-objs := $(SRC_OBJS)

endif
```

### 3.2 建立移植速查表

路径：`driver/doc/porting-cheatsheet.md`，内容见本文件**第五节**。

### 3.3 建立笔记模板

`driver/doc/chapter-XX.md` 建议固定四段：

```markdown
# 第X章 标题
## 1. 核心概念
## 2. 代码结构与阅读顺序
## 3. 2.6 → 6.12 移植点      ← 最有价值的一节
## 4. 实测结果（命令 + 输出）
```

### 3.4 验证环境闭环

```bash
# 主机侧
cd driver/chapter-02 && make install

# guest 侧（QEMU 内）
insmod /mnt/hellop.ko who="test"
dmesg | tail -3
rmmod hellop
```

**验收标准**：这条链路必须顺畅，否则后续每一步都会卡在环境问题上。

---

## 四、阶段 2：字符设备（重点，占全部精力的一半）

`scull` 是全书的脊梁，在书中第 3~6 章被反复改造。建议**分 6 个版本递进**，每个版本单独文件、单独提交。

### V1 最简字符设备（第 3 章前半）

| 项 | 内容 |
|---|---|
| 参考 | `lld3-example/scull/main.c` 的 `open`/`release`/`read`/`write` 部分 |
| 目标 | 只实现 `open`/`release`，`read`/`write` 返回 `-EINVAL` |
| 关键点 | `alloc_chrdev_region`、`cdev_init`、`cdev_add`、`file_operations` |
| 验收 | `mknod /dev/scull0 c <major> 0` → `cat /dev/scull0` 有响应但不返回数据 |

### V2 加内存管理（第 3 章后半）

| 项 | 内容 |
|---|---|
| 关键点 | `kmalloc`、quantum/qset 结构（`struct scull_qset` 链表） |
| 新增 | `scull_trim()`、read/write 的实际数据搬运 |
| 验收 | `echo "hello" > /dev/scull0` 后 `cat /dev/scull0` 能读回 |

### V3 加并发控制（第 5 章）

| 项 | 内容 |
|---|---|
| 关键点 | `struct mutex` 加入 `struct scull_dev` |
| **移植** | LDD3 的 `init_MUTEX()` / `DECLARE_MUTEX()` **已删除** → 用 `mutex_init()` / `DEFINE_MUTEX()` |
| **移植** | `down_interruptible()` → `mutex_lock_interruptible()` |
| 验收 | `misc-progs/load50.c` 并发压测不崩溃 |

### V4 加 ioctl（第 6 章前半）

| 项 | 内容 |
|---|---|
| 参考 | `scull/access.c` |
| 关键点 | `_IO`/`_IOR`/`_IOW` 宏、`switch(cmd)`、`access_ok` |
| **移植** | `.ioctl` → **`.unlocked_ioctl`** |
| **移植** | `access_ok(VERIFY_READ, p, n)` → `access_ok(p, n)`（参数减少） |
| 验收 | ioctl 测试程序能通 |

### V5 加阻塞 I/O 与 poll（第 6 章中后）

| 项 | 内容 |
|---|---|
| 关键点 | 等待队列 `wait_queue_head_t`、`poll_wait`、`POLLIN`/`POLLOUT` |
| 验收 | `misc-progs/polltest.c` + `nbtest.c` 通过 |

### V6 加异步通知 + mmap（第 6 章末 + 第 15 章）

| 项 | 内容 |
|---|---|
| 关键点 | `fasync_helper`、`SIGIO`、`remap_pfn_range` |
| **移植** | `vm_operations_struct.nopage` **已删除** → 用 `.fault` |
| **移植** | `remap_page_range()` → `remap_pfn_range()` |
| 验收 | `misc-progs/asynctest.c`、`mapper.c` 通过 |

### 配套：设备节点脚本

LDD3 的 `scull_load` 用 `mknod` + 从 `/proc/devices` 取 major，**这套在 6.12 依然可用**：

```bash
major=$(awk '$2=="scull" {print $1}' /proc/devices)
mknod /dev/scull0 c $major 0
```

> 放在 9p 共享目录里，guest 内直接 `sh /mnt/scull_load` 执行。

---

## 五、移植速查表（2.6 → 6.12）

> 以下内容已在实际内核源码 `kernel/linux-6.12.10/include/` 中核实。

### 5.1 必须删除的头文件

| LDD3 写法 | 6.12 处理 | 涉及文件数 |
|---|---|---|
| `#include <linux/config.h>` | **整行删除**（已核实文件不存在） | 15 个 |
| `#include <linux/malloc.h>` | 改 `#include <linux/slab.h>` | — |
| `#include <linux/smp_lock.h>` | **整行删除**（已核实文件不存在） | 1 个 |

**含 `config.h` 的 15 个文件**（移植第一课就是删这些行）：

```
pci/pci_skel.c          sbull/sbull.c          scullc/main.c   scullc/mmap.c
sculld/main.c           sculld/mmap.c          scullp/main.c   scullp/mmap.c
scullv/main.c           scullv/mmap.c          shortprint/shortprint.c
simple/simple.c         skull/skull_clean.c    skull/skull_init.c
tty/tiny_tty.c
```

### 5.2 路径变更（头文件仍存在，只是换了目录）

| LDD3 | 6.12 |
|---|---|
| `#include <asm/uaccess.h>` | `#include <linux/uaccess.h>` |

### 5.3 API 替换

| LDD3 (2.6) | 6.12 正确写法 |
|---|---|
| `.ioctl = xxx` | `.unlocked_ioctl = xxx` |
| `create_proc_entry(name, mode, parent)` | `proc_create(name, mode, parent, &fops)` |
| `class_create(THIS_MODULE, "name")` | `class_create("name")` |
| `class_device_create(...)` | `device_create(...)` |
| `class_device_destroy(...)` | `device_destroy(...)` |
| `struct class_device` | 已删除，改用 `struct device` |
| `init_MUTEX(&sem)` | `mutex_init(&mutex)` |
| `DECLARE_MUTEX(x)` | `DEFINE_MUTEX(x)` |
| `down_interruptible(&sem)` | `mutex_lock_interruptible(&mutex)` |
| `init_timer(&t); t.function = cb; t.data = x;` | `timer_setup(&t, cb, 0)`，回调签名 `void cb(struct timer_list *t)` |
| `access_ok(VERIFY_READ, p, n)` | `access_ok(p, n)` |
| `file->f_dentry` | `file_inode(file)` |
| `vm_ops->nopage` | `vm_ops->fault` |
| `remap_page_range(...)` | `remap_pfn_range(...)` |
| `blk_alloc_queue()` + `make_request` | **blk-mq**：`blk_mq_alloc_tag_set()` + `blk_mq_init_queue()` |
| `net_device.hard_start_xmit` | `net_device_ops.ndo_start_xmit` |
| `tty_driver` 静态嵌入 | `tty_alloc_driver()` + `tty_port` |
| `MODULE_PARM` | `module_param` |

### 5.4 保持不变（可以直接用）

```
cdev_init / cdev_add / cdev_del
kmalloc / kfree
copy_to_user / copy_from_user / get_user / put_user
complete / wait_for_completion
wait_event_interruptible / wake_up_interruptible
kthread_create / kthread_stop
register_chrdev / unregister_chrdev
module_param / MODULE_PARM_DESC / MODULE_LICENSE
```

---

## 六、各阶段难度与障碍预判

| 模块 | 主要障碍 |
|---|---|
| `scull` 系 | `.ioctl`→`.unlocked_ioctl`、`create_proc_entry`→`proc_create`、`class_create` 签名变化、`access_ok` 参数减少 |
| `scullc/p/v` | 相对简单，主要是 `config.h` + mmap 的 `.fault` |
| `short` / `shortprint` | **依赖真实 ISA/并口设备，QEMU `virt` 上不存在**，只能读代码理解原理 |
| `lddbus` / `sculld` | `struct class_device` **整套删除**、`driver_attribute` → `device_attribute`、`class_device_create` → `device_create` |
| `pci_skel` | 基本可用，主要是删 `config.h`；需 QEMU 加 PCI 设备才能实测 |
| `usb-skeleton` | 需 USB 控制器；删 `smp_lock.h` |
| `sbull` | 块层**彻底重写**（`blk_alloc_queue`/`make_request` → **blk-mq**），改动量最大 |
| `snull` | `net_device` 的 `hard_start_xmit` 等 → **`net_device_ops` 结构体**（2.6.29 起） |
| `tty` | `tty_driver` API 大改（kref、`tty_port`、`tty_operations`） |

---

## 七、工程规范建议

### 7.1 目录约定

```
driver/
├── chapter-02/              # 每章一目录
│   ├── 01_hello.c           # 编号 = 章内顺序
│   ├── 02_hellop.c
│   └── Makefile
├── chapter-03/
│   ├── scull.h              # 头文件
│   ├── scull_v1.c           # 版本递进，便于对比回退
│   ├── scull_v2.c
│   └── Makefile
├── doc/
│   ├── chapter-02.md        # 每章一篇笔记
│   ├── learning-plan.md     # 本文件
│   └── porting-cheatsheet.md
├── template/Makefile        # 新章复制用
└── mydriver/                # 9p 共享目录（.ko 产物，已 gitignore）
```

### 7.2 git 提交节奏

每完成一个**能跑通的版本**就提交一次：

```
第3章 scull v1：最简字符设备（open/release）
第3章 scull v2：加入内存管理（quantum/qset）
第3章 scull v3：加入互斥锁
```

便于出问题时逐版本回退对比。

### 7.3 笔记要记"卡点"

`doc/chapter-XX.md` 的**移植点**那一节最有价值：

```markdown
## 3. 2.6 → 6.12 移植点
- `init_MUTEX()` 已删除 → 改用 `mutex_init()`
  报错：implicit declaration of function 'init_MUTEX'
- `.ioctl` 已删除 → `.unlocked_ioctl`
  现象：模块加载成功但 ioctl 无响应
```

比抄书本概念有用得多，日后回看能直接定位问题。

---

## 八、近期行动清单

| # | 任务 | 产出 |
|---|---|---|
| 1 | 建 `template/Makefile` | 可复用模板 |
| 2 | 建 `doc/porting-cheatsheet.md` | 速查表（第五节内容） |
| 3 | 清理 `chapter-03/scull.c`，删 `config.h`、改 `asm/uaccess.h` | 能编译的空壳 |
| 4 | 写 **scull V1**（open/release） | 首个可加载字符设备 |
| 5 | 写 `scull_load` 脚本 | 自动建 `/dev/scull0` |
| 6 | 更新 `doc/chapter-03.md` | 第 3 章笔记 |
| 7 | git 提交 | 里程碑 |

---

## 九、两个重要提醒

### 9.1 别一次移植太多

`scull` 完整版 1656 行，一次性移植会淹没在报错里。**按 V1→V6 递进**，每次只加一个特性，跑通再下一步。

### 9.2 对阶段 5、8 降低预期

- `short.c` / `shortprint.c` 依赖**真实 ISA/并口硬件**，QEMU `virt` 机器上没有 → 只能读代码
- `pci_skel` 需要 QEMU 添加 PCI 设备
- `usb-skeleton` 需要 USB 控制器

这几节的定位是**理解原理**，而非完整实测。

---

## 十、参考资料

| 资料 | 位置 |
|---|---|
| LDD3 官方示例代码 | `/home/tcd/Desktop/qemu_driver/lld3-example/` |
| 环境搭建记录 | `driver/doc/buildroot-ext4-migration.md` |
| overlay 机制说明 | `driver/doc/rootfs-overlay-analysis.md` |
| 第 2 章笔记 | `driver/doc/chapter-02.md` |
| 内核源码 | `kernel/linux-6.12.10/` |

---

*文档创建时间：2026-09-21*
