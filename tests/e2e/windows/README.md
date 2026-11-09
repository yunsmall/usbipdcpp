# Windows e2e 测试

在 Windows 上真正导入虚拟设备并验证数据面。Windows 的 USB 驱动栈
（usbser/usbvideo/uaudio/usb8023）校验比 Linux 严格，枚举成功不代表数据能通，
所以每个测试都尽量做到"数据真的流动且内容正确"。

## 前置条件

- **管理员权限**：attach/detach 要写系统设备树，普通权限会失败。
- `usbip.exe` 在 PATH（usbip-win2），`ffmpeg` 在 PATH（UVC/UAC 用），
  `python` + `numpy`（WAV 频谱分析）。
- 已编译 `build` 下的 `mock_*.exe`。
- 端口 53240 空闲（测试会自行检查）。

## 运行

```powershell
# 全部
powershell -NoProfile -File tests\e2e\windows\run_all.ps1

# 单个
powershell -NoProfile -File tests\e2e\windows\test_uac_speaker.ps1
```

可选参数：`-Port <端口>`、`-BusId <busid>`、`-BuildDir <编译目录>`。

每个测试自己负责：

1. 启动 mock 服务器；
2. `usbip attach`；
3. 等 PnP 设备出现（轮询，不是死等）；
4. 验证枚举/数据面；
5. **先 `usbip detach` 并轮询确认端口真正消失，再停服务器**。

最后一步是硬约束：未 detach 直接断 socket 会让 usbip-win2 处于异常状态，
极易蓝屏。`common.ps1` 的 `Stop-ServerSafely` 会检查设备是否仍被导入，
不满足就拒绝停服务器。

## 测试项

| 脚本 | 覆盖 |
|---|---|
| `test_cdc_acm.ps1` | 枚举为 COM 口，串口回显 |
| `test_hid.ps1` | 键盘/鼠标/游戏手柄枚举成对应 HID 类别 |
| `test_keyboard_led.ps1` | 键盘 LED：默认只验证枚举；`-TestInput` 才切换 Caps Lock 验证 SET_REPORT(Output)（测试前后恢复） |
| `test_multi_interface_hid.ps1` | 复合 HID：同一配置里的鼠标与键盘各自枚举成对应子设备 |
| `test_msc.ps1` | 枚举为 USB 磁盘，容量 2 MiB（只读校验，不写盘） |
| `test_rndis.ps1` | Remote NDIS 网卡 Up，ping + TCP echo |
| `test_stress_rndis.ps1` | RNDIS 网络压力：默认 60s 循环 8KB TCP echo，校验无失败、进程存活 |
| `test_uac_mic.ps1` | 麦克风录音 2 秒，数据量合理 |
| `test_uac_speaker.ps1` | 播放 440 Hz 正弦，FFT 验证接收端主峰 |
| `test_uvc.ps1` | 摄像头抓一帧 JPEG |
| `test_reconnect.ps1` | 重连：detach→attach、停服→重启→attach，每次用串口回显验证 |
| `test_repeat_import.ps1` | 重复导入：设备占用时第二个客户端 import 必须失败，原会话不受影响 |
| `test_multi_devices.ps1` | 多设备：一个服务器导出 3 台设备同时 import，detach 一台不影响其余 |

HID 测试用 `--no-demo` 启动 mock：键盘会自动敲 A、鼠标会移动光标，
自动化测试里绝不能向宿主机注入输入。

UAC 扬声器测试只验证"收到字节"不够：`uaudio.sys` 可能收下格式错误的数据。
脚本播放已知频率正弦，再用 `analyze_wav.py`（numpy）对接收文件做 FFT，
并逐段检查主峰，防止"整体正确但中间丢块/错位"。

## MSC 安全

Windows 会自动给 U 盘分配盘符并挂载卷。`test_msc.ps1` 只读容量和序列号，
不写盘、不挂载文件系统。不要在该设备上执行 `format`/`diskpart clean`
等操作——`Get-Disk` 里多个 USB 磁盘时按 `SerialNumber -like 'SBIPDCPSN*'`
定位，先确认盘号再操作。

**测试期间不要插拔、添加或移除任何磁盘**（U 盘、移动硬盘、`diskpart`/存储池操作）：
盘符和 `Get-Disk` 的磁盘号会随磁盘增删变化，脚本或手工命令按盘号定位时可能指向
别的盘。确需改动，先停掉测试。

## 日志

每个测试的服务器 stdout/stderr、抓帧、录音、接收 WAV 都在
`$env:TEMP\usbip_e2e_win\<测试名>\`。
