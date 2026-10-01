// ============================================================
// language_module.cpp — “语言”设置模块
//
// 职责：向设置页注入“语言”Tab，并提供运行期语言切换。
// 位置：Settings 框架 slot=1（Build Version 之后、view_angle 之前）。
//
// ---- 一、语言表来源（外置优先，内置兜底）----
//   调度（按序尝试 → 命中即用 → 失败节流 → 强制重测）由框架 TableLoader
//   （settings/table_loader.h）承担，模块侧只提供 fetch / parse 回调：
//     ① CDN LANGUAGETYPES.rton —— AsciiORM 二进制（language_types_rton.cpp）
//     ② CDN LANGUAGETYPES.json —— 严格最小解析器（language_types.cpp）
//        受编译开关 kEnableFastReadChannel 控制，默认关闭（不入来源表）
//     ③ 数据包 packages\LANGUAGETYPES.rton —— 游戏 RSB 名查找
//        （read_bytes_from_package；仅 ARM64）
//   parse 统一产出 ParseResult：LocalizedName→checkbox 键名、
//   LawnStringsType→locale、Enabled→是否展示；最多 kMaxLangs(16) 项；
//   表内缺 en-us 时自动补首位。全不可用时由 loader 置 3 秒失败节流。
//
// ---- 二、Tab 可见性（kEnableEmptyLangPageFallback）----
//   开关关闭（默认）：来源就绪且 enabled 语言数 > 1 才创建语言 Tab，
//   否则不创建（不进入后续流程）。开关开启：始终创建（旧空白页行为）。
//   Tab 创建晚于 constructor，故 lang_should_create() 会经 lang_loader().retry()
//   强制重测一次——此时 libPVZ2 base 已就绪，数据包兜底可读。
//
// ---- 三、选择持久化 ----
//   每语言一个游戏 bool 配置，key = “lzt.lang.<locale>”（经
//   lzt_settings::config 零字段变体）。单选语义：唯一为 true 者为当前语言，
//   全 false 回落 en-us。files/LanguageFlag 保持双写（启动层真相源）；
//   旧版短标志（zh/en）在首次读取时迁移为 bool 配置。
//
// ---- 四、启动层（重启生效）----
//   DisplayInfo→语言管理器 +0x1E4 为当前语言 FourCC（大写打包，如
//   “ZH-CN”=0x5A48434E）。constructor 启动抢跑线程（5ms 轮询），在游戏首次
//   消费该字段前写入标志文件中的目标 locale。
//   注意：主字段 +0x1E0 恒为 0x600（非 FourCC，游戏他处使用），只读不写——
//   v6.1 曾写它导致进游戏崩溃。
//
// ---- 五、热重载层（ARM64；ARM32 仅保存 + 重启生效）----
//   hot_reload(locale) 主流程：
//     ① write_langmgr_locale()：LangMgr 次语言字段前置写（匹配用）
//     ② try_external_apply()：LangCache/CDN 磁盘表命中 → 原版 CDN apply 链
//     ③ 未命中 → RSB 直载（4 种名字形式，主形式
//        “packages\LawnStrings-<locale>.rton”）→ 官方 apply → 必要时补广播
//     ④ 成功后由 write_state() 调 refresh_all_localized() 回填已加载文本
//   RSB 名字前缀 “packages\” 是 v6.4 定位的命中根因；CDN apply 的 logCtx
//   必须传栈上 FakeLogCtx（v5.0 传 0 即写地址 0x48 崩溃）。
//
// ---- 六、诊断 ----
//   只读探针（字体状态 / 键解析 / CJK 度量 / RSB 树实查）全部由 LZT_DBG_ONLY
//   门包住：Release 不执行、不产生日志；Debug 版保留完整取证能力。
// ============================================================

#include "language_module.h"

#include "lzt_settings_framework_config.h"   // kEnableEmptyLangPageFallback / kEnableFastReadChannel
#include "settings/settings_framework.h"
#include "lzt_core.h"
#include "offsets.h"
#include "resource_file.h"
#include "language_types.h"
#include "language_types_rton.h"
#include "settings/table_loader.h"
#include "settings/game_abi.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <dirent.h>


#ifdef __aarch64__
#include <dlfcn.h>
#include <link.h>
#endif

// 诊断门统一为 lzt_settings_framework_config.h 的 LZT_DBG_ONLY（v7.37 起全工程一套）：
// 正式版不执行探针、不产生日志与内存读取；Debug 版保留全部取证能力。

namespace {

// ---- 文案键（本地化由人工维护）----
constexpr wchar_t kTitleKey[] = L"[LANGUAGE_ANGLE_TITLE]";
// kLabelEn 用于 LANGUAGETYPES 缺 en-us 时补位的默认语言标签
constexpr wchar_t kLabelEn[]  = L"[LANGUAGE_ENGLISH]";

constexpr const char *kLocaleEn = "en-us";

constexpr const char *kFlagFileName   = "LanguageFlag";

// ---- 选择状态缓存（locale 字符串；默认 en-us 兼容首启）----
std::atomic<bool> g_loaded{false};
char g_currentLocale[16] = "en-us";

// files 目录 / 包名统一由 lzt_core 提供（v7.37；/proc/self/cmdline 读取全工程仅一处）。

// ---- 标志文件：一行 locale 字符串（如 zh-cn / en-us / fr-fr ...）----
bool read_flag_locale(char *out, size_t outSize) {
    char dir[256], path[320];
    lzt_core::files_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s", dir, kFlagFileName);
    FILE *f = fopen(path, "r");
    if (!f) {
        lzt_core::log_write("Language flag read: path=%s exists=0 valid=0", path);
        return false;
    }
    char buf[32] = {0};
    bool ok = fgets(buf, sizeof(buf), f) != nullptr;
    fclose(f);
    if (!ok) {
        lzt_core::log_write("Language flag read: path=%s exists=1 valid=0", path);
        return false;
    }
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    if (n == 0 || n >= outSize) {
        lzt_core::log_write("Language flag read: path=%s exists=1 valid=0", path);
        return false;
    }
    memcpy(out, buf, n + 1);
    lzt_core::log_write("Language flag read: path=%s exists=1 valid=1 locale=%s", path, out);
    return true;
}

// 旧短标志迁移（v3.x 遗留）：zh→zh-cn、en→en-us。
// 注意与 lzt_language::normalize_locale 区分——后者是 LANGUAGETYPES 的
// 大小写/下划线归一，两者语义不同。
void migrate_legacy_locale(char *locale, size_t size) {
    if (strcasecmp(locale, "zh") == 0)      snprintf(locale, size, "zh-cn");
    else if (strcasecmp(locale, "en") == 0) snprintf(locale, size, "en-us");
}

void write_flag_locale(const char *locale) {
    char dir[256], path[320];
    lzt_core::files_dir(dir, sizeof(dir));
    mkdir(dir, 0777);
    snprintf(path, sizeof(path), "%s/%s", dir, kFlagFileName);
    FILE *f = fopen(path, "w");
    if (!f) {
        lzt_core::log_write("Language flag write: path=%s result=open-failed", path);
        return;
    }
    fprintf(f, "%s\n", locale);
    fflush(f);
    fclose(f);
    lzt_core::log_write("Language flag write: path=%s locale=%s result=ok", path, locale);
}

// ============================================================
// 动态语言表（阶段3）：LANGUAGETYPES.json（CDN 通道）→ checkbox 列表
//   - 上限 16 项（thunk 分发容量）
//   - 仅收 Enabled==true 条目；表内缺 en-us 时自动补首位（默认语言）
//   - 构建/解析失败时 build_page 渲染空白页（v7.22）
// 标签存储：LocalizedName 是游戏文本键（"[LANGUAGE_ENGLISH]"，ASCII），
//   逐字节扩为 wchar_t；非 ASCII 字节按 Latin-1 扩展（键名场景不出现）
// ============================================================
constexpr int kMaxLangs = 16;

struct LangItem {
    char locale[16];
    wchar_t label[64];
};

LangItem g_langs[kMaxLangs];
int g_langCount = 0;
static int  g_langEnabledCount = 0;     // 来源中 enabled 语言数（Tab 可见性判定）
static bool g_langSourceLogged = false; // 来源摘要日志只打一次（首次加载成功）
// 注：加载失败不锁死进程——失败节流与强制重测由 TableLoader 内部管理
//（v6.0 的 3 秒节流语义保留在框架层，见 settings/table_loader.h）。

// 阶段4：每语言一个游戏 bool 配置，key 形如 "lzt.lang.zh-cn"
void lang_config_key(const char *locale, char *out, size_t outSize) {
    snprintf(out, outSize, "lzt.lang.%s", locale);
}

// 生成本表全部语言的配置 key，并把指针填入 outKeys（指向内部静态缓冲）。
// 供框架 config::query_radio / save_radio 的单选组接口使用；单线程调用。
// 返回 key 数（= min(g_langCount, maxKeys)）。
int lang_config_keys(const char** outKeys, size_t maxKeys) {
    static char buf[kMaxLangs][40];
    const int n = (g_langCount < static_cast<int>(maxKeys))
                      ? g_langCount : static_cast<int>(maxKeys);
    for (int i = 0; i < n; ++i) {
        lang_config_key(g_langs[i].locale, buf[i], sizeof(buf[i]));
        outKeys[i] = buf[i];
    }
    return n;
}

void lang_label_ascii(const wchar_t *label, char *out, size_t outSize) {
    size_t i = 0;
    for (; label[i] && i + 1 < outSize; ++i) {
        wchar_t ch = label[i];
        out[i] = (ch >= 0x20 && ch <= 0x7E) ? static_cast<char>(ch) : '?';
    }
    out[i] = 0;
}

// 语言表加载入口（定义见下方“语言表来源”区）。来源调度、失败节流与
// 强制重测均交由框架 TableLoader（settings/table_loader.h）承担。
bool lang_table_ensure();

// 扫描“唯一为 true”的语言 bool 配置（启动层用）。仅 ARM64 抢跑线程调用，
// ARM32 下无使用者 → [[maybe_unused]] 抑制未使用告警。
[[maybe_unused]] bool read_config_locale(char *out, size_t outSize) {
    if (!lang_table_ensure()) {
        lzt_core::log_write("Language config scan: table unavailable");
        return false;
    }
    uintptr_t base = lzt_core::base();
    if (base == 0) {
        lzt_core::log_write("Language config scan: base unavailable");
        return false;
    }
    const char* keys[kMaxLangs];
    const int n = lang_config_keys(keys, kMaxLangs);
    for (int i = 0; i < n; ++i) {
        lzt_core::log_write("Language config scan: locale=%s key=%s value=%d",
                            g_langs[i].locale, keys[i],
                            lzt_settings::config::query_bool(keys[i], false) ? 1 : 0);
    }
    const int sel = lzt_settings::config::query_radio(keys, static_cast<size_t>(n));
    if (sel < 0) {
        lzt_core::log_write("Language config scan: no selected locale");
        return false;
    }
    snprintf(out, outSize, "%s", g_langs[sel].locale);
    lzt_core::log_write("Language config scan: selected=%s", out);
    return true;
}

// ============================================================
// 语言表来源（外置优先，内置兜底）
//   调度/节流/重测 = 框架 TableLoader；本区只负责"取字节 + 解析 + 落存储"。
//   来源顺序：CDN rton → CDN json → 数据包 packages\LANGUAGETYPES.rton
//   parse 回调返回 enabled 条目数（0 = 该来源不可用，继续下一个）。
// ============================================================

// 把解析结果落到业务存储（g_langs/g_langCount），返回 enabled 条目数。
size_t commit_lang_table(const lzt_language::ParseResult& result) {
    if (!result.ok || result.enabledCount == 0) return 0;

    int count = 0;
    bool hasEnUs = false;
    for (const auto& entry : result.entries) {
        if (!entry.enabled) continue;   // Enabled=false：不显示
        if (count >= kMaxLangs) break;
        if (strcasecmp(entry.lawnStringsType.c_str(), "en-us") == 0) hasEnUs = true;
        snprintf(g_langs[count].locale, sizeof(g_langs[count].locale), "%s",
                 entry.lawnStringsType.c_str());
        size_t n = 0;
        for (; entry.localizedName[n] && n < 63; ++n) {
            g_langs[count].label[n] =
                static_cast<wchar_t>(static_cast<unsigned char>(entry.localizedName[n]));
        }
        g_langs[count].label[n] = 0;
        ++count;
    }

    // 默认语言兜底：表内缺 en-us 时补首位（固定标签）
    if (!hasEnUs && count < kMaxLangs) {
        if (count > 0) {
            memmove(&g_langs[1], &g_langs[0], static_cast<size_t>(count) * sizeof(LangItem));
        }
        snprintf(g_langs[0].locale, sizeof(g_langs[0].locale), "en-us");
        size_t n = 0;
        for (; kLabelEn[n]; ++n) g_langs[0].label[n] = kLabelEn[n];
        g_langs[0].label[n] = 0;
        ++count;
    }

    g_langCount = count;
    g_langEnabledCount = static_cast<int>(result.enabledCount);
    for (int i = 0; i < count; ++i) {
        char label[64];
        lang_label_ascii(g_langs[i].label, label, sizeof(label));
        lzt_core::log_write("LangTable[%d]: locale=%s localizedName=%s enabled=1",
                            i, g_langs[i].locale, label);
    }
    return result.enabledCount;
}

// RTON（AsciiORM）形态解析：magic 非 RTON 时返回 0（该来源不可用）。
// 结果与 JSON 路径同构，Enabled 缺省视为 true。
size_t parse_lang_rton(const std::string& bytes) {
    if (bytes.size() < 4 || memcmp(bytes.data(), "RTON", 4) != 0) {
        lzt_core::log_write("LangTable: source bytes are not RTON (magic=%.4s), skip",
                            bytes.size() >= 4 ? bytes.data() : "----");
        return 0;
    }
    const lzt_language::ParseResult result =
        lzt_language::parse_language_types_rton(bytes, kMaxLangs);
    lzt_core::log_write("LangTable: rton parse ok=%d bytes=%zu langs=%zu",
                        result.ok ? 1 : 0, bytes.size(), result.entries.size());
    return commit_lang_table(result);
}

// JSON 文本形态解析（LANGUAGETYPES 子集）
size_t parse_lang_json(const std::string& bytes) {
    return commit_lang_table(lzt_language::parse_language_types(bytes, kMaxLangs));
}

// 数据包来源：按内容形态分流（RTON 优先，其次 JSON）
size_t parse_lang_package(const std::string& bytes) {
    if (bytes.size() >= 4 && memcmp(bytes.data(), "RTON", 4) == 0) {
        return parse_lang_rton(bytes);
    }
    if (lzt_resource::looks_like_language_types_json(bytes)) {
        return parse_lang_json(bytes);
    }
    lzt_core::log_write("LangTable: package bytes=%zu is neither RTON nor "
                        "LANGUAGETYPES json", bytes.size());
    return 0;
}

// ---- fetch 回调：各来源取原始字节 ----
bool fetch_cdn_rton(std::string& out) {
    return lzt_resource::read_bytes_from_cdn("LANGUAGETYPES.rton", out) ==
           lzt_resource::ReadStatus::Ok;
}
bool fetch_cdn_json(std::string& out) {
    return lzt_resource::read_text_from_cdn("LANGUAGETYPES.json", out) ==
           lzt_resource::ReadStatus::Ok;
}
bool fetch_package_rton(std::string& out) {
    return lzt_resource::read_bytes_from_package("packages\\LANGUAGETYPES.rton", out) ==
           lzt_resource::ReadStatus::Ok;
}

// 来源优先级表 + 加载器实例（外置文件优先，内置兜底；失败节流 3 秒）
//   "cdn-json" 受编译开关 kEnableFastReadChannel 控制：关闭（默认）时该
//   来源不入表——Debug/Release 均不读取 CDN LANGUAGETYPES.json。
[[maybe_unused]] const lzt_settings::TableSource kLangSourcesWithJson[] = {
    {"cdn-rton",     fetch_cdn_rton,     parse_lang_rton},
    {"cdn-json",     fetch_cdn_json,     parse_lang_json},
    {"package-rton", fetch_package_rton, parse_lang_package},
};
[[maybe_unused]] const lzt_settings::TableSource kLangSourcesNoJson[] = {
    {"cdn-rton",     fetch_cdn_rton,     parse_lang_rton},
    {"package-rton", fetch_package_rton, parse_lang_package},
};
// 加载器以“函数内静态”持有：全局对象与 __attribute__((constructor)) 同处
// .init_array，构造顺序不保证——若用全局对象，语言 constructor 抢先执行时
// 会读到尚未构造的空加载器（来源数=0，构造期加载静默失效）。函数内静态
// 在首次调用时才构造（C++11 起线程安全），彻底消除顺序依赖。
lzt_settings::TableLoader& lang_loader() {
    if constexpr (lzt_config::kEnableFastReadChannel) {
        static lzt_settings::TableLoader loader(kLangSourcesWithJson, 3);
        return loader;
    } else {
        static lzt_settings::TableLoader loader(kLangSourcesNoJson, 2);
        return loader;
    }
}

// 语言表加载入口：节流/重测由 TableLoader 承担；此处仅在首次成功后补打
// 一行摘要日志（保持既有 "LangTable: source=..." 观察格式）。
bool lang_table_ensure() {
    const bool ok = lang_loader().ensure();
    if (ok && !g_langSourceLogged) {
        g_langSourceLogged = true;
        lzt_core::log_write("LangTable: source=%s languages=%d enabled=%zu",
                            lang_loader().source_name(), g_langCount,
                            lang_loader().entry_count());
    }
    return ok;
}

// 阶段4迁移：老版本只写 LanguageFlag 文件。首次读取状态且尚无任何
// 语言 bool 为 true 时，把旧 flag 值迁移为对应 bool 配置。
// 必须在配置系统就绪后执行（read_state 首次调用 = UI 阶段），
// constructor 时机调用会因 persist manager 未就绪而静默丢失。
void migrate_legacy_flag_once() {
    static bool tried = false;
    if (tried || g_langCount == 0) return;
    tried = true;

    char flag[16];
    if (!read_flag_locale(flag, sizeof(flag)) ||
        strcasecmp(flag, "en-us") == 0) {
        return;
    }
    const char* keys[kMaxLangs];
    const int n = lang_config_keys(keys, kMaxLangs);
    if (lzt_settings::config::query_radio(keys, static_cast<size_t>(n)) >= 0) {
        return;   // 已有选中项，无需迁移
    }
    for (int i = 0; i < n; ++i) {
        if (strcasecmp(g_langs[i].locale, flag) == 0) {
            lzt_settings::config::save_bool(keys[i], true);
            lzt_core::log_write("LangTable: migrated legacy flag %s "
                                "to game config %s", flag, keys[i]);
            return;
        }
    }
    lzt_core::log_write("LangTable: legacy flag %s not in table, skip migrate",
                        flag);
}

const char *read_state() {
    if (g_loaded.load(std::memory_order_acquire)) return g_currentLocale;

    // 阶段4：优先读游戏 bool 配置（每语言一个 key；单选语义 =
    // 唯一为 true 的语言）。配置系统未就绪时 query 返回默认 false，
    // 自然落入下方标志文件回退——启动早期调用安全。
    if (lang_table_ensure()) {
        migrate_legacy_flag_once();
        const char* keys[kMaxLangs];
        const int n = lang_config_keys(keys, kMaxLangs);
        const int sel = lzt_settings::config::query_radio(keys, static_cast<size_t>(n));
        if (sel >= 0) {
            snprintf(g_currentLocale, sizeof(g_currentLocale), "%s", g_langs[sel].locale);
            g_loaded.store(true, std::memory_order_release);
            lzt_core::log_write("Language state (config): %s", g_currentLocale);
            return g_currentLocale;
        }
    }

    char buf[16];
    if (read_flag_locale(buf, sizeof(buf))) {
        migrate_legacy_locale(buf, sizeof(buf));
        snprintf(g_currentLocale, sizeof(g_currentLocale), "%s", buf);
    } else {
        snprintf(g_currentLocale, sizeof(g_currentLocale), "en-us");
    }
    g_loaded.store(true, std::memory_order_release);
    lzt_core::log_write("Language state (flag): %s", g_currentLocale);
    return g_currentLocale;
}

// 前置声明：write_state 引用热重载入口（定义在本文件后部；
// ARM64=三层通道实现，ARM32=空实现打日志）
void hot_reload(const char *locale);

void write_state(const char *locale) {
    const char *prev = read_state();
    char previousLocale[16] = {0};
    snprintf(previousLocale, sizeof(previousLocale), "%s", prev);
    if (strcmp(previousLocale, locale) == 0) {
        lzt_core::log_write("Language set_state: no change (%s)", locale);
        return;
    }
    // v7.37：单选组写入——框架内部先查旧项，只写"变动的两项"（与原逐项
    // save_bool 行为一致：旧项置 false、新项置 true）
    const char* keys[kMaxLangs];
    const int n = lang_config_keys(keys, kMaxLangs);
    int newIdx = -1;
    for (int i = 0; i < n; ++i) {
        if (strcasecmp(g_langs[i].locale, locale) == 0) { newIdx = i; break; }
    }
    char prevKey[40];
    lang_config_key(previousLocale, prevKey, sizeof(prevKey));
    lzt_settings::config::save_radio(keys, static_cast<size_t>(n), newIdx);
    lzt_core::log_write("Language config save: radio prev=%s(%d) next=%s(idx=%d)",
                        prevKey,
                        lzt_settings::config::query_bool(prevKey, false) ? 1 : 0,
                        locale, newIdx);
    if (newIdx >= 0) {
        lzt_core::log_write("Language config save: readback next=%d",
                            lzt_settings::config::query_bool(keys[newIdx], false) ? 1 : 0);
    }
    write_flag_locale(locale);
    snprintf(g_currentLocale, sizeof(g_currentLocale), "%s", locale);
    g_loaded.store(true, std::memory_order_release);
    lzt_core::log_write("Language state commit: previous=%s current=%s", previousLocale, locale);
    hot_reload(locale);
    // v7.19：统一本地化刷新（Localizer 门面）：设置页 Tab 行标题/根大标题/
    // 主界面 0x2D8 标签在同一接口下按宿主类型回填当前语言。
    int total = lzt_settings::refresh_all_localized();
    lzt_core::log_write("Language localized refresh: total=%d locale=%s", total, locale);
}


// （阶段6清理）旧的 discover_languages/locale_label 文件扫描式语言发现
// 已由 LANGUAGETYPES 动态表（lang_table_ensure）取代并移除。
// swprintf 宽串坑备忘：bionic 遵循 C99，宽串必须 %ls 不是 %s。

// ============================================================
// 语言选择交互与页面构建
// ============================================================

// ---- 动态 checkbox 回调（阶段3）：thunk 分发到带索引的处理器 ----
// CheckboxClickFn 是无上下文函数指针，模板实例化生成 16 个静态 thunk。
void on_lang_checkbox(int index, bool checked) {
    if (index < 0 || index >= g_langCount) return;
    const char *locale = g_langs[index].locale;
    char currentLocale[16] = {0};
    snprintf(currentLocale, sizeof(currentLocale), "%s", read_state());
    lzt_core::log_write("Language checkbox click: index=%d locale=%s checked=%d current=%s",
                        index, locale, checked ? 1 : 0, currentLocale);
    if (checked) {
        write_state(locale);
    } else if (strcasecmp(locale, kLocaleEn) != 0) {
        // 取消勾选当前语言 = 切回默认语言（en-us 取消则维持现状）
        const char *cur = read_state();
        if (strcasecmp(cur, locale) == 0) {
            lzt_core::log_write("Language checkbox: uncheck %s -> fallback en-us",
                                locale);
            write_state(kLocaleEn);
        }
    }
}

// v7.37：原先此处有 16 个静态 thunk（lang_thunk<I>）与 kLangThunks 分发表，
// 用于把"无上下文的 CheckboxClickFn"桥接到带索引处理器。该桥接已由框架的
// CheckboxIndexClickFn（CheckboxDesc.onClickIndexed）统一承担，故删除。

// ---- 页面内容构建（阶段3）：动态语言列表 ----
// v7.34：Tab 可见性——kEnableEmptyLangPageFallback 关闭(默认)时，无来源或
// enabled 语言数<=1 则不建语言 Tab；显式开启时始终创建（空白页兜底）。
bool lang_should_create() {
    if constexpr (lzt_config::kEnableEmptyLangPageFallback) return true;
    // 未就绪时强制重测：Tab 创建（进设置页）时 libPVZ2 base 已就绪，数据包
    // (packages\LANGUAGETYPES.rton) 兜底可读；早期 constructor 的失败节流
    // 会误判“无来源”→ 需绕过节流后真实评估。
    if (!lang_loader().ready()) lang_loader().retry();
    return lang_loader().ready() && g_langEnabledCount > 1;
}
// v7.22：读取不到 LANGUAGETYPES 时直接返回空白页（不再渲染固定
// EN/CN 双选项）。用户把 source 放好后，3 秒节流窗口结束、重建
// 页面即恢复动态列表；全程不 lock、不崩溃。
void build_page(lzt_settings::PageBuilder& b) {
    if (!lang_table_ensure()) {
        lzt_core::log_write("Language build_page: table unavailable, render empty");
        return;
    }
    const char *cur = read_state();
    for (int i = 0; i < g_langCount; ++i) {
        bool checked = strcasecmp(cur, g_langs[i].locale) == 0;
        char label[64];
        lang_label_ascii(g_langs[i].label, label, sizeof(label));
        char key[40];
        lang_config_key(g_langs[i].locale, key, sizeof(key));
        lzt_core::log_write("Language checkbox build: index=%d locale=%s key=%s "
                            "label=%s checked=%d", i, g_langs[i].locale, key,
                            label, checked ? 1 : 0);
        // v7.37：带序号回调——框架按序号分发，无需模块自建 thunk 表
        lzt_settings::CheckboxDesc desc{};
        desc.labelKey = g_langs[i].label;
        desc.initialState = checked;
        desc.onClickIndexed = &on_lang_checkbox;
        b.add_checkbox(desc);
    }
}

// ============================================================
// 热重载层（ARM64 实现；write_state 统一调用，ARM32 为空实现）
// ============================================================
#ifdef __aarch64__

constexpr uint32_t kLawnStringsTableId = 131;   // IDA 实证：vanilla 动态条目同表
constexpr const char *kCdnBase = "/sdcard/Android/data";
constexpr const char *kLangFilePrefix = "LawnStrings-";
constexpr const char *kLangFileSuffix = ".rton";
// v6.4：RSB 查找名前缀（IDA 实证 sub_E0A41C→sub_E0BBEC 拼接规则：
// 名字管理器 qword_26A0878 的 +0 字段 "packages" + '\' + 裸名 + ".rton"）。
// v6.0~v6.3 传裸名是 RSB 全 miss 的根因——树内存名全部带此前缀。
constexpr const char *kRsbPkgPrefix = "packages\\";

// LangCache 目录（files/LangCache，磁盘兜底通道用）
void get_langcache_dir(char *out, size_t outSize) {
    char files[256];
    lzt_core::files_dir(files, sizeof(files));
    snprintf(out, outSize, "%s/LangCache", files);
}

// 游戏 ABI / 表热重载通用工具（CDN apply 函数指针、libc++ SSO 字符串、
// apply 日志上下文、locale→FourCC）已下沉至 settings/game_abi.h（v7.37）；
// 本文件只保留业务侧调用（lzt_settings::game::*）。

// LangCache 内该语言的文件路径
bool langcache_path(const char *locale, char *out, size_t outSize) {
    char dir[512];
    get_langcache_dir(dir, sizeof(dir));
    snprintf(out, outSize, "%s/%s%s%s", dir, kLangFilePrefix, locale, kLangFileSuffix);
    struct stat st;
    return stat(out, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

// CDN 目录查找该语言文件（No_backup/CDN.*.* 枚举）。
// v6.0 修复：Android 11+ 禁止枚举 /sdcard/Android/data 顶层（MuMu 实证），
// 改为读包名直构 No_backup 路径（与 resource_file.cpp 同款修复）。
bool cdn_path(const char *locale, char *out, size_t outSize) {
    char fname[64];
    snprintf(fname, sizeof(fname), "%s%s%s", kLangFilePrefix, locale, kLangFileSuffix);

    const char* pkg = lzt_core::package_name();
    if (pkg == nullptr || pkg[0] == '\0') return false;

    char noBackup[512];
    snprintf(noBackup, sizeof(noBackup), "%s/%s/files/No_backup", kCdnBase, pkg);
    DIR *nb = opendir(noBackup);
    if (!nb) return false;
    struct dirent *cdn;
    while ((cdn = readdir(nb)) != nullptr) {
        if (strncmp(cdn->d_name, "CDN.", 4) != 0) continue;
        snprintf(out, outSize, "%s/%s/%s", noBackup, cdn->d_name, fname);
        struct stat st;
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            closedir(nb);
            return true;
        }
    }
    closedir(nb);
    return false;
}

// 表变更广播（复刻 sub_D58544 广播段）已下沉至 settings/game_abi.*（v7.37）。

[[maybe_unused]] void log_table_diag(uintptr_t db, const char *phase, const char *locale) {
    if (!lzt_core::memory_range_accessible(db + 48, sizeof(uintptr_t), false)) {
        lzt_core::log_write("Lang table diag: phase=%s locale=%s result=db-unreadable",
                            phase, locale);
        return;
    }
    uintptr_t tableArray = *reinterpret_cast<volatile uintptr_t *>(db + 48);
    if (tableArray == 0) {
        lzt_core::log_write("Lang table diag: phase=%s locale=%s result=array-null",
                            phase, locale);
        return;
    }
    uintptr_t desc = tableArray + static_cast<uintptr_t>(kLawnStringsTableId) * 128;
    if (!lzt_core::memory_range_accessible(desc, 128, false)) {
        lzt_core::log_write("Lang table diag: phase=%s locale=%s array=%p desc=%p result=unreadable",
                            phase, locale, reinterpret_cast<void *>(tableArray),
                            reinterpret_cast<void *>(desc));
        return;
    }
    uint8_t raw[16] = {0};
    memcpy(raw, reinterpret_cast<const void *>(desc), sizeof(raw));
    uintptr_t runtimeArray = 0;
    uintptr_t runtimeEntry = 0;
    uint8_t runtimeRaw[32] = {0};
    uint32_t loadedFlag = 0;
    if (lzt_core::memory_range_accessible(db + 72, sizeof(uintptr_t), false)) {
        runtimeArray = *reinterpret_cast<volatile uintptr_t *>(db + 72);
        if (runtimeArray != 0) {
            runtimeEntry = runtimeArray + static_cast<uintptr_t>(kLawnStringsTableId) * 32;
            if (lzt_core::memory_range_accessible(runtimeEntry, sizeof(runtimeRaw), false)) {
                memcpy(runtimeRaw, reinterpret_cast<const void *>(runtimeEntry),
                       sizeof(runtimeRaw));
                loadedFlag = runtimeRaw[24];
            }
        }
    }
    lzt_core::log_write("Lang table diag: phase=%s locale=%s tableId=%u array=%p desc=%p "
                        "raw16=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X "
                        "runtimeEntry=%p loadedFlag=%u runtimeRaw32="
                        "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X"
                        "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                        phase, locale, kLawnStringsTableId,
                        reinterpret_cast<void *>(tableArray), reinterpret_cast<void *>(desc),
                        raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                        raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15],
                        reinterpret_cast<void *>(runtimeEntry), loadedFlag,
                        runtimeRaw[0], runtimeRaw[1], runtimeRaw[2], runtimeRaw[3],
                        runtimeRaw[4], runtimeRaw[5], runtimeRaw[6], runtimeRaw[7],
                        runtimeRaw[8], runtimeRaw[9], runtimeRaw[10], runtimeRaw[11],
                        runtimeRaw[12], runtimeRaw[13], runtimeRaw[14], runtimeRaw[15],
                        runtimeRaw[16], runtimeRaw[17], runtimeRaw[18], runtimeRaw[19],
                        runtimeRaw[20], runtimeRaw[21], runtimeRaw[22], runtimeRaw[23],
                        runtimeRaw[24], runtimeRaw[25], runtimeRaw[26], runtimeRaw[27],
                        runtimeRaw[28], runtimeRaw[29], runtimeRaw[30], runtimeRaw[31]);
}

// ============================================================
// v7.0 只读探针：定位热切换后"文本内容已是新语言却不可见"的层。
// 背景（v6.9 真机日志 + IDA 实证）：
//   - table131 加载、官方 apply（sub_13D0F30）、[KEY] 容器更新、
//     dirty 重建全部成功；进关卡后设置页文本全部变为新语言。
//   - [KEY] 查询链 sub_14F3354→sub_14F30C8 直接哈希查
//     qword_26C5ED8+1872 容器（miss 返回 "<key>"），无中间缓存；
//     因此重建时 widget 拿到的文本内容必为新语言。
//   - 剩余嫌疑 = 字体渲染层（glyph 缓存/字体配置），与 iOS 补丁文档
//     "字符缓存清理"(ClearPrimeGlyphCache) 指向一致。
// 本组探针全程只读、不写任何游戏内存：
//   probe1 resolve：apply 后经官方 sub_14F3354 解析语言键，输出
//         UTF-16 码元 hex（容器新鲜度的铁证）。
//   probe2 font：当前字体配置 qword_26A1D50 的指针、字体名(+32)与
//         缓存上下文(+104)——对比不同启动语言下的差异。
//   probe3 measure：用同一文本上下文测量英文/中文控件字符串宽高；
//         中文为 0/异常而英文正常 = 当前字体缺 CJK 能力的直接证据。
// ============================================================

// 游戏 24 字节 wstring（与 settings_widgets 内部 GameString 同构；
// 本文件独立定义以避免跨层依赖内部头）
struct ProbeGameWStr {
    uint64_t flag;    // +0：短串 = size<<1（bit0=0）；长串 bit0=1
    uint64_t size;    // +8：长串长度
    uint64_t heap;    // +16：长串数据指针
};

// 读取游戏 wstring 的 C 视图（校验可访问性；失败返回 false）。
// 布局（sub_15407DC/sub_14F30C8 双向实证）：短串 len 在【首字节】<<1、
// 数据在 +4（最多 5 个 wchar）；长串 {cap|1, len, ptr}。
[[maybe_unused]] bool probe_view_game_wstr(const void *obj, const wchar_t **data, size_t *len) {
    auto *s = static_cast<const ProbeGameWStr *>(obj);
    if ((s->flag & 1) == 0) {
        size_t n = static_cast<size_t>(*reinterpret_cast<const uint8_t *>(obj)) >> 1;
        const wchar_t *p = reinterpret_cast<const wchar_t *>(
            reinterpret_cast<uintptr_t>(obj) + 4);
        if (n > 64 || !lzt_core::memory_range_accessible(
                           reinterpret_cast<uintptr_t>(p), n * sizeof(wchar_t), false)) {
            return false;
        }
        *data = p;
        *len = n;
        return true;
    }
    const wchar_t *p = reinterpret_cast<const wchar_t *>(s->heap);
    if (s->size > 256 || !lzt_core::memory_range_accessible(
                             reinterpret_cast<uintptr_t>(p), s->size * sizeof(wchar_t), false)) {
        return false;
    }
    *data = p;
    *len = static_cast<size_t>(s->size);
    return true;
}

[[maybe_unused]] void probe_free_game_wstr(const ProbeGameWStr &s) {
    if (s.flag & 1) operator delete(reinterpret_cast<void *>(s.heap));
}

// probe1：官方 LocalizeKey（sub_14F3354，sret 返回 24 字节 wstring）
using ProbeLocalizeKeyFn = ProbeGameWStr (*)(const wchar_t *key);

[[maybe_unused]] void probe_log_resolved(uintptr_t base, const wchar_t *key) {
    char ascii[40];
    size_t n = 0;
    for (; key[n] && n + 1 < sizeof(ascii); ++n) {
        wchar_t ch = key[n];
        ascii[n] = (ch >= 0x20 && ch <= 0x7E) ? static_cast<char>(ch) : '?';
    }
    ascii[n] = 0;

    auto localizeKey = reinterpret_cast<ProbeLocalizeKeyFn>(base + OFF_LocalizeKey);
    ProbeGameWStr s = localizeKey(key);
    const wchar_t *data = nullptr;
    size_t len = 0;
    if (!probe_view_game_wstr(&s, &data, &len)) {
        lzt_core::log_write("Lang resolve probe: key=%s result=unreadable", ascii);
        probe_free_game_wstr(s);
        return;
    }
    char units[64] = {0};
    size_t used = 0;
    size_t dumpN = len < 8 ? len : 8;
    for (size_t i = 0; i < dumpN; ++i) {
        used += static_cast<size_t>(snprintf(units + used, sizeof(units) - used,
                                             "%04X", static_cast<unsigned>(data[i])));
        if (used + 1 >= sizeof(units)) break;
    }
    lzt_core::log_write("Lang resolve probe: key=%s len=%zu units=%s", ascii, len, units);
    probe_free_game_wstr(s);
}

// probe2：字体配置状态（对象指针 = qword_26A1D50；字段偏移 IDA 实证）
[[maybe_unused]] void probe_log_font_state(uintptr_t base, const char *phase) {
    uintptr_t fontConfig = *reinterpret_cast<volatile uintptr_t *>(base + OFF_FontContext);
    if (fontConfig == 0 ||
        !lzt_core::memory_range_accessible(fontConfig + FONTCONFIG_FONT_NAME,
                                           sizeof(ProbeGameWStr), false)) {
        lzt_core::log_write("Lang font probe: phase=%s result=fontconfig-unavailable", phase);
        return;
    }
    uintptr_t cachedCtx = 0;
    if (lzt_core::memory_range_accessible(fontConfig + FONTCONFIG_CACHED_CTX,
                                          sizeof(uintptr_t), false)) {
        cachedCtx = *reinterpret_cast<volatile uintptr_t *>(
            fontConfig + FONTCONFIG_CACHED_CTX);
    }
    const wchar_t *nameData = nullptr;
    size_t nameLen = 0;
    char name[80] = "(unreadable)";
    if (probe_view_game_wstr(reinterpret_cast<const void *>(
                                 fontConfig + FONTCONFIG_FONT_NAME),
                             &nameData, &nameLen)) {
        size_t n = 0;
        for (; nameData[n] && n + 1 < sizeof(name) && n < nameLen; ++n) {
            wchar_t ch = nameData[n];
            name[n] = (ch >= 0x20 && ch <= 0x7E) ? static_cast<char>(ch) : '?';
        }
        name[n] = 0;
    }
    lzt_core::log_write("Lang font probe: phase=%s fontConfig=%p cachedCtx=%p name=%s",
                        phase, reinterpret_cast<void *>(fontConfig),
                        reinterpret_cast<void *>(cachedCtx), name);
}

// probe3：字体能力测量。v7.3.1 审计修复③：原实现只测 [LANGUAGE_*] 键
// （回退后解析为 <Missing> ASCII 标记，失去检验意义），现增加【真实中文
// 短串】（≤5 wchar 短串布局栈上构造：首字节=len<<1、数据在 +4）。在
// 注册表重建前后各测一次：重建前 CJK 度量异常、重建后正常 = 字体方案
// 在真机上的直接证据。全部只读。
[[maybe_unused]] void probe_log_measure(uintptr_t base, const char *phase) {
    uintptr_t fontConfig = *reinterpret_cast<volatile uintptr_t *>(base + OFF_FontContext);
    if (fontConfig == 0) {
        lzt_core::log_write("Lang measure probe: phase=%s result=no-fontconfig", phase);
        return;
    }
    using FontLoadFn = uintptr_t (*)(uintptr_t);
    auto fontLoad = reinterpret_cast<FontLoadFn>(base + OFF_FontLoad);
    uintptr_t ctx = fontLoad(fontConfig);
    using TextMeasureFn = uintptr_t (*)(uintptr_t, uintptr_t, uint32_t *, uint32_t *, float);
    auto textMeasure = reinterpret_cast<TextMeasureFn>(base + OFF_TextMeasure);

    // 真实中文短串（不依赖容器键值）
    alignas(16) uint8_t zhBuf[24] = {0};
    const wchar_t *zhText = L"测试中文";
    zhBuf[0] = static_cast<uint8_t>(4 << 1);
    memcpy(zhBuf + 4, zhText, 4 * sizeof(wchar_t));
    uint32_t zhW = 0, zhH = 0;
    textMeasure(ctx, reinterpret_cast<uintptr_t>(zhBuf), &zhW, &zhH, 400.0f);
    lzt_core::log_write("Lang measure probe: phase=%s tag=cjk w=%u h=%u",
                        phase, zhW, zhH);

    // 容器键测量对照（现状为 <Missing> 标记，仅作容器状态参照）
    auto localizeKey = reinterpret_cast<ProbeLocalizeKeyFn>(base + OFF_LocalizeKey);
    const wchar_t *keys[2] = {L"[LANGUAGE_ENGLISH]", L"[LANGUAGE_SIMPLIFIED_CHINESE]"};
    const char *tags[2] = {"en", "zh"};
    for (int i = 0; i < 2; ++i) {
        ProbeGameWStr s = localizeKey(keys[i]);
        uint32_t w = 0, h = 0;
        textMeasure(ctx, reinterpret_cast<uintptr_t>(&s), &w, &h, 400.0f);
        lzt_core::log_write("Lang measure probe: phase=%s tag=%s w=%u h=%u",
                            phase, tags[i], w, h);
        probe_free_game_wstr(s);
    }
}

// ============================================================
// RSB 只读诊断（Debug 版，v6.3 调查期产物）——定位 LoadPackage 返回 0
// 的失败层：先输出通道状态（开关/挂载数/哈希计数），再对 4 种名字形式
// 逐组 + 全局树实查，命中语言变体复合条目时 dump 子条目语言字段。
// 纯只读访问，无副作用；Release 版由 LM_DBG_ONLY 门短路，不执行。
// ============================================================
[[maybe_unused]] void debug_trace_rsb(uintptr_t base, const char *locale) {
    // (1.5) RSB 通道诊断（v6.2 新增，v6.3 修正：开关按 QWORD 指针读）。
    //     sub_16AD8D0 判定实际是 *(rsm+24) != 0（QWORD），v6.2 按
    //     uint32 读只打出低半（0x8B607C60 一类非开关值）。本次连同
    //     on/off 结论一起输出，并保留 pakMgr/rsm/挂载数/哈希计数。
    uintptr_t diagRsm = 0;
    {
        uintptr_t pakMgr = *reinterpret_cast<volatile uintptr_t *>(base + OFF_PAK_MANAGER);
        if (pakMgr == 0) {
            lzt_core::log_write("Lang RSB diag: pakMgr=NULL (RSB unavailable)");
        } else {
            uintptr_t rsm = *reinterpret_cast<volatile uintptr_t *>(pakMgr + PAKMGR_RES_STREAMS_MGR);
            if (rsm == 0) {
                lzt_core::log_write("Lang RSB diag: rsm=NULL (RSB unavailable)");
            } else {
                diagRsm = rsm;   // 供 (1.6) 复用
                uintptr_t swPtr = *reinterpret_cast<volatile uintptr_t *>(rsm + RSM_RSB_SWITCH);
                uint32_t mounts = *reinterpret_cast<volatile uint32_t *>(rsm + RSM_MOUNT_COUNT);
                uintptr_t mountArr =
                    *reinterpret_cast<volatile uintptr_t *>(rsm + RSM_MOUNT_ARRAY);
                uintptr_t hashRoot =
                    *reinterpret_cast<volatile uintptr_t *>(rsm + RSM_GLOBAL_HASH_TREE);
                uint32_t hashCount = hashRoot
                    ? *reinterpret_cast<volatile uint32_t *>(hashRoot + HASH_TREE_ENTRY_COUNT)
                    : 0;
                lzt_core::log_write("Lang RSB diag: pakMgr=%p rsm=%p switchPtr=%p (%s) "
                                    "mounts=%u mountArr=%p hashRoot=%p hashCount=%u",
                                    reinterpret_cast<void *>(pakMgr), reinterpret_cast<void *>(rsm),
                                    reinterpret_cast<void *>(swPtr), swPtr != 0 ? "on" : "off",
                                    mounts, reinterpret_cast<void *>(mountArr),
                                    reinterpret_cast<void *>(hashRoot), hashCount);
            }
        }
    }

    // (1.6) RSB 树实查诊断（v6.3 新增，纯只读内存访问，无副作用）。
    //     直接调用游戏自己的基数树查找 sub_17AFEDC（GetGroupForFile
    //     同款：树根内嵌于挂载组+32 / rsm+48，传结构地址本身；逐字符
    //     toupper 大小写不敏感），对两种名字形式全量扫描：
    //       A. type 直方图（判定 mounts 真实性与 type==3 组存量）
    //       B. 挂载表逐组树查找 → 命中输出 组号/type/node(flag,off,size)
    //       C. 全局树查找 → 命中输出 node[0]；带 0x10000000 位时为
    //          语言变体复合条目，dump 全部子条目 (group,lang1,lang2)
    //       D. LangMgr main/sub 当前值（语言匹配对照）
    //     判读：
    //       - B/C 全 miss → RSB 索引无此名字（名字形式或通道错误）
    //       - C 命中且子条目 lang1/lang2 与 main/sub 不匹配 → 语言
    //         匹配问题（对照即可看出该写哪个字段/值）
    if (diagRsm != 0) {
        uintptr_t mountArr =
            *reinterpret_cast<volatile uintptr_t *>(diagRsm + RSM_MOUNT_ARRAY);
        if (mountArr == 0) {
            lzt_core::log_write("Lang RSB scan: mountArr=NULL, skip scan");
        } else {
            using RadixFindFn = uintptr_t (*)(uintptr_t, const char *);
            auto radixFind = reinterpret_cast<RadixFindFn>(base + OFF_RADIX_FIND);

            char forms[4][48];
            snprintf(forms[0], sizeof(forms[0]), "%s%s%s%s",
                     kRsbPkgPrefix, kLangFilePrefix, locale, kLangFileSuffix);
            snprintf(forms[1], sizeof(forms[1]), "packages/%s%s%s",
                     kLangFilePrefix, locale, kLangFileSuffix);
            snprintf(forms[2], sizeof(forms[2]), "%s%s%s",
                     kLangFilePrefix, locale, kLangFileSuffix);
            snprintf(forms[3], sizeof(forms[3]), "%s%s", kLangFilePrefix, locale);
            lzt_core::log_write("Lang RSB scan: locale=%s probe \"%s\" / \"%s\" / \"%s\" / \"%s\"",
                                locale, forms[0], forms[1], forms[2], forms[3]);

            uint32_t mounts =
                *reinterpret_cast<volatile uint32_t *>(diagRsm + RSM_MOUNT_COUNT);
            uint32_t limit = mounts > 4096 ? 4096 : mounts;   // 防御性截断

            // A. type 直方图
            uint32_t hist[8] = {0};
            for (uint32_t i = 0; i < limit; ++i) {
                uint32_t type = *reinterpret_cast<volatile uint32_t *>(
                    mountArr + GROUP_ENTRY_STRIDE * i + GROUP_MOUNT_TYPE);
                if (type < 8) ++hist[type];
            }
            lzt_core::log_write("Lang RSB scan: mounts=%u (scan %u) typeHist "
                                "t0=%u t1=%u t2=%u t3=%u t4+=%u",
                                mounts, limit, hist[0], hist[1], hist[2], hist[3],
                                limit - hist[0] - hist[1] - hist[2] - hist[3]);

            // B. 逐组树查找（不限 type，命中全记录——比游戏路径 1 的
            //    type==3 过滤信息量更大）
            int groupHits = 0;
                for (uint32_t i = 0; i < limit; ++i) {
                    uintptr_t grp = mountArr + GROUP_ENTRY_STRIDE * i;
                    for (int f = 0; f < 4; ++f) {
                    uintptr_t node = radixFind(grp + GROUP_NAME_TREE, forms[f]);
                    if (node == 0) continue;
                    uint32_t type = *reinterpret_cast<volatile uint32_t *>(
                        grp + GROUP_MOUNT_TYPE);
                    uint32_t n0 = *reinterpret_cast<volatile uint32_t *>(node);
                    uint32_t n1 = *reinterpret_cast<volatile uint32_t *>(node + 4);
                    uint32_t n2 = *reinterpret_cast<volatile uint32_t *>(node + 8);
                    lzt_core::log_write("Lang RSB scan: group[%u] type=%u HIT \"%s\" "
                                        "node(flag=0x%08X off=%u size=%u)",
                                        i, type, forms[f], n0, n1, n2);
                    ++groupHits;
                }
            }
            if (groupHits == 0) {
                lzt_core::log_write("Lang RSB scan: no group-tree hit for any name form");
            }

            // C. 全局树查找 + 语言变体复合条目 dump
                int globalHits = 0;
                for (int f = 0; f < 4; ++f) {
                uintptr_t node = radixFind(diagRsm + RSM_GLOBAL_HASH_TREE, forms[f]);
                if (node == 0) continue;
                ++globalHits;
                uint32_t v0 = *reinterpret_cast<volatile uint32_t *>(node);
                lzt_core::log_write("Lang RSB scan: global tree HIT \"%s\" node0=0x%08X",
                                    forms[f], v0);
                if ((v0 & RSB_VARIANT_FLAG) == 0) continue;
                // 语言变体复合条目：dump 子条目 (group, lang1, lang2)
                uintptr_t rsb =
                    *reinterpret_cast<volatile uintptr_t *>(diagRsm + RSM_RSB_MAIN);
                if (rsb == 0) {
                    lzt_core::log_write("Lang RSB scan: rsb main obj NULL, skip variant dump");
                    continue;
                }
                uint32_t tableOff = *reinterpret_cast<volatile uint32_t *>(
                    rsb + RSB_MAIN_TABLE_OFF);
                uint32_t recSize = *reinterpret_cast<volatile uint32_t *>(
                    rsb + RSB_MAIN_REC_SIZE);
                uintptr_t rec = rsb + tableOff +
                    static_cast<uint64_t>(recSize) * (v0 & 0x0FFFFFFFu);
                uint32_t count = *reinterpret_cast<volatile uint32_t *>(
                    rec + RSB_VARIANT_COUNT_OFF);
                uint32_t dumpN = count > 16 ? 16 : count;   // 日志量截断
                lzt_core::log_write("Lang RSB scan: variant entries=%u (dump %u) rec=%p",
                                    count, dumpN, reinterpret_cast<void *>(rec));
                for (uint32_t j = 0; j < dumpN; ++j) {
                    uintptr_t e = rec + RSB_VARIANT_FIRST_OFF + RSB_VARIANT_STRIDE * j;
                    uint32_t grpIdx = *reinterpret_cast<volatile uint32_t *>(e);
                    uint32_t lang1 = *reinterpret_cast<volatile uint32_t *>(e + 4);
                    uint32_t lang2 = *reinterpret_cast<volatile uint32_t *>(e + 8);
                    lzt_core::log_write("Lang RSB scan:   [%u] group=%u lang1=0x%08X "
                                        "lang2=0x%08X", j, grpIdx, lang1, lang2);
                }
            }
            if (globalHits == 0) {
                lzt_core::log_write("Lang RSB scan: no global-tree hit for any name form");
            }

            // D. 语言字段当前值（匹配判定对照；main 恒 0x600、sub 为
            //    刚写入的目标 FourCC——若子条目 lang1 非零且不等于
            //    0x600，即语言匹配失败的直接证据）
            uintptr_t gDisp2 =
                *reinterpret_cast<volatile uintptr_t *>(base + OFF_G_DisplayInfo);
            if (gDisp2 != 0) {
                uintptr_t mgr2 = *reinterpret_cast<volatile uintptr_t *>(
                    gDisp2 + DISPLAYINFO_LANGMGR);
                if (mgr2 != 0) {
                    lzt_core::log_write("Lang RSB scan: lang fields main=0x%08X sub=0x%08X",
                        *reinterpret_cast<volatile uint32_t *>(
                            mgr2 + LANGMGR_MAIN_LOCALE_ID),
                        *reinterpret_cast<volatile uint32_t *>(
                            mgr2 + LANGMGR_LOCALE_ID));
                }
            }
        }
    }
}

// ============================================================
// v7.2 调查方向（v7.1 回退后确立）：
//   原版页面标题等文本经 sub_14F324C 本地化（≠ 容器查询 sub_14F3354），
//   即存在第二个字符串源（用户所称"字符串缓存"）。v6.9 官方 apply 只
//   更新 qword_26C5ED8+1872 容器（sub_14F3354 路径），未更新该源——
//   这解释了"原版页面文本切换后不变、进出关卡后才变新语言"（关卡
//   过渡触发游戏自身对该源的重建）。下一步：定位该源及其官方重建入口。
// ============================================================

// 前置写 LangMgr 次语言字段(+0x1E4)。
// GetGroupForFile(sub_16B2DEC) 的语言匹配为双字段（条目 lang1 vs +0x1E0、
// lang2 vs +0x1E4），但真机实测铁证：游戏正常以 zh-cn 运行时 +0x1E0 恒为
// 0x600（=1536，非 FourCC）——说明 LawnStrings 条目 lang1==0，匹配只需
// +0x1E4；而 +0x1E0 在别处被游戏读取，v6.1 写 FourCC 污染它 = 进游戏崩溃
// 的根因。故此处只写 +0x1E4，主字段仅记录不写。
static void write_langmgr_locale(uintptr_t base, const char *locale) {
    uintptr_t gDisp = *reinterpret_cast<volatile uintptr_t *>(base + OFF_G_DisplayInfo);
    if (gDisp == 0) return;
    uintptr_t mgr = *reinterpret_cast<volatile uintptr_t *>(gDisp + DISPLAYINFO_LANGMGR);
    if (mgr == 0) return;
    const uint32_t want = lzt_settings::game::locale_to_fourcc(locale);
    volatile uint32_t *mainField =
        reinterpret_cast<volatile uint32_t *>(mgr + LANGMGR_MAIN_LOCALE_ID);
    volatile uint32_t *subField =
        reinterpret_cast<volatile uint32_t *>(mgr + LANGMGR_LOCALE_ID);
    lzt_core::log_write("Lang hot_reload: main field (RO) = 0x%08X", *mainField);
    if (*subField != want) {
        lzt_core::log_write("Lang hot_reload: sub locale 0x%08X -> 0x%08X (%s)",
                            *subField, want, locale);
        *subField = want;
    }
}

// 通道 0（v7.31 外置优先）：探测 LangCache / CDN 磁盘表（游戏本来的
// “外置 CDN 优先”语义），命中即走原版 CDN apply 链并返回 true（调用方收尾）。
// 修复背景：此前先 LoadPackage(RSB) 会命中内置表（LANGUAGE_* 语言名键为英文
// 正文），使运行切换时 checkbox 名等不随切换更新；改外置优先后与重启一致。
static bool try_external_apply(uintptr_t base, const char *locale) {
    char extPath[640];
    if (!langcache_path(locale, extPath, sizeof(extPath)) &&
        !cdn_path(locale, extPath, sizeof(extPath))) {
        return false;
    }
    char extFname[64];
    snprintf(extFname, sizeof(extFname), "%s%s%s", kLangFilePrefix, locale, kLangFileSuffix);
    lzt_settings::game::SsoString sPathE;
    sPathE.init(extPath);
    lzt_settings::game::SsoString sNameE;
    sNameE.init(extFname);
    alignas(16) lzt_settings::game::FakeLogCtx ctxE{};
    lzt_core::set_stage(425);
    reinterpret_cast<lzt_settings::game::CdnApplyFn>(base + OFF_CDN_APPLY)(
        reinterpret_cast<uintptr_t>(&ctxE), sPathE.bytes, sNameE.bytes, kLawnStringsTableId);
    const char *resE = lzt_settings::game::log_text(ctxE);
    if (strstr(resE, "Apply") == nullptr) {
        lzt_core::log_write("Lang hot_reload: external apply rejected (%s), fallback RSB", resE);
        return false;
    }
    lzt_core::log_write("Lang hot_reload: %s applied from external (CDN-first) (%s)", locale, resE);
    lzt_core::set_stage(430);
    lzt_core::log_write("Lang hot_reload: end locale=%s result=external-disk", locale);
    lzt_core::set_stage(0);
    return true;
}

// ---- 主入口（v6.0 重做）：任意 locale 热切换，三层通道 ----
// 执行上下文：checkbox 回调（主线程），与原版 CDN Apply 一致。
void hot_reload(const char *locale) {
    lzt_core::set_stage(400);
    lzt_core::log_write("Lang hot_reload: begin locale=%s table=%u", locale,
                        kLawnStringsTableId);
    uintptr_t base = lzt_core::base();
    if (base == 0) {
        lzt_core::log_write("Lang hot_reload: end locale=%s result=base-unavailable", locale);
        lzt_core::set_stage(0);
        return;
    }
    uintptr_t db = *reinterpret_cast<volatile uintptr_t *>(base + OFF_PVZDB_SINGLETON);
    if (db == 0 || db == static_cast<uintptr_t>(-1)) {
        lzt_core::log_write("Lang hot_reload: end locale=%s result=db-unavailable", locale);
        lzt_core::set_stage(0);
        return;
    }
    // 只读探针（Debug 版）：切换前字体状态 / 原版 UI 键 / CJK 度量基线
    LZT_DBG_ONLY(
        probe_log_font_state(base, "begin");
        probe_log_resolved(base, L"[SETTINGS_DATASHARING_TITLE]");
        probe_log_measure(base, "begin");
    );

    // (1) LangMgr 次语言字段前置写（必须先于加载：v5.0 在加载后才写，
    //     匹配用的是旧值）。主字段只读不写，原因见函数注释。
    lzt_core::set_stage(410);
    write_langmgr_locale(base, locale);

    // (1.5)/(1.6) RSB 只读诊断（Debug 版）：状态 + 树实查，定位 LoadPackage 返回 0 的失败层
    LZT_DBG_ONLY(debug_trace_rsb(base, locale));
    // (2) 通道 1：RSB 直载（v6.4：名字加 "packages\" 前缀——根因修复）。
    //     IDA 实证游戏真实链 sub_120AC88→sub_E0A41C→sub_E0BBEC：
    //     名字 = mgr+0 前缀 "packages" + '\' + 裸名 + ".rton"，即
    //     "packages\LawnStrings-<locale>.rton"。v6.0~v6.3 传裸名，
    //     RSB 树内名字全带前缀 → 永远 miss（v6.3 树扫描 1648 组
    //     零命中即铁证）。依次 4 种形式尝试：
    //       0: packages\LawnStrings-<locale>.rton（游戏真实形式，主）
    //       1: packages/LawnStrings-<locale>.rton（GetGroupForFile 内部
    //          会把 '/' 归一为 '\'，等价备选）
    //       2: LawnStrings-<locale>.rton（旧裸名，对照/兜底）
    //       3: LawnStrings-<locale>（旧裸名，最后兜底）
    //     注意：前缀名 31 字符 > 22，SsoString 走长串模式（堆分配，
    //     布局与 libc++/游戏一致，sub_120AEFC 长短串分支均已核对）。
    lzt_core::set_stage(420);
    // (2) 通道 0（v7.31 外置优先）：LangCache/CDN 磁盘表命中 → 原版 CDN apply 链
    if (try_external_apply(base, locale)) return;
    LZT_DBG_ONLY(log_table_diag(db, "before-load", locale));
    auto loadFn = reinterpret_cast<lzt_settings::game::DbLoadFn>(base + OFF_DB_LOAD_RTON);
    char name[64];
    lzt_settings::game::SsoString sName;
    bool loaded = false;
    for (int form = 0; form < 4 && !loaded; ++form) {
        switch (form) {
            case 0:
                snprintf(name, sizeof(name), "%s%s%s%s",
                         kRsbPkgPrefix, kLangFilePrefix, locale, kLangFileSuffix);
                break;
            case 1:
                snprintf(name, sizeof(name), "packages/%s%s%s",
                         kLangFilePrefix, locale, kLangFileSuffix);
                break;
            case 2:
                snprintf(name, sizeof(name), "%s%s%s",
                         kLangFilePrefix, locale, kLangFileSuffix);
                break;
            default:
                snprintf(name, sizeof(name), "%s%s", kLangFilePrefix, locale);
                break;
        }
        sName.init(name);
        int r = loadFn(db, kLawnStringsTableId, sName.bytes);
        lzt_core::log_write("Lang hot_reload: LoadPackage(%s) -> %d", name, r);
        loaded = (r != 0);
    }
    LZT_DBG_ONLY(log_table_diag(db, "after-load", locale));

    if (loaded) {
        lzt_core::set_stage(425);
        lzt_core::log_write("Lang strings apply: begin locale=%s table=%u",
                            locale, kLawnStringsTableId);
        using ApplyLocalizedStringsFn = uintptr_t (*)();
        uintptr_t applyResult = reinterpret_cast<ApplyLocalizedStringsFn>(
            base + OFF_APPLY_LOCALIZED_STRINGS)();
        lzt_core::log_write("Lang strings apply: end locale=%s result=0x%lx",
                            locale, static_cast<unsigned long>(applyResult));
        LZT_DBG_ONLY(log_table_diag(db, "after-strings-apply", locale));

        // v7.0/v7.2 只读探针：官方 apply 完成后立即取证（容器新鲜度/字体
        // 配置/CJK 度量）。v7.4：已移除字体注册表重建（真机证实字体样式
        // 解析不随语言变化，调用为空操作）。
        lzt_core::set_stage(427);
        // 只读探针（Debug 版）：apply 后字体状态 / 原版 UI 键 / CJK 度量取证
        LZT_DBG_ONLY(
            probe_log_font_state(base, "after-apply");
            probe_log_resolved(base, L"[SETTINGS_DATASHARING_TITLE]");
            probe_log_resolved(base, L"[LANGUAGE_ENGLISH]");
            probe_log_resolved(base, L"[LANGUAGE_SIMPLIFIED_CHINESE]");
            probe_log_resolved(base, L"[LANGUAGE_ANGLE_TITLE]");
            probe_log_resolved(base, L"[SETTINGS_TITLE]");   // 根视图标题键
            probe_log_measure(base, "after-rebuild");
        );

        // (3) 通道 2：D58544 完整链（校验→重载→游戏日志→广播）。
        //     重载一次幂等（sub_120AEFC 自带旧表清理）；若校验通过，
        //     行为与原版 CDN Apply 逐字节一致。
        alignas(16) lzt_settings::game::FakeLogCtx ctx{};
        auto applyFn = reinterpret_cast<lzt_settings::game::CdnApplyFn>(base + OFF_CDN_APPLY);
        lzt_core::set_stage(430);
        applyFn(reinterpret_cast<uintptr_t>(&ctx), sName.bytes, sName.bytes,
                kLawnStringsTableId);
        const char *result = lzt_settings::game::log_text(ctx);
        if (strstr(result, "Apply") != nullptr) {
            lzt_core::log_write("Lang hot_reload: %s applied via CDN chain (%s)",
                                locale, result);
            lzt_core::set_stage(440);
            lzt_core::log_write("Lang hot_reload: end locale=%s result=cdn-chain", locale);
            lzt_core::set_stage(0);
            return;
        }
        // 校验拒绝了 RSB 名字（Skipped 分支无广播）——表已由通道 1
        // 加载，手动补广播完成 UI 刷新
        lzt_core::log_write("Lang hot_reload: D58544 skipped RSB name (%s), "
                            "table already loaded, manual broadcast", result);
        lzt_settings::game::broadcast_table_changed(base, kLawnStringsTableId);
        lzt_core::log_write("Lang hot_reload: %s applied via direct load", locale);
        lzt_core::log_write("Lang hot_reload: end locale=%s result=direct-load", locale);
        lzt_core::set_stage(0);
        return;
    }

    // v7.31：外置(LangCache/CDN)已在 RSB 之前优先尝试并被拒/不存在；此处 RSB 也未加载
    // → 无可用语言源。RSB 内置兜底仍由上方 LoadPackage 通道负责（外置不存在时）。
    lzt_core::log_write("Lang hot_reload: end locale=%s result=no-source", locale);
    lzt_core::set_stage(0);
    lzt_core::log_write("Lang hot_reload: no source for %s "
                        "(external rejected/absent, RSB miss)", locale);
}

#else

void hot_reload(const char *locale) {
    lzt_core::log_write("Lang hot_reload: ARM32 not supported (restart applies)");
}

#endif // __aarch64__

} // namespace

// ============================================================
// 启动层：locale 直写线程（constructor 抢跑，5ms 轮询）
// 目标值 = 标志文件的 locale FourCC（任意语言通用）
// ============================================================

#ifdef __aarch64__

static int lang_dl_phdr_callback(struct dl_phdr_info *info, size_t, void *data) {
    auto *result = reinterpret_cast<uintptr_t *>(data);
    if (info->dlpi_name && strstr(info->dlpi_name, "libPVZ2.so")) {
        *result = info->dlpi_addr;
        return 1;
    }
    return 0;
}

static uintptr_t lang_get_lib_base() {
    uintptr_t base = 0;
    dl_iterate_phdr(lang_dl_phdr_callback, &base);
    return base;
}

static void start_locale_switch_thread() {
    std::thread([]() {
        auto sleep5 = [] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); };

        int wait_ms = 0;
        while (lang_get_lib_base() == 0) {
            sleep5();
            wait_ms += 5;
            if (wait_ms > 30000) {
                lzt_core::log_write("Language switch: timeout waiting libPVZ2.so");
                return;
            }
        }
        uintptr_t base = lang_get_lib_base();

        char locale[16] = {0};
        bool flagOk = read_flag_locale(locale, sizeof(locale));
        if (flagOk) {
            migrate_legacy_locale(locale, sizeof(locale));
            lzt_core::log_write("Language startup: source=flag locale=%s", locale);
        } else {
            lzt_core::log_write("Language startup: flag missing or invalid");
            int configWaitMs = 0;
            bool configReady = false;
            while (configWaitMs < 30000) {
                uintptr_t coreBase = lzt_core::base();
                if (coreBase != 0) {
                    uintptr_t displayInfo = *reinterpret_cast<volatile uintptr_t *>(
                        coreBase + OFF_G_DisplayInfo);
                    uintptr_t persistManager = *reinterpret_cast<volatile uintptr_t *>(
                        coreBase + OFF_PersistManager);
                    if (displayInfo != 0 && persistManager != 0) {
                        configReady = true;
                        break;
                    }
                }
                sleep5();
                configWaitMs += 5;
            }
            lzt_core::log_write("Language startup: config ready=%d waitMs=%d",
                                configReady ? 1 : 0, configWaitMs);
            if (configReady && read_config_locale(locale, sizeof(locale))) {
                lzt_core::log_write("Language startup: source=game-config locale=%s", locale);
                write_flag_locale(locale);
                lzt_core::log_write("Language startup: rebuilt flag locale=%s", locale);
            } else {
                snprintf(locale, sizeof(locale), "en-us");
                lzt_core::log_write("Language startup: source=default locale=%s", locale);
            }
        }
        uint32_t target = lzt_settings::game::locale_to_fourcc(locale);
        lzt_core::log_write("Language switch: target=0x%08X locale=%s", target, locale);

        for (int i = 0; i < 12000; ++i) {   // 最长 60 秒
            uintptr_t gDisp = *reinterpret_cast<volatile uintptr_t *>(
                base + OFF_G_DisplayInfo);
            if (gDisp != 0) {
                uintptr_t mgr = *reinterpret_cast<volatile uintptr_t *>(
                    gDisp + DISPLAYINFO_LANGMGR);
                if (mgr != 0) {
                    volatile uint32_t *field = reinterpret_cast<volatile uint32_t *>(
                        mgr + LANGMGR_LOCALE_ID);
                    uint32_t cur = *field;
                    if (cur == target) {
                        lzt_core::log_write("Language switch: already 0x%08X, done", cur);
                        return;
                    }
                    // 已知 vanilla 六语初值或 0（构造未完成）均可覆盖；
                    // 其他未知值保守轮询
                    bool knownVanilla =
                        cur == LANG_ID_EN_US || cur == 0 ||
                        cur == lzt_settings::game::locale_to_fourcc("de-de") ||
                        cur == lzt_settings::game::locale_to_fourcc("fr-fr") ||
                        cur == lzt_settings::game::locale_to_fourcc("it-it") ||
                        cur == lzt_settings::game::locale_to_fourcc("es-es") ||
                        cur == lzt_settings::game::locale_to_fourcc("pt-pt");
                    if (knownVanilla) {
                        if (cur != 0) {
                            *field = target;
                            lzt_core::log_write("Language switch: wrote 0x%08X -> 0x%08X (%s)",
                                                cur, target, locale);
                            return;
                        }
                    } else {
                        lzt_core::log_write(
                            "Language switch: unexpected field 0x%08X, polling", cur);
                    }
                }
            }
            sleep5();
        }
        lzt_core::log_write("Language switch: timeout (60s)");
    }).detach();
}

#else

static void start_locale_switch_thread() {
    lzt_core::log_write("Language switch: arch not supported yet (ARM64 only)");
}

#endif // __aarch64__

// ---- 模块级 constructor：与 Settings UI 注册链路解耦，确保抢跑 ----
// CDN 诊断在此立即执行（纯文件 IO）：so 一加载日志即有记录，
// 可区分“so 未加载”与“libPVZ2.so hook 未生效”两类故障。
__attribute__((constructor)) void language_locale_bootstrap() {
    lzt_core::log_write("Language module bootstrap: build=%s mode=%s",
                        "LANGUAGE-V2.1.0-MODULE-TIDY", lzt_config::kBuildMode);
    lang_table_ensure();
    start_locale_switch_thread();
}

bool language_module_init() {
    lzt_settings::ModuleDesc desc{};
    desc.titleKey = kTitleKey;
    desc.iconNormalOffset   = OFF_SettingsBuildVersionIconNormal;   // 复用 Build Version 图标
    desc.iconSelectedOffset = OFF_SettingsBuildVersionIconSelected;
    desc.anchorTabId = SETTINGS_BUILDVERSION_ID;   // Build Version 之后
    desc.slotAfterAnchor = 1;                      // 第一位（view_angle 第二）
    desc.requestedTabId = 0;                       // 由引擎自动分配
    desc.onBuildPage = build_page;
    desc.shouldCreate = lang_should_create;   // v7.34：无来源/单一语言时不建 Tab

    if (!lzt_settings::register_module(desc)) {
        lzt_core::log_write("Language module register failed");
        return false;
    }
    lzt_core::log_write("Language module registered");
    return true;
}
