// ============================================================
// deprecated/resource_id_channel.cpp — 【已废弃 · 不参与构建】
//
// 历史：语言资源的“资源 id 通道”实现（RESFILE_PACKAGES_* 形式）——
//   构造 GenericResFile 描述符 → resolve → getObject 取资源对象，再读内容。
//
// 废弃原因：该 resolve→getObject 链对 LANGUAGETYPES 这类非 GenericResFile
//   资源返回 0（真机 S07 object=0 实证），无法读到目标资源。
//   现役通道见 resource_file.cpp 的 read_bytes_from_package（按 RSB 名字查找）。
//
// 保留方式：本文件未加入 jni/Android.mk 的 LOCAL_SRC_FILES，不参与任何构建，
//   仅为保留实现以备参考；请勿在此实现新功能。若要重新启用，须先加入构建，
//   并重新验证 resolve 链对目标资源的适用性。
// ============================================================

#ifdef __aarch64__

#include "../resource_file.h"
#include "../lzt_core.h"
#include "../offsets.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <vector>

#define LRDBG(...) do {} while (0)   // 归档件不启用诊断日志

[[maybe_unused]] static const char kLanguageResourceBuildTag[] = "LANGUAGE-V7.36-MODULE-TIDY";

namespace lzt_resource {

extern "C" void lzt_call_x8_1(uintptr_t fn, uintptr_t arg0, void* out);
extern "C" void lzt_call_x8_2(uintptr_t fn, uintptr_t arg0, uintptr_t arg1, void* out);

namespace {

struct ResourceDescriptor {
    uintptr_t vtable;
    uintptr_t next;
    uintptr_t reserved;
    const char* id;
    uint64_t handle;
};

struct GameBuffer {
    uintptr_t vtable;
    uint8_t* begin;
    uint8_t* end;
    uintptr_t reserved0;
    uintptr_t reserved1;
    uint32_t reserved2;
};

struct DescriptorEntry {
    std::string id;
    ResourceDescriptor descriptor{};
};

std::mutex g_descriptorMutex;
std::vector<std::unique_ptr<DescriptorEntry>> g_descriptors;

using InitDescriptorFn = void (*)(ResourceDescriptor*);
using InitHandleFn = void (*)(uint64_t*);
using ReleaseHandleFn = void (*)(uint64_t*);
using GetResourceManagerFn = uintptr_t (*)();
using ResourceTableFn = uintptr_t (*)(uintptr_t, uint32_t);
using ResourceObjectFn = uintptr_t (*)(uintptr_t, const uint64_t*);
using ResourceTypeFn = uintptr_t (*)();
using BufferInitFn = void (*)(GameBuffer*);
using BufferDestroyFn = void (*)(GameBuffer*);
using OpenResourceFn = int (*)(uintptr_t, const void*, GameBuffer*);

struct GameStringView {
    const char* data;
    size_t size;
};

GameStringView view_game_string(const void* value) {
    const auto* bytes = static_cast<const uint8_t*>(value);
    if ((bytes[0] & 1u) == 0) {
        return {reinterpret_cast<const char*>(bytes + 1), static_cast<size_t>(bytes[0] >> 1)};
    }
    size_t size = 0;
    const char* data = nullptr;
    memcpy(&size, bytes + 8, sizeof(size));
    memcpy(&data, bytes + 16, sizeof(data));
    return {data, size};
}

ResourceDescriptor* get_descriptor(uintptr_t base, const char* resourceId) {
    std::lock_guard<std::mutex> lock(g_descriptorMutex);
    for (const auto& entry : g_descriptors) {
        if (entry->id == resourceId) return &entry->descriptor;
    }

    auto entry = std::make_unique<DescriptorEntry>();
    entry->id = resourceId;
    auto initDescriptor = reinterpret_cast<InitDescriptorFn>(base + OFF_RESOURCE_DESC_INIT);
    auto initHandle = reinterpret_cast<InitHandleFn>(base + OFF_RESOURCE_HANDLE_INIT);
    initDescriptor(&entry->descriptor);
    entry->descriptor.vtable = base + OFF_GENERIC_RESFILE_DESC_VTABLE;
    entry->descriptor.id = entry->id.c_str();
    initHandle(&entry->descriptor.handle);
    ResourceDescriptor* descriptor = &entry->descriptor;
    g_descriptors.push_back(std::move(entry));
    return descriptor;
}

ReadStatus resolve_path(uintptr_t base, const char* resourceId, std::string& path) {
    lzt_core::set_stage(202);
    LRDBG("LRDBG S02 resolve begin build=%s base=0x%lx id=%s",
          kLanguageResourceBuildTag,
          static_cast<unsigned long>(base), resourceId);
    ResourceDescriptor* descriptor = get_descriptor(base, resourceId);
    LRDBG("LRDBG S03 descriptor=%p vtable=0x%lx idPtr=%p", descriptor,
          static_cast<unsigned long>(descriptor ? descriptor->vtable : 0),
          descriptor ? descriptor->id : nullptr);
    if (descriptor == nullptr ||
        !lzt_core::memory_range_accessible(reinterpret_cast<uintptr_t>(descriptor),
                                           sizeof(ResourceDescriptor), false)) {
        LRDBG("LRDBG FAIL S03 descriptor inaccessible");
        return ReadStatus::ResourceSystemUnavailable;
    }

    lzt_core::set_stage(203);
    uint64_t cached{};
    lzt_call_x8_1(base + OFF_GENERIC_RESFILE_RESOLVE,
                  reinterpret_cast<uintptr_t>(descriptor), &cached);
    LRDBG("LRDBG S04 cachedHandle=0x%016llx",
          static_cast<unsigned long long>(cached));
    if (cached == 0) {
        LRDBG("LRDBG FAIL S04 resource id not found id=%s", resourceId);
        return ReadStatus::ResourceIdNotFound;
    }

    auto releaseHandle = reinterpret_cast<ReleaseHandleFn>(base + OFF_RESOURCE_HANDLE_RELEASE);
    auto getManager = reinterpret_cast<GetResourceManagerFn>(base + OFF_RESOURCE_MANAGER_GET);
    auto getTable = reinterpret_cast<ResourceTableFn>(base + OFF_RESOURCE_TABLE_GET);
    auto getObject = reinterpret_cast<ResourceObjectFn>(base + OFF_RESOURCE_OBJECT_GET);

    lzt_core::set_stage(204);
    uintptr_t manager = getManager();
    LRDBG("LRDBG S05 manager=0x%lx", static_cast<unsigned long>(manager));
    if (manager == 0 || !lzt_core::memory_range_accessible(manager, 224, false)) {
        releaseHandle(&cached);
        LRDBG("LRDBG FAIL S05 manager unavailable");
        return ReadStatus::ResourceSystemUnavailable;
    }

    lzt_core::set_stage(205);
    uint64_t resolved{};
    lzt_call_x8_2(base + OFF_RESOURCE_HANDLE_RESOLVE, manager,
                  reinterpret_cast<uintptr_t>(&cached), &resolved);
    LRDBG("LRDBG S06 resolvedHandle=0x%016llx",
          static_cast<unsigned long long>(resolved));
    if (resolved == 0) {
        releaseHandle(&cached);
        LRDBG("LRDBG FAIL S06 resolved handle empty");
        return ReadStatus::ResourceIdNotFound;
    }

    lzt_core::set_stage(206);
    uint32_t type = static_cast<uint32_t>((resolved >> 49) & 0x3FFFu);
    uintptr_t table = getTable(manager, type);
    uintptr_t object = table ? getObject(table, &resolved) : 0;
    LRDBG("LRDBG S07 type=%u table=0x%lx object=0x%lx", type,
          static_cast<unsigned long>(table), static_cast<unsigned long>(object));
    if (object == 0 || !lzt_core::memory_range_accessible(object, 48, false)) {
        releaseHandle(&resolved);
        releaseHandle(&cached);
        LRDBG("LRDBG FAIL S07 resource object unavailable");
        return ReadStatus::ResourceObjectUnavailable;
    }

    lzt_core::set_stage(207);
    auto getExpectedType = reinterpret_cast<ResourceTypeFn>(base + OFF_GENERIC_RESFILE_TYPE);
    uintptr_t expectedType = getExpectedType();
    uintptr_t objectVtable = *reinterpret_cast<uintptr_t*>(object);
    if (expectedType == 0 || objectVtable == 0 ||
        !lzt_core::memory_range_accessible(objectVtable + 32, sizeof(uintptr_t), false)) {
        releaseHandle(&resolved);
        releaseHandle(&cached);
        LRDBG("LRDBG FAIL S08 expectedType=0x%lx vtable=0x%lx",
              static_cast<unsigned long>(expectedType),
              static_cast<unsigned long>(objectVtable));
        return ReadStatus::ResourceObjectUnavailable;
    }
    uintptr_t isTypeAddress = *reinterpret_cast<uintptr_t*>(objectVtable + 32);
    if (!lzt_core::memory_range_accessible(isTypeAddress, 4, false)) {
        releaseHandle(&resolved);
        releaseHandle(&cached);
        LRDBG("LRDBG FAIL S08 type callback inaccessible addr=0x%lx",
              static_cast<unsigned long>(isTypeAddress));
        return ReadStatus::ResourceObjectUnavailable;
    }
    auto isType = reinterpret_cast<int (*)(uintptr_t, uintptr_t)>(isTypeAddress);
    bool validType = isType(object, expectedType) != 0;
    LRDBG("LRDBG S08 expectedType=0x%lx vtable=0x%lx isType=0x%lx valid=%d",
          static_cast<unsigned long>(expectedType),
          static_cast<unsigned long>(objectVtable),
          static_cast<unsigned long>(isTypeAddress), validType ? 1 : 0);
    if (!validType) {
        releaseHandle(&resolved);
        releaseHandle(&cached);
        return ReadStatus::ResourceObjectUnavailable;
    }

    lzt_core::set_stage(208);
    GameStringView value = view_game_string(reinterpret_cast<const void*>(object + 24));
    LRDBG("LRDBG S09 pathData=%p pathSize=%zu", value.data, value.size);
    if (value.data == nullptr || value.size == 0 || value.size > 1024 ||
        !lzt_core::memory_range_accessible(reinterpret_cast<uintptr_t>(value.data),
                                           value.size, false)) {
        releaseHandle(&resolved);
        releaseHandle(&cached);
        LRDBG("LRDBG FAIL S09 invalid path view");
        return ReadStatus::ResourceObjectUnavailable;
    }
    path.assign(value.data, value.size);
    LRDBG("LRDBG S10 resolved path=%s", path.c_str());
    releaseHandle(&resolved);
    releaseHandle(&cached);
    lzt_core::set_stage(0);
    return ReadStatus::Ok;
}

}

// ============================================================
// 数据包通道·二进制变体（阶段5）：按 id 读取原始字节。
// read_text_by_id 的泛化主体；不做文本/JSON 校验。
// ============================================================
ReadStatus read_bytes_by_id(const char* resourceId, std::string& out) {
    out.clear();
    lzt_core::set_stage(201);
    LRDBG("LRDBG S01 read begin id=%s thread=%lu", resourceId ? resourceId : "<null>",
          static_cast<unsigned long>(pthread_self()));
    if (resourceId == nullptr || resourceId[0] == 0) {
        LRDBG("LRDBG FAIL S01 empty resource id");
        return ReadStatus::ResourceIdNotFound;
    }

    uintptr_t base = lzt_core::base();
    if (base == 0) {
        LRDBG("LRDBG FAIL S01 library base unavailable");
        return ReadStatus::LibraryUnavailable;
    }

    std::string path;
    ReadStatus resolved = resolve_path(base, resourceId, path);
    if (resolved != ReadStatus::Ok) {
        LRDBG("LRDBG FAIL resolve status=%s", status_name(resolved));
        return resolved;
    }

    lzt_core::set_stage(209);
    uintptr_t appSlot = base + OFF_RESOURCE_APP_SINGLETON;
    if (!lzt_core::memory_range_accessible(appSlot, sizeof(uintptr_t), false)) {
        LRDBG("LRDBG FAIL S11 app slot inaccessible addr=0x%lx",
              static_cast<unsigned long>(appSlot));
        return ReadStatus::ResourceSystemUnavailable;
    }
    uintptr_t app = *reinterpret_cast<volatile uintptr_t*>(appSlot);
    LRDBG("LRDBG S11 appSlot=0x%lx app=0x%lx", static_cast<unsigned long>(appSlot),
          static_cast<unsigned long>(app));
    if (app == 0 || !lzt_core::memory_range_accessible(app, 2144, false)) {
        LRDBG("LRDBG FAIL S11 app unavailable");
        return ReadStatus::ResourceSystemUnavailable;
    }

    alignas(16) uint8_t pathString[24]{};
    size_t pathSize = path.size();
    if (pathSize <= 22) {
        pathString[0] = static_cast<uint8_t>(pathSize << 1);
        memcpy(pathString + 1, path.data(), pathSize);
    } else {
        size_t capacity = ((pathSize + 16) & ~static_cast<size_t>(15)) | 1;
        memcpy(pathString, &capacity, sizeof(capacity));
        memcpy(pathString + 8, &pathSize, sizeof(pathSize));
        const char* pointer = path.data();
        memcpy(pathString + 16, &pointer, sizeof(pointer));
    }

    auto initBuffer = reinterpret_cast<BufferInitFn>(base + OFF_RESOURCE_BUFFER_INIT);
    auto destroyBuffer = reinterpret_cast<BufferDestroyFn>(base + OFF_RESOURCE_BUFFER_DESTROY);
    auto openResource = reinterpret_cast<OpenResourceFn>(base + OFF_RESOURCE_OPEN);

    lzt_core::set_stage(210);
    GameBuffer buffer{};
    initBuffer(&buffer);
    LRDBG("LRDBG S12 buffer initialized vtable=0x%lx",
          static_cast<unsigned long>(buffer.vtable));
    int opened = openResource(app, pathString, &buffer);
    LRDBG("LRDBG S13 open returned=%d begin=%p end=%p", opened, buffer.begin, buffer.end);
    if (!opened) {
        destroyBuffer(&buffer);
        LRDBG("LRDBG FAIL S13 open failed id=%s path=%s", resourceId, path.c_str());
        return ReadStatus::OpenFailed;
    }

    lzt_core::set_stage(211);
    if (buffer.begin == nullptr || buffer.end <= buffer.begin) {
        LRDBG("LRDBG FAIL S14 empty/invalid buffer begin=%p end=%p", buffer.begin, buffer.end);
        destroyBuffer(&buffer);
        return ReadStatus::EmptyFile;
    }
    size_t byteCount = static_cast<size_t>(buffer.end - buffer.begin);
    if (byteCount > 4 * 1024 * 1024 ||
        !lzt_core::memory_range_accessible(reinterpret_cast<uintptr_t>(buffer.begin),
                                           byteCount, false)) {
        LRDBG("LRDBG FAIL S14 buffer inaccessible/oversize bytes=%zu", byteCount);
        destroyBuffer(&buffer);
        return ReadStatus::EmptyFile;
    }

    char prefix[49]{};
    size_t prefixBytes = byteCount < 16 ? byteCount : 16;
    for (size_t i = 0; i < prefixBytes; ++i) {
        snprintf(prefix + i * 3, sizeof(prefix) - i * 3, "%02X ", buffer.begin[i]);
    }
    LRDBG("LRDBG S14 bytes=%zu prefix=%s", byteCount, prefix);
    out.assign(reinterpret_cast<const char*>(buffer.begin), byteCount);
    destroyBuffer(&buffer);
    LRDBG("LRDBG SUCCESS bytes id=%s path=%s bytes=%zu", resourceId,
          path.c_str(), out.size());
    lzt_core::set_stage(0);
    return ReadStatus::Ok;
}

ReadStatus read_text_by_id(const char* resourceId, std::string& out) {
    ReadStatus status = read_bytes_by_id(resourceId, out);
    if (status != ReadStatus::Ok) return status;

    lzt_core::set_stage(212);
    if (!looks_like_language_types_json(out)) {
        LRDBG("LRDBG FAIL S15 content is not LANGUAGETYPES JSON bytes=%zu", out.size());
        return ReadStatus::InvalidText;
    }
    LRDBG("LRDBG SUCCESS S15 id=%s bytes=%zu", resourceId, out.size());
    lzt_core::set_stage(0);
    return ReadStatus::Ok;
}

} // namespace lzt_resource

#else   // !__aarch64__

namespace lzt_resource {

ReadStatus read_text_by_id(const char*, std::string& out) {
    out.clear();
    return ReadStatus::UnsupportedArchitecture;
}

ReadStatus read_bytes_by_id(const char*, std::string& out) {
    out.clear();
    return ReadStatus::UnsupportedArchitecture;
}

} // namespace lzt_resource

#endif   // __aarch64__
