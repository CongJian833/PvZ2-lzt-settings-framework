/*
 * ptrfind.c — 在 Mach-O 中查找「指向指定地址的 8 字节指针」
 *
 * 用途：某些字符串不是通过 adrp+add 直接引用，而是先被放进数据段的
 *       指针表（如格式化字符串表），再由代码从表中加载。
 *       此时 adrp/add 扫描找不到引用，必须反过来搜「哪个位置存着这个地址」。
 *
 * 用法：ptrfind <macho> <目标 vmaddr(如 0x101d18636)>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "用法: %s <macho> <vmaddr>\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return 3; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 4; }
    fclose(f);

    const uint64_t target = strtoull(argv[2], NULL, 0);
    printf("查找指向 0x%llx 的 8 字节指针（文件共 %ld 字节）\n",
           (unsigned long long)target, n);

    int hits = 0;
    for (long off = 0; off + 8 <= n; off += 4) {      /* 4 字节步进，兼顾非 8 对齐表 */
        uint64_t v;
        memcpy(&v, buf + off, 8);
        if (v != target) continue;
        /* 数据段指针表项通常 8 字节对齐，这里一并报告对齐情况 */
        printf("  ptr @ file+0x%lx  (vmaddr 0x%llx, %s对齐)\n", off,
               (unsigned long long)(off + 0x100000000ULL),
               (off % 8 == 0) ? "8字节" : "非");
        if (++hits >= 20) { printf("  ...(已达 20 条上限)\n"); break; }
    }
    if (hits == 0) printf("  未找到\n");
    free(buf);
    return 0;
}
