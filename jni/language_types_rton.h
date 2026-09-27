#ifndef LZT_LANGUAGE_TYPES_RTON_H
#define LZT_LANGUAGE_TYPES_RTON_H

// ============================================================
// language_types_rton — LANGUAGETYPES 的 RTON（AsciiORM 二进制）解码器
//
// 背景：LANGUAGETYPES 既可以是 JSON 文本（现有 parse_language_types），
// 也可能以"JSON 加密成 RTON"的二进制形式存在（数据包 package / CDN 目录）。
// 本解码器把 RTON 二进制流解码成与 JSON 路径完全相同的 ParseResult，
// 使 language_module 对"来源是 JSON 还是 RTON"无感知。
//
// === RTON 类型码（已用真实 LANGUAGETYPES.RTON 样本逆向确认）===
//   "RTON" + 版本 4B(LE u32)
//   0x90 <len> <bytes>   —— 新短字符串（并入字符串表）
//   0x91 <n>             —— 字符串表回溯引用（n = 该串首现时的 0 基序号）
//   0x85 / 0xFF          —— Map(对象/键值) 开始 / 结束
//   0x86 <count> / 0xFE  —— List 开始（count 为元素数）/ 结束
//   0x24 <varint>        —— 有符号/无符号整数
//   裸 0x00 / 0x01       —— bool false / true（值上下文）
//   结尾 ASCII "DONE"
//   顶层结构：#comment / version$ / objects[]（每个对象 objclass + objdata{...}）
// 本解码只提取 objdata 的 LocalizedName / LawnStringsType / Enabled 三字段，
// 其余（TypeName 等）跳过。
// ============================================================

#include "language_types.h"   // ParseResult / LanguageEntry / normalize_locale / is_valid_locale

namespace lzt_language {

// RTON 版本解码。bytes 需以 "RTON" 开头（否则 ok=false）。
// maxEntries 同 parse_language_types（条目数上限）。
ParseResult parse_language_types_rton(const std::string& bytes, std::size_t maxEntries);

} // namespace lzt_language

#endif // LZT_LANGUAGE_TYPES_RTON_H