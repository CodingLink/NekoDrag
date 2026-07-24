# SuperDrag 项目技术概述

> 本说明基于当前工作区，而不是仅基于 Git HEAD。当前最新修复尚未提交，其他 agent 不应执行 `git reset`、`git checkout --` 或清理未跟踪文件。

## 1. 项目定位

SuperDrag 是一个原生 C++17/Win32 Windows 10/11 x64 托盘程序。

核心功能：

- 默认按住 `Win+Alt`，在窗口任意位置按住鼠标左键拖动窗口。
- 支持 Win、Ctrl、Alt、Shift 中精确选择 1–3 个修饰键。
- 拖动开始后允许提前松开修饰键，直到左键释放才结束。
- 普通用户权限运行，不申请 UAC。
- 设置存储在当前用户注册表。
- 无 .NET、第三方 UI 框架和后台轮询。

## 2. 技术栈与构建

- C++17、Unicode Win32 API。
- CMake 3.20+。
- Windows GUI 子系统，无控制台。
- MSVC `/W4 /permissive- /utf-8`。
- `/MT` 静态运行时，目标为便携单 EXE。
- 主要系统库：`user32`、`shell32`、`advapi32`、`dwmapi`、`comctl32`。
- Per-Monitor DPI Awareness V2。
- 无第三方运行时依赖。

构建入口：[CMakeLists.txt](../CMakeLists.txt)

用户当前使用 Visual Studio 18：

```powershell
cmake -S . -B build-vs18 -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build build-vs18 --config Release --parallel
ctest --test-dir build-vs18 -C Release --output-on-failure
```

运行前应先退出托盘中的旧版 SuperDrag：

```powershell
.\build-vs18\Release\SuperDrag.exe
```

诊断拖动问题时可用 `-DSUPERDRAG_TRACE=ON` 构建，拖动状态迁移和位置记录
（100ms 节流）通过 `OutputDebugString` 输出，用 DebugView 抓取。

## 3. 模块划分

| 模块 | 职责 |
|---|---|
| [main.cpp](../src/main.cpp) | `wWinMain`、DPI Awareness、Common Controls 初始化 |
| [app.cpp](../src/app.cpp) | 生命周期、消息循环、鼠标钩子、拖动状态机、托盘、设置窗口 |
| [app.h](../src/app.h) | `SuperDragApp` 和 `DragState` 定义 |
| [core.cpp](../src/core.cpp) | 修饰键校验、拖动坐标计算、窗口候选过滤 |
| [settings_store.cpp](../src/settings_store.cpp) | 注册表设置和开机启动 |
| [layout.h](../src/layout.h) | 设置窗口的 96 DPI 逻辑布局 |
| [ui_theme.cpp](../src/ui_theme.cpp) | 深浅色、高对比度及 owner-draw 控件绘制 |
| [superdrag.rc](../src/superdrag.rc) | 图标和版本资源 |
| [superdrag.manifest](../src/superdrag.manifest) | DPI、Common Controls v6、`asInvoker` 权限 |
| [core_tests.cpp](../tests/core_tests.cpp) | 无第三方框架的核心 CTest |
| [WINDOWS_QA.md](WINDOWS_QA.md) | Windows 手工验收清单 |

## 4. 启动和消息架构

程序维护两个窗口：

- 隐藏主窗口：接收托盘、拖动更新、Explorer 重启和单实例消息。
- 设置窗口：首次运行显示，之后通过托盘或第二实例唤醒。

启动流程：

```text
wWinMain
  → 创建 Local\SuperDrag.SingleInstance 互斥量
  → 加载 HKCU 设置
  → 创建隐藏消息窗口
  → 添加托盘图标
  → 校准开机启动路径
  → Enabled=true 时安装 WH_MOUSE_LL
  → 首次运行显示设置窗口
  → GetMessage 消息循环
```

第二实例通过 `FindWindow` 找到隐藏窗口，并投递打开设置消息，不创建第二个驻留进程。

收到 Explorer 的 `TaskbarCreated` 消息后会重新添加托盘图标。

## 5. 当前拖动实现

当前实现是“低级鼠标钩子 + 钩子内即时异步定位”：

```text
WH_MOUSE_LL
  → 跳过 LLMHF_INJECTED 合成输入
  → 检查精确修饰键
  → WindowFromPoint + GA_ROOT
  → 窗口过滤和完整性等级检查
  → 建立 DragState
  → 吞掉本次左键按下/移动/释放
  → 稳态移动在钩子回调内直接 SetWindowPos(SWP_ASYNCWINDOWPOS)
  → 最大化恢复等慢路径仍走 WM_APP 消息
  → 左键释放后清理状态
```

关键行为：

- `CurrentModifierMask()` 使用 `GetAsyncKeyState`。
- `IsExactModifierMatch()` 要求没有额外修饰键。
- 稳态拖动移动**不再经过消息队列合并**，而是在 `WH_MOUSE_LL` 回调里立即用
  `SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
  SWP_ASYNCWINDOWPOS | SWP_DEFERERASE` 调用 `SetWindowPos`。
  `SWP_ASYNCWINDOWPOS` 把位置请求投递到目标线程，本线程永不因目标窗口繁忙而阻塞；
  同一目标的请求按 FIFO 顺序执行，不会乱序。
- `IsDragPositionReady()` 决定移动走钩子内快路径（稳态）还是消息线程慢路径
  （开始待处理、最大化恢复中、释放待处理、移动失败）。
- `lastAppliedOrigin` 跳过与上次相同的位置请求，抑制高回报率鼠标的冗余调用。
- 最大化窗口先通过 `ShowWindowAsync(SW_RESTORE)` 恢复，再按鼠标相对位置重新计算锚点。
- 恢复状态每 16ms 检查一次，最多 30 次。
- 拖动期间鼠标消息被吞掉，防止目标窗口控件误触。
- 拖动激活期间有 500ms 看门狗：物理左键已抬起但状态机仍 active 时强制 `EndDrag`，
  防止钩子超时漏掉 `WM_LBUTTONUP` 后永久吞掉后续点击。

### 重要历史与限制

不要直接恢复为“吞掉左键后投递 `WM_NCLBUTTONDOWN + HTCAPTION`”的方案。该方案已经实测无法拖动：真实左键按下被钩子吞掉后，目标窗口缺少原生移动循环所需的输入状态。

也不要恢复“合并投递 WM_APP + 同步 `SetWindowPos`”的方案。该方案在真实 Windows 上实测抖动且无法拖动：跨进程同步 `SetWindowPos` 会向目标线程 SendMessage，目标繁忙时阻塞本线程消息循环，而 `WH_MOUSE_LL` 回调依赖该循环喂入，阻塞导致钩子事件堆积或被 `LowLevelHooksTimeout` 跳过，解除阻塞后窗口爆发式跳动。此前观测到的“`SWP_ASYNCWINDOWPOS` 抖动”实际源于这一架构（位置按消息循环速率突发应用），而非 async 本身。

## 6. 窗口过滤和权限

以下目标不会进入拖动：

- SuperDrag 自身窗口。
- 桌面、Shell、主/副任务栏。
- 菜单窗口、工具提示。
- 隐藏、禁用、最小化或已销毁窗口。
- 子窗口、DWM cloaked 窗口。
- `WS_EX_NOACTIVATE` 临时窗口。
- 高于 SuperDrag 完整性等级的进程。

无法查询目标进程完整性时按受限窗口处理。管理员或受保护窗口不会触发 UAC，只显示一次托盘说明。

## 7. 设置和注册表

主设置位置：

```text
HKCU\Software\SuperDrag
```

值：

- `Enabled`
- `ModifierMask`
- `FirstRunCompleted`
- `PrivilegeHintShown`

`ModifierMask` 与 Win32 `MOD_*` 位值一致：

- Alt：`0x0001`
- Ctrl：`0x0002`
- Shift：`0x0004`
- Win：`0x0008`

开机启动位置：

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run
值名：SuperDrag
```

值内容是带双引号的当前 EXE 绝对路径。程序启动时会在开机启动已开启的情况下校准路径。

设置提交具有简单回滚逻辑：注册表或开机启动修改失败时，不保留部分状态。

## 8. 设置窗口 UI

设置页使用原生子窗口控件，但复选框、按钮和分组采用 owner-draw。

特别注意：

- 复选框状态保存在控件的 `GWLP_USERDATA`，不使用 `BM_GETCHECK/BM_SETCHECK`。
- 不要把 `BS_OWNERDRAW` 与 `BS_AUTOCHECKBOX` 直接组合；按钮类型位会冲突。
- 快捷键分组必须保持为 `STATIC + SS_OWNERDRAW + WS_CLIPSIBLINGS`。
- 设置窗口使用 `WS_CLIPCHILDREN`。
- 每个 owner-draw 控件必须完整填充自身背景，否则点击、焦点变化或主题刷新后可能出现控件消失。
- 分组内 Win/Ctrl/Alt/Shift 使用 Surface 背景，其他复选框使用窗口背景。
- 支持浅色、深色、高对比度和 `WM_DPICHANGED`。
- 最小客户区为 480×400 个 96-DPI 逻辑像素。

## 9. 资源和清单注意事项

- 图标资源位于 `src/assets`，由 `superdrag.rc` 引用。
- Manifest 已作为 CMake 源文件交给 MSVC。
- 不要再向 `.rc` 添加 `RT_MANIFEST`，否则会再次出现：

```text
CVT1100: 资源重复。类型: MANIFEST
LNK1123: 转换到 COFF 期间失败
```

- `superdrag_core`、`SuperDrag` 和 `superdrag_tests` 必须保持相同的 `/MT` 运行时设置，否则会出现 `LNK2038 RuntimeLibrary` 不匹配。

## 10. 测试状态

自动化测试覆盖：

- 默认设置。
- 1–3 个修饰键校验。
- 精确修饰键匹配。
- 普通拖动坐标。
- 最大化恢复坐标。
- 负坐标显示器。
- 无效窗口过滤。

当前环境已使用 Apple Clang 的 `-Werror` 编译并运行核心测试，结果通过。

尚未在当前环境验证：

- 最新版本的 MSVC 完整构建。
- 实际 Windows 拖动是否完全消除抖动。
- 设置页点击是否不再消失。
- 最大化、贴靠、多 DPI 的最终运行效果。

这些项目必须在 Windows 10/11 上按 [WINDOWS_QA.md](WINDOWS_QA.md) 验收。

## 11. 当前 Git 状态

- 当前 HEAD：`1192429 feat: implement native Windows SuperDrag utility`
- 最新 UI、构建和拖动修复尚未提交。
- 工作区包含已修改及未跟踪文件，尤其是 UI 主题、布局、资源和设计文档。
- `ui_theme.cpp`、`ui_theme.h`、`layout.h` 等文件虽然未跟踪，但已被当前代码和 CMake 引用。

其他 agent 开始工作前应先执行：

```powershell
git status --short
git diff --check
git diff -- CMakeLists.txt src/app.cpp src/app.h src/superdrag.rc
```

不得通过重置工作区来“恢复干净状态”，否则会丢失当前修复并可能导致工程无法构建。
