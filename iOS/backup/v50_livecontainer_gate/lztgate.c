/* lztgate.c —— 最小侵入静态站点生成器
 *
 * 入口只改 4 字节：一条 B 指令跳到 gate。因此只需重放【原始第 1 条指令】。
 * 若第 1 条指令是 PC 相对的（adrp/adr/b/bl/cbz/tbz/ldr literal），拒绝该站点，
 * 不做任何改写 —— 宁可少一个功能，也不产生不确定行为。
 *
 * gate 区放在 __TEXT 头部 slack（load command 之后、首个 section 之前），
 * 不新增段、不移动任何既有数据。
 *
 * 每站点 48 字节：
 *   +0x00  adrp x16, slot_page        ; 取槽
 *   +0x04  ldr  x16, [x16, #slot_off]
 *   +0x08  cbnz x16, +8               ; 槽空 -> 走 tramp（原逻辑）
 *   +0x0C  b    tramp
 *   +0x10  br   x16                   ; 槽非空 -> 进 hook
 *   +0x14  nop
 *   +0x18  (保留)
 *   +0x20  tramp: 原第1条指令
 *   +0x24  adrp x17, page(entry+4)
 *   +0x28  add  x17, x17, #lo
 *   +0x2C  br   x17                   ; 回到原函数剩余部分
 *
 * gate 区头部（16 字节）：
 *   +0x00  magic "LZTGATE1"
 *   +0x08  u32 stride
 *   +0x0C  u32 count
 *
 * 槽位于 __DATA 段尾部（vmsize 覆盖内、filesize 之外的可写零页）。
 *
 * 用法: lztgate <macho> <manifest>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/loader.h>

#define MAX_HOOKS    512
#define STRIDE       48
#define HDR_BYTES    16
#define SLOT_BYTES   4096
#define GATE_MAGIC   "LZTGATE1"

static uint32_t enc_b(uint64_t pc, uint64_t target) {
    int64_t d = (int64_t)target - (int64_t)pc;
    if (d % 4) { fprintf(stderr, "B misaligned: 0x%llx\n", (unsigned long long)d); exit(1); }
    d >>= 2;
    if (d > 0x7FFFFF || d < -0x800000) {
        fprintf(stderr, "B out of range: %lld\n", (long long)d); exit(1);
    }
    return 0x14000000u | ((uint32_t)d & 0x03FFFFFFu);
}
static uint32_t enc_adrp(int rd, uint64_t pc, uint64_t target) {
    int64_t d = ((int64_t)(target & ~0xFFFULL) - (int64_t)(pc & ~0xFFFULL)) >> 12;
    return 0x90000000u | ((uint32_t)(d & 3) << 29) |
           ((uint32_t)((d >> 2) & 0x7FFFF) << 5) | (uint32_t)rd;
}
static uint32_t enc_ldr_x(int rt, int rn, uint32_t off) {
    if (off % 8 || off > 0xFF8) { fprintf(stderr, "slot off bad 0x%x\n", off); exit(1); }
    return 0xF9400000u | ((off / 8) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static uint32_t enc_br(int rn)  { return 0xD61F0000u | ((uint32_t)rn << 5); }
static uint32_t enc_cbnz_x(int rt, uint64_t pc, uint64_t target) {
    int64_t d = ((int64_t)target - (int64_t)pc) >> 2;
    return 0xB5000000u | ((uint32_t)(d & 0x7FFFF) << 5) | (uint32_t)rt;
}
static uint32_t enc_add_imm(int rd, int rn, uint32_t imm12) {
    return 0x91000000u | ((imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
/* 非零 = 该指令含 PC 相对量，不能整体搬走 */
static int is_pcrel(uint32_t i) {
    if ((i & 0x9F000000u) == 0x90000000u) return 1;  /* adrp          */
    if ((i & 0x1F000000u) == 0x10000000u) return 1;  /* adr           */
    if ((i & 0x7C000000u) == 0x14000000u) return 1;  /* b / bl        */
    if ((i & 0x3B000000u) == 0x18000000u) return 1;  /* ldr literal   */
    if ((i & 0x7E000000u) == 0x34000000u) return 1;  /* cbz / cbnz    */
    if ((i & 0x7E000000u) == 0x36000000u) return 1;  /* tbz / tbnz    */
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <macho> <manifest>\n", argv[0]); return 1; }

    FILE* f = fopen(argv[1], "r+b");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc(fsz);
    if (!buf || fread(buf, 1, fsz, f) != (size_t)fsz) { fprintf(stderr, "read fail\n"); return 1; }

    struct mach_header_64* h = (struct mach_header_64*)buf;
    if (h->magic != MH_MAGIC_64) { fprintf(stderr, "not arm64 macho\n"); return 1; }

    struct segment_command_64 *text = NULL, *data = NULL;
    uint64_t first_sect = 0;
    uint32_t off = sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        struct load_command* lc = (struct load_command*)(buf + off);
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64* s = (struct segment_command_64*)lc;
            if (!strcmp(s->segname, "__TEXT")) text = s;
            if (!strcmp(s->segname, "__DATA")) data = s;
            struct section_64* sec = (struct section_64*)((uint8_t*)s + sizeof(*s));
            for (uint32_t k = 0; k < s->nsects; k++)
                if (sec[k].size && sec[k].offset)
                    if (!first_sect || sec[k].offset < first_sect) first_sect = sec[k].offset;
        }
        off += lc->cmdsize;
    }
    if (!text || !data || !first_sect) { fprintf(stderr, "segments missing\n"); return 1; }
    /* ---- 先读 manifest：gate 落点需要知道站点数量 ---- */
    struct { int idx; unsigned toff; } hooks[MAX_HOOKS];
    int nh = 0;
    FILE* m = fopen(argv[2], "r");
    if (!m) { perror("manifest"); return 1; }
    char line[160];
    while (fgets(line, sizeof line, m)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        int i; unsigned o;
        if (sscanf(line, "%d %x", &i, &o) == 2) {
            if (nh >= MAX_HOOKS) { fprintf(stderr, "too many hooks\n"); return 1; }
            hooks[nh].idx = i; hooks[nh].toff = o; nh++;
        }
    }
    fclose(m);
    if (!nh) { fprintf(stderr, "empty manifest\n"); return 1; }

    /* ---- gate 区定位（v50）----
     *
     * 历史教训：原实现把 gate 放在 load command 之后（0x2330），
     * 那里到首个 section 仅 40 字节空隙。LiveContainer 等工具会向主程序
     * 注入 2 条 LC（ncmds 79 -> 81），把 LZTGATE1 直接覆盖掉，
     * 表现为 gate_hdr=0x0、全部站点跳过、设置页无新 Tab（实测）。
     *
     * 新策略：优先使用 __TEXT 段尾零填充区（最后一个 section 结束 ~ 段结束）。
     * 不受 load command 增删影响；位于 __TEXT filesize 内，写入不改变文件大小，
     * ldid 重签不会截断。空间不足时回退到原位置。 */
    uint64_t need = HDR_BYTES + (uint64_t)nh * STRIDE;

    uint64_t last_sect_end = 0;
    {
        uint32_t o2 = sizeof(struct mach_header_64);
        for (uint32_t i = 0; i < h->ncmds; i++) {
            struct load_command* lc = (struct load_command*)(buf + o2);
            if (lc->cmd == LC_SEGMENT_64) {
                struct segment_command_64* s = (struct segment_command_64*)lc;
                if (strcmp(s->segname, "__TEXT") != 0) { o2 += lc->cmdsize; continue; }  /* 只看 __TEXT */
                struct section_64* sec = (struct section_64*)((uint8_t*)s + sizeof(*s));
                for (uint32_t k = 0; k < s->nsects; k++) {
                    if (!sec[k].size) continue;
                    uint64_t e = sec[k].offset + sec[k].size;
                    if (e > last_sect_end) last_sect_end = e;
                }
            }
            o2 += lc->cmdsize;
        }
    }

    uint64_t text_end_off = text->fileoff + text->filesize;
    uint64_t tail_off = (first_sect > need) ? ((first_sect - need) & ~15ULL) : 0;
    uint64_t tail_avail = (first_sect > tail_off) ? (first_sect - tail_off) : 0;

    int tail_ok = (tail_avail >= need) ? 1 : 0;
    if (tail_ok) {
        for (uint64_t k = 0; k < need; ++k)
            if (buf[tail_off + k]) { tail_ok = 0; break; }
    }

    uint64_t lc_end = sizeof(struct mach_header_64) + h->sizeofcmds;
    uint64_t gate_off;
    if (tail_ok) {
        gate_off = tail_off;
        printf("gate location: __TEXT tail zero region @0x%llx (avail=%llu B, need=%llu B)\n",
               (unsigned long long)gate_off,
               (unsigned long long)tail_avail, (unsigned long long)need);
    } else {
        gate_off = (lc_end + 15) & ~15ULL;
        printf("gate location: after load commands @0x%llx (tail unusable: avail=%llu need=%llu)\n",
               (unsigned long long)gate_off,
               (unsigned long long)tail_avail, (unsigned long long)need);
    }
    uint64_t gate_vm  = text->vmaddr + (gate_off - text->fileoff);
    uint64_t cap = tail_ok ? (tail_avail - HDR_BYTES) / STRIDE
                           : ((first_sect > gate_off) ? (first_sect - gate_off - HDR_BYTES) / STRIDE : 0);

    /* 槽基址：__DATA 段尾部（vmsize 内、filesize 外的可写零页） */
    uint64_t slot_base = (data->vmaddr + data->vmsize - SLOT_BYTES) & ~15ULL;

    /* 先校验全部站点，全部可打桩才动手，避免留下半成品 */
    for (int n = 0; n < nh; n++) {
        uint64_t tb = text->fileoff + hooks[n].toff;
        if (tb + 4 > (uint64_t)fsz) { fprintf(stderr, "hook %d beyond file\n", hooks[n].idx); return 1; }
        uint32_t insn = *(uint32_t*)(buf + tb);
        if ((insn & 0xFC000000u) == 0x14000000u) {
            fprintf(stderr, "hook %d already patched (b at entry)\n", hooks[n].idx); return 1;
        }
        if (is_pcrel(insn)) {
            fprintf(stderr, "hook %d @+0x%x: first insn 0x%08x is PC-relative -> REFUSE\n",
                    hooks[n].idx, hooks[n].toff, insn);
            return 2;
        }
        for (int k = 0; k < nh; k++)
            if (k != n && hooks[k].toff == hooks[n].toff) {
                fprintf(stderr, "duplicate target @+0x%x\n", hooks[n].toff); return 1;
            }
    }
    /* 校验 gate 区为空白 */
    for (uint64_t k = 0; k < HDR_BYTES + (uint64_t)nh * STRIDE; k++)
        if (buf[gate_off + k]) {
            fprintf(stderr, "gate area not zero @+0x%llx\n", (unsigned long long)k); return 1;
        }

    /* 写头部 */
    memcpy(buf + gate_off, GATE_MAGIC, 8);
    *(uint32_t*)(buf + gate_off + 8)  = (uint32_t)STRIDE;
    *(uint32_t*)(buf + gate_off + 12) = (uint32_t)nh;
    uint64_t gates_vm = gate_vm + HDR_BYTES;

    for (int n = 0; n < nh; n++) {
        uint64_t target = text->vmaddr + hooks[n].toff;
        uint64_t base   = gates_vm + (uint64_t)n * STRIDE;
        uint64_t slot   = slot_base + (uint64_t)n * 8;   /* 按 manifest 顺序依次分配 */
        uint64_t g_off  = gate_off + HDR_BYTES + (uint64_t)n * STRIDE;

        uint32_t* g = (uint32_t*)(buf + g_off);
        g[0] = enc_adrp(16, base + 0, slot & ~0xFFFULL);
        g[1] = enc_ldr_x(16, 16, (uint32_t)(slot & 0xFFF));
        g[2] = enc_cbnz_x(16, base + 8, base + 0x10);
        g[3] = enc_b(base + 12, base + 0x20);
        g[4] = enc_br(16);
        g[5] = 0xD503201F;                       /* nop */

        /* tramp：重放原第 1 条指令，再绝对跳回入口+4 */
        uint32_t* t = (uint32_t*)(buf + g_off + 0x20);
        t[0] = *(uint32_t*)(buf + text->fileoff + hooks[n].toff);
        t[1] = enc_adrp(17, base + 0x24, (target + 4) & ~0xFFFULL);
        t[2] = enc_add_imm(17, 17, (uint32_t)((target + 4) & 0xFFF));
        t[3] = enc_br(17);

        /* 入口：4 字节 B -> gate */
        *(uint32_t*)(buf + text->fileoff + hooks[n].toff) = enc_b(target, base);

        printf("hook %2d @+0x%-8x gate=0x%llx slot=0x%llx first=0x%08x\n",
               hooks[n].idx, hooks[n].toff, (unsigned long long)base,
               (unsigned long long)slot, t[0]);
    }

    printf("gate area: file 0x%llx vm 0x%llx cap=%llu used=%d stride=%d\n",
           (unsigned long long)gate_off, (unsigned long long)gate_vm,
           (unsigned long long)cap, nh, STRIDE);

    fseek(f, 0, SEEK_SET);
    fwrite(buf, 1, fsz, f);
    fclose(f);
    printf("ok: %d hooks, size unchanged %ld\n", nh, fsz);
    return 0;
}
