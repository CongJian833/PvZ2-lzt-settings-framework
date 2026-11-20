/* daveconv.c —— 生成戴夫动画 / 传送带计数 的静态补丁字节
 *
 * 用法: daveconv <binary>
 * 直接按 vm 地址计算相对偏移（静态包内 vm = 0x100000000 + fileoff，
 * 与运行时 base + off 的相对关系等价）。
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VM 0x100000000ULL

static uint32_t enc_b(uint64_t frm, uint64_t to) {
    int64_t off = (int64_t)(to - frm) >> 2;
    if (off < -(1LL << 25) || off >= (1LL << 25)) { fprintf(stderr, "B range\n"); exit(1); }
    return 0x14000000u | ((uint32_t)off & 0x03FFFFFFu);
}
static uint32_t enc_bl(uint64_t frm, uint64_t to) {
    int64_t off = (int64_t)(to - frm) >> 2;
    if (off < -(1LL << 25) || off >= (1LL << 25)) { fprintf(stderr, "BL range\n"); exit(1); }
    return 0x94000000u | ((uint32_t)off & 0x03FFFFFFu);
}
static uint32_t enc_adrp(int rd, uint64_t pc, uint64_t target) {
    int64_t imm = ((int64_t)(target & ~0xFFFULL) - (int64_t)(pc & ~0xFFFULL)) >> 12;
    if (imm < -(1LL << 20) || imm >= (1LL << 20)) { fprintf(stderr, "ADRP range\n"); exit(1); }
    return 0x90000000u | (((uint32_t)imm & 3u) << 29)
         | ((((uint32_t)(imm >> 2)) & 0x7FFFFu) << 5) | (uint32_t)rd;
}
static uint32_t enc_add_imm(int rd, int rn, uint32_t imm) {
    return 0x91000000u | ((imm & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_ldr_x(int rt, int rn, uint32_t off) {
    return 0xF9400000u | (((off / 8) & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static void put32(uint8_t *buf, uint64_t off, uint32_t w) { memcpy(buf + off, &w, 4); }
static uint32_t get32(const uint8_t *buf, uint64_t off) { uint32_t w; memcpy(&w, buf + off, 4); return w; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <binary>\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "read\n"); return 1; }
    fclose(f);

    /* 前置校验：原始字节必须匹配，否则不动 */
    struct { uint64_t off; uint32_t want; } chk[] = {
        {0x1a7440, 0x910402A0u}, {0x31582c, 0xB40001D7u}, {0x31585c, 0x910003E0u},
    };
    for (unsigned i = 0; i < sizeof(chk) / sizeof(chk[0]); ++i) {
        uint32_t got = get32(buf, chk[i].off);
        if (got != chk[i].want) {
            fprintf(stderr, "FAIL expect %#llx=%#010x got %#010x\n",
                    (unsigned long long)chk[i].off, chk[i].want, got);
            return 1;
        }
    }
    for (uint64_t i = 0; i < 0x60; ++i) if (buf[0x18bea1c + i]) { fprintf(stderr, "FAIL cave not zero\n"); return 1; }
    for (uint64_t i = 0; i < 16; ++i)   if (buf[0x18bea44 + i]) { fprintf(stderr, "FAIL str slot not zero\n"); return 1; }

    /* 戴夫字符串 "anim_%s_idle" 13 字符 + NUL，补齐 16 字节 */
    const char *ds = "anim_%s_idle";
    for (int i = 0; i < 16; ++i) buf[0x18bea44 + i] = (i < 13) ? (uint8_t)ds[i] : 0;

    /* 戴夫：把「NPC+0x100 的字符串指针」换成字面量 */
    uint64_t site = VM + 0x1a7440, str = VM + 0x18bea44;
    put32(buf, 0x1a7440, enc_adrp(0, site, str));
    put32(buf, 0x1a7444, enc_add_imm(0, 0, (uint32_t)(str & 0xFFFu)));
    put32(buf, 0x1a7448, enc_b(site + 8, site + 0x10));
    put32(buf, 0x1a744c, 0xD503201Fu);                    /* NOP（原 LDR X0,[X0]） */

    /* 传送带：10 条指令写入洞穴 */
    uint64_t cave = VM + 0x18bea1c;
    uint32_t c[10];
    c[0] = 0x910003E0u;                                   /* mov x0, sp */
    c[1] = enc_bl(cave + 4,  VM + 0x1c81b20);             /* 析构 v24 */
    c[2] = enc_adrp(8, cave + 8, VM + 0x27eb870);         /* adrp x8, 全局页 */
    c[3] = enc_ldr_x(8, 8, 0x870);                        /* ldr x8,[x8,#0x870] */
    c[4] = enc_ldr_x(0, 8, 0xa18);                        /* ldr x0,[x8,#0xa18] board */
    c[5] = enc_ldr_x(8, 19, 0x2c8);                       /* ldr x8,[x19,#0x2c8] pool */
    c[6] = 0x9B000000u | (24u << 16) | (8u << 10) | (21u << 5) | 1u;  /* madd x1,x21,x24,x8 */
    c[7] = enc_bl(cave + 28, VM + 0x19427c);              /* 场上植物计数 */
    c[8] = 0x0B000000u | (22u << 5) | 22u;                /* add w22,w22,w0 */
    c[9] = enc_b(cave + 36, VM + 0x315864);               /* 回原流程 */
    for (int i = 0; i < 10; ++i) put32(buf, 0x18bea1c + (uint64_t)i * 4, c[i]);

    put32(buf, 0x31585c, enc_b(VM + 0x31585c, cave));     /* 入口跳洞穴 */
    put32(buf, 0x31582c, 0xD503201Fu);                    /* NOP 跳过分支 */

    f = fopen(argv[1], "wb");
    if (!f) { perror("write"); return 1; }
    fwrite(buf, 1, (size_t)sz, f);
    fclose(f);
    free(buf);

    printf("dave @0x1a7440 : %08x %08x %08x %08x\n",
           enc_adrp(0, site, str), enc_add_imm(0, 0, (uint32_t)(str & 0xFFF)),
           enc_b(site + 8, site + 0x10), 0xD503201Fu);
    printf("conv @0x18bea1c :");
    for (int i = 0; i < 10; ++i) printf(" %08x", c[i]);
    printf("\nentry @0x31585c : %08x\n", enc_b(VM + 0x31585c, cave));
    printf("entry @0x31582c : d503201f (NOP)\n");
    printf("str  @0x18bea44 : anim_%%s_idle\n");
    printf("OK\n");
    return 0;
}
