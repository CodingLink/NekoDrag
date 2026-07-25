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

诊断拖动问题时可用 `-DSUPERDRAG_TRACE=ON` 构建。钩子安装尝试和错误码、
移动请求与实际坐标、覆盖数量、耗时及拖动结束原因通过 `OutputDebugString`
输出；高频移动请求和完成日志按 100ms 节流，可用 DebugView 抓取。

## 3. 模块划分

| 模块 | 职责 |
|---|---|
| [main.cpp](../src/main.cpp) | `wWinMain`、DPI Awareness、Common Controls 初始化 |
| [app.cpp](../src/app.cpp) | 生命周期、消息循环、鼠标钩子、拖动状态机、托盘、设置窗口 |
| [app.h](../src/app.h) | `SuperDragApp` 和 `DragState` 定义 |
| [window_move_worker.cpp](../src/window_move_worker.cpp) | 合并最新坐标并在独立线程串行移动窗口 |
| [core.cpp](../src/core.cpp) | 修饰键校验、拖动坐标计算、窗口候选过滤 |
| [settings_store.cpp](../src/settings_store.cpp) | 注册表设置和开机启动 |
| [layout.h](../src/layout.h) | 设置窗口的 96 DPI 逻辑布局 |
| [ui_theme.cpp](../src/ui_theme.cpp) | 深浅色、高对比度及 owner-draw 控件绘制 |
| [superdrag.rc](../src/superdrag.rc) | 图标和版本资源 |
| [superdrag.manifest](../src/superdrag.manifest) | DPI、Common Controls v6、`asInvoker` 权限 |
| [core_tests.cpp](../tests/core_tests.cpp) | 无第三方框架的核心 CTest |
| [window_move_worker_tests.cpp](../tests/window_move_worker_tests.cpp) | Windows 移动线程并发与错误路径 CTest |
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
  → 启动串行窗口移动工作线程
  → 添加托盘图标
  → 校准开机启动路径
  → Enabled=true 时投递 WH_MOUSE_LL 安装消息
  → 首次运行显示设置窗口
  → GetMessage 消息循环
  → 安装钩子；瞬时失败时按 250ms/1s/3s 重试
```

第二实例通过 `FindWindow` 找到隐藏窗口，并投递打开设置消息，不创建第二个驻留进程。

收到 Explorer 的 `TaskbarCreated` 消息后会重新添加托盘图标。

## 5. 当前拖动实现

当前实现是“低级鼠标钩子 + 最新位置合并 + 串行移动线程”：

```text
WH_MOUSE_LL
  → 跳过 LLMHF_INJECTED 合成输入
  → 检查精确修饰键
  → WindowFromPoint + GA_ROOT
  → 窗口过滤和完整性等级检查
  → 建立 DragState
  → 吞掉本次左键按下/释放；移动事件继续传递以更新系统光标
  → 钩子只覆盖容量为 1 的最新移动请求
  → 工作线程串行同步 SetWindowPos
  → 完成消息携带 drag generation 返回主线程
  → 最大化恢复等慢路径仍走 WM_APP 消息
  → 左键释放后等待最终位置完成（最多 500ms）
```

关键行为：

- `CurrentModifierMask()` 使用 `GetAsyncKeyState`。
- `IsExactModifierMatch()` 要求没有额外修饰键。
- `WindowMoveWorker` 同时最多执行一次同步 `SetWindowPos`，等待中的请求只有一个；
  高频鼠标产生的新坐标覆盖旧坐标，不会向目标线程堆积异步位置请求。
- 同步跨进程调用只会阻塞移动工作线程，不会阻塞安装 `WH_MOUSE_LL` 的 UI 线程；
  明显无响应的窗口先由 `IsHungAppWindow` 拒绝。
- 每个请求和完成结果携带单调递增的 drag generation，旧拖动的迟到结果不会修改
  当前状态。`lastAppliedOrigin` 只在工作线程报告成功后更新。
- 关闭时先拒绝新移动并作废当前 generation，再卸载钩子；工作线程最多等待 250ms。
  若目标线程仍阻塞，工作线程只保留共享内部状态，不持有 `SuperDragApp` 指针，
  因而不会在窗口销毁后回调已释放对象。
- `IsDragPositionReady()` 决定移动走钩子内快路径（稳态）还是消息线程慢路径
  （开始待处理、最大化恢复中、释放待处理、移动失败）。
- 最大化窗口先通过 `ShowWindowAsync(SW_RESTORE)` 恢复，再按鼠标相对位置重新计算锚点。
- 恢复状态每 16ms 检查一次，最多 30 次。
- 拖动期间左键按下和释放被吞掉，防止目标窗口控件误触；`WM_MOUSEMOVE`
  在提交最新坐标后继续传给系统，否则低级钩子会阻止光标位置更新并造成窗口抖动。
- 拖动激活期间有 500ms 看门狗：根据系统主/次键映射检查逻辑左键；按键已抬起但
  状态机仍 active 时强制 `EndDrag`，并重装可能因超时被 Windows 静默移除的钩子，
  防止永久吞掉后续点击或后续拖动失效。

### 重要历史与限制

不要直接恢复为“吞掉左键后投递 `WM_NCLBUTTONDOWN + HTCAPTION`”的方案。该方案已经实测无法拖动：真实左键按下被钩子吞掉后，目标窗口缺少原生移动循环所需的输入状态。

不要在钩子/UI 线程直接同步 `SetWindowPos`，否则目标繁忙会阻塞低级钩子并触发
`LowLevelHooksTimeout`。也不要在每个鼠标事件上提交 `SWP_ASYNCWINDOWPOS`；实际
Windows 验证中这会让目标队列积压，表现为原地抖动或严重滞后。同步调用必须保持在
串行工作线程内，钩子只发布最新坐标。

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

设置保存会先快照四个原注册表值；任一写入失败时恢复已经写入的值。开机启动修改
失败时也会恢复主设置，并在恢复本身失败时向用户报告。

## 8. 设置窗口 UI

设置页使用原生子窗口控件，但复选框、按钮和分组采用 owner-draw。

特别注意：

- 复选框状态保存在控件的 `GWLP_USERDATA`，不使用 `BM_GETCHECK/BM_SETCHECK`。
- 不要把 `BS_OWNERDRAW` 与其他 `BS_*` 按钮类型直接组合；默认保存按钮通过
  `DM_GETDEFID` 提供键盘语义，选中状态仍由 `GWLP_USERDATA` 管理。
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

- 所有库、应用和测试目标必须保持相同的 `/MT` 运行时设置，否则会出现
  `LNK2038 RuntimeLibrary` 不匹配。

## 10. 测试状态

自动化测试覆盖：

- 默认设置。
- 1–3 个修饰键校验。
- 精确修饰键匹配。
- 普通拖动坐标。
- 最大化恢复坐标。
- 负坐标显示器。
- 无效窗口过滤。
- 串行移动、最新坐标覆盖、generation 更新和错误结果传递（Windows CTest）。

当前环境已使用 Apple Clang 的 `-Werror` 编译并运行核心测试；移动线程也通过
Win32 API 桩下的严格编译和并发测试。

尚未在当前环境验证：

- 最新版本的 MSVC 完整构建。
- 实际 Windows 拖动是否完全消除抖动。
- 设置页点击是否不再消失。
- 最大化、贴靠、多 DPI 的最终运行效果。

这些项目必须在 Windows 10/11 上按 [WINDOWS_QA.md](WINDOWS_QA.md) 验收。

## 11. 当前 Git 状态

- 本说明不硬编码 HEAD；开始工作前使用 `git log -1 --oneline` 获取当前提交。
- `ui_theme.cpp`、`ui_theme.h`、`layout.h`、资源和设计文档均已纳入版本控制。
- 工作区可能仍包含 review 修复；应以 `git status --short` 为准，不得假定工作区干净。

其他 agent 开始工作前应先执行：

```powershell
git status --short
git diff --check
git diff -- CMakeLists.txt src/app.cpp src/app.h src/settings_store.cpp scripts/generate_icons.py src/assets
```

不得通过重置工作区来“恢复干净状态”，否则会丢失当前修复并可能导致工程无法构建。
