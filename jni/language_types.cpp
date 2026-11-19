// ============================================================
// language_types.cpp — LANGUAGETYPES.json 严格最小解析器实现
// 手写递归下降扫描器，无第三方依赖；任何结构性错误立即整体失败。
// ============================================================

#include "language_types.h"

#include <cctype>

namespace lzt_language {

namespace {

// ---- 扫描游标 ----
struct Cursor {
    const char* p;
    const char* end;
};

void skip_ws(Cursor& c) {
    while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\r' ||
                           *c.p == '\n')) {
        ++c.p;
    }
}

bool consume(Cursor& c, char ch) {
    skip_ws(c);
    if (c.p < c.end && *c.p == ch) {
        ++c.p;
        return true;
    }
    return false;
}

// JSON 字符串（含引号）→ 内容。失败返回 false。
bool parse_string(Cursor& c, std::string& out) {
    skip_ws(c);
    if (c.p >= c.end || *c.p != '"') return false;
    ++c.p;
    out.clear();
    while (c.p < c.end) {
        char ch = *c.p++;
        if (ch == '"') return true;
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        // 转义序列
        if (c.p >= c.end) return false;
        char esc = *c.p++;
        switch (esc) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                if (c.end - c.p < 4) return false;
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = *c.p++;
                    code <<= 4;
                    if (h >= '0' && h <= '9')      code |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                // 键名/语言名为 ASCII；非 ASCII 码点以 '?' 降级（不影响
                // locale 合法性判断——locale 校验本就拒绝非 ASCII）
                out.push_back(code < 0x80 ? static_cast<char>(code) : '?');
                break;
            }
            default:
                return false;   // 非法转义
        }
    }
    return false;   // 未闭合
}

// 跳过任意 JSON 值（数字/布尔/null/字符串/嵌套对象数组）
bool skip_value(Cursor& c) {
    skip_ws(c);
    if (c.p >= c.end) return false;
    char ch = *c.p;
    if (ch == '"') {
        std::string sink;
        return parse_string(c, sink);
    }
    if (ch == '{' || ch == '[') {
        // 括号深度扫描（字符串内的括号不计）
        int depth = 0;
        while (c.p < c.end) {
            char cur = *c.p;
            if (cur == '"') {
                std::string sink;
                if (!parse_string(c, sink)) return false;
                continue;
            }
            ++c.p;
            if (cur == '{' || cur == '[') {
                ++depth;
            } else if (cur == '}' || cur == ']') {
                if (--depth == 0) return true;
            }
        }
        return false;   // 未闭合
    }
    // 字面量：true / false / null / 数字
    const char* start = c.p;
    while (c.p < c.end && *c.p != ',' && *c.p != '}' && *c.p != ']' &&
           *c.p != ' ' && *c.p != '\t' && *c.p != '\r' && *c.p != '\n') {
        ++c.p;
    }
    return c.p > start;
}

// ---- LanguageType 条目字段收集 ----
struct RawEntry {
    std::string objclass;
    std::string localizedName;
    std::string lawnStringsType;
    bool hasLocalizedName = false;
    bool hasLawnStringsType = false;
    bool enabled = true;
    bool hasEnabled = false;
};

// 解析一个对象 {...} 的字段（游标已位于 '{' 之后调用方消费处）。
// 进入时 consume('{') 已由调用方完成；返回 false = 结构错误。
bool parse_object_fields(Cursor& c, RawEntry& out) {
    skip_ws(c);
    if (c.p < c.end && *c.p == '}') {   // 空对象
        ++c.p;
        return true;
    }
    for (;;) {
        std::string key;
        if (!parse_string(c, key)) return false;
        if (!consume(c, ':')) return false;

        if (key == "objclass") {
            if (!parse_string(c, out.objclass)) return false;
        } else if (key == "objdata") {
            // PvZ2 对象包装格式：LocalizedName/LawnStringsType/Enabled
            // 都在 objdata 嵌套对象内（v6.2 修复：此前漏了这层，
            // 字段全被 skip_value 跳过导致 enabled=0）
            if (!consume(c, '{')) return false;
            if (!parse_object_fields(c, out)) return false;
        } else if (key == "LocalizedName") {
            if (!parse_string(c, out.localizedName)) return false;
            out.hasLocalizedName = true;
        } else if (key == "LawnStringsType") {
            if (!parse_string(c, out.lawnStringsType)) return false;
            out.hasLawnStringsType = true;
        } else if (key == "Enabled") {
            skip_ws(c);
            if (c.end - c.p >= 4 && c.p[0] == 't' && c.p[1] == 'r' &&
                c.p[2] == 'u' && c.p[3] == 'e') {
                out.enabled = true;
                out.hasEnabled = true;
                c.p += 4;
            } else if (c.end - c.p >= 5 && c.p[0] == 'f' && c.p[1] == 'a' &&
                       c.p[2] == 'l' && c.p[3] == 's' && c.p[4] == 'e') {
                out.enabled = false;
                out.hasEnabled = true;
                c.p += 5;
            } else {
                return false;   // Enabled 必须是布尔
            }
        } else {
            // TypeName / version / 未知字段：跳过
            if (!skip_value(c)) return false;
        }

        skip_ws(c);
        if (c.p < c.end && *c.p == ',') {
            ++c.p;
            continue;
        }
        if (c.p < c.end && *c.p == '}') {
            ++c.p;
            return true;
        }
        return false;
    }
}

} // namespace

std::string normalize_locale(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        if (ch == '_') ch = '-';
        else if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        out.push_back(ch);
    }
    return out;
}

bool is_valid_locale(const std::string& locale) {
    if (locale.size() < 2 || locale.size() > 15) return false;
    for (char ch : locale) {
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        if (!ok) return false;
    }
    return true;
}

ParseResult parse_language_types(const std::string& text, std::size_t maxEntries) {
    ParseResult result;
    Cursor c{text.data(), text.data() + text.size()};

    // 顶层对象
    if (!consume(c, '{')) return result;

    skip_ws(c);
    if (c.p < c.end && *c.p == '}') {   // 空对象：合法但无条目
        ++c.p;
        result.ok = true;
        return result;
    }

    bool sawObjects = false;
    for (;;) {
        std::string key;
        if (!parse_string(c, key)) return result;
        if (!consume(c, ':')) return result;

        if (key == "objects") {
            sawObjects = true;
            // 数组
            if (!consume(c, '[')) return result;
            skip_ws(c);
            if (c.p < c.end && *c.p == ']') {
                ++c.p;
            } else {
                for (;;) {
                    skip_ws(c);
                    if (c.p >= c.end || *c.p != '{') return result;
                    ++c.p;   // 消费 '{'（parse_object_fields 期望已进入）
                    RawEntry raw;
                    if (!parse_object_fields(c, raw)) return result;

                    // 只收 LanguageType；字段齐全且 locale 合法才入表
                    if (raw.objclass == "LanguageType" && raw.hasLocalizedName &&
                        raw.hasLawnStringsType) {
                        std::string locale = normalize_locale(raw.lawnStringsType);
                        if (is_valid_locale(locale)) {
                            LanguageEntry entry;
                            entry.localizedName = raw.localizedName;
                            entry.lawnStringsType = locale;
                            entry.enabled = raw.enabled;   // 缺省 true
                            // 归一化去重：后出现者替换前者（位置不变）
                            bool duplicate = false;
                            for (auto& existing : result.entries) {
                                if (existing.lawnStringsType == locale) {
                                    existing = entry;
                                    duplicate = true;
                                    break;
                                }
                            }
                            if (!duplicate && result.entries.size() < maxEntries) {
                                result.entries.push_back(entry);
                            } else if (!duplicate) {
                                // 超出上限：丢弃（截断语义）
                            }
                        }
                        // locale 非法：条目拒绝，继续解析后续条目
                    }

                    skip_ws(c);
                    if (c.p < c.end && *c.p == ',') {
                        ++c.p;
                        continue;
                    }
                    if (c.p < c.end && *c.p == ']') {
                        ++c.p;
                        break;
                    }
                    return result;
                }
            }
        } else {
            // version / #comment / 未知顶层键：跳过值
            if (!skip_value(c)) return result;
        }

        skip_ws(c);
        if (c.p < c.end && *c.p == ',') {
            ++c.p;
            continue;
        }
        if (c.p < c.end && *c.p == '}') {
            ++c.p;
            // 顶层对象结束：必须存在过 objects 键
            if (!sawObjects) return result;
            result.ok = true;
            for (const auto& e : result.entries) {
                if (e.enabled) ++result.enabledCount;
            }
            return result;
        }
        return result;
    }
}

} // namespace lzt_language
