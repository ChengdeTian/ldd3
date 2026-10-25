# 第12章 PCI 驱动

> 本章记录 `driver/chapter-12/pci_skel.c` 骨架驱动在 QEMU aarch64 上的**编译、绑定、解绑**验证流程。
>
> 环境：`qemu-system-aarch64` + `linux-6.12.10` + Buildroot ext4 rootfs + 9p 共享目录，
> 环境搭建过程见 [buildroot-ext4-migration.md](./buildroot-ext4-migration.md)。
> 验证设备是 QEMU 的 e1000 网卡（`8086:100e`），启动脚本已加 `-device e1000`。

---

## 一、总体分析

### 1.1 三个对象与一条主线

PCI 驱动涉及三个核心对象，理解它们的时序，后面所有"为什么 probe 没跑"的问题都能自己回答：

```
硬件/虚拟硬件           内核枚举                 驱动注册                绑定
──────────────         ─────────────           ─────────────         ─────────────
-device e1000   ──>   pci_dev                pci_driver
                        (vendor:device)  <──匹配 id_table──>  .probe()  ──> 已绑定
                                                                                │
                                                          .remove()  <──解绑────┘
```

- **设备（`struct pci_dev`）先存在**：由内核扫描配置空间枚举出来，驱动的存在与否不影响设备本身。
- **驱动（`struct pci_driver`）后注册**：`pci_register_driver()` 之后，总线会拿它去和已有设备（以及将来热插的设备）逐个匹配。
- **匹配的唯一依据是 `id_table`**；`probe()` 只有在匹配成功且设备尚未被别的驱动占用时才会被调用。

### 1.2 `id_table` 与 `MODULE_DEVICE_TABLE`

`struct pci_device_id` 决定"这个驱动认哪些设备"，常用字段：

| 字段 | 含义 | 本章用法 |
|---|---|---|
| `vendor` / `device` | 厂商号 / 设备号 | `8086:100e`（e1000）、`8086:2413`（LDD3 原例） |
| `subvendor` / `subdevice` | 子系统号，`PCI_ANY_ID` 表示不限 | 全部用 `PCI_ANY_ID` |
| `class` / `class_mask` | 按设备类别匹配 | 本章未用 |
| `driver_data` | 私有数据，probe 的 `id->driver_data` 可取 | 本章未用 |

```c
#define PCI_DEVICE(vend, dev) \
	.vendor = (vend), .device = (dev), \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
```

`MODULE_DEVICE_TABLE(pci, ids)` **不创建设备、也不参与绑定**，它只是把表导出到模块 ELF 的
`__mod_pci_device_table` 段，供 `depmod` 生成 `/lib/modules/*/modules.alias`，从而实现
"设备出现 → uevent（MODALIAS）→ modprobe 自动加载"。手动 `insmod` 时这个宏可有可无。

### 1.3 `probe` / `remove` 的职责边界

| 回调 | 何时被调用 | 典型职责 |
|---|---|---|
| `probe(pdev, id)` | 设备与驱动匹配成功 | `pci_enable_device()`、`pci_request_regions()`、`ioremap`、申请 IRQ、注册到子系统、`pci_set_drvdata()` |
| `remove(pdev)` | 设备与驱动**解绑** | 与 probe 严格逆序：注销子系统、释放 IRQ、`iounmap`、`pci_release_regions()`、`pci_disable_device()` |

`remove` 的触发时机只有"解绑"这一类，见 2.2。

---

## 二、使用特点

### 2.1 定位设备：BDF 与 sysfs

PCI 设备在 sysfs 里的名字是 **BDF**（`domain:bus:device.function`），这是后续 bind/unbind 的唯一凭据：

```sh
/sys/bus/pci/devices/0000:00:01.0/
├── vendor        # 0x8086
├── device        # 0x100e
├── revision      # 0x03
├── class
└── driver -> ../../../../bus/pci/drivers/e1000    # 已绑定的驱动（没有则不存在）
```

一条命令列出"设备 → 当前驱动"的对应关系：

```sh
for d in /sys/bus/pci/devices/*/; do
  echo "$(basename $d) $(cat $d/vendor):$(cat $d/device) driver=$(basename $(readlink -f $d/driver 2>/dev/null))"
done
```

### 2.2 绑定、解绑与 `new_id`

**一个设备同一时刻只能有一个驱动。** 内核给设备找驱动时会跳过 `dev->driver != NULL` 的设备，所以：

| 操作 | 效果 |
|---|---|
| `insmod pci_skel.ko` | 只注册驱动；已绑定的设备（如内建 `e1000` 占着的网卡）**不会**被抢走，probe 不执行 |
| `echo "8086 100e" > .../pci_skel/new_id` | 动态加一条 ID；仍然只对**未绑定**的匹配设备生效 |
| `echo BDF > .../drivers/e1000/unbind` | 解绑，设备回到"无驱动"状态（此时 `.../devices/BDF/driver` 消失） |
| `insmod`（设备已空闲）或 `echo BDF > .../drivers/pci_skel/bind` | 触发 probe |
| `echo BDF > .../drivers/pci_skel/unbind` | 只解绑、不卸载模块，触发 remove |

`remove` 的完整触发清单：

| 触发动作 | 内核路径 |
|---|---|
| 驱动注销（`rmmod` 的实质） | `pci_unregister_driver()` → `driver_unregister()` → `driver_detach()` → `__device_release_driver()` → `pci_device_remove()` → `drv->remove()` |
| 手动 unbind | sysfs `unbind` → `device_release_driver()` |
| 设备被移除（热拔 / `echo 1 > .../devices/BDF/remove`） | `pci_stop_and_remove_bus_device()` |
| 父桥/插槽被移除、换驱动重绑定、致命错误恢复 | 同上，递归或强制解绑 |

**不触发** remove 的情况：probe 失败（未绑定）、`suspend/resume`（走 `.suspend/.resume`）、
正常关机（走 `.shutdown`）、`rmmod` 因引用计数失败（exit 根本没执行）。

### 2.3 `dev_info` 的输出去向

`dev_info()` 展开为 `dev_printk(KERN_INFO, dev, ...)`，最终进入 `printk()`：

| 去处 | 查看方式 | 说明 |
|---|---|---|
| 串口控制台 `ttyAMA0` | 直接显示在 QEMU `-nographic` 终端 | 内核命令行 `console=ttyAMA0` |
| 内核环形缓冲区 | `dmesg` | `CONFIG_LOG_BUF_SHIFT=17` → 128 KB |
| `/dev/kmsg` | `cat /dev/kmsg` | 流式读取 |
| 系统日志文件 | `/var/log/messages` | 需要 syslogd，最小 rootfs 默认没有 |

格式拆解：

```
[    3.175034] pci_skel 0000:00:01.0: pci_skel: probe 8086:100e rev 03
└─────┬─────┘ └──┬───┘ └────┬─────┘  └──────────────┬──────────────┘
   时间戳        驱动名      设备名(BDF)            消息正文
              dev_driver_string()      dev_name()
```

这就是 `dev_info` 比 `printk` 更适合驱动的原因：**自带驱动名和设备名**，多设备时不至于分不清是谁打印的。

日志级别：`KERN_ERR=3`、`KERN_WARNING=4`、`KERN_NOTICE=5`、`KERN_INFO=6`、`KERN_DEBUG=7`；
`console_loglevel` 默认 7，所以 `dev_info` 默认能在串口看到。若被调低（`dmesg -n 1`、`loglevel=1`），
就只会留在 buffer 里——表现为"`dmesg` 有、屏幕没有"。

---

## 三、本章实现

### 3.1 代码结构

| 文件 | 说明 |
|---|---|
| `driver/chapter-12/pci_skel.c` | PCI 骨架驱动：`id_table` + `probe` + `remove` |
| `driver/chapter-12/Makefile` | kbuild 外部模块编译，`make install` 拷贝到 `../mydriver/` |

`pci_skel` 不做任何真实硬件操作，只演示"匹配 → probe → 解绑 → remove"这条链路，因此非常适合做移植验证。

### 3.2 `id_table`：为什么多写一个 `8086:100e`

LDD3 原例匹配的是 `8086:2413`（Intel 82801AA IDE 控制器），那是 x86 i440fx 平台的设备；
本章环境是 aarch64 `-M virt`，根本没有这个 ID，因此额外加了一条 **QEMU e1000** 的 ID：

```c
static struct pci_device_id pci_skel_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801AA_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x100e) },   /* QEMU e1000 */
	{ 0, }                                         /* 表尾必须以全 0 结束 */
};
```

### 3.3 `probe`：先 enable，再判断板子

```c
static int pci_skel_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	int ret;

	ret = pci_enable_device(dev);
	if (ret) {
		dev_err(&dev->dev, "pci_skel: pci_enable_device failed (%d)\n", ret);
		return ret;
	}

	dev_info(&dev->dev, "pci_skel: probe %04x:%04x rev %02x\n",
		 dev->vendor, dev->device, skel_get_revision(dev));
	return 0;
}
```

两个容易被忽略的点：

1. `pci_enable_device()` 会唤醒设备、分配 BAR 的 I/O 与内存资源，**必须检查返回值**，失败就直接返回错误码；
2. `probe` 失败后内核不会再调用 `remove`，所以失败路径上申请到的东西要自己释放（`pci_disable_device()` 等）。

> LDD3 原例还有一条 `revision == 0x42 → -ENODEV` 的判断（用于拒绝不支持的板子）。
> 调试阶段该判断被注释掉，同时在 probe 里加了一行 `printk(KERN_ALERT "pci_skel: revision=%02x\n", ...)`，
> 用来确认配置空间读到的值。调试完成后建议改回 `dev_info(&dev->dev, ...)`，让日志带上 BDF。

### 3.4 `remove`：与 probe 逆序释放

```c
static void pci_skel_remove(struct pci_dev *dev)
{
	dev_info(&dev->dev, "pci_skel: remove\n");
	pci_disable_device(dev);
}
```

**注意 Linux 6.12 的签名是 `void (*remove)(struct pci_dev *)`**，不是 LDD3 的 `int`：

```951:951:kernel/linux-6.12.10/include/linux/pci.h
	void (*remove)(struct pci_dev *dev);	/* Device removed (NULL if not a hot-plug capable driver) */
```

写成 `int` 会得到 `-Wincompatible-pointer-types` 告警；若开了 `-Werror` 直接编译失败。

### 3.5 `pci_driver` 与 `module_pci_driver`

```c
static struct pci_driver pci_skel_driver = {
	.name     = "pci_skel",
	.id_table = pci_skel_ids,
	.probe    = pci_skel_probe,
	.remove   = pci_skel_remove,
};

module_pci_driver(pci_skel_driver);
```

`module_pci_driver()` 展开为 `module_init(pci_register_driver)` + `module_exit(pci_unregister_driver)`，
等价于手写：

```c
static int __init pci_skel_init(void) { return pci_register_driver(&pci_skel_driver); }
static void __exit pci_skel_exit(void) { pci_unregister_driver(&pci_skel_driver); }
module_init(pci_skel_init);
module_exit(pci_skel_exit);
```

---

## 四、测试

### 4.1 环境准备：加一块 e1000

`kernel/linux-6.12.10/qemu_start.sh` 的 ext4 分支追加：

```sh
        -device e1000,netdev=n0 \
        -netdev user,id=n0 \
```

启动后 guest 里会多出一个 `8086:100e` 设备。

**关键前提**：本章内核 `CONFIG_E1000=y`（内建），开机枚举到这块网卡就立刻绑定了 `e1000` 驱动。
骨架驱动不是"更优先"，所以必须先把设备从 `e1000` 解绑，否则 `insmod` 只会加载模块、不会 probe。

### 4.2 host：编译并共享

```bash
cd /home/tcd/Desktop/qemu_driver/driver/chapter-12
make            # 交叉编译 pci_skel.ko
make install    # 拷贝到 ../mydriver/（9p 实时共享，guest 立即可见）
```

### 4.3 guest：完整测试步骤

```sh
# 1) 挂载 9p
mkdir -p /mnt
mount -t 9p -o trans=virtio,version=9p2000.L host_drv /mnt

# 2) 找到 e1000 的 BDF
BDF=$(for d in /sys/bus/pci/devices/*; do
        [ "$(cat $d/vendor):$(cat $d/device)" = "0x8086:0x100e" ] && basename $d
      done | head -1)
echo "BDF=$BDF"                      # 0000:00:01.0

# 3) 从内建 e1000 驱动解绑（关键一步）
echo $BDF > /sys/bus/pci/drivers/e1000/unbind

# 4) 加载骨架驱动 → 自动匹配并 probe
insmod /mnt/pci_skel.ko

# 5) 确认已绑定
ls /sys/bus/pci/drivers/pci_skel/    # 应出现 0000:00:01.0
dmesg | grep pci_skel

# 6) 卸载 → remove
rmmod pci_skel
dmesg | grep pci_skel | tail -3
```

### 4.4 实测记录（QEMU 串口原始输出）

设备与初始驱动：

```
0000:00:00.0 0x1b36:0x0008 driver=driver          (PCIe root port)
0000:00:01.0 0x8086:0x100e driver=e1000           (测试网卡)
0000:00:02.0 0x1af4:0x1001 driver=virtio-pci      (virtio-blk，根文件系统，勿动)
```

解绑 → 加载 → probe：

```
BDF=0000:00:01.0
echo 0000:00:01.0 > /sys/bus/pci/drivers/e1000/unbind
UNBIND_RC=0

insmod /mnt/pci_skel.ko
[    3.171119] pci_skel: loading out-of-tree module taints kernel.
[    3.174871] pci_skel: revision=03
[    3.175034] pci_skel 0000:00:01.0: pci_skel: probe 8086:100e rev 03
INSMOD_RC=0
```

绑定结果：

```
ls /sys/bus/pci/drivers/pci_skel/
0000:00:01.0  bind  module  new_id  remove_id  uevent  unbind
```

卸载 → remove：

```
rmmod pci_skel
[    3.432748] pci_skel 0000:00:01.0: pci_skel: remove
RMMOD_RC=0
```

### 4.5 结果

| 检查点 | 预期 | 实测 |
|---|---|---|
| 设备枚举 | 出现 `8086:100e`，初始驱动为 `e1000` | ✅ `0000:00:01.0` |
| 解绑 | `.../devices/BDF/driver` 消失 | ✅ `UNBIND_RC=0` |
| `insmod` | probe 被调用并打印 vendor:device/rev | ✅ `probe 8086:100e rev 03` |
| sysfs 绑定 | `/sys/bus/pci/drivers/pci_skel/` 下出现 BDF | ✅ |
| `rmmod` | remove 被调用 | ✅ `pci_skel: remove` |

结论：**`insmod → pci_register_driver → probe`** 与 **`rmmod → pci_unregister_driver → remove`**
两条链路均验证通过。

### 4.6 其他验证路径

```sh
# 不解绑、用 new_id 动态加 ID（只对未绑定设备有效）
insmod /mnt/pci_skel.ko
echo "1b36 0005" > /sys/bus/pci/drivers/pci_skel/new_id    # 需设备处于未绑定状态

# 手动绑定/解绑（模块不卸载）
echo 0000:00:01.0 > /sys/bus/pci/drivers/pci_skel/bind
echo 0000:00:01.0 > /sys/bus/pci/drivers/pci_skel/unbind

# 换一个没有内核驱动认领的设备做靶子（QEMU 支持，不会和 e1000 抢）
#   启动参数：-device pci-testdev        （1b36:0005）
```

---

## 五、2.6 → 6.12 移植点

| LDD3（2.6） | Linux 6.12 | 说明 |
|---|---|---|
| `int (*probe)(struct pci_dev *, const struct pci_device_id *)` | 同签名 | 未变 |
| `int (*remove)(struct pci_dev *)` | `void (*remove)(struct pci_dev *)` | 6.11 起改为 void，写 `int` 会告警 |
| `pci_module_init(&drv)` | `pci_register_driver(&drv)` | 老符号已删除 |
| `__devinit` / `__devexit` / `__devexit_p()` | 全部删除 | 直接去掉这些标注 |
| `pci_request_regions()` + `ioremap()` + `pci_release_regions()` | 可用；更推荐 `pcim_*` 托管 API | `pcim_iomap_regions()` 等在 remove 时自动回收 |
| `pci_enable_device()` | 同 | 与 `pci_disable_device()` 必须配对 |
| `MODULE_DEVICE_TABLE(pci, ids)` | 同 | 仅用于 modprobe alias |
| `pci_set_drvdata` / `pci_get_drvdata` | 同 | 保存私有数据 |
| `dev_info` / `dev_err` | 同（`<linux/dev_printk.h>`） | 自带驱动名 + BDF 前缀 |

---

## 六、踩坑记录

| # | 现象 | 原因 | 解决 |
|---|---|---|---|
| 1 | `insmod` 返回 0，但 `dmesg` 没有 probe、sysfs 里没有设备链接 | 设备已被内建 `e1000` 绑定，内核不会给"已有驱动的设备"再找驱动 | 先 `echo BDF > /sys/bus/pci/drivers/e1000/unbind`，再 `insmod`（或手动 `bind`） |
| 2 | 以为 `new_id` 能抢设备 | `new_id` 也走 `driver_attach()`，同样跳过已绑定设备 | 先解绑，再加 ID / 再 bind |
| 3 | `echo 0000:00:01.0 > .../unbind` 报 `No such device` | 写进去的字符串不是纯 BDF（如带了 `BDF=` 前缀、空白、换行） | 用 `echo $BDF`，`$BDF` 只含 `0000:00:01.0` |
| 4 | `rmmod` 成功但没有 remove 打印 | ① `exit` 里没调 `pci_unregister_driver()`；② 设备从未绑定；③ 用了 `module_pci_driver` 又手写 exit 覆盖 | 检查 `exit`；用 `ls .../drivers/pci_skel/` 确认是否绑定 |
| 5 | `rmmod` 报 `Module pci_skel is in use` | 有别的引用（如被占用、正在 probe） | `lsmod` 查 `Used by`，先解绑/关掉使用者 |
| 6 | 编译告警 `incompatible pointer type` | `.remove` 写成 `int` | 6.12 用 `void pci_skel_remove(struct pci_dev *)` |
| 7 | 日志里只有 `pci_skel: revision=03`，没有设备名 | 用了 `printk` 而非 `dev_info`，没有设备前缀 | 改成 `dev_info(&dev->dev, ...)` |
| 8 | `dmesg` 有、串口没有 | `console_loglevel` 被调低 | `echo 8 > /proc/sys/kernel/printk` 或 `dmesg -n 8` |
| 9 | probe 里 `pci_enable_device()` 失败但 probe 仍返回成功 | 没检查返回值 | `if (ret) return ret;` |
| 10 | 第二次 `insmod` 报 `File exists` | 模块已加载 | 先 `rmmod pci_skel` |
| 11 | 对 `0000:00:02.0`（virtio-blk）做 unbind 后系统卡死 | 它是根文件系统所在设备 | 只用测试网卡/测试设备做实验 |

---

## 七、命令速查

```bash
# ---------- host ----------
cd /home/tcd/Desktop/qemu_driver/driver/chapter-12
make && make install                     # 编译 + 拷到 ../mydriver/

cd /home/tcd/Desktop/qemu_driver/kernel/linux-6.12.10
./qemu_start.sh                          # 已带 -device e1000

# ---------- guest ----------
mkdir -p /mnt
mount -t 9p -o trans=virtio,version=9p2000.L host_drv /mnt

# 列出设备与当前驱动
for d in /sys/bus/pci/devices/*/; do
  echo "$(basename $d) $(cat $d/vendor):$(cat $d/device) driver=$(basename $(readlink -f $d/driver 2>/dev/null))"
done

# 解绑内建驱动 → 加载骨架驱动
BDF=0000:00:01.0
echo $BDF > /sys/bus/pci/drivers/e1000/unbind
insmod /mnt/pci_skel.ko

# 观察
ls /sys/bus/pci/drivers/pci_skel/
dmesg | grep pci_skel

# 卸载 / 解绑 / 重绑
rmmod pci_skel
echo $BDF > /sys/bus/pci/drivers/pci_skel/unbind
echo $BDF > /sys/bus/pci/drivers/pci_skel/bind

# 动态加 ID（设备必须未被绑定）
echo "8086 100e" > /sys/bus/pci/drivers/pci_skel/new_id
```
