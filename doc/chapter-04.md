# 第4章 调试技术：procfs 与 seq_file

> 本章在第三章 scull 的基础上，给驱动加上 **procfs 调试接口**，并对比"老式 `read_proc`"与
> "新式 `seq_file`"两种写法。
>
> 环境搭建见 [buildroot-ext4-migration.md](./buildroot-ext4-migration.md)；
> 驱动的编译 / 加载 / 基础测试流程见 [第三章](./chapter-03.md)，本章只讲**新增的调试接口**。

---

## 一、代码结构

| 文件 | 说明 |
|---|---|
| `driver/chapter-04/scull.c` | 驱动主体 + `/proc` 调试接口 |
| `driver/chapter-04/scull.h` | 数据结构与宏定义 |
| `driver/chapter-04/scull_test.c` | 用户态测试程序（10 组 / 33 条断言） |
| `driver/chapter-04/Makefile` | 交叉编译 + `DEBUG` 开关 |

本章相对第三章的**全部变化**：

1. 新增两个 `/proc` 文件：`/proc/scullmem`（`proc_create_single`）、`/proc/scullseq`（`proc_create_seq`）；
2. 老式 `read_proc` 实现被 `#if 0` 包起来废弃（保留作对照）；
3. `Makefile` 增加 `DEBUG` 开关（决定是否编译调试代码）。

---

## 二、为什么放弃老式 `read_proc`

本章 `scull.c` 里用 `#if 0` 保留了老写法，它就是内核 2.6 时代的接口：

```c
/* 已被淘汰的写法 */
int scull_read_procmem(char *buf, char **start, off_t offset, int count,
                       int *eof, void *data)
{
    int i, j, len = 0;
    int limit = count - 80;                 /* ← 靠"留 80 字节余量"防越界 */

    for (i = 0; i < scull_devs && len <= limit; i++) {
        ...
        len += sprintf(buf + len, " item at %p, qset at %p\n", qs, qs->data);
        ...
    }
    *eof = 1;                               /* ← 无条件宣告 EOF */
    return len;
}
```

三个硬伤：

| # | 问题 | 说明 |
|---|---|---|
| 1 | **没有硬边界检查** | `limit = count - 80` 只是软限制，单条记录超过 80 字节仍会写越界；`count < 80` 时 `limit` 变负，**一条都不输出** |
| 2 | **无条件 `*eof = 1`** | 即使内容被 `count` 截断也宣告结束，用户无法续读，内容是**静默截断**的 |
| 3 | **必须自己管 `buf`/`len`/`limit`** | 每个驱动重复造轮子，且极易出错 |

内核在 **3.10 直接把 `create_proc_entry()` / `create_proc_read_entry()` 和 `read_proc` 回调整体删除**。
现在 `struct proc_dir_entry` 变成了**不透明类型**——头文件里只剩前向声明，你根本拿不到结构体去填字段：

```c
/* include/linux/proc_fs.h:12 */
struct proc_dir_entry;
```

同时 `proc_dir_entry->proc_fops`（`struct file_operations *`）也被 `struct proc_ops *` 取代，
而且新增了 `proc_create_single()` / `proc_create_seq()` 这类**更高层的封装**，连 `proc_ops` 都不用自己写。

---

## 三、seq_file 的核心思想：控制反转

`seq_file` 是一套**框架**：迭代、分页、缓冲扩容、拷贝到用户态、支持 `lseek` 这些"脏活"全由内核包办，
驱动只提供 4 个回调。

```
用户 cat /proc/scullseq
 │
 ├─ VFS → proc_open()                          【内核】
 │         └─ seq_open(file, &scull_seq_ops)    【内核】把 ops 存进 file->private_data
 │
 ├─ VFS → seq_read() / seq_read_iter()          【内核】通用循环
 │         ├─ op->start(s, &pos)   → 迭代起点     【驱动】
 │         ├─ op->show(s, v)       → 输出一条记录 【驱动】
 │         ├─ op->next(s, v, &pos) → 下一条       【驱动】
 │         ├─ ... 直到 start/next 返回 NULL
 │         └─ op->stop(s, v)       → 收尾         【驱动】
 │
 └─ VFS → seq_lseek() / seq_release()            【内核】
```

### 4 个回调的签名与职责

| 回调 | 签名 | 职责 | 注意 |
|---|---|---|---|
| `start` | `void *(*)(struct seq_file *, loff_t *pos)` | 返回第 `pos` 条记录（**位置就是 `*pos`**） | 越界返回 `NULL` |
| `next` | `void *(*)(struct seq_file *, void *v, loff_t *pos)` | 返回下一条 | **必须推进 `*pos`** |
| `stop` | `void (*)(struct seq_file *, void *v)` | 收尾（可为空） | **返回 `void`**，不是 `void *` |
| `show` | `int (*)(struct seq_file *, void *v)` | 用 `seq_printf()` 输出这条记录 | 返回 `0`=正常，`>0`=跳过这条，`<0`=硬错误 |

> ⚠️ **`stop` 的返回类型是 `void`**。写成 `void *` 会报
> `error: initialization of 'void (*)(struct seq_file *, void *)' from incompatible pointer type`。
> 这是本章实际踩到的坑（见第九节）。

> ⚠️ **`next` 必须推进 `*pos`**，否则内核会直接点你的名：
>
> ```c
> /* fs/seq_file.c:262 */
> p = m->op->next(m, p, &m->index);
> if (pos == m->index) {
>     pr_info_ratelimited("buggy .next function %ps did not update position index\n",
>                         m->op->next);
>     m->index++;
> }
> ```
>
> 内核会兜底 `m->index++`（不至于死循环），但 dmesg 里会刷警告。

### `seq_printf()` 会自动处理分页与扩容

这是 `seq_file` 最实用的地方——输出超过缓冲时**自动翻倍重跑**：

```c
/* fs/seq_file.c:239 */
if (!seq_has_overflowed(m))     // got it
    goto Fill;
// need a bigger buffer
m->op->stop(m, p);
kvfree(m->buf);
m->count = 0;
m->buf = seq_buf_alloc(m->size <<= 1);
if (!m->buf)
    goto Enomem;
p = m->op->start(m, &m->index);
```

因为溢出重试时 `next` 还没被调用、`m->index` 仍是 0，`start` 会再次给出同一条记录，
于是 `show` 用更大的缓冲**重写一遍**——所以用 `seq_file` 的输出**不会被截断**。

---

## 四、三种注册方式：区别就在"谁来提供 ops"

| API | 要不要 `seq_operations` | 说明 |
|---|---|---|
| `proc_create_single(name, mode, parent, show)` | **不用** | 内核用 `single_start/next/stop` + 你的 `show` 现造一个。语义="一次性把全部内容输出" |
| `proc_create_seq(name, mode, parent, &seq_ops)` | **必须给** | 你提供完整的 `seq_operations`，支持多记录迭代与 `lseek` 定位续读 |
| `proc_create(name, mode, parent, &proc_ops)` | 不用（但要 `proc_ops`） | 最底层，自己控 open/read/release |

对应的还有带私有数据的变体：`proc_create_single_data(..., show, data)` / `proc_create_seq_data(...)`，
`show` 里通过 `s->private` 取出注册时传入的 `data`。

### 为什么 `proc_create_single` 不需要 ops

因为它内部走 `single_open()`，**内核帮你 `kmalloc` 了一个 `seq_operations`**：

```c
/* fs/seq_file.c:572 */
int single_open(struct file *file, int (*show)(struct seq_file *, void *),
                void *data)
{
    struct seq_operations *op = kmalloc(sizeof(*op), GFP_KERNEL_ACCOUNT);
    ...
    op->start = single_start;      /* 内核固定的"只迭代一轮"实现 */
    op->next  = single_next;
    op->stop  = single_stop;
    op->show  = show;              /* ← 只把你的 show 塞进去 */
    res = seq_open(file, op);
    ...
}
```

而 `proc_create_seq_private()` 则是**直接存你给的 ops**，不代劳：

```c
/* fs/proc/generic.c:615 */
p->proc_ops = &proc_seq_ops;
p->seq_ops  = ops;          /* ← 你的 scull_seq_ops */
```

### ⚠️ open 与 release 必须配对

| open | 必须配 | 原因 |
|---|---|---|
| `single_open()` | `single_release()` | `single_open` 内部 **kmalloc 了 ops**，`single_release` 负责 `kfree` |
| `seq_open()` | `seq_release()` | ops 是你定义的静态变量，只需释放 `seq_file` |

```c
/* fs/seq_file.c:611 */
int single_release(struct inode *inode, struct file *file)
{
    const struct seq_operations *op = ((struct seq_file *)file->private_data)->op;
    int res = seq_release(inode, file);
    kfree(op);                  /* ← 就是这里 */
    return res;
}
```

用错就是**内存泄漏**（`single_open` + `seq_release`）。

> 好消息：用 `proc_create_single()` / `proc_create_seq()` 时，内核已经把 `proc_ops` 全部配好了
> （`proc_single_ops` / `proc_seq_ops`），**根本不用自己操心配对**。

---

## 五、本章 scull 的实现

两个 proc 文件采用不同机制，正好对照：

```c
static void scull_create_proc(void)
{
    proc_create_single("scullmem", 0444, NULL, scullmem_show);
    proc_create_seq("scullseq", 0444, NULL, &scull_seq_ops);
}

static void scull_remove_proc(void)
{
    remove_proc_entry("scullmem", NULL);
    remove_proc_entry("scullseq", NULL);
}
```

| 文件 | 机制 | 迭代方式 | 输出超一页 |
|---|---|---|---|
| `/proc/scullmem` | `proc_create_single` | `scullmem_show()` 一次遍历所有设备 | **会被截断**（单块缓冲，默认 `PAGE_SIZE`） |
| `/proc/scullseq` | `proc_create_seq` | position = 设备号，每个设备一次 `show` | **自动扩容，完整输出** |

### 方式一：`single`（一次性输出）

只需要一个 `show`，把所有内容一次性写出去：

```c
static int scullmem_show(struct seq_file *s, void *v)
{
    int i, j;

    for (i = 0; i < scull_devs; i++) {
        struct scull_dev *d = &scull_devices[i];
        struct scull_qset *qs = d->data;

        if (down_interruptible(&d->sem))
            return -ERESTARTSYS;

        seq_printf(s, "\nDevice %i: qset: %i, q: %i, sz: %li\n",
                   i, d->qset, d->quantum, d->size);
        for (; qs; qs = qs->next) {
            seq_printf(s, " item at %p, qset at %p\n", qs, qs->data);
            if (qs->data && !qs->next)          /* 只展开最后一个节点 */
                for (j = 0; j < d->qset; j++)
                    if (qs->data[j])
                        seq_printf(s, " %4i: %8p\n", j, qs->data[j]);
        }
        up(&d->sem);
    }
    return 0;
}
```

### 方式二：`seq`（逐项迭代）

"position = 设备号"，每个设备输出一条记录：

```c
static void *scull_seq_start(struct seq_file *s, loff_t *pos)
{
    return *pos < scull_devs ? &scull_devices[*pos] : NULL;
}

static void *scull_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
    (*pos)++;                       /* 必须推进！ */
    if (*pos >= scull_devs)
        return NULL;
    return scull_devices + *pos;
}

static void scull_seq_stop(struct seq_file *s, void *v)
{
    /* 返回 void，没有资源要释放 */
}

static int scull_seq_show(struct seq_file *s, void *v)
{
    struct scull_dev *dev = (struct scull_dev *)v;
    struct scull_qset *d;
    int i;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;

    seq_printf(s, "\nDevice %i: qset %i, q %i, sz %li\n",
               (int)(dev - scull_devices), dev->qset, dev->quantum, dev->size);
    for (d = dev->data; d; d = d->next) {
        seq_printf(s, "  item at %p, qset at %p\n", d, d->data);
        if (d->data && !d->next)
            for (i = 0; i < dev->qset; i++)
                if (d->data[i])
                    seq_printf(s, " % 4i: %8p\n", i, d->data[i]);
    }
    up(&dev->sem);
    return 0;
}

static struct seq_operations scull_seq_ops = {
    .start = scull_seq_start,
    .next  = scull_seq_next,
    .stop  = scull_seq_stop,
    .show  = scull_seq_show,
};
```

> **锁为什么加在 `show` 里而不是 open 里？** 因为 `show` 会被**多次调用**，且两次调用之间会睡眠
> （等待用户态把缓冲读走）。若在 open 时持锁，就会跨越睡眠持有信号量，还可能死锁。

### 挂到模块的 init / exit

```c
static int __init scull_init(void)
{
    ...
    for (i = 0; i < scull_devs; i++) { ... scull_setup_cdev(...); }

#ifdef SCULL_DEBUG
    scull_create_proc();            /* 在 cdev 全部建好之后 */
#endif
    return 0;
}

static void __exit scull_exit(void)
{
#ifdef SCULL_DEBUG
    scull_remove_proc();            /* 先摘 proc，再释放数据 */
#endif
    if (scull_devices) {
        for (i = 0; i < scull_devs; i++) {
            cdev_del(&scull_devices[i].cdev);
            scull_trim(scull_devices + i);
        }
        kfree(scull_devices);
    }
    unregister_chrdev_region(devno, scull_devs);
}
```

> `scull_remove_proc()` 必须在 `kfree(scull_devices)` **之前**。否则在"数据已释放、proc 文件还在"的
> 窗口里，别人 `cat /proc/scullmem` 会遍历已释放内存（6.12 的 `proc_dir_entry` 已经没有 `owner`
> 字段，proc 文件不给模块加引用计数，只有 `remove_proc_entry()` 会阻塞等待已打开的调用者退出）。

---

## 六、编译开关：`DEBUG` / `SCULL_DEBUG`

`chapter-04/scull.c` 里调试代码被 `#ifdef SCULL_DEBUG` 包着，所以 **Makefile 必须把它定义出来**。

### ❌ 无效写法：LDD3 的 `CFLAGS += $(DEBFLAGS)`

LDD3 原版 Makefile 是这样：

```makefile
ifeq ($(DEBUG),y)
  DEBFLAGS = -O -g -DSCULL_DEBUG
else
  DEBFLAGS = -O2
endif
CFLAGS += $(DEBFLAGS)
```

但**外部模块构建是 kbuild 驱动的，`CFLAGS` 会被完全忽略**（连 `DEBFLAGS` 定义了也没人用），
所以 `-DSCULL_DEBUG` 根本没生效，`nm scull.ko | grep proc` 一个符号都没有。

### ✅ 正确写法：`ccflags-y` + 显式传给子 make

```makefile
# 顶层：调试开关
DEBUG ?= y

modules:
	$(MAKE) -C $(KERNELDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(PWD) DEBUG=$(DEBUG) modules

else
# kbuild 分支（KERNELRELEASE 非空）
obj-m += $(MODULE_NAME).o

ifeq ($(DEBUG),y)
ccflags-y += -DSCULL_DEBUG
endif
endif
```

两个关键点：

1. **必须用 `ccflags-y`**（kbuild 专有变量），`CFLAGS` 无效；
2. **必须显式 `DEBUG=$(DEBUG)` 传给子 make**——kbuild 会**第二次读取同一个 Makefile**（此时
   `KERNELRELEASE` 非空走 `else` 分支），而普通 make 变量不会自动传递，只有命令行变量会。

### 验证方法

```bash
make                # 默认 DEBUG=y
aarch64-linux-gnu-nm scull.ko | grep -i "proc\|seq"
# 00000000000002f4 t scullmem_show
# 000000000000007c t scull_seq_next
# 0000000000000130 d scull_seq_ops
# 00000000000001a4 t scull_seq_show
# 000000000000004c t scull_seq_start
# 00000000000000b8 t scull_seq_stop
#                  U proc_create_seq_private
#                  U proc_create_single_data
#                  U remove_proc_entry
#                  U seq_printf

make DEBUG=n && aarch64-linux-gnu-nm scull.ko | grep -c "scullmem\|scull_seq"
# 0                          ← 调试代码被完全排除
```

`.ko` 体积对比：`DEBUG=n` 63688 字节 → `DEBUG=y` 69840 字节。

---

## 七、测试

### `scull_test.c` 覆盖范围（10 组 / 33 条断言）

| # | 分组 | 验证内容 |
|---|---|---|
| 1 | 打开与清空 | `stat` 确认字符设备；`O_WRONLY` 打开触发 `scull_trim()` |
| 2 | 基础读写 | 写 11 字节 → 读回一致；`SEEK_END` 返回 size |
| 3 | **短读语义** | 写 `2*quantum+100`，连续三次 `read()` 分别返回 **4000 / 4000 / 100** |
| 4 | 循环读回比对 | `read_all()` 读满 8100 字节，逐字节 `memcmp` |
| 5 | lseek 三种 whence | `SEEK_END` / `SEEK_END-5` / `SEEK_CUR` |
| 6 | 非法参数 | `whence=999`（VFS 拦截）、`lseek(-1, SEEK_SET)`（驱动返回） |
| 7 | 跨 quantum 边界 | 在 `quantum-4` 与 `quantum` 各写标记再读回 |
| 8 | **跨 qset 边界** | 在 `itemsize-4` 与 `itemsize` 写标记，触发第 2 个链表节点新建 |
| 9 | **proc 调试接口** | 读 `/proc/scullmem`、`/proc/scullseq` 并打印内容 |
| 10 | `O_WRONLY` 截断 | trim 后 `read` 返回 0、`SEEK_END` 返回 0 |

用法（`quantum` / `qset` 可用命令行覆盖，默认 4000 / 500）：

```bash
/mnt/scull_test                          # 用 /dev/scull0
/mnt/scull_test /dev/scull1              # 测另一个设备
/mnt/scull_test /dev/scull0 4000 500     # 显式指定参数
```

> 为什么不能从 sysfs 自动读取 `quantum`/`qset`？因为 `module_param(..., 0)` 的参数**不会出现在 sysfs**
> ——内核里 `if (kparam[i].perm == 0) continue;`（`kernel/params.c`）直接跳过。

### 测试步骤

```bash
# ---------- host ----------
cd /home/tcd/Desktop/qemu_driver/driver/chapter-04
make && make install && make test     # 模块 + 测试程序，一起推到 9p 共享目录

cd /home/tcd/Desktop/qemu_driver/kernel/linux-6.12.10
./qemu_start.sh

# ---------- guest ----------
rmmod scull 2>/dev/null
insmod /mnt/scull.ko
M=$(grep scull /proc/devices | cut -d' ' -f1)
rm -f /dev/scull0 && mknod /dev/scull0 c $M 0

echo "hello scull" > /dev/scull0      # 先用 shell 做基础验证
cat /dev/scull0

cat /proc/scullmem                    # single_open 版 dump
cat /proc/scullseq                    # seq_file 版 dump

/mnt/scull_test /dev/scull0           # 完整测试套件
```

### 实测记录（QEMU 串口原始输出）

```text
~ # insmod /mnt/scull.ko && echo ===INSMOD_OK===
[   18.793132] scull: loading out-of-tree module taints kernel.
[   18.798343] Hello, world!
===INSMOD_OK===
~ # set -- $(grep scull /proc/devices); M=$1; rm -f /dev/scull0; mknod /dev/scull0 c $M 0; echo MAJOR=$M
MAJOR=511
~ # /mnt/scull_test /dev/scull0; echo "===EXIT=$?==="
==================== scull 驱动测试 ====================
设备     : /dev/scull0
quantum  : 4000 字节
qset     : 500
itemsize : 2000000 字节 (quantum * qset)

--- 1. 打开设备并清空 ---
  [ OK ] /dev/scull0 是字符设备 (major=511 minor=0)
  [ OK ] 以 O_WRONLY 打开再关闭 → 触发 scull_trim()，设备已清空
  [ OK ] open(/dev/scull0, O_RDWR) = 3

--- 2. 基础读写 ---
  [ OK ] 写入 11 字节 "hello scull" (返回 11)
  [ OK ] lseek(0, SEEK_SET) = 0
  [ OK ] 读回 11 字节: "hello scull"
  [ OK ] lseek(0, SEEK_END) = 11（此时 size 应为 11）

--- 3. 短读语义：单次 read/write 只处理一个 quantum ---
  [ OK ] 清空设备
  [ OK ] 循环写入 8100 字节（= 2*quantum + 100），实际写入 8100
  [ OK ] 第 1 次 read(4512) 返回 4000 字节（应 == quantum = 4000）
  [ OK ] 第 2 次 read 返回 4000 字节（应 == quantum）
  [ OK ] 第 3 次 read 返回 100 字节（只剩 100 字节）

--- 4. 循环读回并逐字节比对 ---
  [ OK ] 读回 8100 字节，与写入内容完全一致

--- 5. lseek: SEEK_SET / SEEK_CUR / SEEK_END ---
  [ OK ] lseek(0, SEEK_END) = 8100（size = 8100）
  [ OK ] lseek(-5, SEEK_END) = 8095
  [ OK ] 从 size-5 处读 5 字节，内容一致
  [ OK ] lseek(10, SEEK_CUR) = 10
  [ OK ] lseek(0, SEEK_CUR) 不移动位置 = 10

--- 6. 非法 lseek 参数 ---
  [ OK ] lseek(whence=999) → -1, errno=EINVAL（VFS 层就拦截了）
  [ OK ] lseek(-1, SEEK_SET) → -1, errno=EINVAL（驱动 scull_llseek 返回）

--- 7. 跨 quantum 边界定位写读 ---
  [ OK ] 在 offset 3996 (quantum-4) 写入 "END0"
  [ OK ] 在 offset 4000 (quantum)   写入 "BEG1"
  [ OK ] offset 3996 读回 "END0"
  [ OK ] offset 4000 读回 "BEG1"

--- 8. 跨 qset 边界定位写读（第 2 个链表节点） ---
  [ OK ] 在 offset 1999996 (itemsize-4，第 0 个 qset 的末尾) 写入 "E0QS"
  [ OK ] 在 offset 2000000 (itemsize，进入第 1 个 qset → 新建链表节点) 写入 "B1QS"
  [ OK ] offset 1999996 读回 "E0QS"
  [ OK ] offset 2000000 读回 "B1QS"

--- 9. proc 调试接口（需编译时定义 SCULL_DEBUG） ---
  [ OK ] /proc/scullmem 读到 274 字节，包含 "Device 0"
         |
         | Device 0: qset: 500, q: 4000, sz: 2000004
         |  item at 00000000357f0aaa, qset at 00000000b783b4a3
         |  item at 000000009cf50c3c, qset at 000000009a8f6d64
         |     0: f6323574
         |
         | Device 1: qset: 500, q: 4000, sz: 0
         |
  [ OK ] /proc/scullseq 读到 264 字节，包含 "Device 0"
         |
         | Device 0: qset 500, q 4000, sz 2000004
         |   item at 00000000357f0aaa, qset at 00000000b783b4a3
         |   item at 000000009cf50c3c, qset at 000000009a8f6d64
         |     0: f6323574
         |
         | Device 1: qset 500, q 4000, sz 0
         |

--- 10. O_WRONLY 截断语义 ---
  [ OK ] 以 O_WRONLY 打开再关闭 → scull_trim()
  [ OK ] 截断后立即读 → 0 字节（EOF，size 已归零）
  [ OK ] lseek(0, SEEK_END) = 0（size = 0）

==================== 测试结果 ====================
PASS = 33, FAIL = 0, SKIP = 0
全部通过
===EXIT=0===
```

从上面的 dump 可以读出几个信息：

- `Device 0: ... sz: 2000004` —— 第 7、8 组测试写到 `itemsize+4 = 2000004`，size 正确；
- **两个 `item at ...`** —— 第 8 组在 `itemsize` 处写入，驱动通过 `scull_follow()` **新建了第 2 个链表节点**；
- `0: f6323574` —— 只展开了最后一个节点（`!d->next`），且**只列非空槽位**；
- 地址是**哈希值**（现代内核 `%p` 默认不泄漏真实地址，见第十节）。

---

## 八、踩坑记录

| # | 现象 | 原因 | 解决 |
|---|---|---|---|
| 1 | `make` 成功，但 `/proc/scullmem`、`/proc/scullseq` 根本不存在 | 调试代码在 `#ifdef SCULL_DEBUG` 里，而 Makefile 没定义该宏（`nm scull.ko` 查不到任何 proc 符号） | Makefile 用 `ccflags-y += -DSCULL_DEBUG`，并显式把 `DEBUG` 传给子 make |
| 2 | LDD3 那套 `DEBFLAGS` / `CFLAGS += ...` 完全不起作用 | 外部模块由 kbuild 驱动，**`CFLAGS` 被忽略** | 改用 kbuild 的 `ccflags-y` |
| 3 | 打开 `SCULL_DEBUG` 后编译失败 | `scull_seq_stop()` 写成 `static void *`，而 `seq_operations.stop` 要求返回 `void`（内核带 `-Werror=incompatible-pointer-types`） | 改成 `static void scull_seq_stop(...)` |
| 4 | `warning: this 'if' clause does not guard...` | `scull_seq_show()` 里 tab 与空格缩进混用，`return` 和下一个 `seq_printf` 视觉同列 | 统一缩进（全 tab 或全 4 空格） |
| 5 | 卸载模块瞬间 `cat /proc/scullmem` 可能崩溃 | `kfree(scull_devices)` 在 `remove_proc_entry()` **之前**；proc 文件不给模块加引用计数 | `scull_remove_proc()` 提前到释放数据之前 |
| 6 | `/proc/scullmem` 输出被截断 | `proc_create_single` 走 `single_open`，一次性写进单块缓冲（默认 `PAGE_SIZE`） | 需要完整输出就用 `proc_create_seq` + `seq_operations`（自动扩容续读） |
| 7 | 用 `single_open()` 却配 `seq_release()` | `single_open` 内部 `kmalloc` 了 ops，`seq_release` 不释放它 | 配对使用 `single_release()`（用 `proc_create_single` 时内核已代劳） |
| 8 | dmesg 刷 `buggy .next function ... did not update position index` | `next` 回调没推进 `*pos` | `next` 里 `++*pos` |
| 9 | dump 出来的地址不是真实地址 | 现代内核 `%p` 默认**哈希**指针（`lib/vsprintf.c` 的 `default_pointer()`），要 `no_hash_pointers` 启动参数或 `%px` 才是真地址 | 属预期行为；哈希值仍可用于判断"是否同一对象" |

---

## 九、参考：本章涉及的内核源码位置

| 内容 | 位置 |
|---|---|
| `struct proc_ops`、各类 `proc_create*` 声明 | `include/linux/proc_fs.h` |
| `struct proc_dir_entry`（不透明类型声明） | `include/linux/proc_fs.h` |
| `proc_create_single_data()` / `proc_create_seq_private()` 实现 | `fs/proc/generic.c:646` / `fs/proc/generic.c:615` |
| `struct proc_dir_entry` 真实定义（procfs 内部） | `fs/proc/internal.h:31` |
| `single_open()` / `single_release()` | `fs/seq_file.c:572` / `fs/seq_file.c:611` |
| `seq_read_iter()` 主循环（含溢出扩容、`buggy .next` 检查） | `fs/seq_file.c:171` |
| `%p` 指针哈希 | `lib/vsprintf.c:837` `default_pointer()` |
| `module_param(..., 0)` 不出现在 sysfs | `kernel/params.c`（`if (kparam[i].perm == 0) continue;`） |

---

## 十、命令速查

```bash
# ---------- host ----------
cd /home/tcd/Desktop/qemu_driver/driver/chapter-04
make                  # DEBUG=y（默认），编入 /proc 调试接口
make DEBUG=n          # 关闭调试代码
make test             # 交叉编译并安装 scull_test
make install          # 安装 scull.ko
make clean

# 验证调试代码是否编进去了
aarch64-linux-gnu-nm scull.ko | grep -i "proc\|seq"

# ---------- guest ----------
rmmod scull 2>/dev/null
insmod /mnt/scull.ko
M=$(grep scull /proc/devices | cut -d' ' -f1)
rm -f /dev/scull0 && mknod /dev/scull0 c $M 0

echo "hello scull" > /dev/scull0       # 基础读写
cat /dev/scull0

cat /proc/scullmem                     # single_open 版 dump（超一页会截断）
cat /proc/scullseq                     # seq_file 版 dump（完整）

/mnt/scull_test /dev/scull0            # 完整测试套件（33 条断言）
strace /mnt/scull_test /dev/scull0     # 观察 syscall 返回值
dmesg | tail                           # 查看 printk

rmmod scull
```
