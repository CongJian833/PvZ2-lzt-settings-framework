# 偏移量填写指南

本项目所有与游戏版本强相关的地址与字段偏移都集中在 `jni/offsets.h`。
换用其他游戏版本时，通常只需要修改该文件即可编译。

## 工作流程

1. 用 IDA Pro 打开目标版本的游戏库文件（ARM64 与 ARM32 各一份）
2. 按下方定位方法确认函数地址与字段偏移
3. 把验证后的数值填入 `jni/offsets.h` 对应架构段
4. 编译并实机验证（进入关卡、设置页面、目标展示、恐龙关卡）

## 必填函数地址

| 常量 | 说明 | ARM64 默认 | ARM32 默认 |
|------|------|-----------|-----------|
| `OFF_BoardZoom` | BoardLayout_ApplyZoom：计算 board 缩放与对齐字段 | `0xADEB80` | `0x75D044` |
| `OFF_BoardZoom2` | BoardZoom2：计算 board[280~282]，Hook 0 目标 | `0xADEDE0` | `0x75D2D8` |
| `OFF_ShakeBoard` | ShakeBoard action 创建函数 | `0xAF8650` | `0x774B64` |
| `OFF_SettingsCreate` | Settings createTab（5 参数） | `0xA4D79C` | `0x6D3068` |
| `OFF_SettingsDispatch` | Settings dispatch(page, tabId) | `0xA501E0` | `0x6D5BE8` |
| `OFF_SettingsDataSharing` | DataSharing 页面创建（dirty 重建入口） | `0xA4EA34` | `0x6D4420` |
| `OFF_SettingsLayout` | 页面布局初始化 | `0xA5084C` | `0x6D61EC` |
| `OFF_G_DisplayInfo` | DisplayInfo 指针全局 | `0x26BFC10` | `0x1E5DCEC` |

## 必填 Board 字段偏移

| 常量 | 说明 | ARM64 默认 | ARM32 默认 |
|------|------|-----------|-----------|
| `BOARD_270` | 种植偏移 (int) | `0x438` | `0x338` |
| `BOARD_280` | PPU 缩放因子 (float) | `0x460` | `0x35C` |
| `BOARD_281` | 棋盘宽度像素 (float) | `0x464` | `0x360` |
| `BOARD_283` | 左对齐偏移 (int) | `0x46C` | `0x368` |
| `BOARD_284` | 种植摄像机位置 (int) | `0x470` | `0x36C` |
| `BOARD_285` | 选卡摄像机位置 (int) | `0x474` | `0x370` |
| `BOARD_286` | 展示僵尸位置 (int) | `0x478` | `0x374` |
| `DISPLAYINFO_SCREEN_WIDTH` | 屏幕宽度 | `0xF4` | `0x88` |
| `DISPLAYINFO_SCREEN_HEIGHT` | 屏幕高度 | `0xF8` | `0x8C` |

## 定位方法

### BoardZoom（Hook 1）

反编译 `OFF_BoardZoom`，观察其读写 Board 成员的位置：

- ARM64：函数内访问 Board+0x460(float)、Board+0x46C/0x470/0x474/0x478(int)
- ARM32：函数内访问 Board+0x35C(float)、Board+0x368~0x374(int)

写入的成员即 `BOARD_270/283/284/285/286`，读取的 float 即 `BOARD_280`。

### BoardZoom2（Hook 0）

紧随 BoardLayout_ApplyZoom 被上层调用。函数内：

- 读取 Board+0x460（ARM32 为 +0x35C）即 `BOARD_280`
- 长屏检测读取 DisplayInfo+0x88/+0x8C（ARM32）或 +0xF4/+0xF8（ARM64），
  同时确认 `OFF_G_DisplayInfo` 指针全局

### 设置页面链（View Angle Tab）

以 ARM32 为例，从 DataSharing 页面函数 `sub_6D4420` 反向映射：

- `OFF_SettingsDataSharing`：页面创建函数本身
- `OFF_SettingsDispatch`：dispatch 的 switch(tabId) 外壳
- `OFF_SettingsCreate`：createTab 5 参数注册
- 标题写入：controller+132（ARM32）/ +184（ARM64）
- dirty flag：page+188（ARM32）/ +292（ARM64）

### UseHighViewAngle 配置字段

在 DisplayInfo 对象内选一个未被原版占用的字节：

- ARM64：偏移 2582（0xA16），位于
  HasDisabledUsageSharing(+2579)/DownloadPermissionOnWWAN(+2580)/
  HasAskedPermissionOnWWAN(+2581) 与 PatchVersionProgressDismissed(+2584) 之间
- ARM32：偏移 1794（0x702），1791/1792/1793/1796 已被原版占用

## 设置框架与语言模块常量（量

以下常量服务于设置界面注入、状态持久化、本地化刷新与语言表热重载，与相机功能
互不影响；缺失或填错时对应功能降级（不注入 Tab / 不刷新文本 / 不热重载），
一般不会影响相机对齐。全部集中在 `jni/offsets.h`。

### 设置界面与控件（两架构均有）

| 常量 | 用途 | ARM64 | ARM32 |
|------|------|-------|-------|
| `OFF_SettingsCreate` | createTab(page,id,title,iconN,iconS) | `0xA4D79C` | `0x6D3068` |
| `OFF_SettingsDispatch` | dispatch(page,tabId)，switch 结构 | `0xA501E0` | `0x6D5BE8` |
| `OFF_SettingsDataSharing` | DataSharing 页面（dirty 重建入口） | `0xA4EA34` | `0x6D4420` |
| `OFF_SettingsLayout` | 页面布局初始化 | `0xA5084C` | `0x6D61EC` |
| `OFF_CheckboxCreate` | checkbox(page,id,labelStr,initState,width) | `0xA4DEAC` | `0x6D3830` |
| `OFF_LocalizeKey` | 本地化键 → 文本（只作取值口，不拦截） | `0x14F3354` | `0x10F6754` |

### 状态持久化（两架构均有）

| 常量 | 用途 |
|------|------|
| `OFF_PersistSave` / `OFF_PersistReadBool` | bool 配置保存 / 读取 |
| `OFF_PersistManager` | 持久化管理器指针全局 |

### 本地化刷新与屏幕绑定（仅 ARM64）

| 常量 | 用途 |
|------|------|
| `OFF_SETTINGS_TAB_VTABLE` / `OFF_SETTINGS_TAB_LABEL_VTABLE` | Tab 对象 / 标签 vtable（回填前校验宿主有效性） |
| `OFF_SETTINGS_LABEL_CREATE` / `OFF_SETTINGS_ROW_INIT` | 标签构造器 / 设置行初始化器（Localizer 的捕获点） |
| `OFF_PERSISTENT_BANNER_REBUILD` | 主界面 PersistentMessage 横幅文本重建函数 |

### 资源读取与语言表热重载（仅 ARM64）

| 常量 | 用途 |
|------|------|
| `OFF_RESOURCE_*` | 资源描述 / 句柄 / 管理器 / 对象取用（CDN 文件通道） |
| `OFF_PACKAGE_FIND_RECORD` / `OFF_RADIX_FIND` | RSB 包内名查找（查找名须带 `packages\` 前缀） |
| `OFF_PVZDB_SINGLETON` / `OFF_DB_LOAD_RTON` | PVZDB 单例与 RTON 加载器（数据包兜底来源） |
| `OFF_CDN_APPLY` / `OFF_DB_SAVE_RTON` | CDN apply 链入口 / 内存表落盘 |
| `OFF_APPLY_LOCALIZED_STRINGS` | 应用本地化字符串 |
| `OFF_DELEGATE_MGR_PTR` / `OFF_DELEGATE_UNLOCK` | 表变更广播所需的 Delegate 管理器与解锁调用 |

### 定位思路

这类常量从“游戏侧可见的行为入口”反查，而不是从相机调用链出发：

- **设置类**：从已知原版 Tab（如 Build Version）的注册 / 分发调用回溯，确认
  createTab 的参数顺序与 dispatch 的 switch 外壳
- **持久化类**：从读写原版 bool 配置项（Music / SoundFX）的调用点回溯
- **本地化类**：从任一带 `[...]` 文本键的控件创建处回溯到键解析口
- **资源 / 语言表类**：从 `LoadPackage` 名的构造链回溯（前缀管理器 → 后缀追加 →
  分隔符归一），RSB 查找名必须带 `packages\` 前缀

每条常量的 IDA 依据都写在 `offsets.h` 对应行的注释里，改动时请同步更新注释。

## 注意事项

- ARM32 与 ARM64 偏移不同：指针宽度 4 vs 8 字节导致结构体字段整体前移，
  必须在对应架构 IDB 中独立验证，不能直接套用
- `offsets.h` 中带验证注释的常量都有 IDA 依据，改动时请同步更新注释
- 部分诊断 Hook 的 ARM32 地址未定位（渲染坐标转换、相机动画链），
  相应诊断在 ARM32 上不安装，不影响功能
