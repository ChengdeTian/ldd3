/*
 * scull_test.c -- scull 驱动（chapter-04）用户态测试程序
 *
 * 用法：
 *   ./scull_test [/dev/scullN] [quantum] [qset]
 *
 *   例：./scull_test                          # 用 /dev/scull0，quantum=4000 qset=500
 *       ./scull_test /dev/scull1              # 测另一个设备
 *       ./scull_test /dev/scull0 4000 500     # 显式指定参数（需与 insmod 时一致）
 *
 * 交叉编译：
 *   aarch64-linux-gnu-gcc -static -O2 -Wall -o scull_test scull_test.c
 *
 * 覆盖范围：
 *   [字符设备]
 *     1. 设备类型检查、以 O_WRONLY 打开触发 scull_trim() 清空
 *     2. 基础读写
 *     3. 短读语义：驱动一次只搬一个 quantum（本测试的核心）
 *     4. 循环读回 + 逐字节比对（cp/cat 语义）
 *     5. lseek：SEEK_SET / SEEK_CUR / SEEK_END
 *     6. 非法 whence / 负偏移
 *     7. 跨 quantum 边界定位写读
 *     8. 跨 qset 边界定位写读（触发链表新建节点，验证 item = f_pos / itemsize）
 *    10. O_WRONLY 截断语义
 *   [proc 调试接口]
 *     9. /proc/scullmem（single_open）与 /proc/scullseq（seq_file）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#define DEV_DEFAULT   "/dev/scull0"
#define PROCMEM_PATH  "/proc/scullmem"
#define PROCSEQ_PATH  "/proc/scullseq"

/* 与 scull.h 里的默认值保持一致（可用命令行参数覆盖） */
#define QUANTUM_DEFAULT 4000
#define QSET_DEFAULT    500

static int g_pass, g_fail, g_skip;

#define OK(...)   do { g_pass++;  printf("  [ OK ] "); printf(__VA_ARGS__); putchar('\n'); } while (0)
#define BAD(...)  do { g_fail++;  printf("  [FAIL] "); printf(__VA_ARGS__); putchar('\n'); } while (0)
#define SKIP(...) do { g_skip++;  printf("  [SKIP] "); printf(__VA_ARGS__); putchar('\n'); } while (0)
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
            break;              /* 正常不该发生 */
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

/* 打印文本的前 max_lines 行，便于人工观察内容 */
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
    long itemsize;
    long len, bsz;
    struct scull_bufs {
        char *big;              /* 写入用的源数据 */
        char *rbuf;             /* 读回用的缓冲 */
        char *pbuf;             /* proc 文件缓冲 */
    } b = { NULL, NULL, NULL };
    int fd;
    ssize_t w, r;
    off_t off;
    long i;

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

    printf("==================== scull 驱动测试 ====================\n");
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
              "lseek(0, SEEK_END) = %ld（此时 size 应为 %zu）",
              (long)off, strlen(msg));
    }

    /* ---------------- 3. 短读语义 ---------------- */
    section("3. 短读语义：单次 read/write 只处理一个 quantum");
    len  = 2 * quantum + 100;
    bsz  = quantum + 512;
    b.big  = malloc(len);
    b.rbuf = malloc(len + 64);
    if (!b.big || !b.rbuf) {
        fprintf(stderr, "malloc 失败\n");
        return 1;
    }
    for (i = 0; i < len; i++)
        b.big[i] = (char)('A' + (i % 26));

    CHECK(dev_reset(dev) == 0, "清空设备");
    lseek(fd, 0, SEEK_SET);
    w = write_all(fd, b.big, len);
    CHECK(w == len, "循环写入 %ld 字节（= 2*quantum + 100），实际写入 %zd",
          len, w);

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

    /* ---------------- 6. 非法参数 ---------------- */
    section("6. 非法 lseek 参数");
    errno = 0;
    off = lseek(fd, 0, 999);
    CHECK(off == -1 && errno == EINVAL,
          "lseek(whence=999) → -1, errno=EINVAL（VFS 层就拦截了）");

    errno = 0;
    off = lseek(fd, -1, SEEK_SET);
    CHECK(off == -1 && errno == EINVAL,
          "lseek(-1, SEEK_SET) → -1, errno=EINVAL（驱动 scull_llseek 返回）");

    /* ---------------- 7. 跨 quantum 边界 ---------------- */
    section("7. 跨 quantum 边界定位写读");
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
    }

    /* ---------------- 8. 跨 qset 边界 ---------------- */
    section("8. 跨 qset 边界定位写读（第 2 个链表节点）");
    {
        char rb[16];

        lseek(fd, itemsize - 4, SEEK_SET);
        w = write_all(fd, "E0QS", 4);
        CHECK(w == 4, "在 offset %ld (itemsize-4，第 0 个 qset 的末尾) 写入 \"E0QS\"",
              itemsize - 4);

        lseek(fd, itemsize, SEEK_SET);
        w = write_all(fd, "B1QS", 4);
        CHECK(w == 4, "在 offset %ld (itemsize，进入第 1 个 qset → 新建链表节点) 写入 \"B1QS\"",
              itemsize);

        memset(rb, 0, sizeof(rb));
        lseek(fd, itemsize - 4, SEEK_SET);
        r = read_all(fd, rb, 4);
        CHECK(r == 4 && memcmp(rb, "E0QS", 4) == 0,
              "offset %ld 读回 \"%.4s\"", itemsize - 4, rb);

        memset(rb, 0, sizeof(rb));
        lseek(fd, itemsize, SEEK_SET);
        r = read_all(fd, rb, 4);
        CHECK(r == 4 && memcmp(rb, "B1QS", 4) == 0,
              "offset %ld 读回 \"%.4s\"", itemsize, rb);
    }

    /* ---------------- 9. proc 调试接口 ---------------- */
    section("9. proc 调试接口（需编译时定义 SCULL_DEBUG）");
    b.pbuf = malloc(1 << 20);
    if (!b.pbuf) {
        SKIP("malloc 1MB 失败，跳过 proc 测试");
    } else {
        r = read_file(PROCMEM_PATH, b.pbuf, 1 << 20);
        if (r < 0) {
            SKIP("%s 打不开: %s（可能编译时没开 SCULL_DEBUG，或未 insmod）",
                 PROCMEM_PATH, strerror(errno));
        } else {
            CHECK(r > 0 && strstr(b.pbuf, "Device 0") != NULL,
                  "%s 读到 %zd 字节，包含 \"Device 0\"", PROCMEM_PATH, r);
            print_head(b.pbuf, 8);
        }

        r = read_file(PROCSEQ_PATH, b.pbuf, 1 << 20);
        if (r < 0) {
            SKIP("%s 打不开: %s", PROCSEQ_PATH, strerror(errno));
        } else {
            CHECK(r > 0 && strstr(b.pbuf, "Device 0") != NULL,
                  "%s 读到 %zd 字节，包含 \"Device 0\"", PROCSEQ_PATH, r);
            print_head(b.pbuf, 8);
        }
    }

    /* ---------------- 10. O_WRONLY 截断语义 ---------------- */
    section("10. O_WRONLY 截断语义");
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

    /* ---------------- 汇总 ---------------- */
    free(b.big);
    free(b.rbuf);
    free(b.pbuf);
    close(fd);

    printf("\n==================== 测试结果 ====================\n");
    printf("PASS = %d, FAIL = %d, SKIP = %d\n", g_pass, g_fail, g_skip);
    if (g_fail)
        printf("存在失败项，请检查驱动实现\n");
    else
        printf("全部通过\n");

    return g_fail ? 1 : 0;
}
