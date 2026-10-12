/*
 * scull_test.c -- scull 驱动（chapter-06：ioctl）用户态测试程序
 *
 * 用法：
 *   ./scull_test [/dev/scullN] [quantum] [qset]
 *
 *   例：./scull_test                          # /dev/scull0，quantum=4000 qset=500
 *       ./scull_test /dev/scull1              # 测另一个设备
 *       ./scull_test /dev/scull0 4000 500     # 显式指定（需与 insmod 时一致）
 *
 * 交叉编译：
 *   aarch64-linux-gnu-gcc -static -O2 -Wall -o scull_test scull_test.c
 *   （或 make test）
 *
 * 测试流程见 doc/chapter-03.md：host 编译 → 9p 共享 → guest insmod + mknod → 运行
 *
 * 覆盖范围：
 *   [字符设备] 1~8   open/close、读写、短读语义、lseek、跨 quantum/qset 边界、proc、trim
 *   [ioctl]    9~16  ★本章重点★
 *                     - Q/G  只读：返回值风格 / 指针风格
 *                     - T/S  写值：值传递风格 / 指针传递风格
 *                     - X/H  原子换值：eXchange / sHift
 *                     - Reset 复位、非法命令 → -ENOTTY
 *                     - QSET 系列实现状态、参数校验契约
 *                     - "全局默认值 vs 设备实际值" 语义验证
 *
 * 计数器含义：
 *   PASS 符合预期 / FAIL 不符合预期 / SKIP 环境不满足 / TODO 驱动尚未实现（不判失败）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

/* ------------------------------------------------------------------ *
 * ioctl 命令号：必须与 driver/chapter-06/scull.h 保持一致
 * （真实项目应抽成 uapi 头文件由内核/用户态共享，避免两边漂移）
 * ------------------------------------------------------------------ */
#define SCULL_IOC_MAGIC   'k'

#define SCULL_IOCRESET    _IO(SCULL_IOC_MAGIC, 0)
#define SCULL_IOCSQUANTUM _IOW(SCULL_IOC_MAGIC,  1, int)
#define SCULL_IOCSQSET    _IOW(SCULL_IOC_MAGIC,  2, int)
#define SCULL_IOCTQUANTUM _IO(SCULL_IOC_MAGIC,   3)
#define SCULL_IOCTQSET    _IO(SCULL_IOC_MAGIC,   4)
#define SCULL_IOCGQUANTUM _IOR(SCULL_IOC_MAGIC,  5, int)
#define SCULL_IOCGQSET    _IOR(SCULL_IOC_MAGIC,  6, int)
#define SCULL_IOCQQUANTUM _IO(SCULL_IOC_MAGIC,   7)
#define SCULL_IOCQQSET    _IO(SCULL_IOC_MAGIC,   8)
#define SCULL_IOCXQUANTUM _IOWR(SCULL_IOC_MAGIC, 9, int)
#define SCULL_IOCXQSET    _IOWR(SCULL_IOC_MAGIC,10, int)
#define SCULL_IOCHQUANTUM _IO(SCULL_IOC_MAGIC,  11)
#define SCULL_IOCHQSET    _IO(SCULL_IOC_MAGIC,  12)

#define SCULL_IOC_MAXNR   14

#define DEV_DEFAULT   "/dev/scull0"
#define PROCMEM_PATH  "/proc/scullmem"
#define PROCSEQ_PATH  "/proc/scullseq"

/* 与 scull.h 的默认值保持一致（命令行可覆盖） */
#define QUANTUM_DEFAULT 4000
#define QSET_DEFAULT    500

/* 反推设备 quantum 时一次写入/读取的总量（必须 >= 被测 quantum 的最大值） */
#define PROBE_TOTAL     (64 * 1024)

static int g_pass, g_fail, g_skip, g_todo;

#define OK(...)   do { g_pass++; printf("  [ OK ] "); printf(__VA_ARGS__); putchar('\n'); } while (0)
#define BAD(...)  do { g_fail++; printf("  [FAIL] "); printf(__VA_ARGS__); putchar('\n'); } while (0)
#define SKIP(...) do { g_skip++; printf("  [SKIP] "); printf(__VA_ARGS__); putchar('\n'); } while (0)
#define TODO(...) do { g_todo++; printf("  [TODO] "); printf(__VA_ARGS__); putchar('\n'); } while (0)
#define CHECK(cond, ...) do { if (cond) OK(__VA_ARGS__); else BAD(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

/* 清空设备：以 O_WRONLY 打开会触发驱动里的 scull_trim() */
static int dev_reset(const char *dev)
{
    int fd = open(dev, O_WRONLY);

    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

/* 驱动一次最多搬一个 quantum，所以用户态必须循环写 */
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

/* 循环读到 n 字节或 EOF */
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
            break;              /* EOF */
        done += r;
    }
    return (ssize_t)done;
}

/* 读整个小文件（proc 调试接口用）；失败返回 -1 */
static ssize_t read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read_all(fd, buf, cap - 1);
    close(fd);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

/* 打印文本的前 max_lines 行 */
static void print_head(const char *text, int max_lines)
{
    const char *p = text;
    int lines = 0;

    while (*p && lines < max_lines) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);

        printf("         | %.*s\n", len, p);
        if (!nl)
            break;
        p = nl + 1;
        lines++;
    }
}

/*
 * 用"单次 read 的返回长度"反推设备当前的 quantum（**不做 trim**，不复位状态）
 *
 * 原理：从偏移 0 读且 count >= quantum 时，驱动会返回 min(count, size, quantum)。
 * 前提：写入量 >= 设备的 quantum。失败返回 -1。
 */
static long probe_quantum(int fd, char *buf, size_t total)
{
    memset(buf, 'P', total);
    if (lseek(fd, 0, SEEK_SET) < 0)
        return -1;
    if (write_all(fd, buf, total) != (ssize_t)total)
        return -1;
    if (lseek(fd, 0, SEEK_SET) < 0)
        return -1;
    return read(fd, buf, total);
}

static void section(const char *name)
{
    printf("\n--- %s ---\n", name);
}

/* ------------------------------------------------------------------ */
/* 测试主流程                                                          */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *dev = DEV_DEFAULT;
    long quantum = QUANTUM_DEFAULT;
    long qset    = QSET_DEFAULT;
    long itemsize, len, bsz;
    struct bufs {
        char *big;              /* 读写测试数据 */
        char *rbuf;             /* 读回缓冲 */
        char *pbuf;             /* proc 文件缓冲 */
        char *probe;            /* quantum 探测缓冲 */
    } b = { NULL, NULL, NULL, NULL };
    int fd;
    ssize_t w, r;
    off_t off;
    long i;
    long ir;                    /* ioctl 返回值 */

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("用法: %s [/dev/scullN] [quantum] [qset]\n", argv[0]);
        return 0;
    }
    if (argc > 1)
        dev = argv[1];
    if (argc > 2)
        quantum = strtol(argv[2], NULL, 0);
    if (argc > 3)
        qset = strtol(argv[3], NULL, 0);
    if (quantum <= 0 || qset <= 0) {
        fprintf(stderr, "quantum/qset 必须为正整数\n");
        return 2;
    }
    itemsize = quantum * qset;

    printf("==================== scull 驱动测试（chapter-06 ioctl）====================\n");
    printf("设备     : %s\n", dev);
    printf("quantum  : %ld 字节\n", quantum);
    printf("qset     : %ld\n", qset);
    printf("itemsize : %ld 字节 (quantum * qset)\n", itemsize);

    /* ---------------- 1. 打开设备并清空 ---------------- */
    section("1. 打开设备并清空");
    {
        struct stat st;

        if (stat(dev, &st) < 0) {
            BAD("stat(%s) 失败: %s", dev, strerror(errno));
            printf("\n提示：先 insmod scull.ko 并按主设备号 mknod（见 doc/chapter-03.md）\n");
            return 1;
        }
        CHECK(S_ISCHR(st.st_mode), "%s 是字符设备 (major=%u minor=%u)",
              dev, major(st.st_rdev), minor(st.st_rdev));

        CHECK(dev_reset(dev) == 0,
              "以 O_WRONLY 打开再关闭 → 触发 scull_trim()，设备已清空");

        fd = open(dev, O_RDWR);
        CHECK(fd >= 0, "open(%s, O_RDWR) = %d", dev, fd);
        if (fd < 0) {
            perror("open");
            return 1;
        }
    }

    /* ---------------- 2. 基础读写 ---------------- */
    section("2. 基础读写");
    {
        const char *msg = "hello scull";
        char rb[64];

        lseek(fd, 0, SEEK_SET);
        w = write_all(fd, msg, strlen(msg));
        CHECK(w == (ssize_t)strlen(msg), "写入 %zu 字节 \"%s\" (返回 %zd)",
              strlen(msg), msg, w);

        off = lseek(fd, 0, SEEK_SET);
        CHECK(off == 0, "lseek(0, SEEK_SET) = %ld", (long)off);

        memset(rb, 0, sizeof(rb));
        r = read_all(fd, rb, strlen(msg));
        CHECK(r == (ssize_t)strlen(msg) && memcmp(rb, msg, strlen(msg)) == 0,
              "读回 %zd 字节: \"%s\"", r, rb);

        off = lseek(fd, 0, SEEK_END);
        CHECK(off == (off_t)strlen(msg),
              "lseek(0, SEEK_END) = %ld（size 应为 %zu）", (long)off, strlen(msg));
    }

    /* ---------------- 3. 短读语义 ---------------- */
    section("3. 短读语义：单次 read/write 只处理一个 quantum");
    len  = 2 * quantum + 100;
    bsz  = quantum + 512;
    b.big   = malloc(len);
    b.rbuf  = malloc(len + 64);
    b.probe = malloc(PROBE_TOTAL);
    if (!b.big || !b.rbuf || !b.probe) {
        fprintf(stderr, "malloc 失败\n");
        return 1;
    }
    for (i = 0; i < len; i++)
        b.big[i] = (char)('A' + (i % 26));

    CHECK(dev_reset(dev) == 0, "清空设备");
    lseek(fd, 0, SEEK_SET);
    w = write_all(fd, b.big, len);
    CHECK(w == len, "循环写入 %ld 字节（= 2*quantum + 100），实际写入 %zd", len, w);

    lseek(fd, 0, SEEK_SET);
    r = read(fd, b.rbuf, bsz);
    CHECK(r == quantum, "第 1 次 read(%ld) 返回 %zd 字节（应 == quantum = %ld）",
          bsz, r, quantum);

    r = read(fd, b.rbuf, bsz);
    CHECK(r == quantum, "第 2 次 read 返回 %zd 字节（应 == quantum）", r);

    r = read(fd, b.rbuf, bsz);
    CHECK(r == 100, "第 3 次 read 返回 %zd 字节（只剩 100 字节）", r);

    /* ---------------- 4. 循环读回比对 ---------------- */
    section("4. 循环读回并逐字节比对");
    lseek(fd, 0, SEEK_SET);
    r = read_all(fd, b.rbuf, len);
    CHECK(r == len && memcmp(b.rbuf, b.big, len) == 0,
          "读回 %zd 字节，与写入内容完全一致", r);

    /* ---------------- 5. lseek 三种 whence ---------------- */
    section("5. lseek: SEEK_SET / SEEK_CUR / SEEK_END");
    off = lseek(fd, 0, SEEK_END);
    CHECK(off == len, "lseek(0, SEEK_END) = %ld（size = %ld）", (long)off, len);

    off = lseek(fd, -5, SEEK_END);
    CHECK(off == len - 5, "lseek(-5, SEEK_END) = %ld", (long)off);

    memset(b.rbuf, 0, 16);
    r = read_all(fd, b.rbuf, 5);
    CHECK(r == 5 && memcmp(b.rbuf, b.big + len - 5, 5) == 0,
          "从 size-5 处读 5 字节，内容一致");

    lseek(fd, 0, SEEK_SET);
    off = lseek(fd, 10, SEEK_CUR);
    CHECK(off == 10, "lseek(10, SEEK_CUR) = %ld", (long)off);

    off = lseek(fd, 0, SEEK_CUR);
    CHECK(off == 10, "lseek(0, SEEK_CUR) 不移动位置 = %ld", (long)off);

    /* ---------------- 6. 非法 lseek 参数 ---------------- */
    section("6. 非法 lseek 参数");
    errno = 0;
    off = lseek(fd, 0, 999);
    CHECK(off == -1 && errno == EINVAL,
          "lseek(whence=999) → -1/EINVAL（VFS 层拦截）");

    errno = 0;
    off = lseek(fd, -1, SEEK_SET);
    CHECK(off == -1 && errno == EINVAL,
          "lseek(-1, SEEK_SET) → -1/EINVAL（驱动 scull_llseek 返回）");

    /* ---------------- 7. 跨 quantum / qset 边界 ---------------- */
    section("7. 跨 quantum / qset 边界定位写读");
    {
        char rb[16];

        lseek(fd, quantum - 4, SEEK_SET);
        w = write_all(fd, "END0", 4);
        CHECK(w == 4, "在 offset %ld (quantum-4) 写入 \"END0\"", quantum - 4);

        lseek(fd, quantum, SEEK_SET);
        w = write_all(fd, "BEG1", 4);
        CHECK(w == 4, "在 offset %ld (quantum)   写入 \"BEG1\"", quantum);

        memset(rb, 0, sizeof(rb));
        lseek(fd, quantum - 4, SEEK_SET);
        r = read_all(fd, rb, 4);
        CHECK(r == 4 && memcmp(rb, "END0", 4) == 0,
              "offset %ld 读回 \"%.4s\"", quantum - 4, rb);

        memset(rb, 0, sizeof(rb));
        lseek(fd, quantum, SEEK_SET);
        r = read_all(fd, rb, 4);
        CHECK(r == 4 && memcmp(rb, "BEG1", 4) == 0,
              "offset %ld 读回 \"%.4s\"", quantum, rb);

        lseek(fd, itemsize - 4, SEEK_SET);
        w = write_all(fd, "E0QS", 4);
        CHECK(w == 4, "在 offset %ld (itemsize-4，第 0 个 qset 末尾) 写入 \"E0QS\"",
              itemsize - 4);

        lseek(fd, itemsize, SEEK_SET);
        w = write_all(fd, "B1QS", 4);
        CHECK(w == 4, "在 offset %ld (itemsize → 新建第 2 个链表节点) 写入 \"B1QS\"",
              itemsize);

        memset(rb, 0, sizeof(rb));
        lseek(fd, itemsize, SEEK_SET);
        r = read_all(fd, rb, 4);
        CHECK(r == 4 && memcmp(rb, "B1QS", 4) == 0,
              "offset %ld 读回 \"%.4s\"", itemsize, rb);
    }

    /* ---------------- 8. proc 调试接口 ---------------- */
    section("8. proc 调试接口（需编译时定义 SCULL_DEBUG）");
    b.pbuf = malloc(1 << 20);
    if (!b.pbuf) {
        SKIP("malloc 1MB 失败，跳过 proc 测试");
    } else {
        r = read_file(PROCMEM_PATH, b.pbuf, 1 << 20);
        if (r < 0) {
            SKIP("%s 打不开（可能编译时没开 SCULL_DEBUG）", PROCMEM_PATH);
        } else {
            CHECK(r > 0 && strstr(b.pbuf, "Device 0") != NULL,
                  "%s 读到 %zd 字节，包含 \"Device 0\"", PROCMEM_PATH, r);
            print_head(b.pbuf, 6);
        }

        r = read_file(PROCSEQ_PATH, b.pbuf, 1 << 20);
        if (r < 0) {
            SKIP("%s 打不开", PROCSEQ_PATH);
        } else {
            CHECK(r > 0 && strstr(b.pbuf, "Device 0") != NULL,
                  "%s 读到 %zd 字节，包含 \"Device 0\"", PROCSEQ_PATH, r);
            print_head(b.pbuf, 6);
        }
    }

    /* ================================================================ *
     *   以下为本章重点：ioctl 测试
     * ================================================================ */

    /* ---------------- 9. ioctl：只读风格 Q / G ---------------- */
    section("9. ioctl：Query（返回值）/ Get（指针）");
    {
        int v = -1;

        ir = ioctl(fd, SCULL_IOCQQUANTUM);          /* Q：结果就是返回值 */
        CHECK(ir == quantum, "SCULL_IOCQQUANTUM → %ld（应为 %ld）", ir, quantum);

        errno = 0;
        ir = ioctl(fd, SCULL_IOCGQUANTUM, &v);      /* G：结果通过指针写回 */
        CHECK(ir == 0 && v == quantum,
              "SCULL_IOCGQUANTUM(&v) → %ld, v = %d（应为 %ld）", ir, v, quantum);
    }

    /* ---------------- 10. ioctl：写值风格 T / S ---------------- */
    section("10. ioctl：Tell（值传递）/ Set（指针传递）");
    {
        int nv = 2048;

        errno = 0;
        ir = ioctl(fd, SCULL_IOCTQUANTUM, (unsigned long)1024);   /* T：arg 就是值 */
        CHECK(ir == 0, "SCULL_IOCTQUANTUM(1024) → %ld", ir);
        CHECK(ioctl(fd, SCULL_IOCQQUANTUM) == 1024, "Query 确认 quantum == 1024");

        errno = 0;
        ir = ioctl(fd, SCULL_IOCSQUANTUM, &nv);                   /* S：arg 是指针 */
        CHECK(ir == 0, "SCULL_IOCSQUANTUM(&2048) → %ld", ir);
        CHECK(ioctl(fd, SCULL_IOCQQUANTUM) == 2048, "Query 确认 quantum == 2048");
    }

    /* ---------------- 11. ioctl：原子换值风格 X / H ---------------- */
    section("11. ioctl：eXchange（指针）/ sHift（值）");
    {
        int x = 4096;               /* 想写入的值；调用后会被换成旧值 */
        long old;

        CHECK(ioctl(fd, SCULL_IOCQQUANTUM) == 2048, "前置：当前 quantum == 2048");

        errno = 0;
        ir = ioctl(fd, SCULL_IOCXQUANTUM, &x);                    /* X：写入 4096 */
        CHECK(ir == 0, "SCULL_IOCXQUANTUM(&4096) → %ld", ir);
        CHECK(x == 2048, "X 调用后 x 被换成旧值 = %d（应为 2048）", x);
        CHECK(ioctl(fd, SCULL_IOCQQUANTUM) == 4096, "Query 确认已变为 4096");

        errno = 0;
        old = ioctl(fd, SCULL_IOCHQUANTUM, (unsigned long)3000);  /* H：设 3000 */
        CHECK(old == 4096, "SCULL_IOCHQUANTUM(3000) 返回旧值 = %ld（应为 4096）", old);
        CHECK(ioctl(fd, SCULL_IOCQQUANTUM) == 3000, "Query 确认已变为 3000");
    }

    /* ---------------- 12. ioctl：Reset 复位 ---------------- */
    section("12. ioctl：Reset 复位");
    {
        long q;

        errno = 0;
        ir = ioctl(fd, SCULL_IOCRESET);
        CHECK(ir == 0, "SCULL_IOCRESET → %ld", ir);

        q = ioctl(fd, SCULL_IOCQQUANTUM);
        CHECK(q == quantum, "复位后 quantum = %ld（应为 %ld）", q, quantum);
    }

    /* ---------------- 13. ioctl：非法命令 ---------------- */
    section("13. ioctl：非法命令 → -ENOTTY");
    errno = 0;
    ir = ioctl(fd, _IO('x', 0));                       /* 魔术字不对 */
    CHECK(ir == -1 && errno == ENOTTY,
          "魔术字 'x' → -1/ENOTTY（errno=%d）", errno);

    errno = 0;
    ir = ioctl(fd, _IO(SCULL_IOC_MAGIC, SCULL_IOC_MAXNR + 1));    /* 序号越界 */
    CHECK(ir == -1 && errno == ENOTTY,
          "序号 %d 越界 → -1/ENOTTY（errno=%d）", SCULL_IOC_MAXNR + 1, errno);

    /* ---------------- 14. ioctl：QSET 系列实现状态 ---------------- */
    section("14. ioctl：QSET 系列实现状态");
    errno = 0;
    ir = ioctl(fd, SCULL_IOCQQSET);
    if (ir == -1 && errno == ENOTTY)
        TODO("SCULL_IOCQQSET 未实现（驱动 switch 里只有 QUANTUM 系列）");
    else
        CHECK(ir == qset, "SCULL_IOCQQSET → %ld（应为 %ld）", ir, qset);

    errno = 0;
    ir = ioctl(fd, SCULL_IOCTQSET, (unsigned long)128);
    if (ir == -1 && errno == ENOTTY) {
        TODO("SCULL_IOCTQSET 未实现（其余 S/G/X/H 的 QSET 命令同样未实现）");
    } else {
        CHECK(ir == 0, "SCULL_IOCTQSET(128) → %ld", ir);
        CHECK(ioctl(fd, SCULL_IOCQQSET) == 128, "Query 确认 qset == 128");
    }
    ioctl(fd, SCULL_IOCRESET);                 /* 无论是否实现都复位一下 */

    /* ---------------- 15. ioctl：参数校验（契约测试） ---------------- */
    section("15. ioctl：参数校验契约（非法值应被拒）");
    /* quantum = 0 会让 itemsize = quantum*qset 变成 0，后续 read/write 会除零 */
    errno = 0;
    ir = ioctl(fd, SCULL_IOCTQUANTUM, (unsigned long)0);
    if (ir == 0) {
        TODO("SCULL_IOCTQUANTUM(0) 被接受 → 驱动缺参数校验（itemsize 会变成 0，触发除零）");
        ioctl(fd, SCULL_IOCRESET);              /* 立刻复位，避免污染后续测试 */
    } else {
        CHECK(ir == -1 && errno == EINVAL,
              "SCULL_IOCTQUANTUM(0) → -1/EINVAL（errno=%d）", errno);
    }

    /* 64 位的超大值会被截断成 int 的 0 */
    errno = 0;
    ir = ioctl(fd, SCULL_IOCTQUANTUM, 0x100000000UL);
    if (ir == 0) {
        long now = ioctl(fd, SCULL_IOCQQUANTUM);

        TODO("SCULL_IOCTQUANTUM(0x100000000) 被接受，quantum 被截断为 %ld", now);
        ioctl(fd, SCULL_IOCRESET);
    } else {
        CHECK(ir == -1 && errno == EINVAL,
              "SCULL_IOCTQUANTUM(超大值) → -1/EINVAL（errno=%d）", errno);
    }

    /* ---------------- 16. ioctl：全局默认值 vs 设备实际值 ---------------- */
    section("16. ioctl：改的是全局默认值，不影响已打开设备");
    {
        long q_dev;

        CHECK(dev_reset(dev) == 0, "先 trim 一次，让设备 quantum 同步为当前全局值");

        q_dev = probe_quantum(fd, b.probe, PROBE_TOTAL);
        CHECK(q_dev == quantum, "trim 后设备 quantum = %ld（= 全局默认 %ld）",
              q_dev, quantum);

        errno = 0;
        ir = ioctl(fd, SCULL_IOCTQUANTUM, (unsigned long)2000);
        CHECK(ir == 0, "只改全局：SCULL_IOCTQUANTUM(2000) → %ld", ir);

        q_dev = probe_quantum(fd, b.probe, PROBE_TOTAL);
        CHECK(q_dev == quantum,
              "已打开设备的 quantum 仍是 %ld（ioctl 未影响它）", q_dev);

        CHECK(ioctl(fd, SCULL_IOCQQUANTUM) == 2000, "但 Query 已能看到新值 2000");

        CHECK(dev_reset(dev) == 0, "再 trim 一次 → dev->quantum = scull_quantum");
        q_dev = probe_quantum(fd, b.probe, PROBE_TOTAL);
        CHECK(q_dev == 2000, "此时设备 quantum 才同步为 %ld", q_dev);
    }

    /* ---------------- 17. O_WRONLY 截断语义 ---------------- */
    section("17. O_WRONLY 截断语义");
    {
        char rb[64];

        CHECK(dev_reset(dev) == 0, "以 O_WRONLY 打开再关闭 → scull_trim()");

        lseek(fd, 0, SEEK_SET);
        memset(rb, 0, sizeof(rb));
        r = read(fd, rb, sizeof(rb));
        CHECK(r == 0, "截断后立即读 → %zd 字节（EOF，size 已归零）", r);

        off = lseek(fd, 0, SEEK_END);
        CHECK(off == 0, "lseek(0, SEEK_END) = %ld（size = 0）", (long)off);
    }

    /* ---------------- 18. 收尾：复位全局参数 ---------------- */
    section("18. 收尾：复位 ioctl 全局参数");
    {
        long q;

        ir = ioctl(fd, SCULL_IOCRESET);
        CHECK(ir == 0, "SCULL_IOCRESET → %ld", ir);

        q = ioctl(fd, SCULL_IOCQQUANTUM);
        CHECK(q == quantum, "quantum 已恢复默认值 %ld", q);

        if (q != quantum)
            printf("         （警告：驱动全局参数未复位，可能影响后续测试）\n");
    }

    /* ---------------- 汇总 ---------------- */
    free(b.big);
    free(b.rbuf);
    free(b.pbuf);
    free(b.probe);
    close(fd);

    printf("\n==================== 测试结果 ====================\n");
    printf("PASS = %d, FAIL = %d, SKIP = %d, TODO = %d\n",
           g_pass, g_fail, g_skip, g_todo);
    if (g_fail)
        printf("存在失败项，请检查驱动实现\n");
    else
        printf("全部通过（TODO 表示驱动尚未实现的功能，不计入失败）\n");

    return g_fail ? 1 : 0;
}
