# 第3章 字符设备驱动：scull 的编译与测试

> 本章记录 scull 字符设备驱动的**编译、加载、测试**完整流程。
>
> 环境：`qemu-system-aarch64` + `linux-6.12.10` + Buildroot ext4 rootfs + 9p 共享目录，
> 环境搭建过程见 [buildroot-ext4-migration.md](./buildroot-ext4-migration.md)。

---

## 一、代码结构

| 文件 | 说明 |
|---|---|
| `driver/chapter-03/scull.c` | 驱动主体：open/release/read/write/llseek、模块初始化与卸载 |
| `driver/chapter-03/scull.h` | 数据结构与宏定义 |
| `driver/chapter-03/scull_test.c` | 用户态测试程序 |
| `driver/chapter-03/Makefile` | 交叉编译模块、交叉编译测试程序、安装到 9p 共享目录 |

关键参数（可用 `insmod` 参数覆盖）：

| 宏 | 默认值 | 含义 |
|---|---|---|
| `SCULL_MAJOR` | 0 | 动态主设备号（从 `/proc/devices` 读取） |
| `SCULL_DEVS` | 4 | 设备个数，`scull0` ~ `scull3` |
| `SCULL_QUANTUM` | 4000 | 单个内存块大小（字节） |
| `SCULL_QSET` | 500 | 每个 qset 里指针个数 |

内存模型是**三级结构**：`scull_dev.data` → qset 链表 → 指针数组 → 量子块，且按需分配（惰性分配）：

```
scull_dev.data ──> [qset] ──> [qset] ──> ...   (链表)
                     │
                     └─> void *data[500]       (指针数组)
                           ├─> 4000B (quantum)
                           └─> ...
```

> 因此读取时要经过两次除法/取模定位：`item`（第几个 qset）→ `s_pos`（qset 内第几个量子）→ `q_pos`（量子内偏移）。

---

## 二、测试链路

```
host                                       guest (QEMU)
────────────────────────────────────────   ────────────────────────
make            → scull.ko
make test       → scull_test
make install ─┐
              │  driver/mydriver/  ──9p 共享──▶  /mnt
              └──────────────────────────────────▶ insmod /mnt/scull.ko
                                                   mknod /dev/scull0
                                                   echo/cat/dd/scull_test
```

要点：`driver/mydriver` 通过 9p **实时共享**，host 重新编译后 guest 立即能看到新文件，**不需要重启 QEMU**，只需 `rmmod` + `insmod`。

---

## 三、测试步骤

### 1. host：编译并安装

```bash
cd /home/tcd/Desktop/qemu_driver/driver/chapter-03
make            # 交叉编译 scull.ko
make test       # 交叉编译 scull_test（静态链接，guest 无需额外库）
make install    # 把 scull.ko 拷到 ../mydriver/
make test 已包含拷贝 scull_test
```

### 2. host：启动 QEMU

```bash
cd /home/tcd/Desktop/qemu_driver/kernel/linux-6.12.10
./qemu_start.sh              # ext4 启动
# 回退到旧的 BusyBox initramfs：BUSYBOX=1 ./qemu_start.sh
```

登录：用户 `root`，无密码。`/etc/init.d/S99drv` 会自动把 9p 共享目录挂到 `/mnt`。

### 3. guest：加载模块并创建设备节点

`SCULL_MAJOR = 0` 是**动态主设备号**，每次加载都可能不同，所以每次都要从 `/proc/devices` 现取：

```bash
cd /mnt
insmod scull.ko
lsmod
dmesg | tail                      # 应看到 "Hello, world!"

M=$(grep scull /proc/devices | cut -d' ' -f1)
echo "major = $M"

rm -f /dev/scull0
mknod /dev/scull0 c $M 0
chmod 666 /dev/scull0
```

> 注意：内核里已有部分主设备号被占用（示例中 240 就被占用，`insmod scull.ko scull_major=240` 会报
> `can't get major 240`），所以**不要写死设备号**。

### 4. 测试一：用 shell 命令做基础验证

```bash
echo "hello scull" > /dev/scull0      # 以 O_WRONLY 打开 → 触发 trim 清零 → 写入
cat /dev/scull0                       # 预期输出 hello scull
```

`dmesg` 中可看到 `scull_open` / `scull_release` 的打印，用于确认回调被正确触发。

跨 quantum 的大数据（`echo` 只能测小数据）：

```bash
dd if=/dev/urandom of=/tmp/in.bin bs=1024 count=8    # 8KB，必然跨 4000 字节的 quantum
cp /tmp/in.bin /dev/scull0
cat /dev/scull0 > /tmp/out.bin
cmp /tmp/in.bin /tmp/out.bin                         # 一致即正确
```

> `read`/`write` 一次只处理**一个 quantum**，单次调用最多搬运 `quantum - q_pos` 字节并返回
> 实际字节数。这是 scull 的设计特性，所以必须用 `cp`/`cat` 这类会循环读到 EOF 的工具对比。

### 5. 测试二：用测试程序验证定位读写

`scull_test.c` 覆盖 shell 命令做不到的部分（`O_RDWR` 不截断、`lseek` 定位读写、单次 `read` 的实际返回值）：

```bash
/mnt/scull_test /dev/scull0
```

预期输出：

```
read 5 bytes: hello
at 10000: at-10000
```

说明：

| 输出 | 含义 |
|---|---|
| `read 5 bytes` | 单次 `read()` 实际只拿到 5 字节（被 `dev->size` 截断），验证短读语义 |
| `at 10000: at-10000` | 验证 `lseek` 定位写 + 定位读，偏移 10000 落在第 0 个 qset 的第 2 个量子内偏移 2000 处 |

若设备里已有数据（例如刚执行过 `echo "hello scull" > /dev/scull0`），第一行会显示 `read 12 bytes: hello scull` —— 因为测试程序用 `O_RDWR` 打开**不会清零**，只覆盖了前 5 字节，`size` 仍为 12。这是正确行为。

### 6. guest：卸载

```bash
rmmod scull
rm -f /dev/scull0
dmesg | tail        # 应看到 "Goodbye, world!"
```

### 7. 调试手段

```bash
strace /mnt/scull_test /dev/scull0     # 看 read/lseek 的实际返回值
strace -e trace=read,write,openat cat /dev/scull0
dmesg -w                               # 实时看 printk
```

---

## 四、测试程序 scull_test.c 的设计要点

```c
int fd = open(argv[1], O_RDWR);          /* 用 O_RDWR，避免触发 O_WRONLY 时的 trim */
write(fd, "hello", 5);                   /* 从 0 写 5 字节 */
lseek(fd, 0, SEEK_SET);
n = read(fd, buf, sizeof(buf));          /* 观察单次实际读取字节数 */
printf("read %zd bytes: %.*s\n", n, (int)n, buf);

lseek(fd, 10000, SEEK_SET);              /* 测跨 quantum 定位写 */
write(fd, "at-10000", 8);
lseek(fd, 10000, SEEK_SET);
n = read(fd, buf, sizeof(buf));
printf("at 10000: %.*s\n", (int)n, buf);
```

为什么这么写：

1. **必须用 `O_RDWR`**：驱动在 `open()` 时判断 `(f_flags & O_ACCMODE) == O_WRONLY` 就 `scull_trim()` 清零，
   用 `O_WRONLY` 打开会把先前的数据抹掉，测不出"追加/覆盖"行为。
2. **必须能 `lseek`**：`file_operations` 里不给 `.llseek` 时，内核会清掉 `FMODE_LSEEK`
   （见 `fs/open.c`），`lseek()` 直接返回 `-ESPIPE`，定位读写完全不可用。
3. **打印 `n`**：把单次 `read()` 的返回值打出来，才能看到 scull"一次只搬一个 quantum / 被 size 截断"的短读特性。

静态编译方式（guest 里没有动态库依赖问题）：

```bash
aarch64-linux-gnu-gcc -static -O2 -Wall -o scull_test scull_test.c
```

---

## 五、踩坑记录

| # | 现象 | 原因 | 解决 |
|---|---|---|---|
| 1 | `cat /dev/scull0` 读不到东西，`read()` 返回 0 | `scull_read()` 成功路径写成 `return 0;`，既没返回 `retval` 也没 `up(&dev->sem)` | 成功路径补 `up()` 并返回 `retval`（或像 `scull_write` 一样穿透到 `out:`） |
| 2 | 第一次操作正常，**第二次操作永久卡死** | 同上，信号量在成功路径被泄漏，后续 `down_interruptible()` 永远拿不到锁 | 保证每条返回路径都 `up()` |
| 3 | `lseek` 返回 `-ESPIPE` | `file_operations.llseek` 为 NULL，内核清掉 `FMODE_LSEEK` | 实现 `scull_llseek()` 并挂到 `.llseek` |
| 4 | `rmmod` 后重载，或第二次 `open(O_WRONLY)` 时内核 Oops | `scull_trim()` 里 `dptr->data[i]` 未判空（`data` 已置 NULL 时空指针解引用） | 加 `if (dptr->data)` 包裹 |
| 5 | 内存一直涨 | `scull_trim()` 没 `kfree(dptr)`，qset 节点本身泄漏 | 保存 `next` 后 `kfree(dptr)` |
| 6 | `trim` 后设备仍指向旧数据 | 末尾误写成 `dev->next = NULL`，而 `next` 是设备链表的遗留字段 | 应清 `dev->data = NULL` |
| 7 | 并发读写时数据错乱 | `scull_open()` 里 `scull_trim()` 未持 `dev->sem` | `down_interruptible()` / `up()` 包裹 |
| 8 | `insmod scull.ko scull_major=240` 失败 | 240 已被内核其他子系统占用 | 用动态主设备号，从 `/proc/devices` 现取 |
| 9 | 读空洞时出现二进制乱码 | 量子块用 `kmalloc` 未清零，空洞区域把内核残留数据拷给了用户态（**信息泄漏**） | 待修复：改用 `kzalloc()`，或在读空洞时填 0 |

---

## 六、实测记录（qemu 串口原始输出）

```text
~ # insmod /mnt/scull.ko && echo ===INSMOD_OK===
[   18.878142] scull: loading out-of-tree module taints kernel.
[   18.881426] Hello, world!
===INSMOD_OK===
~ # grep scull /proc/devices
511 scull
~ # rm -f /dev/scull0; mknod /dev/scull0 c $M 0 && echo ===MKNOD_OK===
===MKNOD_OK===
~ # echo "hello scull" > /dev/scull0 && echo ===WRITE_OK===
[   20.850012] scull_open
[   20.850693] scull_release
===WRITE_OK===
~ # cat /dev/scull0
[   20.867908] scull_open
hello scull
[   20.868856] scull_release
~ # /mnt/scull_test /dev/scull0
[   21.873248] scull_open
read 12 bytes: hello scull

at 10000: at-10000
[   21.878167] scull_release
```

结论：模块加载/卸载、设备节点创建、基础读写、跨 quantum 分段读写、`lseek` 定位读写**均已验证通过**；
遗留问题是第五节第 9 条（空洞读到未初始化内核内存）。

---

## 七、命令速查

```bash
# ---------- host ----------
cd /home/tcd/Desktop/qemu_driver/driver/chapter-03
make && make test && make install      # 编译模块 + 测试程序并共享到 guest
make clean                             # 清理

cd /home/tcd/Desktop/qemu_driver/kernel/linux-6.12.10
./qemu_start.sh                        # 启动 guest

# ---------- guest ----------
cd /mnt
rmmod scull 2>/dev/null                # 重新加载前先卸载
insmod scull.ko
M=$(grep scull /proc/devices | cut -d' ' -f1)
rm -f /dev/scull0 && mknod /dev/scull0 c $M 0 && chmod 666 /dev/scull0

echo "hello scull" > /dev/scull0       # 基础写
cat /dev/scull0                        # 基础读
/mnt/scull_test /dev/scull0            # 定位读写测试
strace /mnt/scull_test /dev/scull0     # 观察系统调用返回值
dmesg | tail                           # 查看 printk
rmmod scull                            # 卸载
```
