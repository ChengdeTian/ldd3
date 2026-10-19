# 第6章 高级字符驱动操作

> 本章讲解用户可以通过一些高级接口来操作字符设备，包括：
> - `ioctl()` 用作设备控制
> - 阻塞 I/O 通过睡眠和唤醒，以及队列缓冲来实现
> - 非阻塞 I/O 通过 `poll()` 和 `select()`
> - 异步通知通过 SIGIO 信号
> - 移位设备通过 `llseek()`
> - 在一个设备文件上的存取控制
>
> 环境见 [buildroot-ext4-migration.md](./buildroot-ext4-migration.md)；
> 驱动代码见 `driver/chapter-06/`，LDD3 原版见 `lld3-example/scull/`。

---

## 一、总体分析

本章的 scull 不是一个设备，而是**一组设备**，正好把"高级字符驱动操作"拆成四块独立可测的能力：

| 设备 | 源文件 | 演示的高级操作 |
|---|---|---|
| `scull0` ~ `scull3` | `main.c` | `ioctl`、`llseek`、按 quantum/qset 分段读写 |
| `scullpipe0` ~ `scullpipe1` | `pipe.c` | 阻塞/非阻塞 I/O、等待队列、`poll`、`fasync`/SIGIO |
| `scullsingle`/`sculluid`/`scullwuid`/`sullpriv` | `access.c` | 一个设备文件上的存取控制（独占、按用户、按控制终端） |

### 1.1 ioctl 接口

`ioctl` 是"设备私有系统调用"：VFS 只做参数搬运，语义完全由驱动定义。它的命令号不是随便写一个整数，而是由
`_IO`/`_IOR`/`_IOW`/`_IOWR` 四个宏编码出来的 32 位整数：

```
 31    30    29          16 15          8 7            0
+-----+-----+--------------+--------------+--------------+
| dir |size |    type      |     nr       |   (none)     |
+-----+-----+--------------+--------------+--------------+
  dir : _IOC_NONE / _IOC_READ / _IOC_WRITE（可组合）
  size: 参数大小（1~14 bit，最大 16KB）
  type: 8 位"魔术字"，区分不同驱动（本章用 'k'）
  nr  : 8 位序号
```

驱动里的标准写法是**先校验方向/魔术字/序号，再 switch**：

```c
if (_IOC_TYPE(cmd) != SCULL_IOC_MAGIC)  return -ENOTTY;   /* 不是我的命令 */
if (_IOC_NR(cmd) > SCULL_IOC_MAXNR)     return -ENOTTY;   /* 序号越界  */
if (_IOC_SIZE(cmd) && !access_ok((void __user *)arg, _IOC_SIZE(cmd)))
        return -EFAULT;                                   /* 用户指针不可用 */
```

本章 scull 按 LDD3 的"六种调用约定"实现 quantum / qset 两组参数：

| 约定 | quantum | qset | 数据流向 |
|---|---|---|---|
| **S**et | `SCULL_IOCSQUANTUM` | `SCULL_IOCSQSET` | 指针传入（`_IOW`） |
| **T**ell | `SCULL_IOCTQUANTUM` | `SCULL_IOCTQSET` | 值传入（`_IO`，参数就是值） |
| **G**et | `SCULL_IOCGQUANTUM` | `SCULL_IOCGQSET` | 指针传出（`_IOR`） |
| **Q**uery | `SCULL_IOCQQUANTUM` | `SCULL_IOCQQSET` | 返回值传出 |
| **X**change | `SCULL_IOCXQUANTUM` | `SCULL_IOCXQSET` | 传入新值，旧值写回指针（`_IOWR`） |
| **H**shift | `SCULL_IOCHQUANTUM` | `SCULL_IOCHQSET` | 传入新值，旧值由返回值带回 |

此外还有 `SCULL_IOCRESET`（`_IO`）把两个参数复位成编译期默认值；
`SCULL_P_IOCTSIZE` / `SCULL_P_IOCQSIZE`（nr = 13、14）在头文件里预留但**未实现**，落到 `default` 返回 `-ENOTTY`。

### 1.2 阻塞 I/O

阻塞的本质是"**条件不满足就睡，别人改变条件后把你叫醒**"。scullpipe 用一个环形缓冲区加两个等待队列：

```
        写端                                          读端
   write() ──> [ wp ->         缓冲区        -> rp ] ──> read()
                  │                               │
                  └── 满了就睡在 outq               └── 空了就睡在 inq
                      被 read() 唤醒                   被 write() 唤醒
```

判定条件只有三条：

| 状态 | 判定 | 阻塞行为 |
|---|---|---|
| 空 | `rp == wp` | `read` 睡在 `inq`；若 `nwriters == 0` 直接返回 0（EOF） |
| 满 | `spacefree(dev) == 0` | `write` 睡在 `outq`；若 `nreaders == 0` 返回 `-EPIPE` |
| 非阻塞 | `filp->f_flags & O_NONBLOCK` | 不睡，直接 `-EAGAIN` |

环形缓冲区故意**留一个字节不用**（`buffersize - 1`），这样"空"和"满"的指针状态不会重合，不需要额外计数器。

### 1.3 poll 和 select

`select`/`poll`/`epoll` 在内核里都收敛到驱动的 `.poll`：

```c
static unsigned int scull_p_poll(struct file *filp, poll_table *wait)
{
        down(&dev->sem);
        poll_wait(filp, &dev->inq, wait);    /* 只是"登记"，不阻塞 */
        poll_wait(filp, &dev->outq, wait);

        if (dev->rp != dev->wp)      mask |= POLLIN  | POLLRDNORM;
        else if (!dev->nwriters)     mask |= POLLHUP;
        if (!dev->nreaders)          mask |= POLLERR;
        else if (spacefree(dev) > 0) mask |= POLLOUT | POLLWRNORM;

        up(&dev->sem);
        return mask;
}
```

`poll_wait()` 把当前进程的等待项挂到驱动的队列上，然后**立即返回**；真正的睡眠发生在 VFS 层。
所以驱动只要能回答"现在能不能读/能不能写"，剩下的超时、唤醒、重扫都由内核负责。

### 1.4 一个设备文件的访问控制

LDD3 给出四种"同一份代码、不同策略"的设备，靠**不同的 minor → 不同的 `file_operations`** 实现：

| 设备 | 策略 | 关键状态 |
|---|---|---|
| `scullsingle` | 全局只允许一个打开者 | `atomic_t scull_s_available` |
| `sculluid` | 同一用户可重复打开，其他用户 `-EBUSY`，root 可越权 | `scull_u_count` + `scull_u_owner` + 自旋锁 |
| `scullwuid` | 同上，但被占用时**阻塞等待**而不是报错 | `scull_w_count` + 等待队列 `scull_w_wait` |
| `sullpriv` | 每个控制终端一份独立数据 | 以 tty 设备号为 key 的链表 |

`sullpriv` 是本章唯一"动态创建设备"的例子：四个 minor 中它只有一个 cdev，`open()` 时用
`current->signal->tty` 算出 key，再从链表里取（或新建）一个 `struct scull_dev`，从而实现
"不同终端互不干扰"。

---

## 二、使用特点

### 2.1 ioctl

**1）返回值就是错误码。** 成功返回 0（或约定好的值），失败返回负的 errno，用户态 `ioctl()` 得到 -1 并置 errno：

```
SCULL_IOCTQUANTUM(1024)    → 返回 0
SCULL_IOCQQUANTUM          → 返回 4000（Query 用返回值传数据）
SCULL_IOCHQUANTUM(3000)    → 返回旧值 4096
魔术字错误 / 序号越界       → -ENOTTY（errno 25）
```

**2）指针参数必须做 `access_ok()`，并用 `__get_user`/`__put_user` 搬运。**
Linux 6 的 `access_ok()` 只剩两个参数（`addr, size`），LDD3 里的 `VERIFY_READ/VERIFY_WRITE` 已被删除。

**3）改参数的命令要限权。** 所有 `S/T/X/H` 命令都先 `capable(CAP_SYS_ADMIN)` 检查，非 root 返回 `-EPERM`；
`G/Q` 只读命令不检查。

**4）参数要做值域校验。** `quantum = 0` 会让 `scull_read()` 里的 `itemsize = quantum * qset` 变成 0，
进而除零；所以所有设置路径都拒绝 `<= 0` 的值，返回 `-EINVAL`。

**5）scull 的 ioctl 改的是"全局默认值"，不是已打开设备。**
`scull_quantum` / `scull_qset` 是模块全局变量，设备结构里的 `dev->quantum` / `dev->qset` 只在
`scull_trim()` 时才会被重新赋值。所以：

```
SCULL_IOCTQUANTUM(2000)  → Query 看到 2000
已打开设备的 dev->quantum 仍是 4000（不受影响）
再 trim 一次（O_WRONLY 打开或下次 open）→ dev->quantum 才同步为 2000
```

这是设计选择（避免已分配的内存块尺寸与参数不一致），测试程序第 16 组专门验证了这个语义。

### 2.2 睡眠和等待队列

`wait_event_interruptible(wq, condition)` 展开后等价于：

```c
DEFINE_WAIT(__wait);
for (;;) {
        prepare_to_wait(&wq, &__wait, TASK_INTERRUPTIBLE);
        if (condition) break;          /* 挂队后重新检查，不会丢唤醒 */
        schedule();
        if (signal_pending(current)) { ret = -ERESTARTSYS; break; }
}
finish_wait(&wq, &__wait);
```

三条使用纪律：

1. **检查条件与 `schedule()` 之间不能有窗口**——宏内部先挂队再检查，所以不要自己手写
   `if (!cond) schedule();`（经典丢唤醒）。
2. **不能持锁睡**。scullpipe 的做法是：`up(&dev->sem)` → 睡 → 醒来 `down_interruptible(&dev->sem)` → 回到循环重新判断。
3. **醒来必须重新判断条件**。等待队列是"广播"，被叫醒不代表条件一定成立（可能被别的进程抢走）。

返回值语义：被信号打断返回 `-ERESTARTSYS`，VFS 根据 `SA_RESTART` 决定"自动重启系统调用"还是"给用户态 -EINTR"。
`scull_test1` 第 5 组用不带 `SA_RESTART` 的 `SIGUSR1` 处理器验证了它最终表现为 `-1/EINTR`。

**关闭一端时必须唤醒另一端**，否则阻塞的读者等不到 EOF、阻塞的写者等不到 EPIPE：

```c
if (dev->nwriters == 0)
        wake_up_interruptible(&dev->inq);    /* 读者：可以返回 0 了 */
if (dev->nreaders == 0)
        wake_up_interruptible(&dev->outq);   /* 写者：可以返回 -EPIPE 了 */
```

### 2.3 poll 的事件挂载触发

一次 `poll()` 的生命周期：

```
用户态 poll()
   └─> VFS 遍历每个 fd，调用驱动 .poll()
         └─> poll_wait() 把当前进程的 __wait 挂到 inq/outq
         └─> 返回当前 mask
   └─> mask 非 0 → 立即返回
   └─> mask 为 0 → 睡眠，直到 inq/outq 被 wake_up 或超时
         └─> 醒来后重新调用 .poll()（所以 .poll 必须"无副作用、可重复调用"）
```

因此 `.poll` 里**只做三件事**：登记队列、算 mask、返回。不能在里面睡眠或做耗时操作。

| mask | 含义 | scullpipe 里置位的条件 |
|---|---|---|
| `POLLIN \| POLLRDNORM` | 有数据可读 | `rp != wp` |
| `POLLOUT \| POLLWRNORM` | 有空间可写 | `spacefree() > 0` |
| `POLLHUP` | 对端全关（读到 EOF） | 缓冲区空且 `nwriters == 0` |
| `POLLERR` | 对端全关（写会 EPIPE） | `nreaders == 0` |

`.poll` 与阻塞 I/O 是**两套独立实现**，但必须给出一致的结论：`poll` 说可读，随后的 `read` 就不能再阻塞。

### 2.4 异步通知

异步通知（fasync）让驱动在"数据到了"时主动给进程发 `SIGIO`，进程不必 poll/阻塞。
驱动侧只有两个动作：

```c
/* 1) open/close 时维护链表：mode 非 0 注册，0 注销 */
.fasync = scull_p_fasync,                  /* → fasync_helper(fd, filp, mode, &dev->async_queue) */

/* 2) 数据可读时通知所有注册者 */
if (dev->async_queue)
        kill_fasync(&dev->async_queue, SIGIO, POLL_IN);
```

用户态要凑齐三件事，缺一不可：

```c
fcntl(fd, F_SETOWN, getpid());                  /* 信号发给谁 */
fcntl(fd, F_SETFL, flags | O_ASYNC);            /* 打开异步模式（触发 .fasync 注册） */
sigaction(SIGIO, &sa, NULL);                    /* 装处理函数 */
```

注意 `kill_fasync()` 只应在**状态变化**时调用（scullpipe 在 `write` 之后调用），
且驱动卸载/`release` 时必须 `scull_p_fasync(-1, filp, 0)` 把 `filp` 从链表里摘掉，否则会留下悬空指针。

---

## 三、本章 scull 的实现

### 3.1 文件结构与设备号布局

| 文件 | 说明 |
|---|---|
| `driver/chapter-06/scull.h` | 数据结构、参数宏、ioctl 命令定义 |
| `driver/chapter-06/main.c` | scull0~3：读写、`llseek`、`ioctl`、proc 调试接口 |
| `driver/chapter-06/pipe.c` | scullpipe：环形缓冲、等待队列、`poll`、`fasync` |
| `driver/chapter-06/access.c` | 四种访问控制设备 |
| `driver/chapter-06/scull_test.c` | ioctl / 读写 / lseek 测试（62 条断言） |
| `driver/chapter-06/scull_test2.c` | scullpipe 测试（31 条断言） |
| `driver/chapter-06/scull_test1.c` | 访问控制测试（27 条断言） |
| `driver/chapter-06/Makefile` | kbuild 模块 + 三个测试程序 + 安装到 9p 目录 |

设备号是**连续排布**的，`scull_init()` 里必须一段接一段地申请，不能各自从 0 开始：

```c
dev = MKDEV(scull_major, scull_minor);           /* scull0..3      : minor 0..3 */
result = scull_p_init(MKDEV(scull_major, scull_minor + scull_devs));        /* 4..5 */
result = scull_access_init(dev + SCULL_P_NR_DEVS);                         /* 6..9 */
```

实测（动态主设备号 511）：

```
主设备号 : 511
起始次号 : 6 (scullsingle=6, sculluid=7, scullwuid=8, sullpriv=9)
```

> 注意 `/proc/devices` 里会出现三条同名记录（`scull` / `scull_p` / `sculla`），它们同主设备号、次号分段，
> 所以取主设备号用 `awk '$2=="scull"{print $1; exit}'` 即可。

### 3.2 ioctl 的实现（main.c）

```c
long scull_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
        long tmp, retval = 0;
        int val;

        if (_IOC_TYPE(cmd) != SCULL_IOC_MAGIC) return -ENOTTY;
        if (_IOC_NR(cmd) > SCULL_IOC_MAXNR)    return -ENOTTY;
        if (_IOC_SIZE(cmd) && !access_ok((void __user *)arg, _IOC_SIZE(cmd)))
                return -EFAULT;

        switch (cmd) {
        case SCULL_IOCSQUANTUM:                       /* 指针 → 值 */
                if (!capable(CAP_SYS_ADMIN))        return -EPERM;
                if (__get_user(val, (int __user *)arg)) return -EFAULT;
                if (val <= 0)                       return -EINVAL;
                scull_quantum = val;
                break;
        case SCULL_IOCQQUANTUM:                       /* 值 ← 返回值 */
                return scull_quantum;
        case SCULL_IOCXQUANTUM:                       /* 交换：新值入，旧值出 */
                ...
                tmp = scull_quantum; scull_quantum = val;
                retval = __put_user(tmp, (int __user *)arg);
                break;
        }
        return retval;
}
```

要点：

- `scull_quantum` / `scull_qset` 是**模块全局**，因此 ioctl 天然对所有设备生效；
  但设备的 `dev->quantum` / `dev->qset` 只在 `scull_trim()` 里同步（见 2.1 第 5 条）。
- `__get_user()` / `__put_user()` 自带 `access_ok` 语义，失败返回非 0，可直接转成 `-EFAULT`。
- 返回 `long`，所以既能返回错误码，也能在 `Query`/`sHift` 场景直接带回数据。

配套的 `scull_llseek()` 实现了 `SEEK_SET/CUR/END` 三种 whence，并对负位置返回 `-EINVAL`；
`scullpipe` 则完全不给 `.llseek`，改用 `nonseekable_open()` 清掉 `FMODE_LSEEK`，让 `lseek` 返回 `-ESPIPE`。

### 3.3 scullpipe 的实现（pipe.c）

数据结构：

```c
struct scull_pipe {
        wait_queue_head_t inq, outq;       /* 读队列、写队列 */
        char *buffer, *end;                /* 缓冲区首尾 */
        int  buffersize;
        char *rp, *wp;                     /* 读/写指针，环形 */
        int  nreaders, nwriters;           /* 打开计数：决定 EOF / EPIPE */
        struct fasync_struct *async_queue;
        struct semaphore sem;
        struct cdev cdev;
};
```

阻塞读：

```c
while (dev->rp == dev->wp) {           /* 空 */
        if (!dev->nwriters) { up(&dev->sem); return 0; }        /* EOF */
        up(&dev->sem);                                          /* 睡之前必须放锁 */
        if (filp->f_flags & O_NONBLOCK) return -EAGAIN;
        if (wait_event_interruptible(dev->inq, dev->rp != dev->wp))
                return -ERESTARTSYS;
        if (down_interruptible(&dev->sem)) return -ERESTARTSYS;  /* 醒来重新持锁 */
}
```

阻塞写的空间等待被抽成 `scull_getwritespace()`，它内部同样"放锁 → 睡 → 重新持锁"，并在
`nreaders == 0` 时提前返回 `-EPIPE`（哪怕缓冲区还有空间）。

`spacefree()` 用环形指针算空闲量，并预留 1 字节：

```c
if (dev->rp == dev->wp) return dev->buffersize - 1;             /* 空 */
return (int)((dev->rp - dev->wp + dev->buffersize) % dev->buffersize) - 1;
```

写路径在 `copy_from_user()` 之后 `wake_up_interruptible(&dev->inq)` 唤醒读者，并 `kill_fasync()` 发 SIGIO。

### 3.4 访问控制的实现（access.c）

四个设备共用一套注册代码，靠 `scull_access_devs[]` 表把 name / 静态 dev / fops 绑在一起：

```c
{ "scullsingle", &scull_s_device, &scull_sngl_fops },
{ "sculluid",    &scull_u_device, &scull_user_fops },
{ "scullwuid",   &scull_w_device, &scull_wusr_fops },
{ "sullpriv",    NULL,            &scull_priv_fops },   /* dev 在 open 时按 tty 克隆 */
```

**scullsingle**：`atomic_dec_and_test()` 抢占，失败回滚并 `-EBUSY`；release 时 `atomic_inc()`。

**sculluid**：`count` + `owner` 用自旋锁保护，"同 uid 或 root" 才能进：

```c
if (scull_u_count && !uid_eq(scull_u_owner, current_uid()) &&
    !uid_eq(scull_u_owner, current_euid()) && !capable(CAP_DAC_OVERRIDE))
        return -EBUSY;
```

**scullwuid**：把上面的 `-EBUSY` 换成等待：

```c
while (!scull_w_available()) {
        spin_unlock(&scull_w_lock);                       /* 睡之前放锁 */
        if (filp->f_flags & O_NONBLOCK) return -EAGAIN;
        if (wait_event_interruptible(scull_w_wait, scull_w_available()))
                return -ERESTARTSYS;
        spin_lock(&scull_w_lock);
}
```

release 时如果计数归零才 `wake_up_interruptible_sync(&scull_w_wait)`，避免惊群。

**sullpriv**：以控制终端为 key 的链表：

```c
struct tty_struct *tty = current->signal->tty;
if (!tty || !tty->driver) return -EINVAL;                 /* 没有控制终端就拒绝 */
key = MKDEV(tty->driver->major, tty->driver->minor_start + tty->index);
dev = scull_c_lookfor_device(key);                        /* 链表里没有就用 kmalloc 新建 */
```

链表用 `mutex` 而不是自旋锁，因为 `scull_c_lookfor_device()` 里会用 `GFP_KERNEL` 分配内存（持自旋锁睡眠是非法操作）。

### 3.5 参数与调试接口

| 参数 | 默认值 | 说明 |
|---|---|---|
| `scull_major` | 0 | 0 = 动态分配，非 0 = 指定 |
| `scull_minor` | 0 | 起始次设备号 |
| `scull_devs` | 4 | scull0 ~ scull3 |
| `scull_quantum` | 4000 | 单块大小 |
| `scull_qset` | 500 | 每个 qset 的指针数 |
| `scull_p_nr_devs` | 2 | scullpipe0 ~ scullpipe1 |
| `scull_p_buffer` | 4000 | 环形缓冲区大小（可用 3999） |

**权限位必须非 0**：`module_param(x, int, 0)` 时内核**不会**创建 `/sys/module/scull/parameters/x`，
而三个测试程序都从这里读实际生效的参数（读不到会静默退化成默认值）。本章统一用 `0444`：

```bash
# guest
for p in /sys/module/scull/parameters/*; do echo -n "$p="; cat $p; done
/sys/module/scull/parameters/scull_devs=4
/sys/module/scull/parameters/scull_major=511
/sys/module/scull/parameters/scull_minor=0
/sys/module/scull/parameters/scull_p_buffer=4000
/sys/module/scull/parameters/scull_p_nr_devs=2
/sys/module/scull/parameters/scull_qset=500
/sys/module/scull/parameters/scull_quantum=4000
```

调试接口（`SCULL_DEBUG`，由 `make` 默认打开、`make DEBUG=n` 关闭）：

```bash
cat /proc/scullmem        # proc_create_single：一次性输出所有设备
cat /proc/scullseq        # proc_create_seq：按设备迭代
```

```
Device 0: qset: 500, q: 4000, sz: 12
 item at 00000000072ec52a, qset at 000000007fc48488
    0: b012fada
```

---

## 四、测试

### 4.1 覆盖范围

| 程序 | 组数 / 断言 | 覆盖点 |
|---|---|---|
| `scull_test` | 18 组 / 62 条 | 基础读写、短读语义、跨 quantum/qset 定位、`lseek` 三种 whence、非法 whence、proc 接口、ioctl 六种约定、Reset、`-ENOTTY`、参数校验、全局参数 vs 设备参数、`O_WRONLY` 截断 |
| `scull_test2` | 9 组 / 31 条 | 基本读写、`-ESPIPE`、`O_NONBLOCK`（空读/写满）、阻塞读被写唤醒、阻塞写被读唤醒、EOF、EPIPE、`poll` 四种 mask、fasync/SIGIO、环形回绕 |
| `scull_test1` | 7 组 / 27 条 | 四种访问控制设备的全部状态机，含异用户、root 越权、阻塞唤醒、信号打断、另一个 pty、200 次 open/close 计数不泄漏 |

三个程序都自己 `mknod` 缺失的设备节点，并从 `/sys/module/scull/parameters/`（回退 `/proc/devices`）取设备号，
所以**不需要手工准备 /dev 节点**。

### 4.2 编译与运行

```bash
cd /home/tcd/Desktop/qemu_driver/driver/chapter-06
make            # 编译 scull.ko（默认 DEBUG=y，带 /proc 调试接口）
make install    # 拷到 ../mydriver/
make tests      # 编译三个测试程序并拷到 ../mydriver/
```

guest 侧（QEMU 内，`root` 免密）：

```bash
mkdir -p /mnt
mount -t 9p -o trans=virtio,version=9p2000.L host_drv /mnt
insmod /mnt/scull.ko

M=$(awk '$2=="scull"{print $1; exit}' /proc/devices)
mknod /dev/scull0 c $M 0 && chmod 666 /dev/scull0

/mnt/scull_test  /dev/scull0
/mnt/scull_test2
/mnt/scull_test1
rmmod scull
```

### 4.3 自动化脚本

host 上的 `run_scull_test.py` 用 pexpect 驱动 `kernel/linux-6.12.10/qemu_start.sh`：
启动 → 等提示符（遇到 `login:` 自动登录 root）→ 挂 9p → `insmod` → 建节点 → 依次跑三个测试 →
`rmmod` → 退出 QEMU，并把三条 `PASS/FAIL` 汇总行解析出来，用退出码表示结果。

```bash
cd /home/tcd/Desktop/qemu_driver
python3 -u run_scull_test.py      # 退出码 0 = 全部通过
```

### 4.4 实测记录（QEMU 串口原始输出）

```
# insmod /mnt/scull.ko
[    2.461575] scull: loading out-of-tree module taints kernel.
[    2.465401] scullsingle cdev added
[    2.465534] sculluid cdev added
[    2.465632] scullwuid cdev added
[    2.465725] sullpriv cdev added
[    2.465868] Hello, world!

# grep scull /proc/devices
511 scull
511 scull_p
511 sculla

# echo 'hello scull' > /dev/scull0; cat /dev/scull0
[    2.982186] scull_open
[    2.983001] scull_release
[    2.997827] scull_open
hello scull
[    2.998499] scull_release
```

ioctl 部分（节选）：

```
--- 9. ioctl：Query（返回值）/ Get（指针） ---
  [ OK ] SCULL_IOCQQUANTUM → 4000（应为 4000）
  [ OK ] SCULL_IOCGQUANTUM(&v) → 0, v = 4000（应为 4000）
--- 11. ioctl：eXchange（指针）/ sHift（值） ---
  [ OK ] SCULL_IOCXQUANTUM(&4096) → 0
  [ OK ] X 调用后 x 被换成旧值 = 2048（应为 2048）
  [ OK ] SCULL_IOCHQUANTUM(3000) 返回旧值 = 4096（应为 4096）
--- 13. ioctl：非法命令 → -ENOTTY ---
  [ OK ] 魔术字 'x' → -1/ENOTTY（errno=25）
--- 16. ioctl：改的是全局默认值，不影响已打开设备 ---
  [ OK ] 只改全局：SCULL_IOCTQUANTUM(2000) → 0
  [ OK ] 已打开设备的 quantum 仍是 4000（ioctl 未影响它）
  [ OK ] 再 trim 一次 → dev->quantum = scull_quantum
```

scullpipe 部分（节选）：

```
--- 3. 阻塞读：写入后唤醒 ---
[    3.313980] scull_p_read: going to sleep
  [ OK ] 无数据时子进程 read 阻塞中
  [ OK ] 写入后子进程被唤醒
  [ OK ] 子进程 read 返回 1 字节（ret=1, errno=0）
--- 4. 阻塞写：读走数据后唤醒 ---
[    3.622691] scull_getwritespace: going to sleep
  [ OK ] 缓冲区满时子进程 write 阻塞中
  [ OK ] 读走数据后子进程被唤醒
--- 8. fasync：写入触发 SIGIO ---
  [ OK ] 在 2 秒内收到 SIGIO（实际 sig=29, errno=11）
--- 9. 环形缓冲区回绕 ---
  [ OK ] 连续 8 轮「写满-读空」数据全部正确（跨 end 回绕）
```

访问控制部分（节选）：

```
--- 1. scullsingle：独占打开 ---
  [ OK ] 第二次 open 应失败 -EBUSY（实际 fd=-1, errno=16 Device or resource busy）
--- 2. sculluid：单用户可重复打开 ---
  [ OK ] 其他用户 uid=65534 打开应 -EBUSY（实际 ret=-1, errno=16）
  [ OK ] root 以 CAP_DAC_OVERRIDE 越权打开成功（fd=4, errno=0）
--- 4. scullwuid：阻塞等待与唤醒 ---
  [ OK ] 持有设备期间，子进程的 open 一直阻塞（未被唤醒）
  [ OK ] 释放设备后，子进程在 3 秒内被唤醒
--- 6. sullpriv：按控制终端区分实例 ---
  [ OK ] 另一个控制终端得到独立实例（数据不共享）
  [ OK ] setsid 脱离控制终端后 open → -EINVAL（ret=-1, errno=22）
```

### 4.5 结果

```
/mnt/scull_test /dev/scull0      PASS 62 / FAIL 0 / SKIP 0 / TODO 0
/mnt/scull_test2                 PASS 31 / FAIL 0 / SKIP 0 / TODO 0
/mnt/scull_test1                 PASS 27 / FAIL 0 / SKIP 0 / TODO 0
```

结论：ioctl 六种传参约定、阻塞/非阻塞 I/O、`poll` 四种 mask、fasync/SIGIO、四种访问控制策略
**全部验证通过**；`rmmod` 后 `dmesg` 出现 `Goodbye, world!`，无内存泄漏与残留 `/proc` 节点。

---

## 五、2.6 → 6.12 移植点

| LDD3（2.6） | Linux 6.12 的写法 | 说明 |
|---|---|---|
| `int (*ioctl)(struct inode *, struct file *, unsigned int, unsigned long)` | `long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long)` | 不再有大内核锁，`inode` 参数也没了 |
| `access_ok(VERIFY_READ/WRITE, addr, size)` | `access_ok(addr, size)` | 方向判断已删除，只剩"这块用户地址能不能碰" |
| `no_llseek` | 不提供 `.llseek` + `nonseekable_open()` | 符号已删除 |
| `SEM_INITIALIZER(n)` 赋值 | `sema_init(&sem, n)` | 结构体初始化方式变更 |
| `read_proc` / `write_proc` | `proc_create_single()` / `proc_create_seq()` | 见第 4 章 |
| `current->signal->tty` | 仍然可用（`struct tty_struct *`） | `sullpriv` 的 key 就靠它 |
| `fasync_helper()` / `kill_fasync()` | 未变 | 签名保持兼容 |
| `copy_to_user` / `copy_from_user` | 未变 | 头文件改为 `<linux/uaccess.h>` |
| `asmlinkage`、`smp_lock`、`MOD_INC_USE_COUNT` | 已删除 | 不要再用 |

---

## 六、踩坑记录

| # | 现象 | 原因 | 解决 |
|---|---|---|---|
| 1 | `cat /sys/module/scull/parameters/scull_quantum` 不存在，测试却"通过" | `module_param(..., 0)` 时内核不创建 sysfs 节点，测试静默退化成默认值 | 参数权限改 `0444`，测试才能真正读到生效值 |
| 2 | `make` 后 `/proc/scullmem` 不存在 | Makefile 里 `DEBUG ?= y` 被注释掉，`DEBUG` 为空 → 不定义 `SCULL_DEBUG` | 取消注释，`ccflags-y += -DSCULL_DEBUG` 才会生效 |
| 3 | 测试程序"与子进程同步失败" | 子进程往管道的**读端** `sync[0]` 写同步字节，`EBADF` 后 `_exit(2)` | 写 `sync[1]`，并只在用完后再关闭写端 |
| 4 | `scullwuid` "释放设备后，子进程在 3 秒内被唤醒" 失败，随后整个进程卡死 | `fork()` 后子进程继承了父进程的设备 fd，父进程 `close()` 只减引用计数，驱动 `release` 不执行 → 没有 `wake_up` | 子进程先 `close(fd)`；父进程读结果改非阻塞，失败时 `SIGKILL` 回收 |
| 5 | `sullpriv` "另一个控制终端"用例得到 `ECHILD` | 子进程 `setsid()` 后关闭 pty master，内核向该会话前台组发 `SIGHUP`，子进程在回写结果前被杀 | 子进程忽略 `SIGHUP`（`signal(SIGHUP, SIG_IGN)`） |
| 6 | 写端全关后阻塞在 `read()` 的进程不返回 0 | `scull_p_release()` 只在计数归零时释放缓冲区，没有唤醒对端队列 | release 里 `nwriters == 0` 唤醒 `inq`、`nreaders == 0` 唤醒 `outq` |
| 7 | `ioctl` 改了 quantum，已打开设备读出来还是旧值 | `dev->quantum` 只在 `scull_trim()` 里同步，ioctl 改的是模块全局变量 | 属既定设计；测试第 16 组显式验证"trim 后才同步" |
| 8 | `lseek` 返回 `-ESPIPE` | `.llseek` 为 NULL 时内核清掉 `FMODE_LSEEK`；scullpipe 还额外调了 `nonseekable_open()` | 需要定位的设备实现 `scull_llseek`，管道类设备则接受 `-ESPIPE` |
| 9 | `quantum` 设成 0 后内核 Oops（除零） | `itemsize = quantum * qset` 为 0，`/` 与 `%` 直接除零 | 所有设置入口拒绝 `<= 0`，返回 `-EINVAL` |
| 10 | `capable()` 在自旋锁里调用 | LDD3 原版就这么写；`capable()` 不睡眠但会走 LSM 钩子，代价不小 | 保持原样（`scull_w_available()` 需在等待条件里复用）；要较真可换 `mutex` |
| 11 | `rmmod scull` 报 `Module scull is in use` | 还有进程阻塞在 `scullwuid` 的 `open()` 里，`struct file` 未释放，模块引用计数不为 0 | 先 `kill` 掉阻塞的测试进程（或被信号打断）再卸载；`scull_exit()` 必须依次 `scull_p_cleanup()` → `scull_access_cleanup()` → 释放 scull 设备 |

---

## 七、命令速查

```bash
# ---------- host：编译 ----------
cd /home/tcd/Desktop/qemu_driver/driver/chapter-06
make && make install          # scull.ko（DEBUG=y，带 /proc 接口）
make tests                    # scull_test / scull_test1 / scull_test2
make DEBUG=n                  # 关闭调试代码（/proc 接口消失）

# ---------- host：启动 ----------
cd /home/tcd/Desktop/qemu_driver/kernel/linux-6.12.10
./qemu_start.sh               # 登录 root（无密码）

# ---------- host：一键自动化 ----------
cd /home/tcd/Desktop/qemu_driver
python3 -u run_scull_test.py  # 启动 + 加载 + 三个测试 + 卸载，退出码 0 为通过

# ---------- guest ----------
mkdir -p /mnt
mount -t 9p -o trans=virtio,version=9p2000.L host_drv /mnt
insmod /mnt/scull.ko
M=$(awk '$2=="scull"{print $1; exit}' /proc/devices)
mknod /dev/scull0 c $M 0 && chmod 666 /dev/scull0

cat /sys/module/scull/parameters/scull_quantum   # 参数
cat /proc/scullmem                               # 调试接口（SCULL_DEBUG）
/mnt/scull_test  /dev/scull0                     # ioctl / lseek / 读写
/mnt/scull_test2                                 # 阻塞 / poll / fasync
/mnt/scull_test1                                 # 访问控制
rmmod scull && dmesg | tail                      # 应看到 Goodbye, world!
```
