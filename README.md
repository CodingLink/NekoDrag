# NekoDrag

NekoDrag 是一个轻量的 Windows 10/11 x64 窗口拖动工具。按住所选修饰键（默认 `Win+Alt`），即可从普通窗口的任意位置按住鼠标左键拖动窗口。

![NekoDrag 日系动漫布偶猫双爪头像](assets/nekodrag.png)

## 功能

- 原生 C++17/Win32 实现，无 .NET 或第三方运行时
- 默认 `Win+Alt + 鼠标左键拖动`
- 支持 Win、Ctrl、Alt、Shift 中任意 1 至 3 个修饰键
- 支持从后台窗口开始拖动并将其激活
- 拖动最大化窗口时自动恢复原始尺寸
- 可选择自动、仅 SC_MOVE 或仅兼容模式；自动模式会在原生移动未启动时回退
- 原生系统托盘、设置窗口和可选的当前用户开机启动
- 设置保存在 `HKCU\Software\NekoDrag`，升级时自动导入旧版设置
- 普通权限运行，不请求 UAC

## 构建

需要 Visual Studio 2022，并安装“使用 C++ 的桌面开发”和 CMake 组件。

```powershell
cmake -S . -B build-nekodrag -A x64 -DBUILD_TESTING=ON
cmake --build build-nekodrag --config Release
ctest --test-dir build-nekodrag -C Release --output-on-failure
```

生成的便携程序位于 `build-nekodrag\Release\NekoDrag.exe`。Release 配置使用静态 MSVC 运行时，因此无需随程序分发额外 DLL。旧构建目录含有原仓库的绝对路径，改名后请使用新的空构建目录。

拖动诊断构建使用 `-DNEKODRAG_TRACE=ON`。旧的
`-DSUPERDRAG_TRACE=ON` 仍会在一次配置中映射到新选项，同时显示弃用提示并清除旧缓存变量。

## 使用

1. 首次运行 `NekoDrag.exe`，在设置窗口中确认快捷键和拖动模式。
2. 按住所选修饰键，在目标窗口任意位置按住鼠标左键并移动。
3. 拖动开始后可以先松开修饰键；释放鼠标左键后结束拖动。
4. 通过托盘菜单暂停功能、修改设置、启用开机启动或退出。

只有配置中选中的修饰键被按下时才会启动拖动。例如配置 `Win+Alt` 后，同时按住 `Shift` 不会触发。

## 从旧版升级

- 若 `HKCU\Software\NekoDrag` 尚不存在，程序会读取
  `HKCU\Software\SuperDrag`，并在托盘初始化后写入新位置；旧键不会删除。
- 旧的 `Run\SuperDrag` 开机启动项会在新项写入成功后迁移为
  `Run\NekoDrag`。迁移失败会回滚新值，避免同时启动两个版本。
- 程序同时检测新旧单实例互斥量；升级前无需手动清理注册表，但应先退出旧版进程。

## 已知边界

- 不控制管理员权限窗口、受保护窗口、安全桌面或 UAC 提示。
- 不保证兼容独占全屏游戏及带有反作弊输入保护的软件。
- 首版为未签名的便携程序；Windows SmartScreen 可能对未知发布者显示提示。
- 移动 EXE 后，直接运行一次即可刷新已启用的开机启动路径。
