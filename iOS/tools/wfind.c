// wfind.c — 在 Mach-O 里查找 UTF-32 宽串并定位引用它的 adrp+add
// 用法: wfind <macho> <ascii>        例如 wfind pvz.arm64 OPTIONS_MUSIC_LABEL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint8_t *g;
static size_t gsz;
static uint64_t textVm, textSz; static uint32_t textOff = 0x7000;

static uint32_t r32(size_t o){ return (uint32_t)g[o] | ((uint32_t)g[o+1]<<8) | ((uint32_t)g[o+2]<<16) | ((uint32_t)g[o+3]<<24); }

int main(int argc, char**argv){
    if (argc < 3) { fprintf(stderr, "usage: %s <macho> <ascii>\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    g = malloc((size_t)n); gsz = (size_t)n;
    if (fread(g, 1, gsz, f) != gsz) { fprintf(stderr, "read fail\n"); return 1; }
    fclose(f);

    textVm = 0x100007000ULL; textSz = 0x1cc186cULL; textOff = 0x7000;
    fprintf(stderr, "text vm=0x%llx off=0x%x size=0x%llx\n",
            (unsigned long long)textVm, textOff, (unsigned long long)textSz);

    char pat[256]; snprintf(pat, sizeof(pat), "[%s]", argv[2]);
    const char *pats[2] = { pat, argv[2] };
    for (int pi = 0; pi < 2; ++pi) {
        size_t plen = strlen(pats[pi]);
        size_t blen = plen * 4;
        uint8_t *needle = calloc(1, blen);
        for (size_t i = 0; i < plen; ++i) needle[i*4] = (uint8_t)pats[pi][i];

        for (size_t o = 0; o + blen <= gsz; ++o) {
            if (memcmp(g + o, needle, blen) != 0) continue;
            uint64_t vm = 0x100000000ULL + o;   // __TEXT: vm == fileoff
            printf("WIDE \"%s\" file=0x%zx vm=0x%llx\n", pats[pi], o, (unsigned long long)vm);
            int found = 0;
            for (size_t i = 0; i + 16 <= textSz; i += 4) {
                uint32_t ins = r32(textOff + i);
                if ((ins & 0x9F000000u) != 0x90000000u) continue;
                int64_t immlo = (ins >> 29) & 3;
                int64_t immhi = (ins >> 5) & 0x7FFFF;
                int64_t imm = ((immhi << 2) | immlo) << 12;
                uint64_t pc = textVm + i;
                uint64_t page = pc & ~0xFFFULL;
                int rd = ins & 0x1F;
                if (page + imm != (vm & ~0xFFFULL)) continue;
                for (int k = 1; k <= 4; ++k) {
                    uint32_t ins2 = r32(textOff + i + k*4);
                    if ((ins2 & 0xFFC00000u) != 0x91000000u) continue;
                    int rd2 = ins2 & 0x1F, rn2 = (ins2 >> 5) & 0x1F;
                    int imm12 = (ins2 >> 10) & 0xFFF, sh = (ins2 >> 22) & 1;
                    if (rd2 != rd || rn2 != rd) continue;
                    uint64_t off = sh ? ((uint64_t)imm12 << 12) : (uint64_t)imm12;
                    if (page + imm + off == vm) {
                        printf("  REF at vm=0x%llx\n", (unsigned long long)(textVm + i));
                        ++found;
                    }
                }
            }
            if (!found) printf("  (no adrp+add reference found)\n");
        }
        free(needle);
    }
    return 0;
}
