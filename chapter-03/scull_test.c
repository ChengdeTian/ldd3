/*
 * scull_test.c -- scull 驱动的用户态测试程序
 *
 * 用法：./scull_test /dev/scull0
 * 交叉编译：aarch64-linux-gnu-gcc -static -o scull_test scull_test.c
 *
 * 说明：
 *   1) 用 O_RDWR 打开，避免触发驱动在 O_WRONLY 时的 scull_trim() 清零
 *   2) 观察单次 read() 返回的字节数（scull 一次只搬一个 quantum）
 *   3) 用 lseek 定位到 10000 验证跨 quantum 读写
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int fd = open(argv[1], O_RDWR);          /* O_RDWR：不触发 trim */
    char buf[128];
    ssize_t n;

    if (argc < 2) {
        fprintf(stderr, "usage: %s /dev/scullX\n", argv[0]);
        return 1;
    }
    if (fd < 0) {
        perror("open");
        return 1;
    }

    memset(buf, 0, sizeof(buf));

    if (write(fd, "hello", 5) < 0)            /* 从 0 写 5 字节 */
        perror("write");
    lseek(fd, 0, SEEK_SET);
    n = read(fd, buf, sizeof(buf));           /* 观察单次实际读取字节数 */
    printf("read %zd bytes: %.*s\n", n, (int)n, buf);

    lseek(fd, 10000, SEEK_SET);               /* 测跨 quantum 定位写 */
    if (write(fd, "at-10000", 8) < 0)
        perror("write at 10000");
    lseek(fd, 10000, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf));
    printf("at 10000: %.*s\n", (int)n, buf);

    close(fd);
    return 0;
}
