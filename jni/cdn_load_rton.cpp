// ============================================================
// CDNLoadRTON — 扩充 PvZ2 国际版 CDN 加载 RTON 列表（教程精简版）
//
// 【并入说明 v3.3】本模块自 project2 并入 settingsframework 工程，
// 编译进同一 libsettingsframework.so，但代码保持完全独立（自带
// constructor / 日志 / hook 安装），与 Settings UI 框架零耦合。
// 两模块各自起线程等待 libPVZ2.so：CDNLoadRTON 5ms 轮询（须抢在
// 启动早期清单收集之前），settingsframework 100ms 轮询，互不影响。
//
// 原理（参考 B 站教程 + IDA 全链路实锤，v3.2）：
//   游戏启动时 sub_AC8418 收集 CDN 清单（53 项），每项经统一入口
//   sub_ACABCC(list, name_string, tableID, flag) push 进清单 vector。
//
//   应用阶段（状态机 sub_ACADB4 case4 → sub_D579B0 "Applying file"）
//   按清单逐条拼路径 <files>/No_backup/CDN.x.x/<文件名> 检查：
//     文件存在 → sub_D58544 "Apply" → sub_120AEFC 按 tableID 加载 RTON
//     文件不存在 → "Skipped" 静默跳过（不崩、不联网）
//
//   故本 so hook sub_ACABCC 固定注入全部条目：文件放 CDN 目录
//   即生效，不放即跳过 —— 天然按需，零配置。
//
// 用法（唯一操作）：
//   把要测试的 RTON 放 /sdcard/Android/data/<pkg>/files/No_backup/
//   CDN.x.x/（x.x = 游戏当前版本号，须与游戏实际使用的目录一致），
//   文件名与 kInjectList 中条目一致（如 PlantTypes.rton），重启游戏。
//
// 注入时机安全性：
//   hook 首次触发时正处于 sub_AC8418 收集流程内（主线程上下文），
//   注入的 push 与原版收集天然串行，无跨线程 vector 竞态。
//
// tableID 体系（v1.2 ARM64 libPVZ2.so 实测提取，与 11.6.1 教程不同！）：
//   tableID 是全局统一的 RtDbTable 索引，本地注册（sub_120A2E4）与
//   CDN 清单（sub_ACABCC）共用。见 kInjectList 注释中的完整对照。
//
// LawnStrings-zh-cn.rton（v3.3 新增，tableId=131 IDA 实证）：
//   vanilla 清单含动态生成的 LawnStrings-<locale>.rton 条目
//   （sub_16A8A4C "%c%c-%c%c" → tolower → "LawnStrings-%s" 拼名，
//   sub_AC8418 内 MOV W2,#0x83 push）。国际版无 zh-cn 语言包且游戏
//   语言为 en-us 时 vanilla 条目名为 LawnStrings-en-us.rton，无法命中
//   zh-cn 文件；此处显式注入静态名条目，使中文表与游戏语言解耦。
//   与 vanilla 的 en-us 条目同名不同文件互不影响（Apply 按名字找文件）。
//
// 时序注意（用户实测教训）：
//   在清单收集函数执行前就已本地加载并缓存的表（如 Fonts.rton）
//   加进清单也无效 —— 字形纹理等已被消费端缓存，重载数据不重建。
//   故本清单只含运行时读取的数据表，不含 fonts/colors/ui_layout。
//
// 注意：联网触发 CDN 版本更新时游戏可能清理 CDN 目录，测试建议断网。
// ============================================================

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <atomic>
#include <thread>
#include <chrono>
#include <sys/stat.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>     // dl_iterate_phdr（API 21+）

#ifdef __aarch64__
#include "And64InlineHook.hpp"
#endif

// ============================================================
// 构建标识（打包后用 zipfile 检查 APK 内 .so 是否含此字符串，
// 防止漏跑 ndk-build 把旧 so 打进包 —— project1 v27 教训）
// ============================================================
static const char kBuildTag[] = "CDNLoadRTON-v3.4-no-language-table";

// ============================================================
// 偏移量（v1.2 ARM64 libPVZ2.so，IDA imagebase=0）
// ============================================================
#ifdef __aarch64__
// sub_ACABCC：CDN 清单条目收集函数
//   void sub_ACABCC(void* list, const std::string& name, uint32_t tableId, uint32_t flag)
//   唯一调用方 = sub_AC8418（清单收集大函数，53 处 BL，全部 MOV W3,#1）
static constexpr uintptr_t OFF_LIST_ADD = 0xACABCC;
#else
// ARM32 适配预留：本版偏移仅对 ARM64 实测有效，ARM32 待另行提取
#endif

// ============================================================
// 本地文件日志（双输出：logcat tag CDNLoadRTON + 本地 .log 文件）
// ============================================================
static FILE *g_logFile = nullptr;
static std::atomic<uintptr_t> g_base{0};
static char g_files_dir[256] = {0};   // /sdcard/Android/data/<pkg>/files

static void log_init() {
    // 读 /proc/self/cmdline 取包名，适配不同包名
    char pkg[128] = {0};
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (f) {
        fgets(pkg, sizeof(pkg), f);   // 包名以 \0 结尾，fgets 读到首段
        fclose(f);
    }
    if (!pkg[0]) snprintf(pkg, sizeof(pkg), "unknown");

    char path[320];
    snprintf(g_files_dir, sizeof(g_files_dir), "/sdcard/Android/data/%s/files", pkg);
    snprintf(path, sizeof(path), "%s/CDNLoadRTON.log", g_files_dir);
    mkdir(g_files_dir, 0777);

    g_logFile = fopen(path, "w");   // 每次启动覆盖
    if (g_logFile) {
        fprintf(g_logFile, "=== CDNLoadRTON Log ===\n");
        fprintf(g_logFile, "build: %s\n", kBuildTag);
        fprintf(g_logFile, "package: %s\n", pkg);
        fflush(g_logFile);
    }
}

static void log_write(const char *fmt, ...) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    __android_log_print(ANDROID_LOG_INFO, "CDNLoadRTON", "%s", buf);
    if (g_logFile) {
        fprintf(g_logFile, "%s\n", buf);
        fflush(g_logFile);
    }
}

// ============================================================
// 注入清单（38 项）
//
// 命名规则：CDN 服务器文件名（自建 CDN 时与此处保持一致即可，
// 服务器目录放同名文件）。风格与 EA CDN 现有条目一致（PascalCase.rton）。
//
// tableID 对照（本版实测 vs 教程 11.6.1）：
//   table  文件名                       教程ID(11.6.1)
//   10     PropertySheets.rton          9
//   12     PinataTypes.rton             10
//   13     PlantTypes.rton              11
//   17     PlantAlmanacData.rton        15
//   18     PlantProperties.rton         16
//   19     Powers.rton                  17
//   20     ZombieTypes.rton             18
//   21     ZombieActions.rton           19
//   22     ZombieProperties.rton        20
//   23     CreatureTypes.rton           21
//   24     ProjectileTypes.rton         22
//   25     GridItemTypes.rton           23
//   26     EffectObjectTypes.rton       25
//   27     CollectableTypes.rton        26
//   28     PresentTables.rton           27
//   29     PresentTypes.rton            28
//   30     PlantFamilyTypes.rton        29
//   32     QuestsCategories.rton        31 (ItemGroups 24 本版已移除)
//   37     NPCDataSheets.rton           37
//   38     LevelModules.rton            38
//   39     HeroTypes.rton               —（本版新增）
//   41     GameFeatures.rton            40
//   42     ToolPackets.rton             41
//   49     BoardGridMaps.rton           48
//   50     LevelModulesDifficulty.rton  49
//   51     LevelMutatorModules.rton     50
//   52     LevelMutatorTables.rton      51
//   53     LevelScoringRules.rton       52
//   54     HotUIConfig.rton             53
//   56     ArmorTypes.rton              55
//   58     WorldMapList.rton            132 (WorldMapData)
//   59     WorldMapListRift.rton        —（本版新增，58 同族）
//   60     WorldMapListUtility.rton     —（本版新增，58 同族）
//   104    QuestRewards.rton            —（本版新增）
//   106    GridItemProps.rton           —（本版新增）
//   131    LawnStrings-zh-cn.rton       —（v3.3 新增，vanilla 动态条目同表；
//                                        见文件头"LawnStrings-zh-cn.rton"节）
//   132    OttoBot.rton                 —（本版新增玩法）
//   138    Autocado.rton                —（本版新增玩法）
// ============================================================
struct InjectEntry {
    const char *name;     // CDN 文件名（静态存储，NUL 结尾）
    uint32_t tableId;     // 本版 tableID
};

static constexpr InjectEntry kInjectList[] = {
    // A 组：教程 31 项中本版仍存在的 30 项
    {"PropertySheets.rton",           10},
    {"PinataTypes.rton",              12},
    {"PlantTypes.rton",               13},
    {"PlantAlmanacData.rton",         17},
    {"PlantProperties.rton",          18},
    {"Powers.rton",                   19},
    {"ZombieTypes.rton",              20},
    {"ZombieActions.rton",            21},
    {"ZombieProperties.rton",         22},
    {"CreatureTypes.rton",            23},
    {"ProjectileTypes.rton",          24},
    {"GridItemTypes.rton",            25},
    {"EffectObjectTypes.rton",        26},
    {"CollectableTypes.rton",         27},
    {"PresentTables.rton",            28},
    {"PresentTypes.rton",             29},
    {"PlantFamilyTypes.rton",         30},
    {"QuestsCategories.rton",         32},
    {"NPCDataSheets.rton",            37},
    {"LevelModules.rton",             38},
    {"GameFeatures.rton",             41},
    {"ToolPackets.rton",              42},
    {"BoardGridMaps.rton",            49},
    {"LevelModulesDifficulty.rton",   50},
    {"LevelMutatorModules.rton",      51},
    {"LevelMutatorTables.rton",       52},
    {"LevelScoringRules.rton",        53},
    {"HotUIConfig.rton",              54},
    {"ArmorTypes.rton",               56},
    {"WorldMapList.rton",             58},
    // B 组：本版新增候选（同族扩展 + 新玩法数据）
    {"HeroTypes.rton",                39},
    {"WorldMapListRift.rton",         59},
    {"WorldMapListUtility.rton",      60},
    {"QuestRewards.rton",            104},
    {"GridItemProps.rton",           106},
    {"OttoBot.rton",                 132},
    {"Autocado.rton",                138},
};
static constexpr size_t kInjectCount = sizeof(kInjectList) / sizeof(kInjectList[0]);

// ============================================================
// libc++ SSO string 布局（24 字节，与游戏 libPVZ2.so 内嵌 libc++ 一致）
//
// 已从 sub_ACABCC 反编译实证：
//   short (len <= 22)：首字节 = len << 1（LSB=0），数据内联于 +1..+23
//   long  (len >= 23)：+0 = cap（LSB=1 为 long 标志），+8 = size，
//                      +16 = data 指针（本 so 只作输入，函数内部
//                      自行深拷贝，指向静态字符串即可）
// ============================================================
struct GameString {
    uint8_t bytes[24];

    void init(const char *s, size_t len) {
        memset(bytes, 0, sizeof(bytes));
        if (len <= 22) {
            bytes[0] = static_cast<uint8_t>(len << 1);
            memcpy(bytes + 1, s, len);
        } else {
            size_t cap = ((len + 16) & ~(static_cast<size_t>(15))) | 1;
            memcpy(bytes + 0, &cap, 8);
            memcpy(bytes + 8, &len, 8);
            const char *p = s;
            memcpy(bytes + 16, &p, 8);
        }
    }
};

// ============================================================
// Hook 实现
// ============================================================
using ListAddFn = void (*)(void *, void *, uint32_t, uint32_t);
static ListAddFn oListAdd = nullptr;
static std::atomic<bool> g_injected{false};

// 诊断计数器：只统计经过 hkListAdd 的 vanilla push（v3 实测终值 53，
// 与 IDA 分析 sub_AC8418 内 53 处 BL 一致）；注入走 oListAdd 直调不计数
static std::atomic<uint32_t> g_add_count{0};
static std::atomic<uintptr_t> g_hook_list{0};

// 只读 dump list 对象前 0x40 字节
// v3 实测布局（两次 dump 数学验证：37 条态 end-begin=1184=37*32，
// 90 条态 =2880=90*32）：
//   +0x00 std::string "main.tar"（24B SSO，CDN 基础包名）
//   +0x18 flag=1
//   +0x20/+0x28/+0x30 = vector<Entry> begin/end/capacity（条目 32B/项）
//   +0x38 flag=1
static void dump_list_layout(void *list, const char *when) {
    auto *p = reinterpret_cast<uintptr_t *>(list);
    // vector 自动解码：直接给出条目数
    uintptr_t begin = p[4], end = p[5], cap = p[6];
    size_t n = (end >= begin) ? (end - begin) / 32 : 0;
    size_t c = (cap >= begin) ? (cap - begin) / 32 : 0;
    log_write("list dump (%s) @%p: entries=%zu cap=%zu (32B/entry)",
              when, list, n, c);
    for (int row = 0; row < 8; ++row) {
        uintptr_t v = p[row];
        log_write("  +0x%02x: 0x%016llx", row * 8,
                  static_cast<unsigned long long>(v));
    }
}

// 注入生效清单项（在 hook 首次触发 = 清单收集流程内部调用，主线程串行安全）
// 全量注入：文件放 No_backup/CDN.x.x/ 即 Apply，不放即 Skipped（v3.2）
static void inject_entries(void *list) {
    int ok = 0;
    for (size_t i = 0; i < kInjectCount; ++i) {
        const InjectEntry *ent = &kInjectList[i];
        GameString s;
        s.init(ent->name, strlen(ent->name));
        oListAdd(list, s.bytes, ent->tableId, 1);
        ++ok;
        log_write("INJECT[%zu/%zu] %-32s table=%u", i + 1, kInjectCount,
                  ent->name, ent->tableId);
    }
    log_write("inject done: %d entries added (vanilla list follows)", ok);
    // 已 push 的状态 dump（与 30s 后终态 dump 对比可定位 vector）
    char when[64];
    snprintf(when, sizeof(when), "after inject, %zu entries", kInjectCount);
    dump_list_layout(list, when);
}

[[maybe_unused]] static void hkListAdd(void *list, void *nameStr, uint32_t tableId, uint32_t flag) {
    g_add_count.fetch_add(1, std::memory_order_relaxed);
    g_hook_list.store(reinterpret_cast<uintptr_t>(list), std::memory_order_relaxed);
    if (!g_injected.exchange(true)) {
        log_write("ListAdd hook first hit (vanilla first entry table=%u), injecting %zu entries",
                  tableId, kInjectCount);
        inject_entries(list);
    }
    oListAdd(list, nameStr, tableId, flag);
}

// ============================================================
// 库基址获取（dl_iterate_phdr 稳定版，同 project1）
// ============================================================
static int dl_phdr_callback(struct dl_phdr_info *info, size_t, void *data) {
    auto *result = reinterpret_cast<uintptr_t *>(data);
    if (info->dlpi_name && strstr(info->dlpi_name, "libPVZ2.so")) {
        *result = info->dlpi_addr;
        return 1;
    }
    return 0;
}

static uintptr_t get_lib_base_stable() {
    uintptr_t base = 0;
    dl_iterate_phdr(dl_phdr_callback, &base);
    return base;
}

// ============================================================
// 诊断线程：hook 计数器 + 双时点 list dump
//
// 计数器语义（v3 实测确认）：只统计经过 hkListAdd 的调用 = vanilla
// push 次数，预期恒 53（与 IDA 分析 sub_AC8418 内 53 处 BL 对照）。
// 注入的条目走 oListAdd 直调不计数。注入总数由 g_injected +
// kInjectCount 自证，vector 条目数由 dump 的 entries= 给出终值
// （预期 53 + 注入数）。
//
// v1 教训：读 0x269A938 单例当清单对象的观测失效——hook 拿到的
// list 参数（+0x20 起 vector）才是真身；v3 双时点 dump 已实测解码。
// ============================================================
static void start_diag_monitor() {
    std::thread([]() {
        uint32_t last_count = static_cast<uint32_t>(-1);
        bool dumped_final = false;
        for (int i = 0; i < 120; ++i) {   // 60 秒
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            uint32_t c = g_add_count.load(std::memory_order_relaxed);

            if (c != last_count) {
                log_write("diag: vanilla ListAdd calls=%u (injected=%s)",
                          c, g_injected.load() ? "yes" : "no");
                last_count = c;
            }
            // 兜底报警：hook 已触发（清单在收集）但注入标志未置位 = 逻辑异常
            if (c > 1 && !g_injected.load(std::memory_order_acquire)) {
                log_write("FATAL-TIMING: ListAdd hit %u times but injection not armed", c);
            }
            // 30 秒：清单应收集完毕，dump 一次终态（entries= 应为 53+注入数）
            if (i == 60 && !dumped_final) {
                uintptr_t lp = g_hook_list.load(std::memory_order_relaxed);
                if (lp != 0) {
                    char when[64];
                    snprintf(when, sizeof(when), "30s, expect %u total entries",
                             53 + static_cast<uint32_t>(kInjectCount));
                    dump_list_layout(reinterpret_cast<void *>(lp), when);
                    dumped_final = true;
                }
            }
        }
        uint32_t final_count = g_add_count.load(std::memory_order_relaxed);
        log_write("diag: monitor exit (60s), vanilla ListAdd calls=%u "
                  "(expect 53), injected=%s (%zu entries)",
                  final_count, g_injected.load() ? "yes" : "no", kInjectCount);
        if (g_injected.load() && final_count != 53) {
            log_write("diag: NOTE vanilla count != 53 — 与 IDA 分析的 53 处 BL 不符，"
                      "可能存在不经过 sub_ACABCC 的 push 路径，需复核");
        }
    }).detach();
}

// ============================================================
// Hook 安装
// ============================================================
static void applyHooks() {
    uintptr_t base = get_lib_base_stable();
    if (base == 0) {
        log_write("applyHooks: libPVZ2.so not found");
        return;
    }
    g_base.store(base, std::memory_order_release);
    log_write("applyHooks: base = 0x%lx", static_cast<unsigned long>(base));

#ifdef __aarch64__
    A64HookFunction(
        reinterpret_cast<void *>(base + OFF_LIST_ADD),
        reinterpret_cast<void *>(hkListAdd),
        reinterpret_cast<void **>(&oListAdd));
    if (oListAdd) {
        log_write("ListAdd hook installed @+0x%lx (o=%p)",
                  static_cast<unsigned long>(OFF_LIST_ADD), (void *)oListAdd);
    } else {
        log_write("FATAL: ListAdd hook install failed");
        return;
    }
#else
    // ARM32 适配预留：本版偏移仅对 ARM64 实测有效，ARM32 待另行提取
    log_write("arch not supported yet (ARM64 only in v1)");
    return;
#endif

    start_diag_monitor();
}

// ============================================================
// 入口：随 libsettingsframework.so 加载自动执行
// 独立线程 5ms 轮询等待 libPVZ2.so（比 Settings UI 框架的 100ms 更快：
// sub_AC8418 是启动早期一次性调用，hook 必须抢在它之前装好）
// ============================================================
__attribute__((constructor)) void CDNLoadRTON_init() {
    log_init();
    log_write("constructor start (%s)", kBuildTag);
    log_write("language table injection=disabled; language_module is sole table 131 owner");

    std::thread([]() {
        int wait_count = 0;
        while (get_lib_base_stable() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (++wait_count > 6000) {   // 30 秒超时
                log_write("timeout waiting for libPVZ2.so (30s), aborting");
                return;
            }
        }
        log_write("libPVZ2.so loaded after %dms", wait_count * 5);
        applyHooks();
    }).detach();
}
