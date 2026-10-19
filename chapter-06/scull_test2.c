/*
 * scull_test2.c -- scull chapter-06「scullpipe」设备测试程序
 *
 * 被测设备（pipe.c）：
 *   /dev/scullpipe0  环形缓冲区的字符管道
 *
 * 覆盖范围：
 *   1. 基本读写、不可 seek
 *   2. O_NONBLOCK：空读 -EAGAIN、写满 -EAGAIN
 *   3. 阻塞读：无数据时睡眠，写入后被唤醒
 *   4. 阻塞写：缓冲区满时睡眠，读走数据后被唤醒
 *   5. EOF：写端全部关闭后 read 返回 0
 *   6. EPIPE：读端全部关闭后 write 返回 -EPIPE
 *   7. poll：POLLIN / POLLOUT / POLLHUP
 *   8. fasync：O_ASYNC 注册后写入触发 SIGIO
 *   9. 环形回绕：跨 end 反复读写，数据顺序正确
 *
 * 用法：
 *   ./scull_test2 [-d /dev] [-M major] [-m minor] [-b buffer]
 *
 *   -d 设备节点目录（默认 /dev）
 *   -M 主设备号（默认从 /proc/devices 找 scull）
 *   -m scullpipe0 的次设备号（默认 scull_minor + scull_devs）
 *   -b 缓冲区大小（默认从 /sys/module/scull 读取，失败用 4000）
 *
 * 编译：
 *   aarch64-linux-gnu-gcc -static -O2 -Wall -o scull_test2 scull_test2.c
 *
 * 说明：设备节点不存在时会自动 mknod，建议 root 运行。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>

static int g_pass, g_fail, g_skip, g_todo;

#define OK(...)   do { g_pass++; printf("  [ OK ] "); printf(__VA_ARGS__); putchar('\n'); } while (0)
#define BAD(...)  do { g_fail++; printf("  [FAIL] "); printf(__VA_ARGS__); putchar('\n'); } while (0)
#define SKIP(...) do { g_skip++; printf("  [SKIP] "); printf(__VA_ARGS__); putchar('\n'); } while (0)
#define TODO(...) do { g_todo++; printf("  [TODO] "); printf(__VA_ARGS__); putchar('\n'); } while (0)
#define CHECK(cond, ...) do { if (cond) OK(__VA_ARGS__); else BAD(__VA_ARGS__); } while (0)

static char g_dev0[256];
static char g_dev1[256];
static int  g_bufsize = 4000;

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static void section(const char *name)
{
    printf("\n--- %s ---\n", name);
}

static int read_int_file(const char *path, int def)
{
    FILE *f = fopen(path, "r");
    int v;

    if (!f)
        return def;
    if (fscanf(f, "%d", &v) != 1)
        v = def;
    fclose(f);
    return v;
}

static int find_scull_major(void)
{
    FILE *f = fopen("/proc/devices", "r");
    char line[256], name[128];
    int major;

    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%d %127s", &major, name) == 2 &&
            strcmp(name, "scull") == 0) {
            fclose(f);
            return major;
        }
    }
    fclose(f);
    return -1;
}

static int ensure_node(const char *path, int maj, int min)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        if (!S_ISCHR(st.st_mode)) {
            fprintf(stderr, "%s 已存在但不是字符设备\n", path);
            return -1;
        }
        return 0;
    }

    if (mknod(path, S_IFCHR | 0666, makedev(maj, min)) < 0) {
        fprintf(stderr, "mknod %s 失败: %s（请用 root 运行）\n",
                path, strerror(errno));
        return -1;
    }
    chmod(path, 0666);
    return 0;
}

/* 驱动一次搬运量受缓冲区边界限制，必须循环写/读 */
static ssize_t write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t done = 0;

    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (w == 0)
            break;
        done += w;
    }
    return (ssize_t)done;
}

static ssize_t read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t done = 0;

    while (done < n) {
        ssize_t r = read(fd, p + done, n - done);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        done += r;
    }
    return (ssize_t)done;
}

/* 读空缓冲区（要求 fd 是 O_NONBLOCK） */
static void drain(int fd)
{
    char b[4096];

    while (read(fd, b, sizeof(b)) > 0)
        ;
}

static int poll_revents(int fd, short events, int timeout_ms)
{
    struct pollfd p;

    p.fd = fd;
    p.events = events;
    p.revents = 0;

    if (poll(&p, 1, timeout_ms) <= 0)
        return 0;
    return p.revents;
}

/* 用 alarm 给可能永久阻塞的调用兜底 */
static void on_alarm(int sig)
{
    (void)sig;
}

/* ------------------------------------------------------------------ */
/* 1. 基本读写、不可 seek                                              */
/* ------------------------------------------------------------------ */

static void test_basic(void)
{
    char wbuf[] = "hello-scullpipe";
    char rbuf[64];
    int fd;
    ssize_t n;
    off_t off;

    section("1. 基本读写与 lseek");

    fd = open(g_dev0, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        BAD("open(%s) 失败: %s", g_dev0, strerror(errno));
        return;
    }
    OK("open 成功 (fd=%d)", fd);

    n = write(fd, wbuf, strlen(wbuf));
    CHECK(n == (ssize_t)strlen(wbuf), "write %zu 字节 → %zd",
          strlen(wbuf), n);

    memset(rbuf, 0, sizeof(rbuf));
    n = read(fd, rbuf, sizeof(rbuf) - 1);
    CHECK(n == (ssize_t)strlen(wbuf) && memcmp(rbuf, wbuf, n) == 0,
          "read 读回 %zd 字节，内容%s", n,
          (n == (ssize_t)strlen(wbuf) && memcmp(rbuf, wbuf, n) == 0)
              ? "一致" : "不一致");

    errno = 0;
    n = read(fd, rbuf, sizeof(rbuf));
    CHECK(n < 0 && errno == EAGAIN,
          "空缓冲区 + O_NONBLOCK → -EAGAIN（实际 n=%zd, errno=%d）",
          n, errno);

    errno = 0;
    off = lseek(fd, 0, SEEK_SET);
    CHECK(off < 0 && errno == ESPIPE,
          "lseek → -ESPIPE（实际 off=%ld, errno=%d %s）",
          (long)off, errno, strerror(errno));

    close(fd);
}

/* ------------------------------------------------------------------ */
/* 2. O_NONBLOCK 写满                                                  */
/* ------------------------------------------------------------------ */

static void test_nonblock_full(void)
{
    char *buf;
    int fd;
    ssize_t total = 0;
    int capacity = g_bufsize - 1;
    int w;

    section("2. O_NONBLOCK：缓冲区写满");

    fd = open(g_dev0, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        BAD("open 失败: %s", strerror(errno));
        return;
    }

    drain(fd);

    buf = malloc(capacity > 0 ? capacity : 1);
    if (!buf) {
        BAD("malloc 失败");
        close(fd);
        return;
    }
    memset(buf, 'F', capacity);

    /* 一次 write 只能写连续的一段，循环直到 EAGAIN */
    while (total < 200000) {
        w = (int)write(fd, buf, capacity - total > 0 ? capacity - total : 1);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (w == 0)
            break;
        total += w;
    }

    CHECK(errno == EAGAIN && total == capacity,
          "写满后共写入 %zd 字节（容量 %d），下一次 write → -EAGAIN",
          total, capacity);

    errno = 0;
    w = (int)write(fd, "x", 1);
    CHECK(w < 0 && errno == EAGAIN,
          "满缓冲区再写 1 字节 → -EAGAIN（实际 %d, errno=%d）", w, errno);

    {
        char c;

        CHECK(read(fd, &c, 1) == 1, "读走 1 字节，腾出空间");
        errno = 0;
        w = (int)write(fd, "y", 1);
        CHECK(w == 1, "腾出空间后可再写 1 字节（实际 %d, errno=%d）",
              w, errno);
        drain(fd);
    }

    free(buf);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* 3. 阻塞读被 write 唤醒                                              */
/* ------------------------------------------------------------------ */

static void test_blocking_read(void)
{
    int fr, fw, pfd[2], sync[2];
    pid_t pid;
    struct child_msg { int ret; int err; } m;
    struct pollfd p;
    char c;

    section("3. 阻塞读：写入后唤醒");

    fr = open(g_dev0, O_RDONLY);
    if (fr < 0) {
        BAD("open O_RDONLY 失败: %s", strerror(errno));
        return;
    }
    fw = open(g_dev0, O_WRONLY);
    if (fw < 0) {
        BAD("open O_WRONLY 失败: %s", strerror(errno));
        close(fr);
        return;
    }

    if (pipe(pfd) < 0 || pipe(sync) < 0) {
        BAD("pipe 创建失败: %s", strerror(errno));
        close(fr); close(fw);
        return;
    }

    pid = fork();
    if (pid < 0) {
        BAD("fork 失败: %s", strerror(errno));
        close(fr); close(fw);
        close(pfd[0]); close(pfd[1]);
        close(sync[0]); close(sync[1]);
        return;
    }

    if (pid == 0) {
        ssize_t n;

        close(pfd[0]);
        close(fw);              /* 子进程不参与写 */

        if (write(sync[1], "S", 1) != 1)
            _exit(2);
        close(sync[1]);

        errno = 0;
        n = read(fr, &c, 1);    /* 应当阻塞 */
        m.ret = (int)n;
        m.err = errno;
        if (write(pfd[1], &m, sizeof(m)) != sizeof(m))
            _exit(2);
        _exit(0);
    }

    close(pfd[1]);
    close(sync[1]);
    close(fr);                  /* 父进程只负责写 */

    if (read(sync[0], &c, 1) != 1) {
        BAD("与子进程同步失败");
        close(sync[0]); close(pfd[0]); close(fw);
        waitpid(pid, NULL, 0);
        return;
    }
    close(sync[0]);

    usleep(300000);
    p.fd = pfd[0];
    p.events = POLLIN;
    p.revents = 0;
    CHECK(poll(&p, 1, 0) == 0, "无数据时子进程 read 阻塞中");

    CHECK(write(fw, "A", 1) == 1, "父进程写入 1 字节");

    p.revents = 0;
    CHECK(poll(&p, 1, 3000) == 1, "写入后子进程被唤醒");

    memset(&m, 0, sizeof(m));
    if (read(pfd[0], &m, sizeof(m)) == sizeof(m))
        CHECK(m.ret == 1 && m.err == 0,
              "子进程 read 返回 1 字节（ret=%d, errno=%d）", m.ret, m.err);
    else
        BAD("读取子进程结果失败");

    close(pfd[0]);
    close(fw);
    waitpid(pid, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* 4. 阻塞写被 read 唤醒                                               */
/* ------------------------------------------------------------------ */

static void test_blocking_write(void)
{
    int fd_full, fd_blk, pfd[2], sync[2];
    char *buf;
    int capacity = g_bufsize - 1;
    ssize_t total = 0;
    pid_t pid;
    struct child_msg { int ret; int err; } m;
    struct pollfd p;
    char c;
    int w;

    section("4. 阻塞写：读走数据后唤醒");

    fd_full = open(g_dev0, O_RDWR | O_NONBLOCK);
    if (fd_full < 0) {
        BAD("open 非阻塞 fd 失败: %s", strerror(errno));
        return;
    }
    drain(fd_full);

    buf = malloc(capacity > 0 ? capacity : 1);
    if (!buf) {
        BAD("malloc 失败");
        close(fd_full);
        return;
    }
    memset(buf, 'W', capacity);

    while (total < 200000) {
        w = (int)write(fd_full, buf, capacity - total > 0 ? capacity - total : 1);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (w == 0)
            break;
        total += w;
    }
    if (total != capacity) {
        BAD("准备写满失败：%zd/%d（errno=%d）", total, capacity, errno);
        free(buf);
        close(fd_full);
        return;
    }
    OK("缓冲区已写满（%d 字节）", capacity);

    fd_blk = open(g_dev0, O_RDWR);
    if (fd_blk < 0) {
        BAD("open 阻塞 fd 失败: %s", strerror(errno));
        free(buf);
        close(fd_full);
        return;
    }

    if (pipe(pfd) < 0 || pipe(sync) < 0) {
        BAD("pipe 创建失败: %s", strerror(errno));
        free(buf);
        close(fd_full); close(fd_blk);
        return;
    }

    pid = fork();
    if (pid < 0) {
        BAD("fork 失败: %s", strerror(errno));
        free(buf);
        close(fd_full); close(fd_blk);
        close(pfd[0]); close(pfd[1]);
        close(sync[0]); close(sync[1]);
        return;
    }

    if (pid == 0) {
        ssize_t n;

        close(pfd[0]);
        close(fd_full);         /* 子进程只用阻塞 fd */

        if (write(sync[1], "S", 1) != 1)
            _exit(2);
        close(sync[1]);

        errno = 0;
        n = write(fd_blk, "Z", 1);   /* 缓冲区满，应当阻塞 */
        m.ret = (int)n;
        m.err = errno;
        if (write(pfd[1], &m, sizeof(m)) != sizeof(m))
            _exit(2);
        _exit(0);
    }

    close(pfd[1]);
    close(sync[1]);

    if (read(sync[0], &c, 1) != 1) {
        BAD("与子进程同步失败");
        close(sync[0]); close(pfd[0]);
        free(buf); close(fd_full); close(fd_blk);
        waitpid(pid, NULL, 0);
        return;
    }
    close(sync[0]);

    usleep(300000);
    p.fd = pfd[0];
    p.events = POLLIN;
    p.revents = 0;
    CHECK(poll(&p, 1, 0) == 0, "缓冲区满时子进程 write 阻塞中");

    {
        char tmp[8];

        CHECK(read(fd_full, tmp, 1) == 1, "父进程读走 1 字节");
    }

    p.revents = 0;
    CHECK(poll(&p, 1, 3000) == 1, "读走数据后子进程被唤醒");

    memset(&m, 0, sizeof(m));
    if (read(pfd[0], &m, sizeof(m)) == sizeof(m))
        CHECK(m.ret == 1 && m.err == 0,
              "子进程 write 返回 1 字节（ret=%d, errno=%d）", m.ret, m.err);
    else
        BAD("读取子进程结果失败");

    close(pfd[0]);
    drain(fd_full);
    free(buf);
    close(fd_full);
    close(fd_blk);
    waitpid(pid, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* 5. EOF：写端全关                                                    */
/* ------------------------------------------------------------------ */

static void test_eof(void)
{
    int fd;
    char c;
    ssize_t n;

    section("5. EOF：写端全部关闭");

    /* 此时没有其他 fd，O_RDONLY 打开后 nwriters == 0 */
    fd = open(g_dev0, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        BAD("open O_RDONLY 失败: %s", strerror(errno));
        return;
    }

    errno = 0;
    n = read(fd, &c, 1);
    CHECK(n == 0, "无写端时 read 返回 0（EOF），实际 %zd, errno=%d",
          n, errno);
    close(fd);

    /* 阻塞模式下也必须立即返回 0，而不是睡死 */
    fd = open(g_dev0, O_RDONLY);
    if (fd < 0) {
        BAD("open 阻塞 O_RDONLY 失败: %s", strerror(errno));
        return;
    }

    signal(SIGALRM, on_alarm);
    alarm(2);
    errno = 0;
    n = read(fd, &c, 1);
    alarm(0);

    CHECK(n == 0, "阻塞 read 也立即返回 0（实际 %zd, errno=%d %s）",
          n, errno, n < 0 ? strerror(errno) : "");
    close(fd);
}

/* ------------------------------------------------------------------ */
/* 6. EPIPE：读端全关                                                  */
/* ------------------------------------------------------------------ */

static void test_epipe(void)
{
    int fd;
    ssize_t n;

    section("6. EPIPE：读端全部关闭");

    signal(SIGPIPE, SIG_IGN);   /* 防止默认动作杀进程 */

    fd = open(g_dev0, O_WRONLY);
    if (fd < 0) {
        BAD("open O_WRONLY 失败: %s", strerror(errno));
        return;
    }

    errno = 0;
    n = write(fd, "x", 1);
    CHECK(n < 0 && errno == EPIPE,
          "无读端时 write → -1/EPIPE（实际 %zd, errno=%d %s）",
          n, errno, n < 0 ? strerror(errno) : "");

    close(fd);
}

/* ------------------------------------------------------------------ */
/* 7. poll                                                             */
/* ------------------------------------------------------------------ */

static void test_poll(void)
{
    int fd, fr;
    char c;
    short re;

    section("7. poll 就绪状态");

    fd = open(g_dev0, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        BAD("open 失败: %s", strerror(errno));
        return;
    }
    drain(fd);

    re = poll_revents(fd, POLLIN | POLLOUT, 0);
    CHECK((re & POLLIN) == 0 && (re & POLLOUT) != 0,
          "空缓冲区：不可读、可写（revents=0x%x）", re);

    CHECK(write(fd, "p", 1) == 1, "写入 1 字节");

    re = poll_revents(fd, POLLIN | POLLOUT, 0);
    CHECK((re & POLLIN) != 0 && (re & POLLOUT) != 0,
          "有数据且未满：可读 + 可写（revents=0x%x）", re);

    /* 写满 */
    {
        char *big = malloc(g_bufsize);
        int total = 1;

        if (big) {
            memset(big, 'P', g_bufsize);
            while (total < g_bufsize - 1) {
                ssize_t w = write(fd, big, g_bufsize - 1 - total);

                if (w <= 0)
                    break;
                total += (int)w;
            }
            free(big);
        }

        re = poll_revents(fd, POLLIN | POLLOUT, 0);
        CHECK((re & POLLIN) != 0 && (re & POLLOUT) == 0,
              "写满：可读、不可写（revents=0x%x）", re);

        drain(fd);
        re = poll_revents(fd, POLLIN | POLLOUT, 0);
        CHECK((re & POLLIN) == 0 && (re & POLLOUT) != 0,
              "读空：不可读、可写（revents=0x%x）", re);
    }

    close(fd);

    /* HUP：只留读端，写端全关 */
    fr = open(g_dev0, O_RDONLY | O_NONBLOCK);
    if (fr < 0) {
        BAD("open O_RDONLY 失败: %s", strerror(errno));
        return;
    }

    re = poll_revents(fr, POLLIN, 0);
    CHECK((re & POLLHUP) != 0,
          "写端全关且无数据：POLLHUP（revents=0x%x）", re);

    close(fr);
    (void)c;
}

/* ------------------------------------------------------------------ */
/* 8. fasync / SIGIO                                                   */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_sigio;

static void on_sigio(int sig)
{
    (void)sig;
    g_sigio = 1;
}

static void test_fasync(void)
{
    struct sigaction sa;
    sigset_t mask, oldmask;
    struct timespec ts;
    int fd, sig;
    int flags;

    section("8. fasync：写入触发 SIGIO");

    fd = open(g_dev0, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        BAD("open 失败: %s", strerror(errno));
        return;
    }
    drain(fd);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigio;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGIO, &sa, NULL) < 0) {
        BAD("sigaction 失败: %s", strerror(errno));
        close(fd);
        return;
    }

    if (fcntl(fd, F_SETOWN, getpid()) < 0) {
        BAD("F_SETOWN 失败: %s", strerror(errno));
        close(fd);
        return;
    }

    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_ASYNC) < 0) {
        BAD("设置 O_ASYNC 失败: %s", strerror(errno));
        close(fd);
        return;
    }

    /* 先阻塞 SIGIO，写入后再等，避免信号在 write 返回前被处理 */
    sigemptyset(&mask);
    sigaddset(&mask, SIGIO);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);
    g_sigio = 0;

    CHECK(write(fd, "s", 1) == 1, "写入 1 字节（已注册 O_ASYNC）");

    ts.tv_sec = 2;
    ts.tv_nsec = 0;
    sig = sigtimedwait(&mask, NULL, &ts);

    CHECK(sig == SIGIO, "在 2 秒内收到 SIGIO（实际 sig=%d, errno=%d）",
          sig, errno);

    sigprocmask(SIG_SETMASK, &oldmask, NULL);

    /* 注销 O_ASYNC */
    fcntl(fd, F_SETFL, flags & ~O_ASYNC);
    drain(fd);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* 9. 环形回绕                                                         */
/* ------------------------------------------------------------------ */

static void test_wrap(void)
{
    int fd;
    unsigned char *wbuf, *rbuf;
    int len = g_bufsize - 2;    /* 留出哨兵，不写满 */
    int round;

    section("9. 环形缓冲区回绕");

    fd = open(g_dev0, O_RDWR);
    if (fd < 0) {
        BAD("open 失败: %s", strerror(errno));
        return;
    }

    wbuf = malloc(len);
    rbuf = malloc(len);
    if (!wbuf || !rbuf) {
        BAD("malloc 失败");
        free(wbuf);
        free(rbuf);
        close(fd);
        return;
    }

    for (round = 0; round < 8; round++) {
        int i;
        ssize_t n;

        memset(wbuf, (round * 37) & 0xff, len);
        n = write_all(fd, wbuf, len);
        if (n != len) {
            BAD("第 %d 轮写入失败: %zd/%d", round + 1, n, len);
            break;
        }

        memset(rbuf, 0, len);
        n = read_all(fd, rbuf, len);
        if (n != len) {
            BAD("第 %d 轮读取失败: %zd/%d", round + 1, n, len);
            break;
        }

        if (memcmp(wbuf, rbuf, len) != 0) {
            BAD("第 %d 轮数据不一致（回绕后顺序错误）", round + 1);
            break;
        }

        for (i = 0; i < len; i++) {
            if (rbuf[i] != (unsigned char)((round * 37) & 0xff)) {
                BAD("第 %d 轮第 %d 字节错误: 0x%02x", round + 1, i, rbuf[i]);
                n = -1;
                break;
            }
        }
        if (n < 0)
            break;
    }

    if (round == 8)
        OK("连续 %d 轮「写满-读空」数据全部正确（跨 end 回绕）", round);

    free(wbuf);
    free(rbuf);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    printf("用法: %s [-d 目录] [-M major] [-m minor] [-b buffer]\n", prog);
    printf("  -d 设备节点目录（默认 /dev）\n");
    printf("  -M 主设备号（默认从 /proc/devices 查找 scull）\n");
    printf("  -m scullpipe0 次设备号（默认 scull_minor + scull_devs）\n");
    printf("  -b 缓冲区大小（默认从 /sys/module/scull 读取）\n");
}

int main(int argc, char **argv)
{
    const char *dir = "/dev";
    int major = -1;
    int minor = -1;
    int opt;

    while ((opt = getopt(argc, argv, "d:M:m:b:h")) != -1) {
        switch (opt) {
        case 'd':
            dir = optarg;
            break;
        case 'M':
            major = atoi(optarg);
            break;
        case 'm':
            minor = atoi(optarg);
            break;
        case 'b':
            g_bufsize = atoi(optarg);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }

    if (major <= 0)
        major = read_int_file("/sys/module/scull/parameters/scull_major",
                              find_scull_major());
    if (minor < 0) {
        int base = read_int_file("/sys/module/scull/parameters/scull_minor", 0);
        int ndev = read_int_file("/sys/module/scull/parameters/scull_devs", 4);

        minor = base + ndev;
    }
    if (g_bufsize <= 1)
        g_bufsize = read_int_file("/sys/module/scull/parameters/scull_p_buffer", 4000);

    if (major <= 0 || minor < 0) {
        fprintf(stderr, "找不到 scull 设备号，请先 insmod scull.ko，"
                        "或用 -M/-m 指定\n");
        return 2;
    }

    snprintf(g_dev0, sizeof(g_dev0), "%s/scullpipe0", dir);
    snprintf(g_dev1, sizeof(g_dev1), "%s/scullpipe1", dir);

    printf("==================== scullpipe 测试 ====================\n");
    printf("主设备号 : %d\n", major);
    printf("起始次号 : %d（scullpipe0=%d, scullpipe1=%d）\n",
           minor, minor, minor + 1);
    printf("缓冲区   : %d 字节（可用 %d）\n", g_bufsize, g_bufsize - 1);
    printf("当前身份 : uid=%u euid=%u%s\n",
           (unsigned)getuid(), (unsigned)geteuid(),
           geteuid() == 0 ? " (root)" : "");

    section("0. 设备节点");
    if (ensure_node(g_dev0, major, minor) ||
        ensure_node(g_dev1, major, minor + 1)) {
        fprintf(stderr, "设备节点准备失败，退出\n");
        return 2;
    }
    OK("设备节点就绪: %s, %s", g_dev0, g_dev1);

    test_basic();
    test_nonblock_full();
    test_blocking_read();
    test_blocking_write();
    test_eof();
    test_epipe();
    test_poll();
    test_fasync();
    test_wrap();

    printf("\n==================== 汇总 ====================\n");
    printf("PASS %d / FAIL %d / SKIP %d / TODO %d\n",
           g_pass, g_fail, g_skip, g_todo);

    return g_fail ? 1 : 0;
}
