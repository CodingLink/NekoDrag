# SuperDrag 项目技术概述

> 本说明基于当前工作区，而不是仅基于 Git HEAD。

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

诊断拖动问题时可用 `-DSUPERDRAG_TRACE=ON` 构建。钩子安装、原生移动循环、
兼容回退、耗时汇总及拖动结束原因通过 `OutputDebugString` 输出，可用 DebugView
抓取。低级钩子回调不直接输出日志，也不记录每个鼠标移动事件。

## 3. 模块划分

| 模块 | 职责 |
|---|---|
| [main.cpp](../src/main.cpp) | `wWinMain`、DPI Awareness、Common Controls 初始化 |
| [app.cpp](../src/app.cpp) | 生命周期、消息循环、鼠标钩子、拖动状态机、托盘、设置窗口 |
| [app.h](../src/app.h) | `SuperDragApp` 和 `DragState` 定义 |
| [native_move_worker.cpp](../src/native_move_worker.cpp) | 在独立线程运行目标窗口的原生标题栏移动循环 |
| [window_move_worker.cpp](../src/window_move_worker.cpp) | 原生移动被拒绝时合并坐标并串行移动窗口 |
| [core.cpp](../src/core.cpp) | 修饰键校验、拖动坐标计算、窗口候选过滤 |
| [settings_store.cpp](../src/settings_store.cpp) | 注册表设置和开机启动 |
| [layout.h](../src/layout.h) | 设置窗口的 96 DPI 逻辑布局 |
| [ui_theme.cpp](../src/ui_theme.cpp) | 深浅色、高对比度及 owner-draw 控件绘制 |
| [superdrag.rc](../src/superdrag.rc) | 图标和版本资源 |
| [superdrag.manifest](../src/superdrag.manifest) | DPI、Common Controls v6、`asInvoker` 权限 |
| [core_tests.cpp](../tests/core_tests.cpp) | 无第三方框架的核心 CTest |
| [native_move_worker_tests.cpp](../tests/native_move_worker_tests.cpp) | 原生移动线程生命周期与错误路径 CTest |
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
  → 启动原生移动和兼容回退工作线程
  → 监听 EVENT_SYSTEM_MOVESIZESTART/END
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

当前实现是“低级鼠标钩子 + Windows 原生移动循环 + 手动兼容回退”：

```text
WH_MOUSE_LL
  → 跳过 LLMHF_INJECTED 合成输入
  → 检查精确修饰键
  → WindowFromPoint + GA_ROOT
  → 窗口过滤和完整性等级检查
  → 建立 DragState
  → 吞掉真实左键按下，避免客户区控件误触
  → NativeMoveWorker 同步发送 WM_NCLBUTTONDOWN + HTCAPTION
  → 目标 DefWindowProc 进入 SC_MOVE 原生模态循环
  → 移动和左键释放继续传给系统，由 Windows 完成移动、Snap 和恢复
  → EVENT_SYSTEM_MOVESIZESTART/END 确认原生循环状态
  → 原生消息被拒绝且左键仍按住时，切换到串行 SetWindowPos 回退
```

关键行为：

- `CurrentModifierMask()` 使用 `GetAsyncKeyState`。
- `IsExactModifierMatch()` 要求没有额外修饰键。
- `NativeMoveWorker` 同步调用 `SendMessage(WM_NCLBUTTONDOWN, HTCAPTION)`；调用会
  阻塞到目标 `DefWindowProc` 离开 `SC_MOVE` 循环，因此不能在钩子或 UI 线程执行。
- 原生循环开始后 SuperDrag 不再计算或提交窗口坐标。最大化恢复、贴边布局、
  跨屏 DPI、Esc 取消和最终释放位置均由 Windows 处理。
- 如果目标自定义窗口过程忽略原生标题栏消息，调用会在左键仍按住时提前返回。
  状态机等待 100ms 排除 WinEvent 投递延迟，之后才进入手动回退。
- 回退用的 `WindowMoveWorker` 同时最多执行一次同步 `SetWindowPos`，等待请求只有
  一个；高频坐标覆盖旧坐标，不会向目标线程堆积异步位置请求。
- 每个请求和完成结果携带单调递增的 drag generation，旧拖动的迟到结果不会修改
  当前状态。`lastAppliedOrigin` 只在工作线程报告成功后更新。
- 关闭时两个工作线程先拒绝请求并作废 generation；最多等待 250ms。阻塞线程只
  保留共享内部状态，不持有 `SuperDragApp` 指针，可以安全 detach。
- 原生状态检测到左键释放后最多等待工作线程返回 1 秒；超时则替换原生工作线程。
  每次原生拖动结束都会重装低级钩子，恢复可能被 Windows 静默移除的 HHOOK。
- 手动回退继续使用原有最大化恢复、500ms 最终坐标等待和按键看门狗。

### 重要历史与限制

不要把原生启动改回异步 `PostMessage(WM_NCLBUTTONDOWN)`：真实左键按下已经被
钩子吞掉，异步投递无法可靠建立系统移动循环。当前方案必须在独立线程使用同步
`SendMessage`，并保持物理按键状态直到目标窗口进入 `SC_MOVE`。

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
