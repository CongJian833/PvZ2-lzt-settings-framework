/*
 *  @date   : 2018/04/18
 *  @author : Rprop (r_prop@outlook.com)
 *  https://github.com/Rprop/And64InlineHook
 */
/*
 MIT License

 Copyright (c) 2018 Rprop (r_prop@outlook.com)

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 */
#define  __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>   // sysconf(_SC_PAGESIZE)：运行时页大小探测
#include <android/log.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_map.h>   // vm_protect（iOS 的 vm_address_t 已是 64 位）
#endif

#if defined(__aarch64__)

#include "And64InlineHook.hpp"
#if !defined(__APPLE__)
#include "lzt_core.h"
#endif
#define   A64_MAX_INSTRUCTIONS 5
#define   A64_MAX_REFERENCES   (A64_MAX_INSTRUCTIONS * 2)
#define   A64_NOP              0xd503201fu
#define   A64_JNIEXPORT        __attribute__((visibility("default")))
#if !defined(__APPLE__)
#define   A64_LOGE(...)        do { \
    __android_log_print(ANDROID_LOG_ERROR, "A64_HOOK", __VA_ARGS__); \
    lzt_core::log_write(__VA_ARGS__); \
} while (0)
#else
#define   A64_LOGE(...)        ((void)__android_log_print(ANDROID_LOG_ERROR, "A64_HOOK", __VA_ARGS__))
#endif
#ifndef NDEBUG
# define  A64_LOGI(...)        ((void)__android_log_print(ANDROID_LOG_INFO, "A64_HOOK", __VA_ARGS__))
#else
# define  A64_LOGI(...)        ((void)0)
#endif // NDEBUG
typedef uint32_t *__restrict *__restrict instruction;
struct context
{
    struct fix_info
    {
        uint32_t *bp;
        uint32_t  ls; // left-shift counts
        uint32_t  ad; // & operand
    };
    struct insns_info
    {
        union
        {
            uint64_t insu;
            int64_t  ins;
            void    *insp;
        };
        fix_info fmap[A64_MAX_REFERENCES];
    };
    int64_t    basep;
    int64_t    endp;
    insns_info dat[A64_MAX_INSTRUCTIONS];

public:
    inline bool is_in_fixing_range(const int64_t absolute_addr) {
        return absolute_addr >= this->basep && absolute_addr < this->endp;
    }
    inline intptr_t get_ref_ins_index(const int64_t absolute_addr) {
        return static_cast<intptr_t>((absolute_addr - this->basep) / sizeof(uint32_t));
    }
    inline intptr_t get_and_set_current_index(uint32_t *__restrict inp, uint32_t *__restrict outp) {
        intptr_t current_idx = this->get_ref_ins_index(reinterpret_cast<int64_t>(inp));
        this->dat[current_idx].insp = outp;
        return current_idx;
    }
    inline void reset_current_ins(const intptr_t idx, uint32_t *__restrict outp) {
        this->dat[idx].insp = outp;
    }
    void insert_fix_map(const intptr_t idx, uint32_t *bp, uint32_t ls = 0u, uint32_t ad = 0xffffffffu) {
        for (auto &f : this->dat[idx].fmap) {
            if (f.bp == NULL) {
                f.bp = bp;
                f.ls = ls;
                f.ad = ad;
                return;
            } //if
        }
        // What? GGing..
    }
    void process_fix_map(const intptr_t idx) {
        for (auto &f : this->dat[idx].fmap) {
            if (f.bp == NULL) break;
            *(f.bp) = *(f.bp) | (((int32_t(this->dat[idx].ins - reinterpret_cast<int64_t>(f.bp)) >> 2) << f.ls) & f.ad);
            f.bp = NULL;
        }
    }
};

//-------------------------------------------------------------------------

#define __intval(p)                reinterpret_cast<intptr_t>(p)
#define __uintval(p)               reinterpret_cast<uintptr_t>(p)
#define __ptr(p)                   reinterpret_cast<void *>(p)
#if defined(__APPLE__)
// iOS arm64 采用 16KB 页（Android 为 4KB）。若按 4KB 对齐，地址落不到真实
// 页边界，mprotect 会返回 EINVAL，Hook 安装直接失败。
#define __page_size                16384
#else
#define __page_size                4096
#endif
#define __page_align(n)            __align_up(static_cast<uintptr_t>(n), __page_size)
#define __ptr_align(x)             __ptr(__align_down(reinterpret_cast<uintptr_t>(x), __page_size))
#define __align_up(x, n)           (((x) + ((n) - 1)) & ~((n) - 1))
#define __align_down(x, n)         ((x) & -(n))
#define __countof(x)               static_cast<intptr_t>(sizeof(x) / sizeof((x)[0])) // must be signed
#define __atomic_increase(p)       __sync_add_and_fetch(p, 1)
#define __sync_cmpswap(p, v, n)    __sync_bool_compare_and_swap(p, v, n)
#define __predict_true(exp)        __builtin_expect((exp) != 0, 1)
#define __flush_cache(c, n)        __builtin___clear_cache(reinterpret_cast<char *>(c), reinterpret_cast<char *>(c) + n)

// ---- 页保护设置（iOS 强制 W^X，必须分两步）----
// iOS 不允许同一页同时可写可执行：内核会静默剥掉 EXEC（实测 mmap 申请
// RWX 后区域保护变成 3=读写）。因此统一约定：
//   写入前 __make_rwx() 取【写】权限（对已签名代码页走 VM_PROT_COPY 私有化）
//   写完后 __make_rx()  恢复【执行】权限
// Android 无此约束，两者都直接给 RWX。
// 计算需要改保护的页范围大小（从 p 所在页首到 p+n 所在页尾）。
// 注意：必须精确，不能多算一整页——旧的写法在 n 不是页大小整数倍时
// 会多覆盖一页，把紧随其后的 dylib 全局数据页一起改成只读可执行，
// 导致后续写这些全局变量时直接 SIGBUS（实测表现为游戏加载即崩）。
// mprotect 的真实页大小：Android 既有 4KB 也有 16KB 页设备，不能写死。
// 注意 __page_size 还要用于 __aligned__ 属性（必须编译期常量），两者不能合并。
#if defined(__APPLE__)
static inline size_t __lzt_runtime_page_size() { return __page_size; }
#else
static inline size_t __lzt_runtime_page_size() {
    static const size_t cached = [] {
        long v = ::sysconf(_SC_PAGESIZE);
        return v > 0 ? static_cast<size_t>(v) : static_cast<size_t>(4096);
    }();
    return cached;
}
#endif

static size_t __lzt_prot_size(void *p, size_t n) {
    const size_t ps = __lzt_runtime_page_size();
    const uintptr_t start = __align_down(__uintval(p), ps);
    const uintptr_t end   = __align_up(__uintval(p) + n, ps);
    return static_cast<size_t>(end - start);
}

#if defined(__APPLE__)
// 查询地址所在区域的当前保护位；失败返回 -1。
static int __lzt_current_prot(void *p) {
    vm_address_t region = (vm_address_t)p;
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                     (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS)
        return -1;
    return (int)info.protection;
}
#endif

static int __lzt_make_rwx(void *p, size_t n) {
    void  *addr = __ptr_align(p);
    size_t size = __lzt_prot_size(p, n);
#if defined(__APPLE__)
    /* 0) 已可写则无需任何操作（hook 流程会对同一页重复调用） */
    if (__lzt_current_prot(addr) & VM_PROT_WRITE) return 0;
    /* 1) 已签名代码页首次触碰：请求私有可写副本（COPY 仅首次有效，
     *    页一旦私有，再次带 COPY 会返回 KERN_NO_SPACE） */
    kern_return_t kr_copy = vm_protect(mach_task_self(), (vm_address_t)addr,
                                       (vm_size_t)size, FALSE,
                                       (vm_prot_t)(VM_PROT_READ | VM_PROT_WRITE |
                                                   VM_PROT_COPY));
    if (kr_copy == KERN_SUCCESS) return 0;
    /* 2) 页已是私有映射：直接给写权限 */
    kern_return_t kr_rw = vm_protect(mach_task_self(), (vm_address_t)addr,
                                     (vm_size_t)size, FALSE,
                                     (vm_prot_t)(VM_PROT_READ | VM_PROT_WRITE));
    if (kr_rw == KERN_SUCCESS) return 0;
    /* 3) 本进程自有映射（dylib 数据页等） */
    if (::mprotect(addr, size, PROT_READ | PROT_WRITE) == 0) return 0;
    A64_LOGE("make_rwx failed addr=%p size=%zu prot=%d copy_kr=%d rw_kr=%d errno=%d",
             addr, size, __lzt_current_prot(addr), (int)kr_copy, (int)kr_rw, errno);
    return -1;
#else
    return ::mprotect(addr, size, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
}

static int __lzt_make_rx(void *p, size_t n) {
    void  *addr = __ptr_align(p);
    size_t size = __lzt_prot_size(p, n);
#if defined(__APPLE__)
    kern_return_t kr = vm_protect(mach_task_self(), (vm_address_t)addr,
                                  (vm_size_t)size, FALSE,
                                  (vm_prot_t)(VM_PROT_READ | VM_PROT_EXECUTE));
    if (kr == KERN_SUCCESS) return 0;
    if (::mprotect(addr, size, PROT_READ | PROT_EXEC) == 0) return 0;
    return -1;
#else
    return ::mprotect(addr, size, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
}
#define __make_rwx(p, n)           __lzt_make_rwx((p), (n))
#define __make_rx(p, n)            __lzt_make_rx((p), (n))

//-------------------------------------------------------------------------

static bool __fix_branch_imm(instruction inpp, instruction outpp, context *ctxp)
{
    static constexpr uint32_t mbits = 6u;
    static constexpr uint32_t mask  = 0xfc000000u; // 0b11111100000000000000000000000000
    static constexpr uint32_t rmask = 0x03ffffffu; // 0b00000011111111111111111111111111
    static constexpr uint32_t op_b  = 0x14000000u; // "b"  ADDR_PCREL26
    static constexpr uint32_t op_bl = 0x94000000u; // "bl" ADDR_PCREL26

    const uint32_t ins = *(*inpp);
    const uint32_t opc = ins & mask;
    switch (opc) {
    case op_b:
    case op_bl:
        {
            intptr_t current_idx  = ctxp->get_and_set_current_index(*inpp, *outpp);
            int64_t absolute_addr = reinterpret_cast<int64_t>(*inpp) + (static_cast<int32_t>(ins << mbits) >> (mbits - 2u)); // sign-extended
            int64_t new_pc_offset = static_cast<int64_t>(absolute_addr - reinterpret_cast<int64_t>(*outpp)) >> 2; // shifted
            bool special_fix_type = ctxp->is_in_fixing_range(absolute_addr);
            // whether the branch should be converted to absolute jump
            if (!special_fix_type && llabs(new_pc_offset) >= (rmask >> 1)) {
                bool b_aligned = (reinterpret_cast<uint64_t>(*outpp + 2) & 7u) == 0u;
                if (opc == op_b) {
                    if (b_aligned != true) {
                        (*outpp)[0] = A64_NOP;
                        ctxp->reset_current_ins(current_idx, ++(*outpp));
                    } //if
                    (*outpp)[0] = 0x58000051u; // LDR X17, #0x8
                    (*outpp)[1] = 0xd61f0220u; // BR X17
                    memcpy(*outpp + 2, &absolute_addr, sizeof(absolute_addr));
                    *outpp += 4;
                } else {
                    if (b_aligned == true) {
                        (*outpp)[0] = A64_NOP;
                        ctxp->reset_current_ins(current_idx, ++(*outpp));
                    } //if
                    (*outpp)[0] = 0x58000071u; // LDR X17, #12
                    (*outpp)[1] = 0x1000009eu; // ADR X30, #16
                    (*outpp)[2] = 0xd61f0220u; // BR X17
                    memcpy(*outpp + 3, &absolute_addr, sizeof(absolute_addr));
                    *outpp += 5;
                } //if
            } else {
                if (special_fix_type) {
                    intptr_t ref_idx = ctxp->get_ref_ins_index(absolute_addr);
                    if (ref_idx <= current_idx) {
                        new_pc_offset = static_cast<int64_t>(ctxp->dat[ref_idx].ins - reinterpret_cast<int64_t>(*outpp)) >> 2;
                    } else {
                        ctxp->insert_fix_map(ref_idx, *outpp, 0u, rmask);
                        new_pc_offset = 0;
                    } //if
                } //if

                (*outpp)[0] = opc | (new_pc_offset & ~mask);
                ++(*outpp);
            } //if

            ++(*inpp);
            return ctxp->process_fix_map(current_idx), true;
        }
    }
    return false;
}

//-------------------------------------------------------------------------

static bool __fix_cond_comp_test_branch(instruction inpp, instruction outpp, context *ctxp)
{
    static constexpr uint32_t lsb     = 5u;
    static constexpr uint32_t lmask01 = 0xff00001fu; // 0b11111111000000000000000000011111
    static constexpr uint32_t mask0   = 0xff000010u; // 0b11111111000000000000000000010000
    static constexpr uint32_t op_bc   = 0x54000000u; // "b.c"  ADDR_PCREL19
    static constexpr uint32_t mask1   = 0x7f000000u; // 0b01111111000000000000000000000000
    static constexpr uint32_t op_cbz  = 0x34000000u; // "cbz"  Rt, ADDR_PCREL19
    static constexpr uint32_t op_cbnz = 0x35000000u; // "cbnz" Rt, ADDR_PCREL19
    static constexpr uint32_t lmask2  = 0xfff8001fu; // 0b11111111111110000000000000011111
    static constexpr uint32_t mask2   = 0x7f000000u; // 0b01111111000000000000000000000000
    static constexpr uint32_t op_tbz  = 0x36000000u; // 0b00110110000000000000000000000000 "tbz"  Rt, BIT_NUM, ADDR_PCREL14
    static constexpr uint32_t op_tbnz = 0x37000000u; // 0b00110111000000000000000000000000 "tbnz" Rt, BIT_NUM, ADDR_PCREL14

    const uint32_t ins = *(*inpp);
    uint32_t     lmask = lmask01;
    if ((ins & mask0) != op_bc) {
        uint32_t opc   = ins & mask1;
        if (opc != op_cbz && opc != op_cbnz) {
            opc = ins & mask2;
            if (opc != op_tbz && opc != op_tbnz) {
                return false;
            } //if
            lmask = lmask2;
        } //if
    } //if

    const uint32_t msb    = __builtin_clz(~lmask);
    intptr_t current_idx  = ctxp->get_and_set_current_index(*inpp, *outpp);
    int64_t absolute_addr = reinterpret_cast<int64_t>(*inpp) + (static_cast<int32_t>((ins & ~lmask) << msb) >> (lsb - 2u + msb));
    int64_t new_pc_offset = static_cast<int64_t>(absolute_addr - reinterpret_cast<int64_t>(*outpp)) >> 2; // shifted
    bool special_fix_type = ctxp->is_in_fixing_range(absolute_addr);
    if (!special_fix_type && llabs(new_pc_offset) >= (~lmask >> (lsb + 1))) {
        if ((reinterpret_cast<uint64_t>(*outpp + 4) & 7u) != 0u) {
            (*outpp)[0] = A64_NOP;
            ctxp->reset_current_ins(current_idx, ++(*outpp));
        } //if
        (*outpp)[0] = (((8u >> 2u) << lsb) & ~lmask) | (ins & lmask); // B.C #0x8
        (*outpp)[1] = 0x14000005u; // B #0x14
        (*outpp)[2] = 0x58000051u; // LDR X17, #0x8
        (*outpp)[3] = 0xd61f0220u; // BR X17
        memcpy(*outpp + 4, &absolute_addr, sizeof(absolute_addr));
        *outpp += 6;
    } else {
        if (special_fix_type) {
            intptr_t ref_idx = ctxp->get_ref_ins_index(absolute_addr);
            if (ref_idx <= current_idx) {
                new_pc_offset = static_cast<int64_t>(ctxp->dat[ref_idx].ins - reinterpret_cast<int64_t>(*outpp)) >> 2;
            } else {
                ctxp->insert_fix_map(ref_idx, *outpp, lsb, ~lmask);
                new_pc_offset = 0;
            } //if
        } //if

        (*outpp)[0] = (static_cast<uint32_t>(new_pc_offset << lsb) & ~lmask) | (ins & lmask);
        ++(*outpp);
    } //if

    ++(*inpp);
    return ctxp->process_fix_map(current_idx), true;
}

//-------------------------------------------------------------------------

static bool __fix_loadlit(instruction inpp, instruction outpp, context *ctxp)
{
    const uint32_t ins = *(*inpp);

    // memory prefetch("prfm"), just skip it
    // http://infocenter.arm.com/help/topic/com.arm.doc.100069_0608_00_en/pge1427897420050.html
    if ((ins & 0xff000000u) == 0xd8000000u) {
        ctxp->process_fix_map(ctxp->get_and_set_current_index(*inpp, *outpp));
        ++(*inpp);
        return true;
    } //if

    static constexpr uint32_t msb        = 8u;
    static constexpr uint32_t lsb        = 5u;
    static constexpr uint32_t mask_30    = 0x40000000u; // 0b01000000000000000000000000000000
    static constexpr uint32_t mask_31    = 0x80000000u; // 0b10000000000000000000000000000000
    static constexpr uint32_t lmask      = 0xff00001fu; // 0b11111111000000000000000000011111
    static constexpr uint32_t mask_ldr   = 0xbf000000u; // 0b10111111000000000000000000000000
    static constexpr uint32_t op_ldr     = 0x18000000u; // 0b00011000000000000000000000000000 "LDR Wt/Xt, label" | ADDR_PCREL19
    static constexpr uint32_t mask_ldrv  = 0x3f000000u; // 0b00111111000000000000000000000000
    static constexpr uint32_t op_ldrv    = 0x1c000000u; // 0b00011100000000000000000000000000 "LDR St/Dt/Qt, label" | ADDR_PCREL19
    static constexpr uint32_t mask_ldrsw = 0xff000000u; // 0b11111111000000000000000000000000
    static constexpr uint32_t op_ldrsw   = 0x98000000u; // "LDRSW Xt, label" | ADDR_PCREL19 | load register signed word
    // LDR S0, #0 | 0b00011100000000000000000000000000 | 32-bit
    // LDR D0, #0 | 0b01011100000000000000000000000000 | 64-bit
    // LDR Q0, #0 | 0b10011100000000000000000000000000 | 128-bit
    // INVALID    | 0b11011100000000000000000000000000 | may be 256-bit

    uint32_t  mask     = mask_ldr;
    uintptr_t faligned = (ins & mask_30) ? 7u : 3u;
    if ((ins & mask_ldr) != op_ldr) {
        mask = mask_ldrv;
        if (faligned != 7u)
            faligned = (ins & mask_31) ? 15u : 3u;
        if ((ins & mask_ldrv) != op_ldrv) {
            if ((ins & mask_ldrsw) != op_ldrsw) {
                return false;
            } //if
            mask     = mask_ldrsw;
            faligned = 7u;
        } //if
    } //if

    intptr_t current_idx  = ctxp->get_and_set_current_index(*inpp, *outpp);
    int64_t absolute_addr = reinterpret_cast<int64_t>(*inpp) + ((static_cast<int32_t>(ins << msb) >> (msb + lsb - 2u)) & ~3u);
    int64_t new_pc_offset = static_cast<int64_t>(absolute_addr - reinterpret_cast<int64_t>(*outpp)) >> 2; // shifted
    bool special_fix_type = ctxp->is_in_fixing_range(absolute_addr);
    // special_fix_type may encounter issue when there are mixed data and code
    if (special_fix_type || (llabs(new_pc_offset) + (faligned + 1u - 4u) / 4u) >= (~lmask >> (lsb + 1))) { // inaccurate, but it works
        while ((reinterpret_cast<uint64_t>(*outpp + 2) & faligned) != 0u) {
            *(*outpp)++ = A64_NOP;
        }
        ctxp->reset_current_ins(current_idx, *outpp);

        // Note that if memory at absolute_addr is writeable (non-const), we will fail to fetch it.
        // And what's worse, we may unexpectedly overwrite something if special_fix_type is true...
        uint32_t ns = static_cast<uint32_t>((faligned + 1) / sizeof(uint32_t));
        (*outpp)[0] = (((8u >> 2u) << lsb) & ~mask) | (ins & lmask); // LDR #0x8
        (*outpp)[1] = 0x14000001u + ns; // B #0xc
        memcpy(*outpp + 2, reinterpret_cast<void *>(absolute_addr), faligned + 1);
        *outpp += 2 + ns;
    } else {
        faligned >>= 2; // new_pc_offset is shifted and 4-byte aligned
        while ((new_pc_offset & faligned) != 0) {
            *(*outpp)++   = A64_NOP;
            new_pc_offset = static_cast<int64_t>(absolute_addr - reinterpret_cast<int64_t>(*outpp)) >> 2;
        }
        ctxp->reset_current_ins(current_idx, *outpp);

        (*outpp)[0] = (static_cast<uint32_t>(new_pc_offset << lsb) & ~mask) | (ins & lmask);
        ++(*outpp);
    } //if

    ++(*inpp);
    return ctxp->process_fix_map(current_idx), true;
}

//-------------------------------------------------------------------------

static bool __fix_pcreladdr(instruction inpp, instruction outpp, context *ctxp)
{
    // Load a PC-relative address into a register
    // http://infocenter.arm.com/help/topic/com.arm.doc.100069_0608_00_en/pge1427897645644.html
    static constexpr uint32_t msb     = 8u;
    static constexpr uint32_t lsb     = 5u;
    static constexpr uint32_t mask    = 0x9f000000u; // 0b10011111000000000000000000000000
    static constexpr uint32_t rmask   = 0x0000001fu; // 0b00000000000000000000000000011111
    static constexpr uint32_t lmask   = 0xff00001fu; // 0b11111111000000000000000000011111
    static constexpr uint32_t fmask   = 0x00ffffffu; // 0b00000000111111111111111111111111
    static constexpr uint32_t max_val = 0x001fffffu; // 0b00000000000111111111111111111111
    static constexpr uint32_t op_adr  = 0x10000000u; // "adr"  Rd, ADDR_PCREL21
    static constexpr uint32_t op_adrp = 0x90000000u; // "adrp" Rd, ADDR_ADRP

    const uint32_t ins = *(*inpp);
    intptr_t current_idx;
    switch (ins & mask) {
    case op_adr:
        {
            current_idx           = ctxp->get_and_set_current_index(*inpp, *outpp);
            int64_t lsb_bytes     = static_cast<uint32_t>(ins << 1u) >> 30u;
            int64_t absolute_addr = reinterpret_cast<int64_t>(*inpp) + (((static_cast<int32_t>(ins << msb) >> (msb + lsb - 2u)) & ~3u) | lsb_bytes);
            int64_t new_pc_offset = static_cast<int64_t>(absolute_addr - reinterpret_cast<int64_t>(*outpp));
            bool special_fix_type = ctxp->is_in_fixing_range(absolute_addr);
            if (!special_fix_type && llabs(new_pc_offset) >= (max_val >> 1)) {
                if ((reinterpret_cast<uint64_t>(*outpp + 2) & 7u) != 0u) {
                    (*outpp)[0] = A64_NOP;
                    ctxp->reset_current_ins(current_idx, ++(*outpp));
                } //if

                (*outpp)[0] = 0x58000000u | (((8u >> 2u) << lsb) & ~mask) | (ins & rmask); // LDR #0x8
                (*outpp)[1] = 0x14000003u; // B #0xc
                memcpy(*outpp + 2, &absolute_addr, sizeof(absolute_addr));
                *outpp += 4;
            } else {
                if (special_fix_type) {
                    intptr_t ref_idx = ctxp->get_ref_ins_index(absolute_addr & ~3ull);
                    if (ref_idx <= current_idx) {
                        new_pc_offset = static_cast<int64_t>(ctxp->dat[ref_idx].ins - reinterpret_cast<int64_t>(*outpp));
                    } else {
                        ctxp->insert_fix_map(ref_idx, *outpp, lsb, fmask);
                        new_pc_offset = 0;
                    } //if
                } //if

                // the lsb_bytes will never be changed, so we can use lmask to keep it
                (*outpp)[0] = (static_cast<uint32_t>(new_pc_offset << (lsb - 2u)) & fmask) | (ins & lmask);
                ++(*outpp);
            } //if
        }
        break;
    case op_adrp:
        {
            current_idx           = ctxp->get_and_set_current_index(*inpp, *outpp);
            int32_t lsb_bytes     = static_cast<uint32_t>(ins << 1u) >> 30u;
            int64_t absolute_addr = (reinterpret_cast<int64_t>(*inpp) & ~0xfffll) + ((((static_cast<int32_t>(ins << msb) >> (msb + lsb - 2u)) & ~3u) | lsb_bytes) << 12);
            A64_LOGI("ins = 0x%.8X, pc = %p, abs_addr = %p",
                     ins, *inpp, reinterpret_cast<int64_t *>(absolute_addr));
            if (ctxp->is_in_fixing_range(absolute_addr)) {
                intptr_t ref_idx = ctxp->get_ref_ins_index(absolute_addr/* & ~3ull*/);
                if (ref_idx > current_idx) {
                    // the bottom 12 bits of absolute_addr are masked out,
                    // so ref_idx must be less than or equal to current_idx!
                    A64_LOGE("ref_idx must be less than or equal to current_idx!");
                } //if

                // *absolute_addr may be changed due to relocation fixing
                A64_LOGI("What is the correct way to fix this?");
                *(*outpp)++ = ins; // 0x90000000u;
            } else {
                if ((reinterpret_cast<uint64_t>(*outpp + 2) & 7u) != 0u) {
                    (*outpp)[0] = A64_NOP;
                    ctxp->reset_current_ins(current_idx, ++(*outpp));
                } //if

                (*outpp)[0] = 0x58000000u | (((8u >> 2u) << lsb) & ~mask) | (ins & rmask); // LDR #0x8
                (*outpp)[1] = 0x14000003u; // B #0xc
                memcpy(*outpp + 2, &absolute_addr, sizeof(absolute_addr)); // potential overflow?
                *outpp += 4;
            } //if
        }
        break;
    default:
        return false;
    }

    ctxp->process_fix_map(current_idx);
    ++(*inpp);
    return true;
}

//-------------------------------------------------------------------------

static void __fix_instructions(uint32_t *__restrict inp, int32_t count, uint32_t *__restrict outp)
{
    context ctx;
    ctx.basep = reinterpret_cast<int64_t>(inp);
    ctx.endp  = reinterpret_cast<int64_t>(inp + count);
    memset(ctx.dat, 0, sizeof(ctx.dat));
    static_assert(sizeof(ctx.dat) / sizeof(ctx.dat[0]) == A64_MAX_INSTRUCTIONS,
                  "please use A64_MAX_INSTRUCTIONS!");
#ifndef NDEBUG
    if (count > A64_MAX_INSTRUCTIONS) {
        A64_LOGE("too many fixing instructions!");
    } //if
#endif // NDEBUG

    uint32_t *const outp_base = outp;

    while (--count >= 0) {
        if (__fix_branch_imm(&inp, &outp, &ctx)) continue;
        if (__fix_cond_comp_test_branch(&inp, &outp, &ctx)) continue;
        if (__fix_loadlit(&inp, &outp, &ctx)) continue;
        if (__fix_pcreladdr(&inp, &outp, &ctx)) continue;

        // without PC-relative offset
        ctx.process_fix_map(ctx.get_and_set_current_index(inp, outp));
        *(outp++) = *(inp++);
    }

    static constexpr uint_fast64_t mask = 0x03ffffffu; // 0b00000011111111111111111111111111
    auto callback  = reinterpret_cast<int64_t>(inp);
    auto pc_offset = static_cast<int64_t>(callback - reinterpret_cast<int64_t>(outp)) >> 2;
    if (static_cast<uint_fast64_t>(llabs(pc_offset)) >= (mask >> 1)) {
        if ((reinterpret_cast<uint64_t>(outp + 2) & 7u) != 0u) {
            outp[0] = A64_NOP;
            ++outp;
        } //if
        outp[0] = 0x58000051u; // LDR X17, #0x8
        outp[1] = 0xd61f0220u; // BR X17
        *reinterpret_cast<int64_t *>(outp + 2) = callback;
        outp += 4;
    } else {
        outp[0] = 0x14000000u | (pc_offset & mask); // "B" ADDR_PCREL26
        ++outp;
    } //if

    const uintptr_t total = (outp - outp_base) * sizeof(uint32_t);
    __flush_cache(outp_base, total); // necessary
}

//-------------------------------------------------------------------------

extern "C" {
    // trampoline 池：必须独占【整数个内存页】。
    // 原因：池的页面会被改成「可读 + 可执行」以承载 trampoline；若它与其它
    // 全局数据共用一页，那些数据会一并变成只读，之后写它们就是 SIGBUS
    // （实测表现为 dylib 加载后、构造函数第一步就崩，日志都写不出来）。
    // 因此这里用字节数组并按页向上取整，保证池边界与页边界对齐。
    static constexpr size_t kInsnsRowBytes = A64_MAX_INSTRUCTIONS * 10 * sizeof(uint32_t);
    static constexpr size_t kInsnsPoolBytes = A64_MAX_BACKUPS * kInsnsRowBytes;
    // 池必须整页独占。按最大可能页（16KB）对齐并向上取整：
    // 这样在 4KB 与 16KB 页设备上都恰好处在页边界，不会与相邻全局数据共页。
    static constexpr size_t kInsnsPoolAlign =
        __page_size > 16384 ? __page_size : 16384;
    static constexpr size_t kInsnsPoolPadded =
        (kInsnsPoolBytes + kInsnsPoolAlign - 1) / kInsnsPoolAlign * kInsnsPoolAlign;
    static __attribute__((__aligned__(kInsnsPoolAlign))) uint8_t __insns_pool_raw[kInsnsPoolPadded];
#define __insns_pool (reinterpret_cast<uint32_t (*)[A64_MAX_INSTRUCTIONS * 10]>(__insns_pool_raw))

    //-------------------------------------------------------------------------

    class A64HookInit
    {
    public:
        A64HookInit()
        {
            // 此处【不要】改池的保护：宿主的 W^X 策略要求「写入时取写权限、
            // 写完后恢复执行权限」，这已由每次装 hook 时的 __make_rwx / __make_rx
            // 成对完成。启动阶段把整池设为 RX 会连带把同页数据变只读。
            A64_LOGI("insns pool ready: %zu bytes (%zu pages of %zu, runtime page %zu)",
                     kInsnsPoolBytes, kInsnsPoolPadded / kInsnsPoolAlign,
                     kInsnsPoolAlign, __lzt_runtime_page_size());
        }
    };
    static A64HookInit __init;

    //-------------------------------------------------------------------------

    static uint32_t *FastAllocateTrampoline()
    {
        static_assert((A64_MAX_INSTRUCTIONS * 10 * sizeof(uint32_t)) % 8 == 0, "8-byte align");
        static volatile int32_t __index = -1;

        int32_t i = __atomic_increase(&__index);
        if (__predict_true(i >= 0 && i < A64_MAX_BACKUPS)) {
            return __insns_pool[i];
        } //if

        A64_LOGE("failed to allocate trampoline!");
        return NULL;
    }

    //-------------------------------------------------------------------------

    A64_JNIEXPORT void *A64HookFunctionV(void *const symbol, void *const replace,
                                         void *const rwx, const uintptr_t rwx_size)
    {
        static constexpr uint_fast64_t mask = 0x03ffffffu; // 0b00000011111111111111111111111111

        uint32_t *trampoline = static_cast<uint32_t *>(rwx), *original = static_cast<uint32_t *>(symbol);

        static_assert(A64_MAX_INSTRUCTIONS >= 5, "please fix A64_MAX_INSTRUCTIONS!");
        auto pc_offset = static_cast<int64_t>(__intval(replace) - __intval(symbol)) >> 2;
        if (static_cast<uint_fast64_t>(llabs(pc_offset)) >= (mask >> 1)) {
            int32_t count = (reinterpret_cast<uint64_t>(original + 2) & 7u) != 0u ? 5 : 4;
            if (trampoline) {
                if (rwx_size < count * 10u) {
                    A64_LOGE("rwx size is too small to hold %u bytes backup instructions!", count * 10u);
                    return NULL;
                } //if
                __make_rwx(trampoline, A64_MAX_INSTRUCTIONS * 10 * sizeof(uint32_t));
                __fix_instructions(original, count, trampoline);
            } //if

            if (__make_rwx(original, 5 * sizeof(uint32_t)) == 0) {
                if (count == 5) {
                    original[0] = A64_NOP;
                    ++original;
                } //if
                original[0] = 0x58000051u; // LDR X17, #0x8
                original[1] = 0xd61f0220u; // BR X17
                *reinterpret_cast<int64_t *>(original + 2) = __intval(replace);
                __flush_cache(symbol, 5 * sizeof(uint32_t));
                __make_rx(symbol, 5 * sizeof(uint32_t));
                if (trampoline)
                    __make_rx(trampoline, A64_MAX_INSTRUCTIONS * 10 * sizeof(uint32_t));

                A64_LOGI("inline hook %p->%p successfully! %zu bytes overwritten",
                         symbol, replace, 5 * sizeof(uint32_t));
            } else {
                A64_LOGE("mprotect failed with errno = %d, p = %p, size = %zu",
                         errno, original, 5 * sizeof(uint32_t));
                trampoline = NULL;
            } //if
        } else {
            if (trampoline) {
                if (rwx_size < 1u * 10u) {
                    A64_LOGE("rwx size is too small to hold %u bytes backup instructions!", 1u * 10u);
                    return NULL;
                } //if
                __make_rwx(trampoline, A64_MAX_INSTRUCTIONS * 10 * sizeof(uint32_t));
                __fix_instructions(original, 1, trampoline);
            } //if

            if (__make_rwx(original, 1 * sizeof(uint32_t)) == 0) {
                __sync_cmpswap(original, *original, 0x14000000u | (pc_offset & mask)); // "B" ADDR_PCREL26
                __flush_cache(symbol, 1 * sizeof(uint32_t));
                __make_rx(symbol, 1 * sizeof(uint32_t));
                if (trampoline)
                    __make_rx(trampoline, A64_MAX_INSTRUCTIONS * 10 * sizeof(uint32_t));

                A64_LOGI("inline hook %p->%p successfully! %zu bytes overwritten",
                         symbol, replace, 1 * sizeof(uint32_t));
            } else {
                A64_LOGE("mprotect failed with errno = %d, p = %p, size = %zu",
                         errno, original, 1 * sizeof(uint32_t));
                trampoline = NULL;
            } //if
        } //if

        return trampoline;
    }

    //-------------------------------------------------------------------------

    A64_JNIEXPORT void A64HookFunction(void *const symbol, void *const replace, void **result)
    {
        void *trampoline = NULL;
        if (result != NULL) {
            trampoline = FastAllocateTrampoline();
            *result = trampoline;
            if (trampoline == NULL) return;
        } //if

        // fix Android 10 .text segment is read-only by default
        __make_rwx(symbol, 5 * sizeof(size_t));

        trampoline = A64HookFunctionV(symbol, replace, trampoline, A64_MAX_INSTRUCTIONS * 10u);
        if (trampoline == NULL && result != NULL) {
            *result = NULL;
        } //if
    }
}

#endif // defined(__aarch64__)
