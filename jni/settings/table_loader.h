#ifndef LZT_TABLE_LOADER_H
#define LZT_TABLE_LOADER_H

// ============================================================
// table_loader — 有序来源数据表加载器（框架通用层）
//
// 收敛一类重复模式：一张表有多条候选来源（外置文件优先、内置兜底），
// 需要"按序尝试 → 命中即用 → 全失败后节流重试 → 支持绕过节流强制重测"。
//
// 语义：
//   - sources 数组顺序即优先级；第一个 fetch+parse 均成功的来源被采用
//   - parse 返回"可用条目数"（0 = 该来源不可用，继续尝试下一个）
//   - ensure()：惰性；已就绪直接 true；失败后 retryWindowMs 内不再重扫
//   - retry() ：清失败节流并立即重试（供"Tab 可见性判定"等需绕过节流的时机）
//   - 本层只做调度与节流，不关心表格式——解析与存储由调用方回调完成
//
// 线程模型：与设置框架一致（模块初始化线程 / UI 线程，非并发场景），不加锁。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <string>

namespace lzt_settings {

// 一条候选来源。
struct TableSource {
    const char* name;                            // 日志/诊断标识（如 "cdn-rton"）
    bool (*fetch)(std::string& bytes);           // 取原始字节；false = 该来源不可用
    size_t (*parse)(const std::string& bytes);   // 解析并自行存储；返回可用条目数（0=不可用）
};

class TableLoader {
public:
    TableLoader(const TableSource* sources, size_t count, int64_t retryWindowMs = 3000);

    bool ensure();                    // 惰性加载（失败节流窗口内直接返回 false）
    bool retry();                     // 强制重测（清节流后立即尝试）
    bool ready() const { return ready_; }
    size_t entry_count() const { return entries_; }
    const char* source_name() const { return source_; }

private:
    bool load();

    const TableSource* sources_;
    size_t count_;
    int64_t retryWindowMs_;
    bool ready_ = false;
    size_t entries_ = 0;
    const char* source_ = "none";
    int64_t failMs_ = 0;              // 0 = 尚无失败记录
};

} // namespace lzt_settings

#endif // LZT_TABLE_LOADER_H