#ifndef LANGUAGE_TYPES_H
#define LANGUAGE_TYPES_H

// ============================================================
// LANGUAGETYPES.json 严格最小解析器（阶段2 GREEN）
//
// 契约见 tests/language_types_test.cpp：
//   - 仅支持游戏实际用到的 JSON 子集：对象/数组/字符串/布尔/null/数字
//   - 只提取 objclass=="LanguageType" 条目的 objdata 三字段：
//     LocalizedName / LawnStringsType / Enabled
//   - LawnStringsType 归一化：小写 + '_'→'-'（"EN_US"→"en-us"）
//   - locale 校验：2~15 字符，仅 [a-z0-9-]（自动排除 "../" 类路径注入）
//   - 归一化后重复的 locale：后出现者替换前者（位置保持首次出现处）
//   - LocalizedName 缺失或 locale 非法：该条目被拒绝（不影响整体 ok）
//   - Enabled 缺省视为 true
//   - 结构性错误（括号不闭合/非法 token）：整体 ok=false
//   - 条目数超过 maxEntries：截断
// ============================================================

#include <cstddef>
#include <string>
#include <vector>

namespace lzt_language {

struct LanguageEntry {
    std::string localizedName;    // 原样保留（形如 "[LANGUAGE_ENGLISH]" 的游戏文本键）
    std::string lawnStringsType;  // 归一化 locale（如 "en-us"）
    bool enabled = true;          // LANGUAGETYPES Enabled 字段（缺省 true）
};

struct ParseResult {
    bool ok = false;              // 结构合法性（false 时 entries 为空）
    std::vector<LanguageEntry> entries;
    std::size_t enabledCount = 0; // enabled==true 的合法条目数
};

ParseResult parse_language_types(const std::string& text, std::size_t maxEntries);

// locale 合法性校验（归一化后调用）：2~15 字符，仅 [a-z0-9-]
bool is_valid_locale(const std::string& locale);

// 归一化：ASCII 小写 + '_'→'-'；非 ASCII 字符原样保留（随后由
// is_valid_locale 拒绝）
std::string normalize_locale(const std::string& raw);

} // namespace lzt_language

#endif // LANGUAGE_TYPES_H
