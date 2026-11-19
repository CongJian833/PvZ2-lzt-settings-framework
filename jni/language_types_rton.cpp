// ============================================================
// language_types_rton.cpp — RTON(AsciiORM 二进制) 解码实现
// 见 language_types_rton.h 的类型码说明。
// ============================================================

#include "language_types_rton.h"

#include "lzt_core.h"

#include <cstdint>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

namespace lzt_language {

namespace {

// ---- RTON 解析最小 DOM ----
struct Node {
    enum Kind : uint8_t { STR, INT, BOOL, MAP, LIST } kind = STR;
    std::string s;        // STR
    bool b = false;       // BOOL
    std::vector<std::pair<std::string, Node>> m;  // MAP
    std::vector<Node> l;  // LIST
};

struct Cursor {
    const uint8_t* d = nullptr;
    size_t sz = 0;
    size_t i = 0;
    bool fail = false;
    std::vector<std::string> dict;   // 字符串表（首现序号 0 基）
};

inline uint8_t peek(const Cursor& c) {
    return c.i < c.sz ? c.d[c.i] : 0xFF;
}

// 读 varint（LEB128 小端）
static uint64_t read_varint(Cursor& c) {
    uint64_t v = 0;
    int sh = 0;
    while (c.i < c.sz) {
        const uint8_t b = c.d[c.i++];
        v |= static_cast<uint64_t>(b & 0x7F) << sh;
        sh += 7;
        if (!(b & 0x80)) break;
        if (sh > 63) break;
    }
    return v;
}

// 读字符串。实测 RTON 有三种字符串编码，需全部支持：
//   0x81 <len:varint> <bytes>  —— 官方导出工具的形态（LANGUETYPES.rton 实测），
//                                不做字符串表引用（该样本中无 0x91/0x93）。
//   0x90 <len:varint> <bytes>  —— 新短串（并入字符串表）
//   0x91 / 0x93 <idx:varint>   —— 字符串表引用
std::string read_str(Cursor& c) {
    if (c.fail || c.i >= c.sz) { c.fail = true; return {}; }
    uint8_t t = c.d[c.i++];
    if (t == 0x81 || t == 0x90) {
        const uint64_t len = read_varint(c);
        // 注意：长度 0 是合法的（空字符串，实测 LawnStrings 中存在 90 00），不能判失败。
        if (c.fail || c.i + len > c.sz) { c.fail = true; return {}; }
        std::string s(reinterpret_cast<const char*>(c.d + c.i), static_cast<size_t>(len));
        c.i += static_cast<size_t>(len);
        c.dict.push_back(s);
        return s;
    }
    if (t == 0x91 || t == 0x93) {
        // 字符串表引用（索引为 varint）。实测 LawnStrings 两种都用，
        // 且索引会超过 127（0x93 be 01 即索引 190），故必须按 varint 读。
        const uint64_t idx = read_varint(c);
        if (c.fail || idx >= c.dict.size()) { c.fail = true; return {}; }
        return c.dict[static_cast<size_t>(idx)];
    }
    c.fail = true;
    return {};
}

// 值节点（含标量容器）。在 map 的"值"位置调用。
Node parse_node(Cursor& c) {
    Node n;
    if (c.fail || c.i >= c.sz) { c.fail = true; return n; }
    uint8_t t = c.d[c.i];
    if (t == 0x92) {
        // 0x92 <字符数:1B> <字节数:1B> <UTF-8 数据>
        // 实测（LawnStrings 的文本值）：92 04 0c e8 b4 a6 ... 即 4 字符 / 12 字节
        // 「账户删除」。与 0x90 的区别是带 UTF-8 长度信息；该类型【不入字符串表】
        // （0x91 的索引只指向 0x90 登记的串）。
        c.i++;
        if (c.i + 2 > c.sz) { c.fail = true; return n; }
        const uint64_t nchars = read_varint(c);   // 字符数（此处不需要）
        const uint64_t nbytes = read_varint(c);   // 字节数
        (void)nchars;
        if (nbytes > 0 && c.i + nbytes > c.sz) { c.fail = true; return n; }
        n.kind = Node::STR;
        n.s.assign(reinterpret_cast<const char*>(c.d + c.i), static_cast<size_t>(nbytes));
        c.i += static_cast<size_t>(nbytes);
        return n;
    }
    if (t == 0x81 || t == 0x90 || t == 0x91 || t == 0x93) {
        n.kind = Node::STR;
        n.s = read_str(c);
        return n;
    }
    if (t == 0x24 || t == 0x08) {              // int: 0x24/0x08 <varint>（0x08 见 681B 实测 version 字段）
        c.i++;
        uint64_t v = 0; int sh = 0;
        while (c.i < c.sz) {
            uint8_t b = c.d[c.i++];
            v |= static_cast<uint64_t>(b & 0x7F) << sh;
            sh += 7;
            if (!(b & 0x80)) break;
        }
        (void)v;   // INT 值对 LANGUAGETYPES 提取无需要，仅 consume
        n.kind = Node::INT;
        return n;
    }
    if (t == 0x00 || t == 0x01) {             // 裸 bool（值上下文）
        n.kind = Node::BOOL;
        n.b = (t == 0x01);
        c.i++;
        return n;
    }
    if (t == 0x85) {                          // Map
        c.i++;
        n.kind = Node::MAP;
        while (c.i < c.sz && peek(c) != 0xFF) {
            std::string k = read_str(c);
            if (c.fail) break;
            Node v = parse_node(c);
            if (c.fail) break;
            n.m.emplace_back(std::move(k), std::move(v));
        }
        if (c.i < c.sz && peek(c) == 0xFF) c.i++;   // 消费 FF
        return n;
    }
    if (t == 0x86) {                          // List: 0x86 [0xFD] <count:varint> ... 0xFE
        // 必须按 count 解析：嵌套结构里每一层都有自己的 0xFE 结束标记，
        // 若用「读到 0xFE 为止」会把内层的结束误判成外层结束（实测：objects 数组
        // 只有 1 个元素，却被读成 2 个，导致后续取值路径全部错位）。
        // count 为 varint（实测：aliases=1 编码为 86 fd 01；LocStringValues 有
        // 约 24400 项，编码为 86 fd d0 be 01）。
        c.i++;
        n.kind = Node::LIST;
        if (c.i < c.sz && c.d[c.i] == 0xFD) c.i++;      // 变长计数标记
        const uint64_t count = read_varint(c);
        for (uint64_t k = 0; k < count && !c.fail && c.i < c.sz; ++k) {
            n.l.push_back(parse_node(c));
        }
        if (!c.fail && c.i < c.sz && peek(c) == 0xFE) c.i++;   // 消费本层 FE
        return n;
    }
    // 未知类型：输出失败点上下文（便于定位 RTON 的新类型码）
    {
        size_t pos = (c.i > 0) ? c.i - 1 : 0;
        const size_t from = (pos > 16) ? pos - 16 : 0;
        const size_t to = (pos + 16 < c.sz) ? pos + 16 : c.sz;
        char hex[3 * 40 + 1];
        size_t k = 0;
        for (size_t j = from; j < to && k + 3 < sizeof(hex); ++j) {
            k += static_cast<size_t>(snprintf(hex + k, sizeof(hex) - k, "%02x ", c.d[j]));
        }
        hex[k] = 0;
        lzt_core::log_write("RTON unknown type=0x%02x at %zu ctx[%zu..%zu]: %s",
                            t, pos, from, to, hex);
    }
    c.fail = true;
    return n;
}

// 仅用于 map 值上下文，字符串可能来自 0x90/0x91——但执行到 map 值已由
// parse_node 处理；此函数保留占位（未用）。
bool rton_enabled_bool(const Node& v) {
    return v.kind == Node::BOOL ? v.b : false;
}

// 解析根节点。实测存在两种形态：
//   · 显式 map：首字节 0x85（部分样本）
//   · 隐式 map：顶层直接以「键」开始（官方导出工具的 LANGUETYPES.rton、
//     LawnStrings.rton 均属此形态）
// 统一按「键值对序列直到 0xFF」解析，两种形态都兼容。
static Node parse_root(Cursor& c) {
    if (peek(c) == 0x85) return parse_node(c);
    Node n;
    n.kind = Node::MAP;
    while (c.i < c.sz && !c.fail && peek(c) != 0xFF) {
        std::string k = read_str(c);
        if (c.fail) break;
        Node v = parse_node(c);
        if (c.fail) break;
        n.m.emplace_back(std::move(k), std::move(v));
    }
    if (!c.fail && c.i < c.sz && peek(c) == 0xFF) c.i++;
    return n;
}

} // namespace

ParseResult parse_language_types_rton(const std::string& bytes, std::size_t maxEntries) {
    ParseResult out{};
    if (bytes.size() < 8) return out;                       // 需 magic + 版本
    if (memcmp(bytes.data(), "RTON", 4) != 0) return out;

    Cursor c;
    c.d = reinterpret_cast<const uint8_t*>(bytes.data());
    c.sz = bytes.size();
    c.i = 8;                                                // 跳过 RTON + version(4B)

    // 顶层结构统一交给 parse_root 处理（支持显式 0x85 map 与隐式键值流两种形态）。
    // 早期这里有一份只认 0x90/0x91 作键 token 的独立循环，导致官方导出工具生成的
    // LANGUETYPES.rton（字符串用 0x81 编码）解析失败 —— 实测该文件开头即 0x81。
    Node root = parse_root(c);
    if (c.fail || root.kind != Node::MAP) {
        lzt_core::log_write("[LZ] RTON: 顶层解析失败 (at %zu/%zu)", c.i, c.sz);
        return out;
    }

    // 遍历顶层键：找 "objects"（List of obj Map）
    const Node* objects = nullptr;
    for (const auto& kv : root.m) {
        if (kv.first == "objects" && kv.second.kind == Node::LIST) {
            objects = &kv.second;
            break;
        }
    }
    if (objects == nullptr) return out;

    bool hasEnUs = false;
    for (const Node& obj : objects->l) {
        if (obj.kind != Node::MAP) continue;
        const std::string* objclass = nullptr;
        const Node* objdata = nullptr;
        for (const auto& kv : obj.m) {
            if (kv.first == "objclass" && kv.second.kind == Node::STR)
                objclass = &kv.second.s;
            else if (kv.first == "objdata" && kv.second.kind == Node::MAP)
                objdata = &kv.second;
        }
        if (objclass == nullptr || *objclass != "LanguageType") continue;
        if (objdata == nullptr) continue;

        LanguageEntry entry;
        bool enabled = false;
        std::string localizedName, lawnStringsType;
        for (const auto& kv : objdata->m) {
            if (kv.first == "LocalizedName" && kv.second.kind == Node::STR)
                localizedName = kv.second.s;
            else if (kv.first == "LawnStringsType" && kv.second.kind == Node::STR)
                lawnStringsType = kv.second.s;
            else if (kv.first == "Enabled")
                enabled = rton_enabled_bool(kv.second);
        }
        if (lawnStringsType.empty()) continue;
        if (!enabled) continue;                              // Enabled 缺省视情况：RTON 一般显式
        if (out.entries.size() >= maxEntries) break;

        entry.localizedName = localizedName.length() ? localizedName : lawnStringsType;
        entry.lawnStringsType = normalize_locale(lawnStringsType);
        entry.enabled = true;
        if (!is_valid_locale(entry.lawnStringsType)) continue;
        if (strcasecmp(entry.lawnStringsType.c_str(), "en-us") == 0) hasEnUs = true;
        out.entries.push_back(std::move(entry));
    }

    // en-us 缺失补首位（与 JSON 路径一致）
    if (!hasEnUs && out.entries.size() < maxEntries) {
        if (!out.entries.empty())
            out.entries.insert(out.entries.begin(), LanguageEntry{});
        LanguageEntry first{};
        first.localizedName = "[LANGUAGE_ENGLISH]";
        first.lawnStringsType = "en-us";
        first.enabled = true;
        out.entries[0] = std::move(first);
    }

    out.ok = true;
    for (const auto& e : out.entries)
        if (e.enabled) ++out.enabledCount;
    lzt_core::log_write("[LZ] RTON parse: entries=%zu enabled=%zu bytes=%zu",
                        out.entries.size(), out.enabledCount, bytes.size());
    return out;
}

// ============================================================
// LawnStrings 表解析（供「切换语言后 UI 文本立即更新」使用）
//
// 复用上面的 RTON DOM：解析整表后按路径取值
//   objects[0].objdata.LocStringValues[]
// 该数组为【键、文本交替】的一维列表，两两配对即可。
// 文本为 UTF-8，这里转成 wchar_t（UTF-32）以便直接交给游戏控件。
// ============================================================

// UTF-8 → UTF-32（容错：非法字节按单字节处理，不中断解析）
static std::wstring utf8_to_wstring(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp = c;
        size_t extra = 0;
        if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
        if (i + extra >= s.size()) extra = 0;   // 截断则按单字节处理
        for (size_t k = 0; k < extra; ++k) {
            const uint8_t cc = static_cast<uint8_t>(s[i + 1 + k]);
            if ((cc & 0xC0) != 0x80) { extra = 0; cp = c; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        out.push_back(static_cast<wchar_t>(cp));
        i += 1 + extra;
    }
    return out;
}

// 在 MAP 中按键取值（未命中返回 nullptr）
static const Node* map_get(const Node& m, const char* key) {
    if (m.kind != Node::MAP) return nullptr;
    for (const auto& kv : m.m) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

// （parse_root 定义见上，供两种表格式共用）

std::size_t parse_lawnstrings_rton(const std::string& bytes,
                                   std::unordered_map<std::wstring, std::wstring>& out) {
    out.clear();
    if (bytes.size() < 8 || memcmp(bytes.data(), "RTON", 4) != 0) return 0;

    Cursor c;
    c.d = reinterpret_cast<const uint8_t*>(bytes.data());
    c.sz = bytes.size();
    c.i = 8;   // 跳过 "RTON" + 版本(4B)

    Node root = parse_root(c);
    if (c.fail || root.kind != Node::MAP) {
        lzt_core::log_write("LawnStrings: RTON 解析失败 (at %zu/%zu)", c.i, c.sz);
        return 0;
    }

    // objects[0].objdata.LocStringValues
    const Node* objects = map_get(root, "objects");
    if (objects == nullptr || objects->kind != Node::LIST || objects->l.empty()) {
        lzt_core::log_write("LawnStrings: 结构不符（无 objects 数组）");
        return 0;
    }
    const Node* objdata = map_get(objects->l[0], "objdata");
    if (objdata == nullptr) {
        lzt_core::log_write("LawnStrings: 结构不符（无 objdata）");
        return 0;
    }
    const Node* values = map_get(*objdata, "LocStringValues");
    if (values == nullptr || values->kind != Node::LIST) {
        lzt_core::log_write("LawnStrings: 结构不符（无 LocStringValues）");
        return 0;
    }

    const auto& list = values->l;
    std::size_t n = 0;
    for (std::size_t i = 0; i + 1 < list.size(); i += 2) {
        if (list[i].kind != Node::STR || list[i + 1].kind != Node::STR) continue;
        if (list[i].s.empty()) continue;
        // 键统一补上方括号：表中形如 "ACCOUNT_DELETION"，而游戏内部一律用
        // "[ACCOUNT_DELETION]" 形式（控件文本键即此格式），补上后可直接用于查询。
        std::wstring k = L"[" + utf8_to_wstring(list[i].s) + L"]";
        out[k] = utf8_to_wstring(list[i + 1].s);
        ++n;
    }
    lzt_core::log_write("LawnStrings: parsed %zu entries", n);
    return n;
}

// ============================================================
// 明文 JSON 版 LawnStrings 解析
//
// 为什么需要：CDN 目录里放 .json 比 .rton 方便（不需 RTON 编码器），
// 而游戏数据包内自带的那份完整表也是明文 JSON 形态。结构与 RTON 版同构：
//   { "objects": [ { "objdata": { "LocStringValues": [ "KEY","文本", ... ] } } ] }
// 这里只做「定位 LocStringValues 数组 → 逐个解析 JSON 字符串 → 两两配对」，
// 不引入通用 JSON 解析器（表内只有字符串数组，无需完整语法支持）。
// ============================================================
namespace {

bool json_skip_ws(const std::string& s, size_t& i) {
    while (i < s.size()) {
        const char ch = s[i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == ',') { ++i; continue; }
        return true;
    }
    return false;
}

// 解析一个 JSON 字符串（i 指向开引号；成功后 i 指向闭引号之后）
bool json_parse_string(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        const char ch = s[i++];
        if (ch == '"') return true;
        if (ch != '\\') { out.push_back(ch); continue; }
        if (i >= s.size()) return false;
        const char e = s[i++];
        switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case '/': out.push_back('/'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case 'u': {
                if (i + 4 > s.size()) return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = s[i + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                i += 4;
                if (cp < 0x80) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: out.push_back(e); break;
        }
    }
    return false;
}

} // namespace

std::size_t parse_lawnstrings_json(const std::string& text,
                                   std::unordered_map<std::wstring, std::wstring>& out) {
    out.clear();
    static const char kNeedle[] = "LocStringValues";
    size_t p = text.find(kNeedle);
    if (p == std::string::npos) {
        lzt_core::log_write("LawnStrings: JSON 结构不符（无 LocStringValues）");
        return 0;
    }
    p = text.find('[', p + sizeof(kNeedle) - 1);
    if (p == std::string::npos) return 0;

    std::vector<std::string> items;
    size_t i = p + 1;
    while (true) {
        if (!json_skip_ws(text, i)) break;
        if (i >= text.size() || text[i] == ']') break;
        if (text[i] != '"') {
            lzt_core::log_write("LawnStrings: JSON 数组元素非字符串 (at %zu)", i);
            return 0;
        }
        std::string v;
        if (!json_parse_string(text, i, v)) return 0;
        items.push_back(std::move(v));
        if (items.size() > 200000) break;   // 防御：异常输入不至于爆内存
    }

    std::size_t n = 0;
    for (std::size_t k = 0; k + 1 < items.size(); k += 2) {
        if (items[k].empty()) continue;
        // 键统一补方括号：表中形如 "ACCOUNT_DELETION"，游戏内部一律用
        // "[ACCOUNT_DELETION]" 形式（控件文本键即此格式）。
        std::wstring key = L"[" + utf8_to_wstring(items[k]) + L"]";
        out[key] = utf8_to_wstring(items[k + 1]);
        ++n;
    }
    lzt_core::log_write("LawnStrings: json parsed %zu entries", n);
    return n;
}

} // namespace lzt_language