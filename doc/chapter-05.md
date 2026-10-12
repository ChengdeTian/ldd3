# 第5章 并发与竞争：内核同步原语全景

> 本章讲解 Linux 内核里"一次做多件事"时如何保护共享资源。
> 竞争（race condition）来自对同一份资源的并发访问，解决手段是**把临界区串行化**。
>
> 环境见 [buildroot-ext4-migration.md](./buildroot-ext4-migration.md)；
> 驱动代码见 `driver/chapter-03/`、`driver/chapter-04/`，LDD3 原版见 `lld3-example/scull/`。

---

## 一、总体分析

### 1.1 并发从哪里来

内核代码可能被**多个执行流同时进入**，至少有这五个来源：

| # | 并发源 | 说明 |
|---|---|---|
| 1 | **多进程/多线程** | 多个进程同时 `open()` 同一个设备、同时 `read()`/`write()` |
| 2 | **SMP 多处理器** | 同一份代码在两个 CPU 上真正并行执行 |
| 3 | **中断处理程序** | 硬中断可以抢占正在执行的进程上下文 |
| 4 | **软中断 / tasklet / 定时器** | 在 `_bh` 或 `_irq` 上下文里再次进入同一段代码 |
| 5 | **内核抢占** | `CONFIG_PREEMPT=y` 时，内核代码随时可能被抢占并切换到另一个执行流 |

所以"驱动里只有我一个人在改这个结构体"这个假设**永远不成立**。

### 1.2 竞争的本质

```
        共享资源 R（全局变量 / 设备结构体 / 硬件寄存器 / 链表）
                 ▲            ▲            ▲
        进程 A ──┘     进程 B ─┘     中断处理 ──┘
              read-modify-write 不是原子操作 → 交错 → 结果不确定
```

典型症状：
- **数据损坏**：两个进程同时写 `dev->size`，其中一个的更新丢失；
- **链表被破坏**：一个进程在遍历，另一个在 `kfree`；
- **内核 Oops / 死锁 / 内存泄漏**。

### 1.3 内核把锁分成三大类

这是内核官方文档给出的分类（`Documentation/locking/locktypes.rst`），也是理解全部原语的**总纲**：

```12:21:kernel/linux-6.12.10/Documentation/locking/locktypes.rst
The kernel provides a variety of locking primitives which can be divided
into three categories:

 - Sleeping locks
 - CPU local locks
 - Spinning locks

This document conceptually describes these lock types and provides rules
for their nesting, including the rules for use under PREEMPT_RT.
```

| 类别 | 成员 | 获取后能睡眠吗 | 能用在中断上下文吗 | 跨 CPU 互斥 |
|---|---|---|---|---|
| **睡眠锁** | `mutex` / `rt_mutex` / `semaphore` / `rw_semaphore` / `ww_mutex` / `percpu_rw_semaphore` | ✅ 本来就会睡 | ❌ 绝对不行 | ✅ |
| **CPU 本地锁** | `local_lock` | — | 见下 | ❌ **只保护本 CPU** |
| **自旋锁** | `raw_spinlock_t`、位自旋锁；非 RT 内核上还有 `spinlock_t`、`rwlock_t` | ❌ 忙等，禁止睡眠 | ✅ | ✅ |

```37:44:kernel/linux-6.12.10/Documentation/locking/locktypes.rst
Sleeping lock types:

 - mutex
 - rt_mutex
 - semaphore
 - rw_semaphore
 - ww_mutex
 - percpu_rw_semaphore
```

### 1.4 三条选型铁律

```
要保护的东西会被中断处理程序访问吗？
├─ 会 → 必须用自旋锁（且要 _irqsave/_irq/_bh 变体），因为中断上下文不能睡眠
└─ 不会
   ├─ 临界区里有可能睡眠的操作吗？（copy_to_user/kmalloc(GFP_KERNEL)/wait_event/...）
   │  ├─ 有 → 只能用睡眠锁（mutex / semaphore / rw_semaphore）
   │  └─ 没有 → 两者都行，优先自旋锁（更快），但要保证临界区极短
   └─ 只是"等某个事件发生"吗？ → completion 或等待队列，不要用锁硬凑
```

再补两条：

- **睡眠锁不能在原子上下文（中断、软中断、持有自旋锁时）获取** —— 这是最常见的致命错误；
- **自旋锁临界区必须极短，且其中绝不能睡眠**（不能 `copy_to_user`、不能 `kmalloc(GFP_KERNEL)`、不能 `msleep`）。

### 1.5 一个"选择决策表"

| 需求 | 首选原语 | 备选 / 说明 |
|---|---|---|
| 保护一小段共享数据，中断也会访问 | `spin_lock_irqsave()` | `spin_lock_irq()` / `spin_lock_bh()`（见 2.2） |
| 保护一段可能睡眠的临界区 | `mutex` | `semaphore`（旧的写法） |
| 只需要"资源计数"（N 个槽位） | `semaphore` | `atomic_t` + 等待队列 |
| 等待某件事/某个硬件操作完成 | `completion` | 等待队列 |
| 读多写少、且临界区短 | `rwlock_t`（不推荐新增）/ RCU | 长临界区用 `rw_semaphore` |
| 只保护本 CPU 的数据 | `local_lock` / `preempt_disable()` | 不能防中断，需要配 `local_irq_disable()` |
| 单个整数计数/标志 | `atomic_t` / `refcount_t` | 无锁，最快 |
| 64 位一致性读取（如时间戳） | `seqlock_t` / `seqcount_t` | 读者无锁、重试 |

---

## 二、逐个原语的特点与用法

### 2.1 原子变量：`atomic_t` / `refcount_t`

```c
atomic_t counter = ATOMIC_INIT(0);

atomic_inc(&counter);              /* counter++ 的原子版本 */
atomic_dec(&counter);
if (atomic_dec_and_test(&counter)) /* 减到 0 时返回 true */
        ...
int v = atomic_read(&counter);
atomic_set(&counter, 5);
```

| 项 | 说明 |
|---|---|
| 能否睡眠 | ❌ 不睡眠（本质是加锁的 CPU 指令） |
| 中断上下文 | ✅ 可以 |
| 能保护 | **只能保护单个整数变量**，不能保护"变量之间的联系" |
| 典型用途 | 引用计数、打开计数、统计量、状态标志 |

**`refcount_t` 比 `atomic_t` 更适合引用计数**：它在**溢出/下溢**时会报警（并发 bug 的信号），而 `atomic_t` 会静默回绕。

> 你在 `lld3-example/scull/access.c` 里能看到它的用法——`scullsingle` 用 `atomic_dec_and_test()` 实现"只允许一个进程打开"：
> ```c
> static atomic_t scull_s_available = ATOMIC_INIT(1);
> if (!atomic_dec_and_test(&scull_s_available)) {
>         atomic_inc(&scull_s_available);   /* 回滚，必须做 */
>         return -EBUSY;
> }
> ```

### 2.2 自旋锁：`spinlock_t`

**特点**：拿不到锁就**原地忙等（busy-wait）**，同时**关抢占**。

```c
static DEFINE_SPINLOCK(my_lock);          /* 静态初始化 */

spin_lock_init(&my_lock);                 /* 动态初始化 */

unsigned long flags;
spin_lock_irqsave(&my_lock, flags);       /* 最安全的标准写法 */
        /* 临界区：极短，绝不允许睡眠 */
spin_unlock_irqrestore(&my_lock, flags);
```

#### 三个变体，解决三种不同的抢占源

```76:83:kernel/linux-6.12.10/Documentation/locking/locktypes.rst
Spinning locks implicitly disable preemption and the lock / unlock functions
can have suffixes which apply further protections:

 ===================  ====================================================
 _bh()                Disable / enable bottom halves (soft interrupts)
 _irq()               Disable / enable interrupts
 _irqsave/restore()   Save and disable / restore interrupt disabled state
 ===================  ====================================================
```

| 变体 | 额外关闭 | 用在哪 | 代价 |
|---|---|---|---|
| `spin_lock()` | 只关抢占 | 确认不会被中断/软中断访问的纯进程上下文 | 最快 |
| `spin_lock_bh()` | + 软中断 | 数据会被 tasklet/软中断访问 | 中 |
| `spin_lock_irq()` | + 硬中断 | 数据会被硬中断访问（已知中断本来就开着） | 较慢 |
| `spin_lock_irqsave()` | + 硬中断（保存原状态） | **通用安全写法**，函数可能被不同上下文调用 | 最慢但最稳 |

**为什么需要 `_irq` 变体？** 官方文档给的死锁场景：

```121:134:kernel/linux-6.12.10/Documentation/locking/spinlocks.rst
The reasons you mustn't use these versions if you have interrupts that
play with the spinlock is that you can get deadlocks::

	spin_lock(&lock);
	...
		<- interrupt comes in:
			spin_lock(&lock);

where an interrupt tries to lock an already locked variable. This is ok if
the other interrupt happens on another CPU, but it is _not_ ok if the
interrupt happens on the same CPU that already holds the lock, because the
lock will obviously never be released
```

> 关键点：**只要"持锁的代码"和"中断处理程序"可能访问同一个锁，就必须用 `_irqsave`（或 `_irq`）**，
> 因为中断可能在本 CPU 上打断持锁者，然后去抢同一把锁 → 自己等自己 → 死锁。

#### 自旋锁的使用禁忌

| 禁忌 | 后果 |
|---|---|
| 临界区里 `copy_to_user` / `copy_from_user` | 可能缺页睡眠 → "scheduling while atomic" BUG |
| 临界区里 `kmalloc(GFP_KERNEL)` | 可能睡眠 → 同上（要用 `GFP_ATOMIC`） |
| 临界区里调用任何可能睡眠的函数（`msleep`、`mutex_lock`、`wait_event`） | 同上 |
| 递归获取同一把锁 | 死锁（非递归锁） |
| 临界区太长 | 其他 CPU 空转烧 CPU，性能崩塌 |
| 在中断上下文用 `spin_lock()` 而不是 `_irqsave` | 可能死锁 |

自旋锁**保证内存序**（acquire/release 语义），所以临界区内的写操作对释放锁之后的另一个 CPU 是可见的：

```24:30:kernel/linux-6.12.10/Documentation/locking/spinlocks.rst
   NOTE! Implications of spin_locks for memory are further described in:

     Documentation/memory-barriers.txt

       (5) ACQUIRE operations.

       (6) RELEASE operations.
```

### 2.3 读写自旋锁：`rwlock_t`

```c
rwlock_t lock = __RW_LOCK_UNLOCKED(lock);

read_lock(&lock);      read_unlock(&lock);      /* 多个读者可并发 */
write_lock(&lock);     write_unlock(&lock);     /* 写者独占 */
```

| 项 | 说明 |
|---|---|
| 优点 | 允许多个读者同时进入临界区 |
| 缺点 | **原子操作比普通自旋锁更多**，读者临界区不长时反而更慢 |
| 限制 | **不能把读锁升级成写锁**（必须一开头就申请写锁） |

```61:63:kernel/linux-6.12.10/Documentation/locking/spinlocks.rst
   NOTE! reader-writer locks require more atomic memory operations than
   simple spinlocks.  Unless the reader critical section is long, you
   are better off just using spinlocks.
```
```87:93:kernel/linux-6.12.10/Documentation/locking/spinlocks.rst
Also, you cannot "upgrade" a read-lock to a write-lock, so if you at _any_
time need to do any changes (even if you don't do it every time), you have
to get the write-lock at the very beginning.

   NOTE! We are working hard to remove reader-writer spinlocks in most
   cases, so please don't add a new one without consensus.
```

> **现代建议**：新代码**不要再引入 `rwlock_t`**。读多写少的场景优先考虑 **RCU** 或 `rwsem`。

### 2.4 顺序锁：`seqlock_t` / `seqcount_t`

```5:9:kernel/linux-6.12.10/include/linux/seqlock.h
/*
 * seqcount_t / seqlock_t - a reader-writer consistency mechanism with
 * lockless readers (read-only retry loops), and no writer starvation.
 *
 * See Documentation/locking/seqlock.rst
 */
```

**核心思想**：写者每次进出临界区给序数 `+1`（进入时变奇、退出时变偶）；读者**不加锁**，读前后各取一次序数，如果两次不一致或为奇数就**重试**。

```c
/* 写者 */
write_seqlock(&lock);
   /* 修改数据 */
write_sequnlock(&lock);

/* 读者（无锁） */
unsigned int seq;
do {
        seq = read_seqbegin(&lock);
        /* 读数据到局部变量 */
} while (read_seqretry(&lock, seq));
```

| 项 | 说明 |
|---|---|
| 读者 | **完全无锁、不阻塞、不写内存**（只读） → 非常适合 `gettimeofday` 这类场景 |
| 写者 | 需要独占（`seqlock_t` 内部用自旋锁） |
| 缺点 | 读者可能反复重试；**读者的临界区里不能有副作用**（不能 `write`、不能改状态），因为可能被丢弃重来 |
| 典型用途 | 时间戳、统计信息、读多写极少的小结构 |

### 2.5 互斥体：`mutex`（首选）

**这是现代内核里"互斥"的推荐原语**，2006 年引入以取代"用二值信号量当锁"的写法。

```c
static DEFINE_MUTEX(my_mutex);      /* 静态 */
mutex_init(&my_mutex);              /* 动态 */

mutex_lock(&my_mutex);              /* 不可打断（TASK_UNINTERRUPTIBLE） */
        /* 临界区里可以睡眠 */
mutex_unlock(&my_mutex);

if (mutex_lock_interruptible(&my_mutex))   /* 可被信号打断 → 返回 -EINTR */
        return -ERESTARTSYS;
if (mutex_trylock(&my_mutex)) { ... }      /* 不等待，拿到返回 1 */
```

#### 实现上的"三档路径"

```37:67:kernel/linux-6.12.10/Documentation/locking/mutex-design.rst
When acquiring a mutex, there are three possible paths that can be
taken, depending on the state of the lock:

(i) fastpath: tries to atomically acquire the lock by cmpxchg()ing the owner with
    the current task. ...
(ii) midpath: aka optimistic spinning, tries to spin for acquisition
     while the lock owner is running ...
(iii) slowpath: last resort, if the lock is still unable to be acquired,
      the task is added to the wait-queue and sleeps until woken up by the
      unlock path. Under normal circumstances it blocks as TASK_UNINTERRUPTIBLE.
```

所以 mutex **虽然叫"睡眠锁"，但短竞争时会先乐观自旋**（比信号量快），这正是它性能更好的原因。

#### mutex 的"九条军规"（内核强制检查）

```78:89:kernel/linux-6.12.10/Documentation/locking/mutex-design.rst
The mutex subsystem checks and enforces the following rules:

    - Only one task can hold the mutex at a time.
    - Only the owner can unlock the mutex.
    - Multiple unlocks are not permitted.
    - Recursive locking/unlocking is not permitted.
    - A mutex must only be initialized via the API (see below).
    - A task may not exit with a mutex held.
    - Memory areas where held locks reside must not be freed.
    - Held mutexes must not be reinitialized.
    - Mutexes may not be used in hardware or software interrupt
      contexts such as tasklets and timers.
```

> 打开 `CONFIG_DEBUG_MUTEXES` 后这些规则会被真正检查，违规会打印详细信息（含加锁点、持有者）。

#### 一个容易被忽略的陷阱：`mutex_unlock()` 之后结构体还不能立刻释放

```104:116:kernel/linux-6.12.10/Documentation/locking/mutex-design.rst
Mutexes - and most other sleeping locks like rwsems - do not provide an
implicit reference for the memory they occupy, ...

mutex_unlock() may access the mutex structure even after it has internally
released the lock already - so it's not safe for another context to
acquire the mutex and assume that the mutex_unlock() context is not using
the structure anymore.
```

对比：`spin_unlock()` 之后内核保证不再碰这个锁结构，所以自旋锁可以"解锁后立刻 `kfree`"；而 mutex 不行。

### 2.6 信号量：`semaphore`

```15:19:kernel/linux-6.12.10/include/linux/semaphore.h
struct semaphore {
	raw_spinlock_t		lock;
	unsigned int		count;
	struct list_head	wait_list;
};
```

```c
DEFINE_SEMAPHORE(my_sem, 1);        /* 静态：注意必须给初值 */
sema_init(&sem, 1);                 /* 动态：1 = 二值（当互斥用），N = 计数 */

down(&sem);                         /* 不可打断（已 deprecated） */
if (down_interruptible(&sem))       /* 被信号打断 → 返回 -EINTR */
        return -ERESTARTSYS;
if (down_killable(&sem))            /* 只被致命信号打断 */
        return -ERESTARTSYS;
if (down_trylock(&sem))             /* 不等待 */
        return -EBUSY;
if (down_timeout(&sem, HZ))         /* 超时 → -ETIME */
        return -ETIME;
up(&sem);
```

**与 mutex 的本质差异**：**信号量没有 owner**。

```28:33:kernel/linux-6.12.10/include/linux/semaphore.h
/*
 * Unlike mutexes, binary semaphores do not have an owner, so up() can
 * be called in a different thread from the one which called down().
 * It is also safe to call down_trylock() and up() from interrupt
 * context.
 */
```

| 维度 | `semaphore` | `mutex` |
|---|---|---|
| 计数 | 可为任意 N（计数信号量） | 恒为 1（互斥） |
| owner 语义 | ❌ 无 → `up()` 可跨线程/中断调用 | ✅ 有 → 必须由持有者解锁 |
| 中断上下文 | `down_trylock()` / `up()` 可用 | ❌ 完全不可用 |
| 性能 | 较慢（无乐观自旋） | 更快（fastpath + 乐观自旋） |
| 调试 | 有限 | lockdep / DEBUG_MUTEXES 完整检查 |
| 优先级继承 | 不支持（可能优先级反转） | 非 RT 也不支持；`rt_mutex` 支持 |

**内核的态度很明确：新代码不要再用信号量做互斥。**

```113:120:kernel/linux-6.12.10/Documentation/locking/locktypes.rst
semaphore
=========

semaphore is a counting semaphore implementation.

Semaphores are often used for both serialization and waiting, but new use
cases should instead use separate serialization and wait mechanisms, such
as mutexes and completions.
```

> 也就是说：**"串行化"用 mutex，"等待事件"用 completion**，别再让信号量兼顾两职。

### 2.7 读写信号量：`rw_semaphore`

```132:141:kernel/linux-6.12.10/Documentation/locking/locktypes.rst
rw_semaphore
============

rw_semaphore is a multiple readers and single writer lock mechanism.

On non-PREEMPT_RT kernels the implementation is fair, thus preventing
writer starvation.

rw_semaphore complies by default with the strict owner semantics, but there
exist special-purpose interfaces that allow non-owner release for readers.
```

```c
struct rw_semaphore rwsem;
init_rwsem(&rwsem);

down_read(&rwsem);    up_read(&rwsem);      /* 多个读者 */
down_write(&rwsem);   up_write(&rwsem);     /* 写者独占 */
down_read_trylock(&rwsem);
downgrade_write(&rwsem);                    /* 写锁 → 读锁（这是允许的降级）*/
```

| 项 | 说明 |
|---|---|
| 能否睡眠 | ✅ 可以（比 `rwlock_t` 更适合长临界区） |
| 公平性 | 非 RT 内核是公平的（不会饿死写者） |
| 与 `rwlock_t` 的区别 | 临界区可以睡眠、可以更长的操作 |
| 典型用途 | 文件系统的 inode 锁、`mmap_lock`、配置表（读多写少且临界区长） |

### 2.8 完成量：`completion`（等事件的专用工具）

```26:29:kernel/linux-6.12.10/include/linux/completion.h
struct completion {
	unsigned int done;
	struct swait_queue_head wait;
};
```

```c
/* 初始化 */
DECLARE_COMPLETION(done);                 /* 静态 */
DECLARE_COMPLETION_ONSTACK(done);         /* 栈上：函数内使用 */
init_completion(&done);                   /* 动态分配时 */
reinit_completion(&done);                 /* 复用前必须重置（尤其是用过 complete_all）*/

/* 等待方（通常是一个内核线程或进程上下文） */
wait_for_completion(&done);                           /* 不可打断 */
if (wait_for_completion_interruptible(&done))         /* 可被信号打断 → -ERESTARTSYS */
        return -ERESTARTSYS;
if (wait_for_completion_killable(&done))              /* 只被致命信号打断 */
        return -ERESTARTSYS;
long left = wait_for_completion_timeout(&done, HZ);   /* 超时：返回剩余 jiffies，0 = 超时 */
if (!try_wait_for_completion(&done)) { ... }          /* 不等待 */

/* 唤醒方（可以是中断上下文！） */
complete(&done);            /* 唤醒一个等待者 */
complete_all(&done);        /* 唤醒所有等待者 */
```

| 特性 | 说明 |
|---|---|
| **专为"等一件事发生"设计** | 语义比信号量清晰得多：`wait_for_completion()` / `complete()` |
| **唤醒方可以在中断上下文** | 这是它最常用的场景：中断处理程序 `complete()` → 唤醒睡眠的进程 |
| 一次性 | `done` 递减，唤醒一个少一个；`complete_all` 后要 `reinit_completion()` 才能复用 |
| 不会"丢失事件" | `complete()` 在无人等待时会把 `done++`，之后来的等待者立刻返回（不像条件变量那样丢通知） |
| 睡眠 | 等待方睡眠，必须在可睡眠的上下文调用 |

> **典型场景**：驱动发起一次硬件操作（DMA 完成、ADC 采样结束、命令响应到达），
> 中断处理程序里 `complete(&dev->done)`，进程上下文里 `wait_for_completion_interruptible(&dev->done)`。
> `misc-modules/` 与 `short` 驱动里都有这种用法。

### 2.9 CPU 本地锁：`local_lock` / `preempt_disable()`

```167:174:kernel/linux-6.12.10/Documentation/locking/locktypes.rst
 ===============================  ======================
 local_lock(&llock)               preempt_disable()
 local_unlock(&llock)             preempt_enable()
 local_lock_irq(&llock)           local_irq_disable()
 local_unlock_irq(&llock)         local_irq_enable()
 local_lock_irqsave(&llock)       local_irq_save()
 local_unlock_irqrestore(&llock)  local_irq_restore()
 ===============================  ======================
```

| 项 | 说明 |
|---|---|
| 保护范围 | **只保护本 CPU**（`per-CPU` 变量），对其他 CPU 无效 |
| 防不防中断 | 不加 `_irq` 后缀就**不防中断** |
| 为什么要用它 | 关抢占/关中断是**最便宜的**并发控制（无需原子指令），而 `local_lock` 给它一个**具名作用域**，便于 lockdep 检查和阅读 |
| 常见用途 | `per-CPU` 计数器、`this_cpu_inc()` 这类操作的保护 |

### 2.10 RCU（读多写少的终极方案，简述）

| 项 | 说明 |
|---|---|
| 读者 | `rcu_read_lock()`（本质只关抢占）→ 遍历 → `rcu_read_unlock()`，**零原子操作、几乎无开销** |
| 写者 | 拷贝-修改-替换指针（`rcu_assign_pointer()`），旧数据等**宽限期（grace period）**结束再 `kfree_rcu()` |
| 限制 | 读者临界区内**不能睡眠**；写者要处理"旧数据还在被读"的问题 |
| 典型用途 | 链表/哈希表查找（路由表、dentry 缓存、设备列表） |

> `Documentation/locking/spinlocks.rst` 里也建议：**链表遍历优先考虑 RCU**。

---

## 三、典型使用场景

### 场景 1：保护驱动内部数据结构（scull 的做法）

`scull` 用一个"二值信号量"保护 `dev->data` / `dev->size`。

```c
/* 初始化（chapter-03/scull.c） */
sema_init(&scull_devices[i].sem, 1);     /* count=1，当互斥用 */

/* read/write 里 */
if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
... 修改 dev->data / dev->size ...
up(&dev->sem);
```

**点评**：
- 语义正确（二值信号量与 mutex 等价），临界区里调用了 `copy_to_user`/`copy_from_user`（可能睡眠）→ **必须用睡眠锁**，不能用自旋锁 ✅；
- 但按现代规范应改成 `struct mutex` + `mutex_lock_interruptible()`：有 owner 检查、性能更好。

### 场景 2：等硬件/等事件（completion）

```c
struct my_dev {
        struct completion done;
        ...
};

/* 发起操作（进程上下文） */
reinit_completion(&dev->done);
start_hardware_operation();
if (wait_for_completion_interruptible(&dev->done))
        return -ERESTARTSYS;

/* 中断处理程序 */
irqreturn_t my_isr(int irq, void *arg)
{
        struct my_dev *dev = arg;
        complete(&dev->done);       /* 中断里可以安全调用 */
        return IRQ_HANDLED;
}
```

### 场景 3：资源计数（计数信号量）

比如"最多允许 4 个并发使用者共享一个缓冲区池"：

```c
sema_init(&pool->sem, 4);        /* 4 个槽位 */

if (down_interruptible(&pool->sem))
        return -ERESTARTSYS;
slot = alloc_slot();
...
free_slot(slot);
up(&pool->sem);
```

### 场景 4：中断与进程共享数据（自旋锁 + `_irqsave`）

```c
struct my_dev {
        spinlock_t lock;         /* 中断处理程序也会访问 */
        u32 status;
        struct list_head queue;
};

/* 进程上下文 */
unsigned long flags;
spin_lock_irqsave(&dev->lock, flags);
        dev->status |= USER_BIT;
spin_unlock_irqrestore(&dev->lock, flags);

/* 中断上下文 */
spin_lock(&dev->lock);           /* 已经关中断了，不需要 _irqsave */
        dev->status |= IRQ_BIT;
spin_unlock(&dev->lock);
```

> **规则**：只要"进程上下文持锁的代码"和"中断处理程序"会碰同一把锁，进程侧就**必须**用 `_irqsave`。

### 场景 5：生产者-消费者（scullpipe）

`lld3-example/scull/pipe.c` 是教科书式的组合：**自旋锁/信号量 + 等待队列 + 环形缓冲**。

| 机制 | 代码 | 作用 |
|---|---|---|
| 互斥 | `struct semaphore sem` + `down_interruptible()` | 保护 `rp`/`wp`/缓冲区 |
| 等待队列 | `wait_queue_head_t inq, outq` | 读者睡在 `inq`、写者睡在 `outq` |
| 阻塞 | `wait_event_interruptible(dev->inq, dev->rp != dev->wp)` | 无数据时睡眠 |
| 唤醒 | `wake_up_interruptible(&dev->inq)` | 写完后叫醒读者 |
| 异步通知 | `kill_fasync(&dev->async_queue, SIGIO, POLL_IN)` | 支持 `SIGIO` |
| 非阻塞 | `O_NONBLOCK` → `-EAGAIN` | 不睡眠的调用者 |
| 多路复用 | `poll_wait()` + `POLLIN/POLLOUT` | 支撑 `poll/select` |

### 场景 6：读多写少的大结构

| 数据特征 | 选择 |
|---|---|
| 几乎只读、偶尔更新、读者不能睡眠 | **RCU** |
| 读多写少、临界区长（可能睡眠） | `rw_semaphore` |
| 读多写少、临界区极短、不睡眠 | `rwlock_t`（不推荐新增） |
| 只是几个字节的一致性读取 | `seqlock_t` |

### 场景 7：打开设备时的"独占/权限"控制

（`lld3-example/scull/access.c` 的三种实现，正好代表三种工具）

| 设备 | 手段 | 特点 |
|---|---|---|
| `scullsingle` | `atomic_t` + `atomic_dec_and_test()` | 最快、无锁 |
| `sculluid` | `spinlock_t` + uid 比较 | 保护"计数 + 属主"两个变量的一致性 |
| `scullwuid` | `wait_queue_head_t` + `wait_event_interruptible()` | 让别人**阻塞等待**而不是报错 |
| `scullpriv` | `spinlock_t` + 链表 + `kmalloc` | 动态克隆设备 |

---

## 四、实战对照：把 scull 的锁"现代化"

| 位置 | 现状 | 建议 |
|---|---|---|
| `struct scull_dev.sem` | `struct semaphore` + `sema_init(...,1)` | 换 `struct mutex` + `mutex_init()` + `mutex_lock_interruptible()` |
| `scull_read/write` | 持信号量调 `copy_*_user` | 保持睡眠锁（**不能**改自旋锁，否则 sleep-in-atomic） |
| `scull_open` 里的 `scull_trim()` | 已加 `down/up` ✅ | 继续保证"每一条返回路径都解锁" |
| `scull_ioctl` 改全局量 | **完全没加锁** | 用 mutex 保护 `scull_quantum/scull_qset`，或改成 `atomic_t` |
| `scullpipe` 的等待队列 | `wait_event_interruptible` | 正确；注意"先解锁再睡眠"的模式 |
| `access.c` 自旋锁内 `kmalloc(GFP_KERNEL)` | ⚠️ 原子上下文睡眠 | 改成 `GFP_ATOMIC`，或把 `kmalloc` 移到锁外 |

---

## 五、常见错误与死锁排查

### 5.1 五类高频错误

| # | 错误 | 症状 | 修法 |
|---|---|---|---|
| 1 | **持自旋锁时睡眠**（`copy_to_user` / `kmalloc(GFP_KERNEL)` / `mutex_lock`） | `BUG: scheduling while atomic` | 移到锁外，或用睡眠锁 |
| 2 | **中断上下文用睡眠锁** | 同上 / 死锁 | 换自旋锁或用 completion |
| 3 | **该用 `_irqsave` 却用 `spin_lock`** | 偶发死锁（本 CPU 中断打断持锁者） | 加 `_irqsave` |
| 4 | **加锁顺序不一致**（A→B vs B→A） | 死锁（lockdep 会报） | 全局统一顺序 |
| 5 | **忘记在某条返回路径解锁** | 第二次操作永久卡死 | 用 `goto out` 统一出口；或用 `guard()`/`scoped_guard()` |

> 第 5 条是我们在第 3 章真实踩到的 bug：`scull_read()` 成功路径写成 `return 0;`，
> 既没返回 `retval` 也没 `up(&dev->sem)` → 读为空 + 第二次操作死锁。

### 5.2 死锁的四个必要条件与对策

| 条件 | 对策 |
|---|---|
| 互斥 | 无法消除（锁的本质） |
| 持有并等待 | 一次性申请所有锁，或先释放再加 |
| 不可抢占 | 使用 `_trylock` 失败就回退重试 |
| 循环等待 | **给所有锁定义全局顺序，永远按同一顺序加锁**（最有效） |

### 5.3 调试工具

| 工具 | 作用 |
|---|---|
| `CONFIG_PROVE_LOCKING`（lockdep） | 静态分析锁的依赖关系，检测"潜在"死锁与错误用法 |
| `CONFIG_DEBUG_MUTEXES` | 检查 mutex 的九条规则（递归、错主解锁、中断上下文等） |
| `CONFIG_DEBUG_ATOMIC_SLEEP` | 检测"原子上下文里睡眠" |
| `might_sleep()` | 在可能睡眠的函数入口加它，让内核在原子上下文调用时报警 |
| `lockdep_assert_held(&lock)` | 断言"此处应该持有某锁"，便于早期发现遗漏 |
| `CONFIG_DEBUG_SPINLOCK` | 检查自旋锁的重复加锁/解锁 |

---

## 六、API 速查表

| 原语 | 初始化 | 加锁 | 解锁 | 可睡眠 | 中断可用 | owner |
|---|---|---|---|---|---|---|
| `atomic_t` | `ATOMIC_INIT(n)` | `atomic_inc/dec/set` | — | ❌ | ✅ | — |
| `refcount_t` | `REFCOUNT_INIT(n)` | `refcount_inc()` | `refcount_dec*()` | ❌ | ✅ | — |
| `spinlock_t` | `DEFINE_SPINLOCK(x)` / `spin_lock_init()` | `spin_lock[_irqsave/_irq/_bh]()` | `spin_unlock[...]()` | ❌ | ✅ | 无（但必须同上下文解锁） |
| `rwlock_t` | `DEFINE_RWLOCK(x)` | `read_lock()` / `write_lock()` | `read_unlock()` / `write_unlock()` | ❌ | ✅ | — |
| `seqlock_t` | `DEFINE_SEQLOCK(x)` | `write_seqlock()` / `read_seqbegin()` | `write_sequnlock()` / `read_seqretry()` | ❌（读者无锁） | ✅ | — |
| `mutex` | `DEFINE_MUTEX(x)` / `mutex_init()` | `mutex_lock[_interruptible/_killable/_trylock]()` | `mutex_unlock()` | ✅ | ❌ | ✅ 严格 |
| `semaphore` | `sema_init(&s, n)` / `DEFINE_SEMAPHORE(s, n)` | `down[_interruptible/_killable/_trylock/_timeout]()` | `up()` | ✅ | `trylock`/`up` 可用 | ❌ 无 |
| `rw_semaphore` | `init_rwsem()` | `down_read()` / `down_write()` | `up_read()` / `up_write()` | ✅ | ❌ | 读者可降级 |
| `completion` | `DECLARE_COMPLETION(x)` / `init_completion()` | `wait_for_completion*()` | `complete[_all]()` | ✅（等待方） | 唤醒方 ✅ | — |
| `local_lock` | `DEFINE_LOCAL_LOCK(x)` | `local_lock[_irq/_irqsave]()` | `local_unlock[...]()` | ❌ | ✅ | 仅本 CPU |

---

## 七、参考：内核源码与文档位置

| 内容 | 位置 |
|---|---|
| 锁的分类与嵌套规则（**本章总纲**） | `Documentation/locking/locktypes.rst` |
| 自旋锁实战教训（含死锁图） | `Documentation/locking/spinlocks.rst` |
| mutex 设计、九条规则、三档路径 | `Documentation/locking/mutex-design.rst` |
| 顺序锁设计 | `Documentation/locking/seqlock.rst` |
| lockdep 原理 | `Documentation/locking/lockdep-design.rst` |
| 抢占与锁 | `Documentation/locking/preempt-locking.rst` |
| RCU | `Documentation/RCU/rcu.rst`、`Documentation/RCU/listRCU.rst` |
| 内存屏障（锁的内存序语义） | `Documentation/memory-barriers.txt` |
| `struct semaphore` / 无 owner 说明 | `include/linux/semaphore.h:15`、`:28` |
| `struct mutex` / `mutex_init` | `include/linux/mutex.h:60`、`:86` |
| `struct completion` / API | `include/linux/completion.h:26`、`:102` |
| `struct seqcount/seqlock` | `include/linux/seqlock.h:5` |
| mutex 实现（owner 位编码、乐观自旋） | `kernel/locking/mutex.c:59` |
| semaphore 实现 | `kernel/locking/semaphore.c` |
| completion 实现 | `kernel/sched/completion.c` |

---

## 八、一句话总结

| 原语 | 一句话 |
|---|---|
| `atomic_t` | 只保护一个整数，无锁，最快 |
| `spinlock_t` | 忙等、关抢占，用于**中断也会碰**的短临界区；跨上下文一律 `_irqsave` |
| `mutex` | **互斥的首选**，可睡眠、有 owner、有调试；不能在中断用 |
| `semaphore` | 计数/无 owner，可跨上下文 `up()`；**新代码请改用 mutex + completion** |
| `rw_semaphore` | 读多写少且临界区可睡眠 |
| `completion` | **"等一件事"**的专用工具，中断里 `complete()` 唤醒进程 |
| `seqlock` | 读者无锁重试，写者独占；适合只读快照 |
| `local_lock` | 只保护本 CPU，本质是关抢占/关中断的具名封装 |
| RCU | 读多写少、读侧必须无睡眠、写侧靠宽限期回收 |
