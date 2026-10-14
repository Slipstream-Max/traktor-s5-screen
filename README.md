# Traktor S5 Screen

Windows 11 ARM64 上针对 Traktor Kontrol S5 屏幕的一套工具：一个接管 S5 显示接口的内核驱动，以及一个不依赖 Traktor 的独立屏幕播放器。

## 项目

| 目录 | 说明 |
| --- | --- |
| [Traktor S5 Screen Driver](./Traktor%20S5%20Screen%20Driver/) | ARM64 KMDF 驱动（`s5usb.sys`）：接管 `USB\VID_17CC&PID_1420&MI_04` 显示接口，让 Traktor / VirtualDJ 能在 ARM64 Windows 上正常驱动 S5 显示画面。 |
| [S5 Screen Player](./S5%20Screen%20Player/) | 独立播放器：把图片 / 视频转换成 480×272 的 RGB565 帧，经驱动直接写入左右屏幕，不需要运行 Traktor。 |

两个项目各自独立构建：驱动需要编译、签名并安装；播放器依赖已安装的驱动运行。构建、安装与使用细节见各目录下的 README。

