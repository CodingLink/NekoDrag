# SuperDrag

SuperDrag 是一个轻量的 Windows 10/11 x64 窗口拖动工具。按住所选修饰键（默认 `Win+Alt`），即可从普通窗口的任意位置按住鼠标左键拖动窗口。

## 功能

- 原生 C++17/Win32 实现，无 .NET 或第三方运行时
- 默认 `Win+Alt + 鼠标左键拖动`
- 支持 Win、Ctrl、Alt、Shift 中任意 1 至 3 个修饰键
- 支持从后台窗口开始拖动并将其激活
- 拖动最大化窗口时自动恢复原始尺寸
- 可选择自动、仅 SC_MOVE 或仅兼容模式；自动模式会在原生移动未启动时回退
- 原生系统托盘、设置窗口和可选的当前用户开机启动
- 设置保存在 `HKCU\Software\SuperDrag`
- 普通权限运行，不请求 UAC

## 构建

需要 Visual Studio 2022，并安装“使用 C++ 的桌面开发”和 CMake 组件。

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

生成的便携程序位于 `build\Release\SuperDrag.exe`。Release 配置使用静态 MSVC 运行时，因此无需随程序分发额外 DLL。

## 使用

1. 首次运行 `SuperDrag.exe`，在设置窗口中确认快捷键和拖动模式。
2. 按住所选修饰键，在目标窗口任意位置按住鼠标左键并移动。
3. 拖动开始后可以先松开修饰键；释放鼠标左键后结束拖动。
4. 通过托盘菜单暂停功能、修改设置、启用开机启动或退出。

只有配置中选中的修饰键被按下时才会启动拖动。例如配置 `Win+Alt` 后，同时按住 `Shift` 不会触发。

## 已知边界

- 不控制管理员权限窗口、受保护窗口、安全桌面或 UAC 提示。
- 不保证兼容独占全屏游戏及带有反作弊输入保护的软件。
- 首版为未签名的便携程序；Windows SmartScreen 可能对未知发布者显示提示。
- 移动 EXE 后，直接运行一次即可刷新已启用的开机启动路径。
