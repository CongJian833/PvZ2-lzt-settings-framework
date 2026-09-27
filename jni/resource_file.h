#ifndef RESOURCE_FILE_H
#define RESOURCE_FILE_H

#include <string>

namespace lzt_resource {

enum class ReadStatus {
    Ok,
    UnsupportedArchitecture,
    LibraryUnavailable,
    ResourceSystemUnavailable,
    ResourceIdNotFound,
    ResourceObjectUnavailable,
    OpenFailed,
    EmptyFile,
    InvalidText
};

const char* status_name(ReadStatus status);
bool looks_like_language_types_json(const std::string& text);

// 注：资源 id 通道（read_text_by_id / read_bytes_by_id）已废弃——其
//     resolve→getObject 链对 LANGUAGETYPES 返回 0（S07 实证）。声明与实现
//     一并移存 deprecated/resource_id_channel.cpp（不参与构建），原因见该文件头。

// 阶段一 CDN 通道（当前启用）：在
//   /sdcard/Android/data/<pkg>/files/No_backup/CDN.x.x/
// 目录下查找与 fileName【全名相同】（大小写不敏感）的文件并读取文本；
// 扩展名变体（.bak/.xx/.xxx）不匹配——只认精确文件名。
// 纯文件系统操作，不调用任何游戏代码。文件若为 RTON 二进制（魔数
// "RTON"）返回 InvalidText 并在日志说明——本通道要求放置 JSON 文本。
ReadStatus read_text_from_cdn(const char* fileName, std::string& out);

// 阶段5·二进制变体：在 CDN 目录查找 fileName（全名相同，大小写不敏感）
// 并读原始字节（RTON 等），不做文本/JSON 校验。
ReadStatus read_bytes_from_cdn(const char* fileName, std::string& out);

// 数据包通道·RSB 原语（v7.24）：与其余 rton 同法——经游戏 RSB
// 按名字（形如 "packages\LANGUAGETYPES.rton"）取记录指针+大小，
// 再复制原始字节。调用游戏 sub_16B31EC（ResStreamsManager 遍历 type3
// 挂载组 radix 查找）。仅 ARM64。这替代了不适配 LANGUAGETYPES 的
// read_bytes_by_id（其 resolve→getObject 对非 GenericResFile 返回 0）。
ReadStatus read_bytes_from_package(const char* rsbName, std::string& out);

}

#endif
