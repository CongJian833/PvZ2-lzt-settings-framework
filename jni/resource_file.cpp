#include "resource_file.h"

#include "lzt_settings_framework_config.h"   // kDebugMode：诊断日志门
#include "lzt_core.h"
#include "offsets.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>

namespace lzt_resource {

// 构建标识（随诊断日志输出）；日志门统一为 lzt_settings_framework_config.h 的
// LZT_DBG_LOG（v7.37 起全工程一套：Release 静默、Debug 保留）。
[[maybe_unused]] static const char kLanguageResourceBuildTag[] = "LANGUAGE-V7.36-MODULE-TIDY";

const char* status_name(ReadStatus status) {
    switch (status) {
        case ReadStatus::Ok: return "ok";
        case ReadStatus::UnsupportedArchitecture: return "unsupported-architecture";
        case ReadStatus::LibraryUnavailable: return "library-unavailable";
        case ReadStatus::ResourceSystemUnavailable: return "resource-system-unavailable";
        case ReadStatus::ResourceIdNotFound: return "resource-id-not-found";
        case ReadStatus::ResourceObjectUnavailable: return "resource-object-unavailable";
        case ReadStatus::OpenFailed: return "open-failed";
        case ReadStatus::EmptyFile: return "empty-file";
        case ReadStatus::InvalidText: return "invalid-text";
    }
    return "unknown";
}

bool looks_like_language_types_json(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    if (begin == text.size() || text[begin] != '{') return false;

    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    if (end <= begin || text[end - 1] != '}') return false;

    return text.find("\"objects\"", begin) != std::string::npos;
}

// ============================================================
// CDN 通道（阶段一启用）：/sdcard/Android/data/<pkg>/files/
// No_backup/CDN.x.x/ 下按文件名读取。纯文件系统操作，不触碰
// 游戏内存与函数，任意线程/任意时机调用均无崩溃风险。
// 日志阶段号 C01~C09（与数据包通道 S01~S15 区分）。
// ============================================================
namespace {

constexpr const char* kCdnRootDir = "/sdcard/Android/data";
constexpr const char* kCdnDirPrefix = "CDN.";
constexpr size_t kMaxCdnFileBytes = 4 * 1024 * 1024;

bool is_rton_magic(const std::string& text) {
    return text.size() >= 4 && memcmp(text.data(), "RTON", 4) == 0;
}

// 包名读取统一走 lzt_core::package_name()（v7.37）。
// Android 11+ scoped storage：/sdcard/Android/data 顶层不可枚举（MuMu 真机
// 日志实证 C02 失败），必须读自身包名直接构造路径；应用自己的
// files/No_backup 私有目录可自由 opendir。

// 文件名匹配：v7.35 仅当全名与请求文件名完全相同（大小写不敏感）才命中；
// 不再接受主名相同但扩展名不同（.bak/.xx/.xxx）的变体文件。
bool cdn_name_matches(const char* entryName, const std::string& wanted,
                      const std::string& wantedStem) {
    (void)wantedStem;
    return strcasecmp(entryName, wanted.c_str()) == 0;
}

// 单个候选文件的读取与校验；成功时填充 out 并返回 Ok
ReadStatus read_cdn_candidate(const char* path, std::string& out) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        LZT_DBG_LOG("LRCDN C05 skip non-regular/empty %s", path);
        return ReadStatus::EmptyFile;
    }
    if (static_cast<unsigned long>(st.st_size) > kMaxCdnFileBytes) {
        LZT_DBG_LOG("LRCDN FAIL C05 oversize %s bytes=%ld", path,
              static_cast<long>(st.st_size));
        return ReadStatus::EmptyFile;
    }
    LZT_DBG_LOG("LRCDN C05 candidate %s bytes=%ld", path, static_cast<long>(st.st_size));

    FILE* f = fopen(path, "rb");
    if (!f) {
        LZT_DBG_LOG("LRCDN FAIL C06 open failed %s", path);
        return ReadStatus::OpenFailed;
    }
    out.resize(static_cast<size_t>(st.st_size));
    size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    if (got != out.size()) {
        LZT_DBG_LOG("LRCDN FAIL C06 short read %s got=%zu want=%zu", path, got, out.size());
        out.clear();
        return ReadStatus::OpenFailed;
    }

    char prefix[49]{};
    size_t prefixBytes = out.size() < 16 ? out.size() : 16;
    for (size_t i = 0; i < prefixBytes; ++i) {
        snprintf(prefix + i * 3, sizeof(prefix) - i * 3, "%02X ",
                 static_cast<unsigned char>(out[i]));
    }
    LZT_DBG_LOG("LRCDN C06 read ok %s bytes=%zu prefix=%s", path, out.size(), prefix);

    if (is_rton_magic(out)) {
        LZT_DBG_LOG("LRCDN FAIL C07 %s is RTON binary (magic=RTON); "
              "stage1 requires plain JSON text — place the decoded .json file",
              path);
        return ReadStatus::InvalidText;
    }
    if (!looks_like_language_types_json(out)) {
        LZT_DBG_LOG("LRCDN FAIL C07 %s content is not LANGUAGETYPES JSON", path);
        return ReadStatus::InvalidText;
    }
    LZT_DBG_LOG("LRCDN SUCCESS C07 %s bytes=%zu", path, out.size());
    return ReadStatus::Ok;
}

} // namespace

ReadStatus read_text_from_cdn(const char* fileName, std::string& out) {
    out.clear();
    lzt_core::set_stage(220);
    if (fileName == nullptr || fileName[0] == 0) {
        LZT_DBG_LOG("LRCDN FAIL C01 empty file name");
        return ReadStatus::ResourceIdNotFound;
    }
    std::string wanted(fileName);
    size_t dot = wanted.rfind('.');
    std::string wantedStem = (dot == std::string::npos) ? wanted : wanted.substr(0, dot);
    LZT_DBG_LOG("LRCDN C01 begin file=%s stem=%s build=%s", fileName, wantedStem.c_str(),
          kLanguageResourceBuildTag);

    // v6.0 修复：不再枚举 /sdcard/Android/data 顶层（Android 11+ 拒绝，
    // MuMu 日志 C02 铁证），改为读自身包名直接构造 No_backup 路径。
    const char* pkg = lzt_core::package_name();
    if (pkg == nullptr || pkg[0] == '\0') {
        LZT_DBG_LOG("LRCDN FAIL C02 cannot read package name from /proc/self/cmdline");
        return ReadStatus::ResourceSystemUnavailable;
    }

    lzt_core::set_stage(221);
    char noBackup[512];
    snprintf(noBackup, sizeof(noBackup), "%s/%s/files/No_backup", kCdnRootDir, pkg);
    DIR* nb = opendir(noBackup);
    if (!nb) {
        LZT_DBG_LOG("LRCDN FAIL C02 cannot open %s (No_backup dir missing?)", noBackup);
        return ReadStatus::ResourceSystemUnavailable;
    }

    int cdnDirCount = 0;
    int candidateCount = 0;
    struct dirent* ent;
    while ((ent = readdir(nb)) != nullptr) {
        if (strncmp(ent->d_name, kCdnDirPrefix, strlen(kCdnDirPrefix)) != 0) continue;
        ++cdnDirCount;
        char path[640];
        snprintf(path, sizeof(path), "%s/%s", noBackup, ent->d_name);
        LZT_DBG_LOG("LRCDN C03 cdn dir %s", path);

        DIR* cdn = opendir(path);
        if (!cdn) continue;
        struct dirent* file;
        while ((file = readdir(cdn)) != nullptr) {
            if (!cdn_name_matches(file->d_name, wanted, wantedStem)) continue;
            ++candidateCount;
            snprintf(path, sizeof(path), "%s/%s/%s", noBackup, ent->d_name,
                     file->d_name);
            ReadStatus status = read_cdn_candidate(path, out);
            if (status == ReadStatus::Ok) {
                closedir(cdn);
                closedir(nb);
                lzt_core::set_stage(0);
                return ReadStatus::Ok;
            }
            out.clear();
        }
        closedir(cdn);
    }
    closedir(nb);

    lzt_core::set_stage(222);
    LZT_DBG_LOG("LRCDN FAIL C08 not found file=%s pkg=%s cdnDirs=%d candidates=%d "
          "(place the file under files/No_backup/CDN.x.x/)",
          fileName, pkg, cdnDirCount, candidateCount);
    return ReadStatus::ResourceIdNotFound;
}

// ---- 阶段5·CDN 二进制变体：读原始字节（RTON 等），不做 JSON/RTON 校验 ----
ReadStatus read_bytes_from_cdn(const char* fileName, std::string& out) {
    out.clear();
    if (fileName == nullptr || fileName[0] == 0) return ReadStatus::ResourceIdNotFound;
    std::string wanted(fileName);
    size_t dot = wanted.rfind('.');
    std::string wantedStem = (dot == std::string::npos) ? wanted : wanted.substr(0, dot);

    const char* pkg = lzt_core::package_name();
    if (pkg == nullptr || pkg[0] == '\0')
        return ReadStatus::ResourceSystemUnavailable;

    char noBackup[512];
    snprintf(noBackup, sizeof(noBackup), "%s/%s/files/No_backup", kCdnRootDir, pkg);
    DIR* nb = opendir(noBackup);
    if (!nb) return ReadStatus::ResourceSystemUnavailable;

    ReadStatus result = ReadStatus::ResourceIdNotFound;
    struct dirent* ent;
    while ((ent = readdir(nb)) != nullptr) {
        if (strncmp(ent->d_name, kCdnDirPrefix, strlen(kCdnDirPrefix)) != 0) continue;
        char path[640];
        snprintf(path, sizeof(path), "%s/%s", noBackup, ent->d_name);
        DIR* cdn = opendir(path);
        if (!cdn) continue;
        struct dirent* file;
        while ((file = readdir(cdn)) != nullptr) {
            if (!cdn_name_matches(file->d_name, wanted, wantedStem)) continue;
            snprintf(path, sizeof(path), "%s/%s/%s", noBackup, ent->d_name, file->d_name);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
            if (static_cast<unsigned long>(st.st_size) > kMaxCdnFileBytes) continue;
            FILE* f = fopen(path, "rb");
            if (!f) continue;
            out.resize(static_cast<size_t>(st.st_size));
            size_t got = fread(out.data(), 1, out.size(), f);
            fclose(f);
            if (got == out.size()) {
                LZT_DBG_LOG("LRCDN READ bytes %s bytes=%zu", path, out.size());
                closedir(cdn);
                closedir(nb);
                return ReadStatus::Ok;
            }
            out.clear();
        }
        closedir(cdn);
    }
    closedir(nb);
    LZT_DBG_LOG("LRCDN READ not-found id=%s cdnDirs scanned", fileName);
    return result;
}

#ifdef __aarch64__

// ============================================================
// 数据包通道·RSB 原语（v7.24）：按 RSB 名字取原始字节。
// 与其余 rton 同法——调用游戏 sub_16B31EC（OFF_PACKAGE_FIND_RECORD）
// 在 ResStreamsManager 里遍历 type3 挂载组，radix 命中后返回组内
// 数据 (dataPtr,size)，再复制字节。名字形如 "packages\LANGUAGETYPES.rton"
// （游戏会把 / 归一为 \）。替代不适配 LANGUAGETYPES 的 GenericResFile 链。
// ============================================================
ReadStatus read_bytes_from_package(const char* rsbName, std::string& out) {
    out.clear();
    lzt_core::set_stage(230);
    LZT_DBG_LOG("LRPKG P01 begin name=%s", rsbName ? rsbName : "<null>");
    if (rsbName == nullptr || rsbName[0] == 0) return ReadStatus::ResourceIdNotFound;

    uintptr_t base = lzt_core::base();
    if (base == 0) {
        LZT_DBG_LOG("LRPKG FAIL P01 library base unavailable");
        return ReadStatus::LibraryUnavailable;
    }

    uintptr_t pakMgrSlot = base + OFF_PAK_MANAGER;
    if (!lzt_core::memory_range_accessible(pakMgrSlot, sizeof(uintptr_t), false)) {
        LZT_DBG_LOG("LRPKG FAIL P02 pakMgr slot inaccessible");
        return ReadStatus::ResourceSystemUnavailable;
    }
    uintptr_t pakMgr = *reinterpret_cast<volatile uintptr_t*>(pakMgrSlot);
    if (pakMgr == 0 ||
        !lzt_core::memory_range_accessible(pakMgr + PAKMGR_RES_STREAMS_MGR,
                                           sizeof(uintptr_t), false)) {
        LZT_DBG_LOG("LRPKG FAIL P02 pakMgr=%p unavailable", reinterpret_cast<void*>(pakMgr));
        return ReadStatus::ResourceSystemUnavailable;
    }
    uintptr_t rsm = *reinterpret_cast<volatile uintptr_t*>(pakMgr + PAKMGR_RES_STREAMS_MGR);
    if (rsm == 0 || !lzt_core::memory_range_accessible(rsm + RSM_MOUNT_COUNT, 16, false)) {
        LZT_DBG_LOG("LRPKG FAIL P02 rsm=%p unavailable", reinterpret_cast<void*>(rsm));
        return ReadStatus::ResourceSystemUnavailable;
    }
    LZT_DBG_LOG("LRPKG P02 pakMgr=%p rsm=%p mounts=%u",
          reinterpret_cast<void*>(pakMgr), reinterpret_cast<void*>(rsm),
          *reinterpret_cast<volatile uint32_t*>(rsm + RSM_MOUNT_COUNT));

    // libc++ SSO string（名字可能 >22 字符 → 长串指向静态名字串）
    size_t len = strlen(rsbName);
    alignas(16) uint8_t nameStr[24]{};
    if (len <= 22) {
        nameStr[0] = static_cast<uint8_t>(len << 1);
        memcpy(nameStr + 1, rsbName, len);
    } else {
        size_t cap = ((len + 16) & ~static_cast<size_t>(15)) | 1;
        memcpy(nameStr + 0, &cap, 8);
        memcpy(nameStr + 8, &len, 8);
        const char* p = rsbName;
        memcpy(nameStr + 16, &p, 8);
    }

    using FindRecFn = uintptr_t (*)(uintptr_t, int, void*, uint64_t*, uint32_t*);
    auto findRec = reinterpret_cast<FindRecFn>(base + OFF_PACKAGE_FIND_RECORD);
    uint64_t dataPtr = 0;
    uint32_t dataSize = 0;
    uintptr_t hit = findRec(rsm, -1, nameStr, &dataPtr, &dataSize);
    LZT_DBG_LOG("LRPKG P03 find name=%s hit=%lu dataPtr=%p size=%u",
          rsbName, static_cast<unsigned long>(hit),
          reinterpret_cast<void*>(dataPtr), dataSize);
    if (hit == 0 || dataPtr == 0 || dataSize == 0) {
        LZT_DBG_LOG("LRPKG FAIL P03 no record for %s (not in RSB packages?)", rsbName);
        return ReadStatus::ResourceIdNotFound;
    }
    if (dataSize > 64u * 1024 * 1024 ||
        !lzt_core::memory_range_accessible(dataPtr, dataSize, false)) {
        LZT_DBG_LOG("LRPKG FAIL P04 record inaccessible ptr=%p size=%u",
              reinterpret_cast<void*>(dataPtr), dataSize);
        return ReadStatus::ResourceObjectUnavailable;
    }
    out.assign(reinterpret_cast<const char*>(dataPtr), dataSize);
    char prefix[25]{};
    size_t pn = dataSize < 8 ? dataSize : 8;
    for (size_t i = 0; i < pn; ++i)
        snprintf(prefix + i * 3, sizeof(prefix) - i * 3, "%02X ",
                 static_cast<unsigned char>(out.data()[i]));
    LZT_DBG_LOG("LRPKG P04 READ bytes=%u prefix=%s", dataSize, prefix);
    lzt_core::set_stage(0);
    return ReadStatus::Ok;
}

// 注：资源 id 通道（read_text_by_id / read_bytes_by_id）已废弃——
//     其 resolve→getObject 链对 LANGUAGETYPES 返回 0（S07 实证），
//     实现移存 deprecated/resource_id_channel.cpp（不参与构建）。

#else

ReadStatus read_bytes_from_package(const char*, std::string& out) {
    out.clear();
    return ReadStatus::UnsupportedArchitecture;
}

#endif

}
