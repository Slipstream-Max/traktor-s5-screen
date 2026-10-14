# Traktor S5 Screen Driver

Windows 11 ARM64 上的 Traktor Kontrol S5 屏幕驱动（KMDF，`s5usb.sys`）。它接管 S5 的显示接口（`USB\VID_17CC&PID_1420&MI_04`，接口类 `FF/BD`，Bulk OUT `0x03`），让 Traktor 或 VirtualDJ 能在 ARM64 Windows 上正常驱动 S5 显示画面。

> 这是社区实现，不是 Native Instruments 官方组件。

## 工作原理

- S5 的显示功能是复合 USB 设备中的一个 vendor-specific 接口（MI_04）。`s5usb` 让该接口改由本驱动绑定，并发布 `NIHardwareService` 现场搜索时使用的设备接口 GUID（`c757f7b0-a56e-4d3d-b8d4-9516e932caf7`），使 HWS 能重新发现显示功能并完成握手。
- 驱动实现了抓包确认过的控制路径：
  - `0x80892004`：转发真实设备的 USB 设备描述符 / 字符串描述符（HWS 依赖其中的序列号与 HID 控制器关联）。
  - `0x80892804`：返回已观测到的 12 字节能力记录。
  - `0x80892880`：原始 Bulk 写入路径（单次 ≤ 4 MiB，超时 2 秒），把 480×272 帧数据直接写入屏幕端点；同仓库的 [S5 Screen Player](../S5%20Screen%20Player/) 独立播放时使用的就是这条路径。

## 目录结构

```text
TraktorS5ScreenDriver.sln    Visual Studio 解决方案入口
.vsconfig                    Visual Studio Installer 组件清单
src/
  s5usb.vcxproj              ARM64 KMDF 驱动工程
  s5usb.c                    驱动实现
  s5usb.h                    驱动声明、IOCTL 与接口 GUID
  s5usb.inf                  设备匹配与安装信息
tests/
  test_s5_protocol.py        已捕获协议样本检查（离线，不连接设备）
build/                       构建输出（不纳入版本管理）
```

## 环境要求

- Windows 11 ARM64（已在真机验证）
- Visual Studio 2026 或 Build Tools（含 ARM64 C++ 工具链与驱动开发组件），以及 Windows 11 WDK（26100）
  - 在 Visual Studio Installer 的“更多 → 导入配置”中导入本项目目录下的 `.vsconfig`，可自动补齐所需组件；WDK 安装见[官方说明](https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk)。
- 管理员权限（签名、安装驱动、修改启动配置）

## 构建

1. 创建测试签名证书：

```powershell
$s5Cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=S5 Screen Driver Test' -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable
```

2. 编译带签名驱动（产物在 `build\ARM64\Release\`）：

26100 WDK 自带的 MSBuild 任务程序集版本为 17.0；即使使用 VS 2026，编译时也要指定该 WDK 任务版本。

```powershell
msbuild .\TraktorS5ScreenDriver.sln /m /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:VisualStudioVersion=17.0 /p:SignMode=TestSign /p:GenerateTestCertificate=false /p:CertificateStoreName=My "/p:TestCertificate=$($s5Cert.Thumbprint)"
```

3. 导出安装用的公共证书（构建未自动生成 `s5usb.cer` 时使用）：

```powershell
Export-Certificate -Cert $s5Cert -FilePath .\build\ARM64\Release\s5usb.cer
```

## 安装

1. 关闭 Traktor；在 UEFI 中关闭 Secure Boot。以管理员身份开启测试签名：

```powershell
bcdedit /set testsigning on
```

2. 导入证书并安装驱动：

```powershell
Import-Certificate -FilePath .\build\ARM64\Release\s5usb.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath .\build\ARM64\Release\s5usb.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher

Stop-Service NIHardwareService
try {
    pnputil /add-driver .\build\ARM64\Release\package\s5usb.inf /install
} finally {
    Start-Service NIHardwareService
}
```

如果 Windows 提示需要重启，先重启再打开 Traktor。

3. 核对显示接口实际使用的驱动：

```powershell
Get-PnpDevice -PresentOnly |
    Where-Object InstanceId -like 'USB\VID_17CC&PID_1420&MI_04\*' |
    ForEach-Object {
        $_ | Format-List Status,FriendlyName,InstanceId
        Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Service,DEVPKEY_Device_DriverVersion,DEVPKEY_Device_DriverInfPath
    }
```

预期设备状态为 `OK`，服务为 `s5usb`，版本与构建出的 INF 相符。

> “包已添加”不等于设备已切换驱动。若本机已有版本号更高的实验包，`pnputil` 可能保留旧绑定；在设备管理器中为 **MI_04** 选择“更新驱动程序 → 浏览我的电脑 → 让我选取 → 从磁盘安装”，指定本次生成的 INF。

## 卸载

关闭 Traktor，先用上面的查询确认本驱动实际对应的 `oemNNN.inf`，然后在管理员终端执行（将示例名称替换为实际值）：

```powershell
pnputil /delete-driver oemNNN.inf /uninstall
pnputil /scan-devices
```

## 测试

`tests/test_s5_protocol.py` 对捕获到的 HWS 初始化帧、IOCTL 数值、能力记录与设备描述符身份做离线回归检查（不连接设备，也不参与驱动构建）：

```powershell
python .\tests\test_s5_protocol.py
```

## 已验证范围与限制

- 屏幕写入路径（`0x80892880`）已在真机验证可用，同仓库播放器可以独立播放图片 / 视频。
- 驱动仍保留原型的 `0x80892014` / `0x80892018` 固定返回分支，对应原厂控制协议尚未完整实现。
- 安装需要关闭 Secure Boot 并开启测试签名（见上文安装步骤）。

