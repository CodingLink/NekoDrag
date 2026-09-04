# NekoDrag 项目技术概述

> 本说明基于当前工作区，而不是仅基于 Git HEAD。

## 1. 项目定位

NekoDrag 是一个原生 C++17/Win32 Windows 10/11 x64 托盘程序。

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
cmake -S . -B build-vs18-nekodrag -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build build-vs18-nekodrag --config Release --parallel
ctest --test-dir build-vs18-nekodrag -C Release --output-on-failure
```

仓库改名后不得复用含旧绝对路径的 CMake 缓存。运行前应先退出托盘中的旧版：

```powershell
.\build-vs18-nekodrag\Release\NekoDrag.exe
```

诊断拖动问题时可用 `-DNEKODRAG_TRACE=ON` 构建。钩子安装、原生移动循环、
兼容回退、耗时汇总及拖动结束原因通过 `OutputDebugString` 输出，可用 DebugView
抓取。低级钩子回调不直接输出日志，也不记录每个鼠标移动事件。
旧缓存变量 `SUPERDRAG_TRACE` 会被一次性映射为 `NEKODRAG_TRACE`，配置时显示弃用
提示并从缓存删除；新脚本和文档不得继续使用旧名称。

## 3. 模块划分

| 模块 | 职责 |
|---|---|
| [main.cpp](../src/main.cpp) | `wWinMain`、DPI Awareness、Common Controls 初始化 |
| [app.cpp](../src/app.cpp) | 生命周期、消息循环、鼠标钩子、拖动状态机、托盘、设置窗口 |
| [app.h](../src/app.h) | `NekoDragApp` 和 `DragState` 定义 |
| [native_move_worker.cpp](../src/native_move_worker.cpp) | 在独立线程运行目标窗口的原生标题栏移动循环 |
| [window_move_worker.cpp](../src/window_move_worker.cpp) | 原生移动被拒绝时合并坐标并串行移动窗口 |
| [core.cpp](../src/core.cpp) | 修饰键校验、拖动坐标计算、窗口候选过滤 |
| [settings_store.cpp](../src/settings_store.cpp) | 注册表设置和开机启动 |
| [layout.h](../src/layout.h) | 设置窗口的 96 DPI 逻辑布局 |
| [ui_theme.cpp](../src/ui_theme.cpp) | 深浅色、高对比度及 owner-draw 控件绘制 |
| [nekodrag.rc](../src/nekodrag.rc) | 图标和版本资源 |
| [nekodrag.manifest](../src/nekodrag.manifest) | DPI、Common Controls v6、`asInvoker` 权限 |
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
  → 同时占用 NekoDrag 与 legacy SuperDrag 单实例互斥量
  → 优先加载 HKCU\Software\NekoDrag；缺失时导入旧设置
  → 创建隐藏消息窗口
  → 启动原生移动和兼容回退工作线程
  → 监听 EVENT_SYSTEM_MOVESIZESTART/END
  → 添加托盘图标
  → 将导入的旧设置写入新键（失败只警告）
  → 校准开机启动路径
  → Enabled=true 时投递 WH_MOUSE_LL 安装消息
  → 首次运行显示设置窗口
  → GetMessage 消息循环
  → 安装钩子；瞬时失败时按 250ms/1s/3s 重试
```

第二实例通过 `FindWindow` 找到隐藏窗口，并投递打开设置消息，不创建第二个驻留进程。
升级兼容检查还会检测旧窗口类和 `Local\SuperDrag.SingleInstance`；这些 legacy
常量只用于避免新旧进程同时安装鼠标钩子。

收到 Explorer 的 `TaskbarCreated` 消息后会重新添加托盘图标。

## 5. 当前拖动实现

当前实现是“低级鼠标钩子 + 默认兼容移动 + 实验性 Windows
原生移动循环”：

```text
WH_MOUSE_LL
  → 跳过 LLMHF_INJECTED 合成输入
  → 检查精确修饰键
  → WindowFromPoint + GA_ROOT
  → 窗口过滤和完整性等级检查
  → 建立 DragState
  → 按 DragEngineMode 快照按下路由：兼容吞键，实验性原生路径放行真实按下
  → 原生路径等待首次真实鼠标移动，并确认钩子与异步键状态均为按下
  → NativeMoveWorker 尽力取消客户区交互
  → 最大化目标先执行带超时的 SC_RESTORE，并按相对鼠标锚点定位
  → 同步发送 WM_NCLBUTTONDOWN + HTCAPTION
  → 100ms 内没有匹配的 move-start 时再尝试 WM_SYSCOMMAND(SC_MOVE | HTCAPTION)
  → 目标 DefWindowProc 进入 SC_MOVE 原生模态循环
  → 原生循环激活后，移动和真实左键释放继续传给系统
  → EVENT_SYSTEM_MOVESIZESTART/END 确认原生循环状态
  → 原生消息被拒绝且左键仍按住时，切换到串行 SetWindowPos 回退
```

关键行为：

- `CurrentModifierMask()` 使用 `GetAsyncKeyState`。
- `IsExactModifierMatch()` 要求没有额外修饰键。
- 每次手势在开始时快照 `DragEngineMode`：兼容模式（推荐）不提交原生
  请求；自动模式（实验）优先尝试原生并允许兼容回退；原生模式（诊断）
  禁止所有回退。
- Chromium 自绘窗口（类名 `Chrome_WidgetWin_0/1`，Edge/Chrome/Electron
  等）会忽略针对客户区的合成原生 caption 移动消息，原生移动循环永不启动；
  自动模式检测到这类窗口时跳过原生尝试、直接走兼容路由，原生模式保持尝试
  以作诊断。
- 原生模式激活目标后进入 `NativeAwaitingMovement`，并让真实主按钮按下进入
  Windows 输入队列；兼容模式仍吞掉按下。首次真实、非注入 `WM_MOUSEMOVE`
  到达后，只有钩子观察状态和 `GetAsyncKeyState` 都仍为按下才提交 attempt 1；
  重复移动不会重复提交。未移动便释放时放行真实释放并按普通点击结束。
  `SM_SWAPBUTTON` 用于选择实际主按钮以及竞态释放补发。
- attempt 1 派发前通过 `GetGUIThreadInfo` 获取目标线程捕获窗口，并按捕获窗口、
  初始子窗口、根窗口的去重顺序同步发送 `WM_CANCELMODE`。只接受根窗口属于拖动
  目标的窗口。清理失败或超时不会发送原生移动消息，也不会进入兼容回退；后续
  真实释放继续放行。该清理是最佳努力，控件仍可能已经处理焦点或选择变化。
- 最大化原生手势在放行按下前安装一个仅用于本次手势的 `WH_KEYBOARD_LL`。
  安装失败时自动模式改走兼容路由，仅原生模式拒绝启动，因此不会在无法观察
  Esc 的情况下预恢复窗口。钩子只记录真实、非注入的 Esc 按下并继续传递该按键。
- attempt 1 清理交互后，对最大化目标同步发送带超时的 `SC_RESTORE`，等待窗口
  退出最大化状态，再用实际恢复尺寸和 `ComputeRestoredOrigin` 保持鼠标相对锚点。
  定位成功且取消令牌仍未置位时才发送标题栏消息；attempt 2 不重复清理或恢复。
  恢复或定位失败时不派发原生移动消息。
- `NativeMoveWorker` 同步调用带挂起检测的 `SendMessageTimeout`。attempt 1 使用
  `WM_NCLBUTTONDOWN + HTCAPTION`；消息返回后 100ms 内没有匹配的
  `EVENT_SYSTEM_MOVESIZESTART`，且主按钮仍按下、取消令牌未置位时，attempt 2
  使用 `WM_SYSCOMMAND(SC_MOVE | HTCAPTION)`，并在 `lParam` 传递当前屏幕坐标。
  每种策略每次手势最多执行一次。成功调用会阻塞到目标 `DefWindowProc` 离开
  `SC_MOVE` 循环，因此不能在钩子或 UI 线程执行。
- 两个 attempt 共享同一原子取消令牌。工作线程在调用
  `SendMessageTimeoutW` 前再次检查；若已观察到左键释放，返回
  `ERROR_CANCELLED` 而不再启动 `SC_MOVE`。
- 已放行按下但开始消息未处理或尚在 `NativeAwaitingMovement` 时，释放也放行并
  作为普通点击结束。`NativeStarting` 中的真实释放会被吞掉，并在退出原生状态前
  补发一次匹配物理主按钮的释放；原生已激活时真实释放直接交给 Windows。
- 如果取消与消息派发发生竞态，只在真实、非注入释放已被观察到，且
  generation、目标窗口和 `EVENT_SYSTEM_MOVESIZESTART` 全部匹配时，补发一次
  带 NekoDrag 标记、与交换键设置相符的物理主按钮释放事件。补发失败则向目标
  发送 `WM_CANCELMODE`。
- 原生循环成功开始后 NekoDrag 不再计算或提交窗口坐标。最大化窗口的首次恢复
  和锚点定位由 NekoDrag 在循环前完成；后续贴边布局、跨屏 DPI 和移动循环取消
  由 Windows 处理。若本次临时键盘钩子观察到 Esc，工作线程返回后窗口会重新
  最大化。该路径是最佳努力功能，不承诺所有第三方窗口都与标题栏拖动完全一致。
- 如果目标窗口过程忽略第一种原生标题栏消息，调用会在主按钮仍按住时提前
  返回。状态机等待 100ms 排除 WinEvent 投递延迟后尝试一次系统命令；第二种
  策略仍未产生匹配的 move-start 时，自动模式进入兼容回退，仅原生模式结束
  当前拖动并每个运行周期最多通知一次。
- 最大化目标已恢复但两种策略都未启动时，自动模式从当前恢复位置进入兼容移动；
  仅原生模式重新最大化。目标已退出或句柄失效时只清理状态，不操作窗口。
- 回退用的 `WindowMoveWorker` 同时最多执行一次同步 `SetWindowPos`，等待请求只有
  一个；高频坐标覆盖旧坐标，不会向目标线程堆积异步位置请求。
- 每个请求和完成结果携带 drag generation、strategy 和 attempt；每个 attempt
  另分配单调递增的 WinEvent token，并用事件生成时间过滤开始前的事件。因此
  第一种策略或旧拖动的迟到事件不会被第二种策略或新拖动接受。
  `lastAppliedOrigin` 只在工作线程报告成功后更新。
- 关闭时两个工作线程先拒绝请求并作废 generation；最多等待 250ms。阻塞线程只
  保留共享内部状态，不持有 `NekoDragApp` 指针，可以安全 detach。
- 原生状态检测到左键释放后最多等待工作线程返回 1 秒；超时会再次
  发送 `WM_CANCELMODE`，暂停原生路径并让
  后续自动模式使用兼容路径，迟到的工作线程返回后再恢复原生路径。这样同一时刻
  最多只有一个潜在阻塞线程，不会反复 detach 并累积。每次原生拖动结束都会重装
  低级钩子，恢复可能被 Windows 静默移除的 HHOOK。
- 兼容路径继续使用原有最大化恢复和 500ms 最终坐标等待，以钩子观察到的真实
  左键释放结束拖动。该路径只通过 `SetWindowPos` 移动窗口，不模拟 Windows
  Snap、跨屏 DPI 重算或 Esc 回滚。

### 重要历史与限制

不要把原生启动提前到低级钩子的按下回调：此时真实主按钮状态尚未完成建立，标准
窗口可能立即退回且不会产生 move-start。当前方案必须等待首次真实移动，再在独立
线程使用同步 `SendMessageTimeout`，并使用钩子观察到的按键释放结束或回退拖动。

不要在钩子/UI 线程直接同步 `SetWindowPos`，否则目标繁忙会阻塞低级钩子并触发
`LowLevelHooksTimeout`。也不要在每个鼠标事件上提交 `SWP_ASYNCWINDOWPOS`；实际
Windows 验证中这会让目标队列积压，表现为原地抖动或严重滞后。同步调用必须保持在
串行工作线程内，钩子只发布最新坐标。

## 6. 窗口过滤和权限

以下目标不会进入拖动：

- NekoDrag 自身窗口。
- 桌面、Shell、主/副任务栏。
- 菜单窗口、工具提示。
- 隐藏、禁用、最小化或已销毁窗口。
- 子窗口、DWM cloaked 窗口。
- `WS_EX_NOACTIVATE` 临时窗口。
- 高于 NekoDrag 完整性等级的进程。

无法查询目标进程完整性时按受限窗口处理。管理员或受保护窗口不会触发 UAC，只显示一次托盘说明。

## 7. 设置和注册表

主设置位置：

```text
HKCU\Software\NekoDrag
```

值：

- `Enabled`
- `ModifierMask`
- `DragMode`
- `FirstRunCompleted`
- `PrivilegeHintShown`

`ModifierMask` 与 Win32 `MOD_*` 位值一致：

- Alt：`0x0001`
- Ctrl：`0x0002`
- Shift：`0x0004`
- Win：`0x0008`

`DragMode` 为 DWORD：

- 自动模式（实验）：`0`
- 原生模式（诊断）：`1`
- 兼容模式（推荐）：`2`

新安装、缺失值或非法值均使用兼容模式。已保存的 `0`/`1`/`2` 值保持
原有语义，不进行静默迁移。

开机启动位置：

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run
值名：NekoDrag
```

值内容是带双引号的当前 EXE 绝对路径。程序启动时会在开机启动已开启的情况下校准路径。

设置保存会先快照五个原注册表值；任一写入失败时恢复已经写入的值。开机启动修改
失败时也会恢复主设置，并在恢复本身失败时向用户报告。

升级规则：

- 新设置键存在时始终优先，不再读取旧键。
- 新设置键缺失而 `HKCU\Software\SuperDrag` 存在时，读取旧值且不再显示首次运行页；
  托盘初始化后写入新键，旧键保留以便回退。
- 开机启动值名为 `NekoDrag`。仅当本次确实导入旧设置且旧 `SuperDrag` 值是
  指向 `SuperDrag.exe` 的无参数、带引号路径时，才先写入当前 `NekoDrag.exe`
  的带引号绝对路径再删除旧值；删除失败会恢复新值原状。其他同名值保持不变。
- 关闭开机启动只删除 NekoDrag 自己的值，不修改未确认归属的旧值。

## 8. 设置窗口 UI

设置页使用原生子窗口控件，但复选框、按钮和分组采用 owner-draw。

特别注意：

- 复选框状态保存在控件的 `GWLP_USERDATA`，不使用 `BM_GETCHECK/BM_SETCHECK`。
- 拖动模式使用三枚互斥 owner-draw 单选控件，选中状态同样保存在
  `GWLP_USERDATA`；顺序为“兼容模式（推荐）”、“自动模式（实验）”、“原生模式（诊断）”。
- 不要把 `BS_OWNERDRAW` 与其他 `BS_*` 按钮类型直接组合；默认保存按钮通过
  `DM_GETDEFID` 提供键盘语义，选中状态仍由 `GWLP_USERDATA` 管理。
- 快捷键分组必须保持为 `STATIC + SS_OWNERDRAW + WS_CLIPSIBLINGS`。
- 设置窗口使用 `WS_CLIPCHILDREN`。
- 每个 owner-draw 控件必须完整填充自身背景，否则点击、焦点变化或主题刷新后可能出现控件消失。
- 分组内 Win/Ctrl/Alt/Shift 使用 Surface 背景，其他复选框使用窗口背景。
- 支持浅色、深色、高对比度和 `WM_DPICHANGED`。
- 最小客户区为 480×500 个 96-DPI 逻辑像素。

## 9. 资源和清单注意事项

- 图标资源位于 `src/assets`，由 `nekodrag.rc` 引用。
- `assets/nekodrag.png` 是非像素、透明背景的高分辨率日系动漫布偶猫 Logo 母版，采用两只前爪伸向镜头的抓取构图。
- `scripts/generate_icons.py` 使用 Pillow/Lanczos 生成 16/20/24/32/48/256px
  应用图标及 16/20/24px 托盘图标。Pillow 只用于开发期资源生成，必须安装在
  项目虚拟环境中；应用构建和运行不依赖 Pillow。
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
- 三种拖动引擎模式的持久化值校验、默认回退和起始路由。
- 开始消息前、等待首次移动、原生排队期间和原生激活后的左键释放决策。
- 首次移动触发条件、重复移动去重，以及两种原生策略的顺序和终止决策。
- 原生取消令牌阻止派发，以及严格匹配下的单次释放补发决策。
- 最大化预恢复、自动回退、仅原生失败和 Esc 的重新最大化决策。
- 迟到 WinEvent 的 attempt token/起始时间过滤及 32 位系统时钟回绕。
- 普通拖动坐标。
- 最大化恢复坐标。
- 负坐标显示器。
- 无效窗口过滤。
- 设置页拖动模式分组在 96/144/192 DPI 下的边界和控件间距。
- 串行移动、最新坐标覆盖、generation 更新和错误结果传递（Windows CTest）。

当前 macOS 环境已使用 Apple Clang 的 `-Werror` 编译并运行核心测试，
并通过 AddressSanitizer/UndefinedBehaviorSanitizer。当前环境没有 Windows SDK，
本次原生工作线程变更仍需在 Windows CTest 中验证。

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
