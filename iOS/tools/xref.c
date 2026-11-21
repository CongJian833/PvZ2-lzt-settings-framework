/*
 * xref.c — Mach-O (arm64) 字符串交叉引用扫描器
 *
 * 用途：在设备上直接定位代码引用。原理是标准的「字符串交叉引用」，
 *       但只做必要的那一步，因此极快（整个 __text 单遍扫描）。
 *
 * 背景：arm64 引用字符串的标准序列是
 *         adrp xN, page@PAGE
 *         add  xN, xN, imm@PAGEOFF
 *       本工具遍历 __text，解码 ADRP+ADD 指令对，算出其解析地址，与目标
 *       地址比对，命中即输出该处指令地址。
 *
 * 用法：
 *   xref <macho> find <子串>     # 在文件里找字符串，打印其 vmaddr
 *   xref <macho> xref <vmaddr>   # 找引用该地址的 ADRP+ADD 位置
 *   xref <macho> xrefs <子串>    # 合体：找字符串 → 直接列交叉引用
 *
 * 输出：每个交叉引用一行的 "code_vmaddr" 或 "字符串行/代码行"。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MH_MAGIC_64   0xfeedfacf
#define LC_SEGMENT_64 0x19

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    int32_t  maxprot;
    int32_t  initprot;
    uint32_t nsects;
    uint32_t flags;
} seg64_t;

typedef struct {
    char     sectname[16];
    char     segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} sect64_t;

static uint8_t *g_file = NULL;
static size_t   g_size = 0;

/* ---- __TEXT 段与 __text 节信息 ---- */
static uint64_t g_textVmaddr = 0, g_textSize = 0;
static uint32_t g_textFileoff = 0;

static int load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "无法打开 %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    g_file = (uint8_t *)malloc((size_t)n);
    if (!g_file) { fclose(f); return 0; }
    if (fread(g_file, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f);
    g_size = (size_t)n;
    return 1;
}

static int parse_macho(void) {
    uint32_t magic;
    memcpy(&magic, g_file, 4);
    if (magic != MH_MAGIC_64) {
        fprintf(stderr, "不是 thin arm64 Mach-O（magic=0x%08x）\n", magic);
        return 0;
    }
    uint32_t ncmds;
    memcpy(&ncmds, g_file + 16, 4);
    const uint8_t *lc = g_file + 32;   /* sizeof(mach_header_64) */

    for (uint32_t i = 0; i < ncmds; ++i) {
        uint32_t cmd, cmdsize;
        memcpy(&cmd, lc, 4);
        memcpy(&cmdsize, lc + 4, 4);
        if (cmdsize == 0) break;

        if (cmd == LC_SEGMENT_64) {
            seg64_t *s = (seg64_t *)lc;   /* 结构布局与 Mach-O 一致 */
            if (strncmp(s->segname, "__TEXT", 16) == 0) {
                const uint8_t *sec = lc + sizeof(seg64_t);
                for (uint32_t j = 0; j < s->nsects; ++j) {
                    const sect64_t *sc = (const sect64_t *)(sec + j * sizeof(sect64_t));
                    if (strncmp(sc->sectname, "__text", 16) == 0) {
                        g_textVmaddr  = sc->addr;
                        g_textSize    = sc->size;
                        g_textFileoff = sc->offset;
                    }
                }
            }
        }
        lc += cmdsize;
    }
    if (g_textSize == 0) { fprintf(stderr, "__text 未找到\n"); return 0; }
    return 1;
}

/* 文件偏移 → vmaddr（仅处理 __TEXT 段内，fileoff==0 情形） */
static uint64_t fileoff_to_vmaddr(size_t off) {
    /* 主可执行文件 __TEXT 段 fileoff = 0，vmaddr = 基址 */
    return (uint64_t)off + 0x100000000ULL;
}

/* 在文件里搜索子串，输出所有匹配的 vmaddr */
static int find_string(const char *needle, int quiet) {
    size_t nlen = strlen(needle);
    int hits = 0;
    for (size_t i = 0; i + nlen <= g_size; ++i) {
        if (g_file[i] == (uint8_t)needle[0] &&
            memcmp(g_file + i, needle, nlen) == 0) {
            uint64_t va = fileoff_to_vmaddr(i);
            if (!quiet) printf("%llu 0x%llx %s\n",
                               (unsigned long long)va, (unsigned long long)va, needle);
            ++hits;
            i += nlen - 1;
        }
    }
    return hits;
}

/* 解码 ADRP，返回目标页地址；非 ADRP 返回 0 */
static uint64_t decode_adrp(uint32_t insn, uint64_t pc) {
    if ((insn & 0x9F000000u) != 0x90000000u) return 0;
    uint64_t immlo = (insn >> 29) & 0x3u;
    uint64_t immhi = (insn >> 5) & 0x7FFFFu;
    uint64_t imm = (immhi << 2) | immlo;
    /* 21 位符号扩展 */
    if (imm & (1u << 20)) imm |= ~((uint64_t)0x1FFFFF);
    return (pc & ~(uint64_t)0xFFF) + (imm << 12);
}

static uint32_t rd_of(uint32_t insn) { return insn & 0x1Fu; }

/* 解码 ADD (imm, 64 位, sh=0)，成功返回 1 并给出 rn/imm12 */
static int decode_add_imm64(uint32_t insn, uint32_t *rn, uint32_t *imm12) {
    if ((insn & 0xFFC00000u) != 0x91000000u) return 0;
    *rn = (insn >> 5) & 0x1Fu;
    *imm12 = (insn >> 10) & 0xFFFu;
    return 1;
}

/* 扫描 __text，找引用 target 的 ADRP+ADD 对 */
static int scan_xrefs(uint64_t target, int limit) {
    int hits = 0;
    const uint8_t *t = g_file + g_textFileoff;

    for (uint64_t off = 0; off + 8 <= g_textSize; off += 4) {
        uint32_t i0, i1;
        memcpy(&i0, t + off, 4);
        memcpy(&i1, t + off + 4, 4);

        uint64_t pc = g_textVmaddr + off;
        uint64_t page = decode_adrp(i0, pc);
        if (page == 0) continue;

        uint32_t rn, imm12;
        if (!decode_add_imm64(i1, &rn, &imm12)) continue;
        if (rd_of(i0) != rn) continue;     /* 同一寄存器才成对 */

        uint64_t resolved = page + imm12;
        if (resolved == target) {
            printf("  xref @ 0x%llx\n", (unsigned long long)pc);
            if (++hits >= limit) return hits;
        }
    }
    return hits;
}

/* 反向找函数起点：往前找最近的 "ret"(d65f03c0) 或 nop 填充之后的地址 */
static uint64_t find_func_start(uint64_t codeAddr) {
    const uint8_t *t = g_file + g_textFileoff;
    if (codeAddr < g_textVmaddr) return 0;
    uint64_t off = codeAddr - g_textVmaddr;
    for (uint64_t k = off; k >= 4; k -= 4) {
        uint32_t prev;
        memcpy(&prev, t + k - 4, 4);
        if (prev == 0xd65f03c0u /* ret */ || prev == 0xd503201fu /* nop */) {
            return g_textVmaddr + k;
        }
    }
    return g_textVmaddr;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "用法: %s <macho> find|xref|xrefs <子串|vmaddr>\n", argv[0]);
        return 1;
    }
    if (!load(argv[1])) return 1;
    if (!parse_macho()) return 1;

    printf("__text: vmaddr=0x%llx size=0x%llx fileoff=0x%x\n",
           (unsigned long long)g_textVmaddr, (unsigned long long)g_textSize,
           g_textFileoff);

    const char *mode = argv[2];

    if (strcmp(mode, "find") == 0) {
        int n = find_string(argv[3], 0);
        printf("匹配 %d 处\n", n);
        return 0;
    }
    if (strcmp(mode, "xref") == 0) {
        uint64_t target = strtoull(argv[3], NULL, 0);
        printf("引用 0x%llx 的位置：\n", (unsigned long long)target);
        scan_xrefs(target, 32);
        return 0;
    }
    if (strcmp(mode, "xrefs") == 0) {
        /* 先收集字符串地址，再逐个找交叉引用 */
        size_t nlen = strlen(argv[3]);
        for (size_t i = 0; i + nlen <= g_size; ++i) {
            if (g_file[i] != (uint8_t)argv[3][0] ||
                memcmp(g_file + i, argv[3], nlen) != 0) continue;
            uint64_t va = fileoff_to_vmaddr(i);
            printf("字符串 \"%s\" @ 0x%llx\n", argv[3], (unsigned long long)va);
            int hits = scan_xrefs(va, 64);
            if (hits == 0) printf("  （无 ADRP+ADD 引用）\n");
            i += nlen - 1;
        }
        return 0;
    }
    if (strcmp(mode, "callers") == 0) {
        /* 列出代码区间内所有 bl/b 目标（越界者为外部符号不列） */
        uint64_t a = strtoull(argv[3], NULL, 0), b = strtoull(argv[4], NULL, 0);
        const uint8_t *t = g_file + g_textFileoff;
        for (uint64_t va = a; va < b; va += 4) {
            if (va < g_textVmaddr || va + 4 > g_textVmaddr + g_textSize) continue;
            uint32_t ins;
            memcpy(&ins, t + (va - g_textVmaddr), 4);
            if ((ins & 0xFC000000u) != 0x94000000u) continue;   /* BL */
            int64_t imm = (int64_t)(ins & 0x03FFFFFFu);
            if (imm & (1 << 25)) imm -= (1 << 26);
            uint64_t tgt = va + ((uint64_t)imm << 2);
            printf("  0x%llx -> 0x%llx\n", (unsigned long long)va,
                   (unsigned long long)tgt);
        }
        return 0;
    }
    if (strcmp(mode, "strings") == 0) {
        /* 列出代码区间内被 adrp+add 引用的字符串 */
        uint64_t a = strtoull(argv[3], NULL, 0), b = strtoull(argv[4], NULL, 0);
        const uint8_t *t = g_file + g_textFileoff;
        for (uint64_t va = a; va + 8 <= b; va += 4) {
            if (va < g_textVmaddr || va + 8 > g_textVmaddr + g_textSize) continue;
            uint32_t i0, i1;
            memcpy(&i0, t + (va - g_textVmaddr), 4);
            memcpy(&i1, t + (va + 4 - g_textVmaddr), 4);
            uint64_t page = decode_adrp(i0, va);
            if (page == 0) continue;
            uint32_t rn, imm12;
            if (!decode_add_imm64(i1, &rn, &imm12)) continue;
            if (rd_of(i0) != rn) continue;
            uint64_t resolved = page + imm12;
            /* 只处理落在文件映射范围内的（数据段） */
            uint64_t off = resolved - 0x100000000ULL;
            if (resolved < 0x100000000ULL || off + 1 >= g_size) continue;
            const char *s = (const char *)(g_file + off);
            int n = 0;
            while (n < 48 && s[n] >= 0x20 && s[n] < 0x7F) ++n;
            if (n >= 3 && s[n] == 0) {
                printf("  0x%llx -> 0x%llx \"%s\"\n", (unsigned long long)va,
                       (unsigned long long)resolved, s);
            }
        }
        return 0;
    }
    if (strcmp(mode, "func") == 0) {
        uint64_t a = strtoull(argv[3], NULL, 0);
        printf("含 0x%llx 的函数起点 ≈ 0x%llx\n", (unsigned long long)a,
               (unsigned long long)find_func_start(a));
        return 0;
    }
    fprintf(stderr, "未知模式 %s\n", mode);
    return 1;
}
