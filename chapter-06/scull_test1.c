/*
 * scull_test1.c -- scull chapter-06「访问控制」设备测试程序
 *
 * 被测设备（access.c）：
 *   /dev/scullsingle  同一时刻只允许一个进程打开
 *   /dev/sculluid     同一用户可重复打开，其他用户 -EBUSY，root 可越权
 *   /dev/scullwuid    同上，但被占用时阻塞等待（非阻塞 -EAGAIN）
 *   /dev/sullpriv     按控制终端（tty）区分的独立实例
 *
 * 用法：
 *   ./scull_test1 [-d /dev] [-M major] [-m minor] [-u uid]
 *
 *   -d /dev   设备节点所在目录（默认 /dev）
 *   -M major  手工指定主设备号（默认从 /proc/devices 找 "scull"）
 *   -m minor  手工指定 scullsingle 的起始次设备号
 *             （默认 scull_minor + scull_devs，从 /sys/module/scull 读取）
 *   -u uid    用于"其他用户"测试的 uid（默认 65534/nobody）
 *
 * 编译：
 *   aarch64-linux-gnu-gcc -static -O2 -Wall -o scull_test1 scull_test1.c
 *
 * 说明：
 *   - 设备节点不存在时会自动 mknod，因此建议以 root 运行；
 *   - 非 root 运行时，涉及切换用户的用例会标记 SKIP；
 *   - 全部用例通过返回 0，有 FAIL 返回 1。
 *
 * 覆盖范围：
 *   1. 环境与设备节点
 *   2. scullsingle：独占、-EBUSY、释放后可再次打开
 *   3. sculluid：同用户可重入、异用户 -EBUSY、root 越权
 *   4. scullwuid：-EAGAIN、阻塞等待唤醒、信号打断 -EINTR
 *   5. sullpriv：有/无控制终端、同 tty 共享实例、异 tty 独立实例
 *   6. 反复 open/close 后计数不泄漏
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
#include <termios.h>
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

/* 设备路径 */
static char g_single[256];
static char g_uid[256];
static char g_wuid[256];
static char g_priv[256];

/* 用于"其他用户"测试 */
static uid_t g_other_uid = 65534;

static void section(const char *name)
{
    printf("\n--- %s ---\n", name);
}

/* ------------------------------------------------------------------ */
/* 环境准备                                                            */
/* ------------------------------------------------------------------ */

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

static int ensure_node(const char *path, int major, int minor)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        if (!S_ISCHR(st.st_mode)) {
            fprintf(stderr, "%s 已存在但不是字符设备\n", path);
            return -1;
        }
        if ((int)major(st.st_rdev) != major) {
            fprintf(stderr, "警告：%s 的主设备号 %u 与驱动 %d 不一致\n",
                    path, major(st.st_rdev), major);
        }
        return 0;
    }

    if (mknod(path, S_IFCHR | 0666, makedev(major, minor)) < 0) {
        fprintf(stderr, "mknod %s 失败: %s（请用 root 运行）\n",
                path, strerror(errno));
        return -1;
    }
    chmod(path, 0666);
    return 0;
}

/* 当前进程是否有控制终端 */
static int has_ctty(void)
{
    int fd = open("/dev/tty", O_RDWR);

    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

/* ------------------------------------------------------------------ */
/* 在子进程 / 指定 uid 下执行一段逻辑                                   */
/* ------------------------------------------------------------------ */

struct child_msg {
    int ret;    /* 0 成功，-1 失败 */
    int err;    /* 失败时的 errno */
};

/*
 * fn 返回 0 表示成功，-1 表示失败；errno 会被带回父进程。
 * uid == (uid_t)-1 表示不切换用户。
 */
static int run_as_user(uid_t uid, int (*fn)(void *), void *arg)
{
    int p[2];
    pid_t pid;
    struct child_msg m = { -1, 0 };
    ssize_t n;

    if (pipe(p) < 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }

    if (pid == 0) {
        close(p[0]);

        if (uid != (uid_t)-1 && (setgid(uid) < 0 || setuid(uid) < 0)) {
            m.ret = -1;
            m.err = errno;
        } else {
            errno = 0;
            m.ret = fn(arg);
            m.err = m.ret ? errno : 0;
        }

        if (write(p[1], &m, sizeof(m)) != sizeof(m))
            _exit(2);
        _exit(0);
    }

    close(p[1]);
    n = read(p[0], &m, sizeof(m));
    close(p[0]);
    waitpid(pid, NULL, 0);

    if (n != sizeof(m)) {
        errno = ECHILD;
        return -1;
    }
    errno = m.err;
    return m.ret;
}

/* 以指定身份打开一次后立即关闭 */
static int child_open_once(void *arg)
{
    const char *path = arg;
    int fd = open(path, O_RDWR);

    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

/* 以非阻塞方式打开一次（预期失败并带回 EAGAIN） */
static int child_open_nonblock(void *arg)
{
    const char *path = arg;
    int fd = open(path, O_RDWR | O_NONBLOCK);

    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

/* setsid 脱离控制终端后再打开 */
static int child_detached_open(void *arg)
{
    const char *path = arg;
    int fd;

    if (setsid() < 0)
        return -1;

    fd = open(path, O_RDWR);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

/*
 * 在新建的 pty 上建立"另一个控制终端"，然后打开 sullpriv。
 * 期望得到一个全新的空实例（read 返回 0）。
 */
static int child_other_tty_open(void *arg)
{
    const char *path = arg;
    char slave[128];
    char buf[64];
    int mfd, sfd, fd;
    ssize_t n;

    mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if (mfd < 0)
        return -1;
    if (grantpt(mfd) < 0 || unlockpt(mfd) < 0) {
        close(mfd);
        return -1;
    }

    if (setsid() < 0) {
        close(mfd);
        return -1;
    }

    /*
     * 关闭 pty master 时内核会向该终端的会话前台进程组发 SIGHUP，
     * 而本进程正是新会话的首进程；忽略它，否则会在把结果写回父进程
     * 之前就被杀掉（父进程只能看到 ECHILD）。
     */
    signal(SIGHUP, SIG_IGN);

    {
        const char *slave_name = ptsname(mfd);

        if (!slave_name) {
            close(mfd);
            return -1;
        }
        snprintf(slave, sizeof(slave), "%s", slave_name);
    }

    sfd = open(slave, O_RDWR);
    if (sfd < 0) {
        close(mfd);
        return -1;
    }
    if (ioctl(sfd, TIOCSCTTY, 0) < 0) {
        close(sfd);
        close(mfd);
        return -1;
    }

    fd = open(path, O_RDWR);
    if (fd < 0) {
        close(sfd);
        close(mfd);
        return -1;
    }

    lseek(fd, 0, SEEK_SET);
    n = read(fd, buf, sizeof(buf));

    close(fd);
    close(sfd);
    close(mfd);

    /* 新终端的实例应当是空的 */
    return (n == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* 1. scullsingle                                                      */
/* ------------------------------------------------------------------ */

static void test_single(void)
{
    int fd1, fd2, fd3;

    section("1. scullsingle：独占打开");

    errno = 0;
    fd1 = open(g_single, O_RDWR);
    if (fd1 < 0) {
        BAD("首次 open(%s) 失败: %s", g_single, strerror(errno));
        return;
    }
    OK("首次 open 成功 (fd=%d)", fd1);

    errno = 0;
    fd2 = open(g_single, O_RDWR);
    CHECK(fd2 < 0 && errno == EBUSY,
          "第二次 open 应失败 -EBUSY（实际 fd=%d, errno=%d %s）",
          fd2, errno, strerror(errno));
    if (fd2 >= 0)
        close(fd2);

    /* 非阻塞语义对 single 无影响，仍是 -EBUSY */
    errno = 0;
    fd2 = open(g_single, O_RDWR | O_NONBLOCK);
    CHECK(fd2 < 0 && errno == EBUSY,
          "O_NONBLOCK 打开仍为 -EBUSY（实际 errno=%d）", errno);
    if (fd2 >= 0)
        close(fd2);

    close(fd1);

    errno = 0;
    fd3 = open(g_single, O_RDWR);
    CHECK(fd3 >= 0, "关闭后可再次打开（实际 fd=%d, errno=%d）", fd3, errno);
    if (fd3 >= 0)
        close(fd3);
}

/* ------------------------------------------------------------------ */
/* 2. sculluid                                                         */
/* ------------------------------------------------------------------ */

static void test_uid(void)
{
    int fd1 = -1, fd2 = -1, fd3 = -1;
    int ret;

    section("2. sculluid：单用户可重复打开");

    errno = 0;
    fd1 = open(g_uid, O_RDWR);
    if (fd1 < 0) {
        BAD("open(%s) 失败: %s", g_uid, strerror(errno));
        return;
    }
    OK("用户 uid=%u 首次打开成功 (fd=%d)", (unsigned)geteuid(), fd1);

    errno = 0;
    fd2 = open(g_uid, O_RDWR);
    CHECK(fd2 >= 0, "同一用户可重复打开（fd=%d, errno=%d）", fd2, errno);

    /* 其他用户 */
    if (geteuid() == 0) {
        int s1[2], s2[2];
        pid_t pid;
        char c;

        errno = 0;
        ret = run_as_user(g_other_uid, child_open_once, g_uid);
        CHECK(ret < 0 && errno == EBUSY,
              "其他用户 uid=%u 打开应 -EBUSY（实际 ret=%d, errno=%d %s）",
              (unsigned)g_other_uid, ret, errno, strerror(errno));

        /* 释放 root 的引用，改由 other 用户持有 */
        if (fd2 >= 0) { close(fd2); fd2 = -1; }
        if (fd1 >= 0) { close(fd1); fd1 = -1; }

        if (pipe(s1) < 0 || pipe(s2) < 0) {
            BAD("pipe 创建失败: %s", strerror(errno));
            return;
        }

        pid = fork();
        if (pid == 0) {
            close(s1[0]);
            close(s2[1]);

            if (setgid(g_other_uid) < 0 || setuid(g_other_uid) < 0) {
                if (write(s1[1], "E", 1) != 1)
                    _exit(2);
                _exit(1);
            }

            fd1 = open(g_uid, O_RDWR);      /* count==0 → owner = other */
            if (fd1 < 0) {
                if (write(s1[1], "E", 1) != 1)
                    _exit(2);
                _exit(1);
            }
            if (write(s1[1], "O", 1) != 1)
                _exit(2);

            if (read(s2[0], &c, 1) != 1)    /* 等父进程测完 */
                _exit(2);
            close(fd1);
            _exit(0);
        }

        close(s1[1]);
        close(s2[1]);

        if (read(s1[0], &c, 1) == 1 && c == 'O') {
            OK("uid=%u 持有设备（root 的引用已全部关闭）",
               (unsigned)g_other_uid);

            /* 设备 owner 是 other，root 靠 CAP_DAC_OVERRIDE 越权 */
            errno = 0;
            fd3 = open(g_uid, O_RDWR);
            CHECK(fd3 >= 0,
                  "root 以 CAP_DAC_OVERRIDE 越权打开成功（fd=%d, errno=%d）",
                  fd3, errno);
            if (fd3 >= 0)
                close(fd3);
        } else {
            SKIP("uid=%u 无法打开设备，跳过越权测试", (unsigned)g_other_uid);
        }

        {
            ssize_t wret = write(s2[1], "X", 1);
            (void)wret;     /* 子进程提前退出时写失败，忽略即可 */
        }
        close(s1[0]);
        close(s2[0]);
        close(s2[1]);
        waitpid(pid, NULL, 0);

        errno = 0;
        ret = run_as_user(g_other_uid, child_open_once, g_uid);
        CHECK(ret == 0,
              "原持有者全部关闭后，uid=%u 可以打开（实际 ret=%d, errno=%d）",
              (unsigned)g_other_uid, ret, errno);
    } else {
        SKIP("非 root，无法切换到 uid=%u 验证异用户拒绝", (unsigned)g_other_uid);
        if (fd2 >= 0)
            close(fd2);
        if (fd1 >= 0)
            close(fd1);
    }
}

/* ------------------------------------------------------------------ */
/* 3. scullwuid                                                        */
/* ------------------------------------------------------------------ */

static void test_wuid_nonblock(void)
{
    int fd;
    int ret;

    section("3. scullwuid：非阻塞语义");

    if (geteuid() != 0) {
        SKIP("需要 root 才能切换到 uid=%u 验证被占用语义",
             (unsigned)g_other_uid);
        return;
    }

    errno = 0;
    fd = open(g_wuid, O_RDWR);
    if (fd < 0) {
        BAD("open(%s) 失败: %s", g_wuid, strerror(errno));
        return;
    }
    OK("root 持有设备 (fd=%d)", fd);

    /* 同用户（root）再次打开总是可用，不能用来测 EAGAIN；
     * 必须换一个 uid 去打开，才会走到 wait_event 分支。 */
    errno = 0;
    ret = run_as_user(g_other_uid, child_open_nonblock, g_wuid);
    CHECK(ret < 0 && errno == EAGAIN,
          "其他用户 uid=%u + O_NONBLOCK → -EAGAIN（ret=%d, errno=%d %s）",
          (unsigned)g_other_uid, ret, errno, strerror(errno));

    close(fd);
}

static void test_wuid_blocking(void)
{
    int fd, pfd[2], sync[2];
    pid_t pid;
    struct child_msg m;
    struct pollfd p;
    char c;

    section("4. scullwuid：阻塞等待与唤醒");

    if (geteuid() != 0) {
        SKIP("需要 root 才能用另一个 uid 进入阻塞 open");
        return;
    }

    errno = 0;
    fd = open(g_wuid, O_RDWR);
    if (fd < 0) {
        BAD("open(%s) 失败: %s", g_wuid, strerror(errno));
        return;
    }

    if (pipe(pfd) < 0 || pipe(sync) < 0) {
        BAD("pipe 创建失败: %s", strerror(errno));
        close(fd);
        return;
    }

    pid = fork();
    if (pid < 0) {
        BAD("fork 失败: %s", strerror(errno));
        close(fd);
        close(pfd[0]); close(pfd[1]);
        close(sync[0]); close(sync[1]);
        return;
    }

    if (pid == 0) {
        int cfd;

        close(pfd[0]);
        /*
         * 必须关掉从父进程继承来的设备 fd：struct file 的引用计数还在子进程手里时，
         * 父进程 close() 不会触发驱动的 release，scullwuid 永远等不到唤醒。
         */
        close(fd);

        /* 换一个 uid，才会被判定为"其他用户"而进入阻塞 */
        if (setgid(g_other_uid) < 0 || setuid(g_other_uid) < 0) {
            m.ret = -1;
            m.err = errno;
            if (write(pfd[1], &m, sizeof(m)) != sizeof(m))
                _exit(2);
            _exit(0);
        }

        /* 通知父进程：即将进入阻塞 open（sync 的写端是 sync[1]） */
        if (write(sync[1], "S", 1) != 1)
            _exit(2);
        close(sync[1]);

        errno = 0;
        cfd = open(g_wuid, O_RDWR);     /* 应当阻塞 */
        m.ret = (cfd >= 0) ? 0 : -1;
        m.err = errno;
        if (cfd >= 0)
            close(cfd);

        if (write(pfd[1], &m, sizeof(m)) != sizeof(m))
            _exit(2);
        _exit(0);
    }

    /* 父进程 */
    close(pfd[1]);
    close(sync[1]);

    if (read(sync[0], &c, 1) != 1) {
        BAD("与子进程同步失败");
        close(sync[0]);
        close(pfd[0]);
        close(fd);
        waitpid(pid, NULL, 0);
        return;
    }
    close(sync[0]);

    usleep(300000);     /* 给子进程时间进入睡眠 */

    p.fd = pfd[0];
    p.events = POLLIN;
    p.revents = 0;
    CHECK(poll(&p, 1, 0) == 0,
          "持有设备期间，子进程的 open 一直阻塞（未被唤醒）");

    close(fd);          /* 释放设备，触发 wake_up_interruptible_sync() */

    p.revents = 0;
    CHECK(poll(&p, 1, 3000) == 1,
          "释放设备后，子进程在 3 秒内被唤醒");

    memset(&m, 0, sizeof(m));
    fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL) | O_NONBLOCK);  /* 失败时不要读死 */
    if (read(pfd[0], &m, sizeof(m)) == sizeof(m)) {
        CHECK(m.ret == 0 && m.err == 0,
              "被唤醒后 open 成功（ret=%d, errno=%d）", m.ret, m.err);
    } else {
        BAD("读取子进程结果失败");
        kill(pid, SIGKILL);     /* 子进程可能还阻塞在 open 里 */
    }

    close(pfd[0]);
    waitpid(pid, NULL, 0);
}

static void on_signal(int sig)
{
    (void)sig;
}

static void test_wuid_signal(void)
{
    int fd, pfd[2], sync[2];
    pid_t pid;
    struct child_msg m;
    struct pollfd p;
    char c;
    int got;

    section("5. scullwuid：阻塞期间被信号打断");

    if (geteuid() != 0) {
        SKIP("需要 root 才能用另一个 uid 进入阻塞 open");
        return;
    }

    errno = 0;
    fd = open(g_wuid, O_RDWR);
    if (fd < 0) {
        BAD("open(%s) 失败: %s", g_wuid, strerror(errno));
        return;
    }

    if (pipe(pfd) < 0 || pipe(sync) < 0) {
        BAD("pipe 创建失败: %s", strerror(errno));
        close(fd);
        return;
    }

    pid = fork();
    if (pid < 0) {
        BAD("fork 失败: %s", strerror(errno));
        close(fd);
        close(pfd[0]); close(pfd[1]);
        close(sync[0]); close(sync[1]);
        return;
    }

    if (pid == 0) {
        struct sigaction sa;
        int cfd;

        close(pfd[0]);
        close(fd);      /* 不要继承父进程持有的设备 fd */

        /* 换一个 uid，才会进入阻塞分支 */
        if (setgid(g_other_uid) < 0 || setuid(g_other_uid) < 0) {
            m.ret = -1;
            m.err = errno;
            if (write(pfd[1], &m, sizeof(m)) != sizeof(m))
                _exit(2);
            _exit(0);
        }

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;      /* 真正的处理函数，但不设 SA_RESTART */
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGUSR1, &sa, NULL);

        if (write(sync[1], "S", 1) != 1)
            _exit(2);
        close(sync[1]);

        errno = 0;
        cfd = open(g_wuid, O_RDWR);

        m.ret = (cfd >= 0) ? 0 : -1;
        m.err = errno;
        if (cfd >= 0)
            close(cfd);

        if (write(pfd[1], &m, sizeof(m)) != sizeof(m))
            _exit(2);
        _exit(0);
    }

    close(pfd[1]);
    close(sync[1]);

    if (read(sync[0], &c, 1) != 1) {
        BAD("与子进程同步失败");
        close(sync[0]); close(pfd[0]); close(fd);
        waitpid(pid, NULL, 0);
        return;
    }
    close(sync[0]);

    usleep(300000);

    p.fd = pfd[0];
    p.events = POLLIN;
    p.revents = 0;
    got = poll(&p, 1, 0);
    CHECK(got == 0, "信号到来前子进程仍阻塞");

    kill(pid, SIGUSR1);

    p.revents = 0;
    got = poll(&p, 1, 3000);
    CHECK(got == 1, "收到信号后 open 立即返回");

    memset(&m, 0, sizeof(m));
    fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL) | O_NONBLOCK);  /* 失败时不要读死 */
    if (read(pfd[0], &m, sizeof(m)) == sizeof(m)) {
        CHECK(m.ret < 0 && m.err == EINTR,
              "被信号打断 → -1/EINTR（实际 ret=%d, errno=%d %s）",
              m.ret, m.err, strerror(m.err));
    } else {
        BAD("读取子进程结果失败");
        kill(pid, SIGKILL);     /* 子进程可能还阻塞在 open 里 */
    }

    close(pfd[0]);
    close(fd);
    waitpid(pid, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* 6. sullpriv                                                         */
/* ------------------------------------------------------------------ */

static void test_priv(void)
{
    int fd1, fd2;
    char buf[64];
    ssize_t n;
    int ret;

    section("6. sullpriv：按控制终端区分实例");

    if (has_ctty()) {
        printf("  [INFO] 当前进程有控制终端\n");

        errno = 0;
        fd1 = open(g_priv, O_RDWR);
        if (fd1 < 0) {
            BAD("有控制终端时 open 失败: %s", strerror(errno));
            return;
        }
        OK("有控制终端时可打开 (fd=%d)", fd1);

        if (write(fd1, "priv-data", 9) != 9) {
            BAD("写入失败: %s", strerror(errno));
            close(fd1);
            return;
        }

        errno = 0;
        fd2 = open(g_priv, O_RDWR);
        CHECK(fd2 >= 0, "同一 tty 可再次打开（fd=%d）", fd2);

        if (fd2 >= 0) {
            lseek(fd2, 0, SEEK_SET);
            memset(buf, 0, sizeof(buf));
            n = read(fd2, buf, sizeof(buf) - 1);
            CHECK(n == 9 && memcmp(buf, "priv-data", 9) == 0,
                  "同一 tty 的两个 fd 共享同一实例（读到 %zd 字节）", n);
            close(fd2);
        }

        close(fd1);

        /* 另一个 pty 上的进程应看到全新的空实例 */
        if (geteuid() == 0 || has_ctty()) {
            errno = 0;
            ret = run_as_user((uid_t)-1, child_other_tty_open, g_priv);
            if (ret == 0) {
                OK("另一个控制终端得到独立实例（数据不共享）");
            } else if (errno == EPERM || errno == ENODEV || errno == ENOENT) {
                SKIP("无法创建第二块 pty 作为控制终端: %s", strerror(errno));
            } else {
                BAD("另一终端的实例读到了本终端的数据（ret=%d, errno=%d %s）",
                    ret, errno, strerror(errno));
            }
        } else {
            SKIP("无法创建第二个控制终端");
        }
    } else {
        printf("  [INFO] 当前进程没有控制终端\n");

        errno = 0;
        fd1 = open(g_priv, O_RDWR);
        CHECK(fd1 < 0 && errno == EINVAL,
              "无控制终端 → -EINVAL（实际 fd=%d, errno=%d %s）",
              fd1, errno, strerror(errno));
        if (fd1 >= 0)
            close(fd1);
    }

    /* 无论父进程有没有 tty，setsid 之后都没有控制终端 */
    errno = 0;
    ret = run_as_user((uid_t)-1, child_detached_open, g_priv);
    CHECK(ret < 0 && errno == EINVAL,
          "setsid 脱离控制终端后 open → -EINVAL（ret=%d, errno=%d %s）",
          ret, errno, strerror(errno));
}

/* ------------------------------------------------------------------ */
/* 7. 反复打开关闭                                                     */
/* ------------------------------------------------------------------ */

static void test_churn(void)
{
    int i, fd, bad = 0;

    section("7. 反复 open/close 后计数不泄漏");

    for (i = 0; i < 200; i++) {
        fd = open(g_single, O_RDWR);
        if (fd < 0) {
            bad++;
            continue;
        }
        close(fd);
    }
    CHECK(bad == 0, "scullsingle 连续 200 次打开/关闭全部成功（失败 %d 次）", bad);

    bad = 0;
    for (i = 0; i < 200; i++) {
        fd = open(g_uid, O_RDWR);
        if (fd < 0) {
            bad++;
            continue;
        }
        close(fd);
    }
    CHECK(bad == 0, "sculluid 连续 200 次打开/关闭全部成功（失败 %d 次）", bad);

    /* 最终仍可正常打开，确认计数归零 */
    fd = open(g_single, O_RDWR);
    CHECK(fd >= 0, "收尾：scullsingle 仍可打开（fd=%d）", fd);
    if (fd >= 0)
        close(fd);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    printf("用法: %s [-d 目录] [-M major] [-m minor] [-u uid]\n", prog);
    printf("  -d 设备节点目录（默认 /dev）\n");
    printf("  -M 主设备号（默认从 /proc/devices 查找 scull）\n");
    printf("  -m scullsingle 起始次设备号（默认 scull_minor + scull_devs + scull_p_nr_devs）\n");
    printf("  -u 用于异用户测试的 uid（默认 65534）\n");
}

int main(int argc, char **argv)
{
    const char *dir = "/dev";
    int major = -1;
    int minor = -1;
    int opt;

    while ((opt = getopt(argc, argv, "d:M:m:u:h")) != -1) {
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
        case 'u':
            g_other_uid = (uid_t)strtoul(optarg, NULL, 10);
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
        int npipe = read_int_file("/sys/module/scull/parameters/scull_p_nr_devs", 2);

        /* 设备号排布：scull0..N-1 → scullpipe → 访问控制设备 */
        minor = base + ndev + npipe;
    }

    if (major <= 0 || minor < 0) {
        fprintf(stderr, "找不到 scull 主设备号，请先 insmod scull.ko，"
                        "或用 -M/-m 指定\n");
        return 2;
    }

    snprintf(g_single, sizeof(g_single), "%s/scullsingle", dir);
    snprintf(g_uid,    sizeof(g_uid),    "%s/sculluid",    dir);
    snprintf(g_wuid,   sizeof(g_wuid),   "%s/scullwuid",   dir);
    snprintf(g_priv,   sizeof(g_priv),   "%s/sullpriv",    dir);

    printf("==================== scull 访问控制测试 ====================\n");
    printf("主设备号 : %d\n", major);
    printf("起始次号 : %d (scullsingle=%d, sculluid=%d, scullwuid=%d, sullpriv=%d)\n",
           minor, minor, minor + 1, minor + 2, minor + 3);
    printf("当前身份 : uid=%u euid=%u%s\n",
           (unsigned)getuid(), (unsigned)geteuid(),
           geteuid() == 0 ? " (root)" : "");

    section("0. 设备节点");
    if (ensure_node(g_single, major, minor) ||
        ensure_node(g_uid,    major, minor + 1) ||
        ensure_node(g_wuid,   major, minor + 2) ||
        ensure_node(g_priv,   major, minor + 3)) {
        fprintf(stderr, "设备节点准备失败，退出\n");
        return 2;
    }
    OK("四个设备节点就绪");
    printf("         %s\n         %s\n         %s\n         %s\n",
           g_single, g_uid, g_wuid, g_priv);

    test_single();
    test_uid();
    test_wuid_nonblock();
    test_wuid_blocking();
    test_wuid_signal();
    test_priv();
    test_churn();

    printf("\n==================== 汇总 ====================\n");
    printf("PASS %d / FAIL %d / SKIP %d / TODO %d\n",
           g_pass, g_fail, g_skip, g_todo);

    return g_fail ? 1 : 0;
}
