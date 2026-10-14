# S5 Screen Player

独立的 S5 屏幕播放器（Windows 11 ARM64）。它打开已经安装的 S5 驱动接口，把图片或视频转换成 480×272 的 S5 RGB565 帧，然后通过 `0x80892880` 交给驱动，驱动再写入 MI_04 的 Bulk OUT `0x03`。

播放器不依赖 Traktor 的画面，也不修改 Traktor。独立使用时应先关闭 Traktor 并停止 `NIHardwareService`，否则播放器和 Traktor 会同时写同一块屏幕，显示内容会互相覆盖。

> 需要先安装同仓库的 [Traktor S5 Screen Driver](../Traktor%20S5%20Screen%20Driver/)：`USB\VID_17CC&PID_1420&MI_04` 需要由 `s5usb.sys` 绑定。

## 功能

- 图片：JPG / PNG / BMP / GIF / TIFF，由 WIC 解码，拉伸到 480×272 后发送一帧并停留在屏幕上。
- 视频：MP4 / MOV 等，由 Media Foundation 解码，按时间戳播放一遍，结束后退出、屏幕停留在最后一帧；当前只播放画面，不播放音轨。
- 左右屏幕可以播放同一个文件，也可以各放各的。
- 只使用 Windows 自带的 WIC 与 Media Foundation，不需要 FFmpeg、Rust crate 或其他外部工具。
- 帧编码按抓包得到的 S5 payload 结构生成（16 字节头、RGB565 双像素对、`0x01` 重复命令、`0x00` 双像素 literal 命令、结束记录），纯色帧压缩后仅 32 字节。

## 目录结构

```text
S5ScreenPlayer.sln             Visual Studio 解决方案入口
src/
  s5_frame.h                   帧编码（480×272 RGB565 → S5 payload）
  s5_screen_player.cpp         播放器（WIC / Media Foundation / IOCTL 写入）
  s5_screen_player.vcxproj     ARM64 工程
tests/
  test_s5_frame.cpp            帧编码离线测试（可校验本地抓包 .bin）
build/                         构建输出（不纳入版本管理）
```

## 构建

使用 Visual Studio 或 Build Tools 的 ARM64 Developer PowerShell。已验证环境：VS 2026 的 v145 C++ 工具集 + Windows SDK 10.0.26100.0；编译播放器需要 C++ ARM64 组件和 Windows SDK，运行时需要已经安装好的 S5 屏幕驱动。

```powershell
msbuild .\S5ScreenPlayer.sln /m /t:Rebuild /p:Configuration=Release /p:Platform=ARM64
```

生成文件：

```text
build\ARM64\Release\s5-screen-player.exe
```

## 使用

```powershell
.\build\ARM64\Release\s5-screen-player.exe --left .\picture.jpg
.\build\ARM64\Release\s5-screen-player.exe --right .\picture.png
.\build\ARM64\Release\s5-screen-player.exe --all .\picture.jpg
```

`--left` 使用屏幕选择值 0，`--right` 使用屏幕选择值 1，`--all` 把同一个输入同时发送给左右两块屏幕；不带参数或 `--help` 时打印用法。

视频同样使用这三个参数：

```powershell
.\build\ARM64\Release\s5-screen-player.exe --left .\demo.mp4
.\build\ARM64\Release\s5-screen-player.exe --right .\demo.mp4
.\build\ARM64\Release\s5-screen-player.exe --all .\demo.mp4
```

MOV 也通过 Media Foundation 解码；是否能打开取决于文件内部的编码和系统解码器。

也可以同时给两块屏幕指定不同的文件：

```powershell
.\build\ARM64\Release\s5-screen-player.exe `
    --left .\left.mp4 `
    --right .\right.jpg
```

独立播放前，在管理员 PowerShell 中停止 NI 服务；播放结束后如果要回到 Traktor，恢复服务并重新打开 Traktor：

```powershell
# 播放前
Stop-Service NIHardwareService
# 播放后，如果要回到 Traktor
Start-Service NIHardwareService
```

退出码：`0` 正常结束；`1` 未提供参数（打印用法）；`2` 参数错误；`3` COM / Media Foundation 初始化失败；`4` 播放过程出错。

## 验证

- 已在真实 S5 上确认左右屏静态照片正常。
- 已确认 `Bad Apple!!.mov`（480×360、源视频 30 fps）在两屏正常播放。该次播放进程以退出码 0 结束，日志未出现 USB 超时；这不代表测得屏幕实际刷新率为 30 fps。
- 编码器的离线测试检查已验证的 32 字节纯色包、双色重复对、完整 literal 帧、奇数长度色块与随机像素的还原结果；不连接设备、不写 USB。

在 ARM64 Developer PowerShell 中可选运行：

```powershell
New-Item -ItemType Directory -Force .\build\tests | Out-Null
cl /nologo /std:c++17 /EHsc /W4 /WX .\tests\test_s5_frame.cpp /Fo.\build\tests\test_s5_frame.obj /Fe.\build\tests\test_s5_frame.exe
.\build\tests\test_s5_frame.exe
```

测试程序也接受本地抓包 `.bin` 文件作为参数，用独立解码器检查命令边界及 65,280 对像素的覆盖。`0x02` 在抓包中表示跳过已有像素，不携带 literal 数据；播放器只用 `0x00` / `0x01` 生成完整帧。

## 前提和限制

- 驱动必须已经安装，并且 `USB\VID_17CC&PID_1420&MI_04` 正在使用 `s5usb.sys`。
- 播放器调用的是驱动的 `0x80892880` 原始 Bulk 写入路径，不发送外层 HWS 的 `Dsd 03` 消息头。
- 当前帧编码依据已经捕获到的 S5 payload 结构：16 字节头、RGB565 双像素对、`0x01` 重复命令、`0x00` 双像素 literal 命令和结束记录。播放器保留完整帧顺序。
- 如果出现 `Could not open the S5 display interface`，先检查驱动服务、MI_04 绑定和是否有别的程序独占设备。

