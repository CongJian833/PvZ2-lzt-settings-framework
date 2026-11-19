/*
 * xref2.c — 宽松版字符串交叉引用扫描
 *
 * 与 xref.c 的区别：不要求 adrp 与 add 紧邻，允许两者之间隔最多 N 条指令
 * （编译器有时会把索引计算插在中间）。用于定位 xref.c 漏掉的引用点。
 *
 * 用法：xref2 <macho> <字符串文件偏移>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MH_MAGIC_64   0xfeedfacf
#define LC_SEGMENT_64 0x19

static uint8_t *g_file = NULL;
static size_t   g_size = 0;
static uint64_t g_textVmaddr = 0, g_textSize = 0;
static uint32_t g_textFileoff = 0;

typedef struct { uint32_t cmd, cmdsize; char segname[16]; uint64_t vmaddr, vmsize, fileoff, filesize;
                 int32_t maxprot, initprot; uint32_t nsects, flags; } seg64_t;
typedef struct { char sectname[16], segname[16]; uint64_t addr, size; uint32_t offset, align,
                 reloff, nreloc, flags, reserved1, reserved2, reserved3; } sect64_t;

static int load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    g_file = (uint8_t *)malloc((size_t)n);
    if (!g_file) { fclose(f); return 0; }
    if (fread(g_file, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f); g_size = (size_t)n; return 1;
}

static int parse(void) {
    uint32_t magic; memcpy(&magic, g_file, 4);
    if (magic != MH_MAGIC_64) return 0;
    uint32_t ncmds; memcpy(&ncmds, g_file + 16, 4);
    const uint8_t *lc = g_file + 32;
    for (uint32_t i = 0; i < ncmds; ++i) {
        uint32_t cmd, cmdsize;
        memcpy(&cmd, lc, 4); memcpy(&cmdsize, lc + 4, 4);
        if (!cmdsize) break;
        if (cmd == LC_SEGMENT_64) {
            seg64_t *s = (seg64_t *)lc;
            if (!strncmp(s->segname, "__TEXT", 16)) {
                const uint8_t *sec = lc + sizeof(seg64_t);
                for (uint32_t j = 0; j < s->nsects; ++j) {
                    const sect64_t *sc = (const sect64_t *)(sec + j * sizeof(sect64_t));
                    if (!strncmp(sc->sectname, "__text", 16)) {
                        g_textVmaddr = sc->addr; g_textSize = sc->size; g_textFileoff = sc->offset;
                    }
                }
            }
        }
        lc += cmdsize;
    }
    return g_textSize != 0;
}

static uint64_t decode_adrp(uint32_t insn, uint64_t pc) {
    if ((insn & 0x9F000000u) != 0x90000000u) return 0;
    uint64_t immlo = (insn >> 29) & 0x3u;
    uint64_t immhi = (insn >> 5) & 0x7FFFFu;
    uint64_t imm = (immhi << 2) | immlo;
    if (imm & (1u << 20)) imm |= ~((uint64_t)0x1FFFFF);
    return (pc & ~(uint64_t)0xFFF) + (imm << 12);
}
static uint32_t rd_of(uint32_t i) { return i & 0x1Fu; }
static int decode_add(uint32_t i, uint32_t *rn, uint32_t *imm12) {
    if ((i & 0xFFC00000u) != 0x91000000u) return 0;
    *rn = (i >> 5) & 0x1Fu; *imm12 = (i >> 10) & 0xFFFu; return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "用法: %s <macho> <字符串文件偏移>\n", argv[0]); return 1; }
    if (!load(argv[1]) || !parse()) { fprintf(stderr, "加载失败\n"); return 1; }
    const long strOff = strtol(argv[2], NULL, 0);
    const uint64_t target = (uint64_t)strOff + 0x100000000ULL;
    printf("目标字符串 vmaddr = 0x%llx（文件偏移 %ld）\n",
           (unsigned long long)target, strOff);

    const uint8_t *t = g_file + g_textFileoff;
    int hits = 0;
    /* 窗口 8 条指令：adrp 之后 8 条内出现配对的 add 即算命中 */
    for (uint64_t off = 0; off + 4 * 64 <= g_textSize; off += 4) {
        uint32_t i0; memcpy(&i0, t + off, 4);
        const uint64_t pc = g_textVmaddr + off;
        const uint64_t page = decode_adrp(i0, pc);
        if (page == 0) continue;
        const uint32_t r = rd_of(i0);
        for (int k = 1; k < 64; ++k) {
            uint32_t ik; memcpy(&ik, t + off + 4 * k, 4);
            uint32_t rn, imm12;
            if (!decode_add(ik, &rn, &imm12)) continue;
            if (rn != r) continue;
            if (page + imm12 == target) {
                printf("  xref @ 0x%llx (adrp+%d 条后 add)\n",
                       (unsigned long long)pc, k);
                ++hits;
            }
        }
    }
    if (hits == 0) printf("  未找到引用\n");
    return 0;
}
