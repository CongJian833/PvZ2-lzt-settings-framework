// ============================================================
// table_loader.cpp — 有序来源数据表加载器实现（语义见头文件）
// ============================================================

#include "table_loader.h"

#include "../lzt_core.h"

#include <chrono>

namespace lzt_settings {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

TableLoader::TableLoader(const TableSource* sources, size_t count, int64_t retryWindowMs)
    : sources_(sources), count_(count), retryWindowMs_(retryWindowMs) {}

bool TableLoader::ensure() {
    if (ready_) return true;
    // 失败节流：窗口内不重扫（避免 UI 高频重建时反复读磁盘/内存）
    if (failMs_ != 0 && now_ms() - failMs_ < retryWindowMs_) return false;
    return load();
}

bool TableLoader::retry() {
    failMs_ = 0;              // 清节流：绕过节流窗口立即重测
    return ensure();
}

bool TableLoader::load() {
    for (size_t i = 0; i < count_; ++i) {
        const TableSource& s = sources_[i];
        if (s.fetch == nullptr || s.parse == nullptr) continue;
        std::string bytes;
        if (!s.fetch(bytes)) continue;      // 该来源不可用（文件缺失 / 资源未挂载等）
        const size_t n = s.parse(bytes);    // 解析并写入业务存储
        if (n == 0) continue;               // 形态或内容不符 → 继续下一个来源
        ready_ = true;
        entries_ = n;
        source_ = (s.name != nullptr) ? s.name : "unknown";
        lzt_core::log_write("[SFW] table loader: source=%s entries=%zu",
                            source_, entries_);
        return true;
    }
    failMs_ = now_ms();
    lzt_core::log_write("[SFW] table loader: all sources unavailable (tried %zu)",
                        count_);
    return false;
}

} // namespace lzt_settings