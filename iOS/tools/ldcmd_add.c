/*
 * ldcmd_add.c — 给 Mach-O 加一条 LC_LOAD_DYLIB（用于 iOS 包内注入）
 *
 * 为什么自己写：本机 llvm-install-name-tool-16 只支持 rpath 类选项，
 * 没有 -add_dylib；设备上也没有 insert_dylib / optool。
 *
 * 原理（安全前提）：Mach-O 头部 load commands 区之后到第一个 section
 * 文件偏移之间有填充空隙。PvZ2 的 arm64 切片空隙约 19KB，加一条
 * LC_LOAD_DYLIB（几十字节）绰绰有余，因此：
 *   - 不挪动任何 section，不改任何已有 offset
 *   - 只在空隙内追加一条 load command，并更新 ncmds / sizeofcmds
 * 符号表、__LINKEDIT、代码签名位置全部保持原样（签名随后由 ldid 重建）。
 *
 * 支持：fat（含 arm64 切片，按切片内偏移就地修改）与 thin arm64。
 *
 * 用法：ldcmd_add <macho> <dylib_load_path>
 *   dylib_load_path 形如 @executable_path/Frameworks/libxxx.dylib
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

/* fat 头以【大端】存储；小端机器按 u32 直读得到翻转值：
   0xcafebabe -> 0xbebafeca，0xcafebabf(fat64) -> 0xbfbafeca */
#define FAT_MAGIC_LE  0xbebafeca
#define FAT_MAGIC_LE2 0xbfbafeca
#define MH_MAGIC_64 0xfeedfacf
#define LC_LOAD_DYLIB 0x0c
#define LC_SEGMENT_64 0x19
#define CPU_TYPE_ARM64 0x0100000c

static uint32_t bswap32(uint32_t v) {
    return ((v & 0xff) << 24) | ((v & 0xff00) << 8) |
           ((v >> 8) & 0xff00) | ((v >> 24) & 0xff);
}

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* 在给定 mach-o 起始偏移处解析并追加 load command。返回 0 成功。 */
static int patch_macho(uint8_t *buf, size_t size, size_t off, const char *path) {
    if (off + 32 > size) { fprintf(stderr, "macho header out of range\n"); return 1; }

    uint32_t magic = rd32(buf + off);
    if (magic != MH_MAGIC_64) {
        fprintf(stderr, "not a 64-bit mach-o (magic=0x%08x)\n", magic);
        return 1;
    }

    uint32_t ncmds = rd32(buf + off + 16);
    uint32_t sizeofcmds = rd32(buf + off + 20);
    const size_t hdrSize = 32;
    size_t lcEnd = hdrSize + sizeofcmds;

    /* 1. 遍历 load commands：查重 + 求“空隙”右边界（最小 section 文件偏移） */
    size_t minSecOff = size;   /* 相对本 mach-o 起始 */
    size_t p = hdrSize;
    for (uint32_t i = 0; i < ncmds; ++i) {
        if (off + p + 8 > size) { fprintf(stderr, "load cmd out of range\n"); return 1; }
        uint32_t cmd = rd32(buf + off + p);
        uint32_t cmdsize = rd32(buf + off + p + 4);
        if (cmdsize == 0) { fprintf(stderr, "zero cmdsize\n"); return 1; }

        if (cmd == LC_LOAD_DYLIB) {
            /* dylib 名字在 cmd 起始 +24 处（name 为相对于 cmd 的偏移） */
            uint32_t nameOff = rd32(buf + off + p + 8);
            if (off + p + nameOff < size) {
                const char *nm = (const char *)(buf + off + p + nameOff);
                if (strcmp(nm, path) == 0) {
                    printf("LC_LOAD_DYLIB already present, nothing to do\n");
                    return 0;
                }
            }
        } else if (cmd == LC_SEGMENT_64) {
            uint32_t nsects = rd32(buf + off + p + 64);
            for (uint32_t s = 0; s < nsects; ++s) {
                size_t so = p + 72 + (size_t)s * 80;      /* section_64 = 80 字节 */
                if (off + so + 80 > size) break;
                uint32_t foff = rd32(buf + off + so + 48); /* section file offset */
                if (foff != 0 && foff < minSecOff) minSecOff = foff;
            }
        }
        p += cmdsize;
    }

    /* 2. 组装新 load command：dylib_command = 24 字节头 + 路径 */
    size_t pathLen = strlen(path) + 1;
    size_t cmdSize = 24 + pathLen;
    cmdSize = (cmdSize + 7) & ~(size_t)7;             /* 8 字节对齐 */

    if (lcEnd + cmdSize > minSecOff) {
        fprintf(stderr, "no header padding: need %zu, free %zu\n",
                cmdSize, minSecOff > lcEnd ? minSecOff - lcEnd : 0);
        return 1;
    }

    uint8_t *dst = buf + off + lcEnd;
    memset(dst, 0, cmdSize);
    wr32(dst + 0, LC_LOAD_DYLIB);
    wr32(dst + 4, (uint32_t)cmdSize);
    wr32(dst + 8, 24);        /* name offset（相对本 command） */
    wr32(dst + 12, 0);        /* timestamp */
    wr32(dst + 16, 0);        /* current_version */
    wr32(dst + 20, 0);        /* compatibility_version */
    memcpy(dst + 24, path, pathLen);

    /* 3. 更新头部的 ncmds / sizeofcmds */
    wr32(buf + off + 16, ncmds + 1);
    wr32(buf + off + 20, sizeofcmds + (uint32_t)cmdSize);

    printf("added LC_LOAD_DYLIB (%zu bytes), ncmds %u -> %u, free-after=%zu\n",
           cmdSize, ncmds, ncmds + 1,
           minSecOff > (lcEnd + cmdSize) ? minSecOff - (lcEnd + cmdSize) : 0);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <macho> <dylib_load_path>\n", argv[0]);
        return 2;
    }
    const char *file = argv[1];
    const char *path = argv[2];

    FILE *f = fopen(file, "r+b");
    if (!f) { fprintf(stderr, "fopen %s: %s\n", file, strerror(errno)); return 3; }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 4; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return 4; }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return 5; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 6; }

    uint32_t magic = rd32(buf);
    int rc;

    if (magic == FAT_MAGIC_LE || magic == FAT_MAGIC_LE2) {
        uint32_t nfat = bswap32(rd32(buf + 4));      /* fat 头是大端 */
        printf("fat binary, %u arch slices\n", nfat);
        size_t want = (size_t)-1;
        for (uint32_t i = 0; i < nfat; ++i) {
            size_t fo = 8 + (size_t)i * 20;
            uint32_t cputype = bswap32(rd32(buf + fo));
            uint32_t soff = bswap32(rd32(buf + fo + 8));
            printf("  slice[%u] cputype=0x%08x offset=0x%x\n", i, cputype, soff);
            if (cputype == CPU_TYPE_ARM64 && soff < want) want = soff;
        }
        if (want == (size_t)-1) { fprintf(stderr, "arm64 slice not found\n"); free(buf); fclose(f); return 7; }
        rc = patch_macho(buf, (size_t)sz, want, path);
    } else {
        rc = patch_macho(buf, (size_t)sz, 0, path);
    }

    if (rc == 0) {
        rewind(f);
        if (fwrite(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 8; }
        fflush(f);
        printf("written: %s\n", file);
    }
    free(buf);
    fclose(f);
    return rc;
}
