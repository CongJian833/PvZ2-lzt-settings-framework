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

#include <string>
#include <unordered_map>

namespace lzt_language {

// RTON 版本解码。bytes 需以 "RTON" 开头（否则 ok=false）。
// maxEntries 同 parse_language_types（条目数上限）。
ParseResult parse_language_types_rton(const std::string& bytes, std::size_t maxEntries);

// ---- LawnStrings 表解析（用于「切换语言后 UI 文本立即更新」）----
//
// LawnStrings 的结构（与 LANGUAGETYPES 不同）：
//   { "objects": [ { "objclass": "LawnStringsData",
//                     "objdata": { "LocStringValues": [ "KEY1","文本1","KEY2","文本2", ... ] } } ] }
// 即 LocStringValues 是一维数组，【两两配对】（键在前、文本在后）。
//
// 为什么需要它：iOS 上游戏按语言标识加载 LawnStrings 后常驻内存，改标识不会
// 触发重载；因此本模块自行解析该表，在物化控件时直接把「键」替换为「当前语言
// 的文本」，从而让设置页文本随语言切换立即更新（无需重启、无需重载游戏表）。
//
// 编码：表中文本为 UTF-8，转成 wchar_t（UTF-32）后供游戏控件使用。
// 返回成功解析的条目数（0 表示该文件不可用）。
std::size_t parse_lawnstrings_rton(const std::string& bytes,
                                   std::unordered_map<std::wstring, std::wstring>& out);

// 明文 JSON 版（结构同构：objdata.LocStringValues 一维数组两两配对）。
// 用途：CDN 目录放 .json 更省事；游戏数据包内置表也是该形态。
std::size_t parse_lawnstrings_json(const std::string& text,
                                   std::unordered_map<std::wstring, std::wstring>& out);

} // namespace lzt_language

#endif // LZT_LANGUAGE_TYPES_RTON_H