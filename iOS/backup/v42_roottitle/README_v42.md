# v42_roottitle 基线存档

## 包

```
PvZ2_9.8.1_v42_roottitle.ipa
110,279,389 字节
SHA256: bf1b34da6baf1a44cbfc671b1ed1c499863bf03392ebafa56fc4651ddaee8de3
```

manifest：`m_H.txt`（17 站点，含 `16 756ae4`）

## 本版内容

在 v24_forceswitch 基础上，补齐**设置页根标题随语言切换实时刷新**。

### 根标题定位结论（反汇编 + 运行时双重证实）

| 项 | 值 | 依据 |
|---|---|---|
| 设置根页 | `0x756AE4` 入口 `x0` | 构造函数 `str x8,[x0]` 装 vtable |
| 标题对象 | `0x75BCB4` 构造，216 字节 | `mallc 216` + `str x8,[x0]` |
| 对象挂载 | 根页 `+216` 起 vector | `0x756DE0 stp x25,x20,[x19,#216]` |
| vector 三指针 | begin=+216 / end=+224 / cap=+232 | `0x756B58 str xzr,[x25,#216]!` 前变址 |
| 标题文本 | 对象 `+184`，24 字节 wstring | 析构 `0x75BD24 ldr x0,[x19,#184]` |
| 对象 vtable | `0x10224A330` | `0x75BCD0 adrp+add #816` |
| 标题键 | `[SETTINGS_TITLE]` | 二进制 `0x101F04540` 解码 |

### 实现要点

- 钩子 `0x756AE4`（必须放在 `engine_install` 之后，早期批次全部 FAIL）
  只记录根页指针，不做任何写入
- 写入时机与安卓 `settings_framework.cpp:923` 完全同位：
  语言切换 → 换表 → `refresh_rows` → `refresh_root_title`
- 写入方式复用行标题刷新的路径：
  `row_current_text()` 取当前语言文本 → `oWstringCtor(obj+184, text)`

### 关键教训（勿重犯）

| 错误做法 | 后果 | 正确做法 |
|---|---|---|
| 把键交给 `0x75B128` | 它是行对象**构造函数**，对已存在对象调用 = 就地重建 → SIGSEGV | 用 `oWstringCtor(字段, 文本)` |
| 读 `+224` 当 vector begin | 实为 end，读到垃圾（`0x46e0feed2870`） | begin 是 `+216` |
| vtable 拿 vm 常量直接比 | 运行时地址需加 slide，必然 mismatch | `base + (0x10224A330 - 0x100000000)` |
| 构造后 dispatch 延迟写 | 页面可能已析构 → 野指针 | 语言切换时写 |
| 构造返回后同步写 | 页面仍在装配 → 崩 | 同上 |
| 页面显示时写 | 同样崩 | 同上 |

### 已移除

- `lzt_ios_probe_root_title` 探针系列（168 行死代码，无调用点）
- 日志 `%ls` 改为先 `wstr_to_utf8` 再 `%s`
  （本机 `vsnprintf` 对 `%ls` 支持不完整，会截断）

## 已知限制

- 横幅（主界面底部）不随语言切换刷新，见 TODO
