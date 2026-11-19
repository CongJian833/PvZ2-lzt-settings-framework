/*
 * lzt_fishhook.cpp — 符号重绑定（fishhook 原理），用于捕获游戏的文件访问
 *
 * 为什么需要它：
 *   本项目要在 iOS 上定位「游戏读取 CDN 资源（LawnStrings）的代码」。
 *   inline hook 不能用于 libc 函数——它们在 dyld 共享缓存里，patch 会走远跳
 *   路径导致启动崩溃（实测）。fishhook 只改数据段的符号指针表
 *   （__la_symbol_ptr / __got），不触碰代码页，因此安全。
 *
 * 与安卓的差异说明：
 *   Android 可直接 inline hook libc（无共享缓存问题）；iOS 必须用本方式。
 *   这是平台差异的必要处理，不改变上层业务逻辑。
 *
 * 关键修正（两处，均为实测踩坑）：
 *   1) 符号表偏移是【文件偏移】，须换算：
 *        linkedit_base = slide + __LINKEDIT.vmaddr - __LINKEDIT.fileoff
 *      早期漏减 fileoff，导致表指针全错位、遍历为空。
 *   2) 必须遍历【所有】镜像，不能只处理主程序：游戏的文件读取发生在
 *      libc++/Foundation 等库内部，其调用经各自镜像的符号槽进行。
 *      早期只处理主程序，结果「装了钩子却零调用」。
 */

#include <cstring>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <dlfcn.h>
#include <unistd.h>

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <mach/vm_map.h>

#include "lzt_core.h"

#ifdef __APPLE__

namespace {

struct SectRef {
    uint64_t addr;
    uint64_t size;
    uint32_t reserved1;
};

// 让目标地址所在页可写。__got/__la_symbol_ptr 通常位于只读段，
// 直接写指针会 SIGSEGV；用 vm_protect + VM_PROT_COPY 取私有可写副本
// （与 And64InlineHook 在 iOS 上的做法一致）。
void make_writable(void* addr, size_t size) {
    const long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) return;
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    const vm_address_t page = a & ~(vm_address_t)(ps - 1);
    const size_t len = (a - page) + size;
    vm_protect(mach_task_self(), page, (vm_size_t)len, FALSE,
               (vm_prot_t)(VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY));
    vm_protect(mach_task_self(), page, (vm_size_t)len, FALSE,
               (vm_prot_t)(VM_PROT_READ | VM_PROT_WRITE));
}

// 在单个镜像中把 symbol 的指针槽替换为 replacement，返回替换数。
int rebind_in_image(const struct mach_header_64* hdr, intptr_t slide,
                    const char* symbol, void* replacement, bool enableDiag) {
    if (hdr == nullptr || hdr->magic != MH_MAGIC_64) return 0;

    const struct symtab_command* symtab = nullptr;
    const struct dysymtab_command* dysym = nullptr;
    const struct segment_command_64* linkedit = nullptr;
    SectRef ptrSects[32];
    int ptrSectCount = 0;

    const struct load_command* lc =
        (const struct load_command*)((const uint8_t*)hdr + sizeof(*hdr));
    for (uint32_t i = 0; i < hdr->ncmds; ++i) {
        if (lc->cmd == LC_SYMTAB) {
            symtab = (const struct symtab_command*)lc;
        } else if (lc->cmd == LC_DYSYMTAB) {
            dysym = (const struct dysymtab_command*)lc;
        } else if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* seg = (const struct segment_command_64*)lc;
            if (strncmp(seg->segname, "__LINKEDIT", 16) == 0) linkedit = seg;
            const struct section_64* sec =
                (const struct section_64*)((const uint8_t*)seg + sizeof(*seg));
            for (uint32_t j = 0; j < seg->nsects; ++j, ++sec) {
                const bool isPtr =
                    strncmp(sec->sectname, "__la_symbol_ptr", 16) == 0 ||
                    strncmp(sec->sectname, "__got", 16) == 0;
                if (!isPtr || ptrSectCount >= 32) continue;
                ptrSects[ptrSectCount].addr = sec->addr;
                ptrSects[ptrSectCount].size = sec->size;
                ptrSects[ptrSectCount].reserved1 = sec->reserved1;
                ++ptrSectCount;
            }
        }
        lc = (const struct load_command*)((const uint8_t*)lc + lc->cmdsize);
    }
    if (symtab == nullptr || dysym == nullptr || linkedit == nullptr ||
        ptrSectCount == 0) {
        return 0;
    }

    // 【关键】符号表相关偏移是文件偏移，必须换算成 vmaddr。
    const uint8_t* linkBase =
        (const uint8_t*)(linkedit->vmaddr + slide - linkedit->fileoff);
    const char* strtab = (const char*)(linkBase + symtab->stroff);
    const struct nlist_64* syms = (const struct nlist_64*)(linkBase + symtab->symoff);
    const uint32_t* indirect = (const uint32_t*)(linkBase + dysym->indirectsymoff);

    int patched = 0;
    for (int s = 0; s < ptrSectCount; ++s) {
        void** slots = (void**)(ptrSects[s].addr + slide);
        const size_t count = ptrSects[s].size / sizeof(void*);
        for (size_t k = 0; k < count; ++k) {
            const uint32_t idxInInd = ptrSects[s].reserved1 + (uint32_t)k;
            if (!lzt_core::memory_range_accessible((uintptr_t)(indirect + idxInInd),
                                                   sizeof(uint32_t), false)) {
                break;
            }
            const uint32_t symIdx = indirect[idxInInd];
            if (symIdx == INDIRECT_SYMBOL_LOCAL || symIdx == INDIRECT_SYMBOL_ABS) continue;
            if (symIdx >= symtab->nsyms) continue;
            if (!lzt_core::memory_range_accessible((uintptr_t)(syms + symIdx),
                                                   sizeof(struct nlist_64), false)) continue;
            const uint32_t strx = syms[symIdx].n_un.n_strx;
            if (strx == 0 || strx >= symtab->strsize) continue;
            if (!lzt_core::memory_range_accessible((uintptr_t)(strtab + strx), 1, false)) continue;
            const char* name = strtab + strx;
            const char* bare = (name[0] == '_') ? name + 1 : name;
            if (strcmp(bare, symbol) != 0) continue;

            // 真正写入（早期只统计不写，导致「装了钩子却零生效」）
            make_writable(&slots[k], sizeof(void*));
            slots[k] = replacement;
            ++patched;
            if (enableDiag) {
                lzt_core::log_write("rebind: 写入 %s @ img_slide=0x%lx slot=%p",
                                    symbol, (unsigned long)slide, (void*)&slots[k]);
            }
        }
    }
    return patched;
}

} // namespace

// 把所有已加载镜像中 symbol 的指针槽替换为 replacement。
// 成功返回被替换的槽数；*orig_out 为原函数地址（经 dlsym 取得）。
extern "C" int lzt_rebind(const char* symbol, void* replacement, void** orig_out) {
    if (symbol == nullptr || replacement == nullptr) return 0;

    void* real = dlsym(RTLD_DEFAULT, symbol);
    if (real == nullptr) {
        lzt_core::log_write("rebind: dlsym(%s) 失败", symbol);
        return 0;
    }
    if (orig_out) *orig_out = real;

    // 【关键】遍历所有镜像：游戏的文件读取发生在 libc++/Foundation 等库内部，
    // 其调用经各自镜像的符号槽进行；只处理主程序会「装了钩子却零调用」（实测）。
    const uint32_t n = _dyld_image_count();
    int total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const struct mach_header_64* hdr =
            (const struct mach_header_64*)_dyld_get_image_header(i);
        if (hdr == nullptr || hdr->magic != MH_MAGIC_64) continue;
        const intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        total += rebind_in_image(hdr, slide, symbol, replacement, false);
    }
    lzt_core::log_write("rebind: %s -> %p，共替换 %d 个槽（原函数 %p）",
                        symbol, replacement, total, real);
    return total;
}

#endif // __APPLE__
