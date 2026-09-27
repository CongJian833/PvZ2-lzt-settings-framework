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

// 读字符串：0x90 内联新串 / 0x91 表引用
std::string read_str(Cursor& c) {
    if (c.fail || c.i >= c.sz) { c.fail = true; return {}; }
    uint8_t t = c.d[c.i++];
    if (t == 0x90) {
        if (c.i >= c.sz) { c.fail = true; return {}; }
        uint32_t len = c.d[c.i++];
        if (c.i + len > c.sz) { c.fail = true; return {}; }
        std::string s(reinterpret_cast<const char*>(c.d + c.i), len);
        c.i += len;
        c.dict.push_back(s);
        return s;
    }
    if (t == 0x91) {
        if (c.i >= c.sz) { c.fail = true; return {}; }
        size_t idx = c.d[c.i++];
        if (idx >= c.dict.size()) { c.fail = true; return {}; }
        return c.dict[idx];
    }
    c.fail = true;
    return {};
}

// 值节点（含标量容器）。在 map 的"值"位置调用。
Node parse_node(Cursor& c) {
    Node n;
    if (c.fail || c.i >= c.sz) { c.fail = true; return n; }
    uint8_t t = c.d[c.i];
    if (t == 0x90 || t == 0x91) {
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
    if (t == 0x86) {                          // List: 0x86 [0xFD] <count> ... 0xFE
        c.i++;
        n.kind = Node::LIST;
        uint32_t count = 0;
        if (c.i < c.sz && c.d[c.i] == 0xFD) { c.i++; if (c.i < c.sz) count = c.d[c.i++]; }
        else if (c.i < c.sz) count = c.d[c.i++];
        for (uint32_t k = 0; k < count && !c.fail; ++k) {
            n.l.push_back(parse_node(c));
        }
        if (!c.fail && c.i < c.sz && peek(c) == 0xFE) c.i++;   // 消费 FE
        return n;
    }
    // 未知类型：标记失败（容错 → ok=false 走 JSON/兜底）
    c.fail = true;
    return n;
}

// 仅用于 map 值上下文，字符串可能来自 0x90/0x91——但执行到 map 值已由
// parse_node 处理；此函数保留占位（未用）。
bool rton_enabled_bool(const Node& v) {
    return v.kind == Node::BOOL ? v.b : false;
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

    // 顶层结构有两种编码（v7.23 用 681B 真实样本实证）：
    //   A. 开放键值流：#comment/version/objects… 键以 0x90/0x91 直接出现
    //      （无 0x85 外壳），以一个 0xFF 关闭顶层，文件尾附 "DONE"；
    //      版本字段 int 类型码为 0x08。
    //   B. 0x85 包起的 Map（兼容旧样本，反向仍保留）。
    Node root;
    root.kind = Node::MAP;
    if (c.i < c.sz && peek(c) == 0x85) {
        Node m = parse_node(c);
        if (c.fail || m.kind != Node::MAP) return out;
        root = std::move(m);
    } else {
        while (c.i < c.sz && !c.fail) {
            uint8_t t = peek(c);
            if (t == 0xFF) { c.i++; break; }               // 顶层 Map 结束
            if (t == 0x90 || t == 0x91) {                  // 键 token
                std::string key = read_str(c);
                if (c.fail) break;
                Node val = parse_node(c);
                if (c.fail) break;
                root.m.emplace_back(std::move(key), std::move(val));
            } else {
                break;                                     // "DONE" 等非键 token
            }
        }
        if (c.fail) return out;
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

} // namespace lzt_language