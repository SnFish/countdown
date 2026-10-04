# 语音倒计时

轻量的 Windows 原生 C++ 倒计时器，版本 **0.1.0**。单文件运行，使用系统语音，无广告、联网功能或第三方运行时依赖。支持 Windows 10 1703 及以上和 Windows 11。

## 使用

从 [Releases](https://github.com/SnFish/countdown/releases/latest) 下载 Windows x64 压缩包，解压后运行 `countdown.exe`。

| 操作 | 功能 |
| --- | --- |
| 单击时间 | 开始、暂停、继续；结束后重新开始 |
| 双击时间 | 复位 |
| 指向时、分、秒后滚动 | 调整对应数值，下划线提示当前单位 |
| 拖动窗口 | 移动位置 |
| 单击锁图标 | 锁定或解锁，防止误触 |
| 右键或托盘菜单 | 设置、置顶、语音、隐藏、退出 |

快捷键：`Ctrl+Alt+Space` 开始或暂停，`Ctrl+Alt+R` 复位，`Ctrl+Alt+L` 锁定，`Ctrl+Alt+X` 退出。

默认时长 `04:50`，可调范围 `00:01–05:00`，启用最后 45 秒的语音倒数，音量 70%。可在设置中修改范围、配色和不透明度；自然倒计时始终继续到零。优先使用已安装的普通话语音；语音不可用或关闭时，到时使用系统提示音。

配置仅保存在 `%LOCALAPPDATA%\Countdown\settings.ini`，不存在时使用内置默认值。首次启动位于屏幕上部并水平居中，后续保存窗口位置。

## 构建

需要 C++17 编译器，可使用 MinGW-w64，或安装了 Windows SDK 的 Visual Studio。以下构建方式任选其一，在项目目录执行。

MinGW-w64（将 `g++` 和 `windres` 加入 `PATH`）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
```

也可通过 `-Compiler` 指定 `g++.exe` 的完整路径。

CMake 与 MinGW-w64（另需 Ninja）：

```powershell
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake 与 Visual Studio（开发者终端）：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

发布产物为 `dist/countdown.exe` 和许可证文件；`build` 和 `dist` 均不纳入源码提交。测试覆盖计时精度、暂停继续、调时、范围限制和结束后重启。Visual Studio 构建尚未在本机验证。

## 许可证

[MIT](LICENSE)
