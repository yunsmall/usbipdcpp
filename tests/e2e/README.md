# e2e 端到端测试

本目录的测试脚本在 WSL2（Ubuntu）中运行：启动 mock 服务器 → 本地 usbip 客户端
attach → 验证虚拟设备行为（网络/串口/块设备/HID/音频/视频）。

> **建议**：有条件者尽量在虚拟机里跑这套测试（WSL2 的 /dev 里直接就有
> Windows 的物理硬盘，一样可能造成破坏），避免测试中的设备枚举、网络改动、
> 块设备操作影响物理机上的日常环境。

## 依赖

| 依赖 | 用途 | 说明 |
|---|---|---|
| WSL2（Ubuntu） | 测试环境 | 脚本在 WSL 内执行；Windows 侧用 `wsl -e bash` 调 |
| `sudo` 免密 | usbip attach/detach 必需 | vhci 写 /sys 要 root；非免密时脚本会卡在密码输入 |
| usbip 客户端（`/usr/local/sbin/usbip`） | attach/list/detach | 内核 usbip 用户层工具，支持 `-t <端口>`；**必须 sudo** |
| usbip-win2 vhci 内核模块 | USB 总线模拟 | `lsmod \| grep vhci` 确认已加载；没加载则 attach 失败 |
| 编译产物（`build_wsl2/`） | mock 服务器二进制 | 先 `cmake --build build_wsl2 -j6`（WSL 里编译，依赖走 vcpkg） |
| python3 + pyusb（`python3-usb` 包） | cdc_acm / pipe 数据面测试 | 没有则这两个脚本失败 |
| alsa-utils（aplay/arecord） | audio / speaker 测试 | 缺失时对应脚本 SKIP 或失败 |
| ffmpeg | uvc 取流测试 | 缺失时 SKIP |
| evtest | HID（键盘/鼠标/手柄）测试 | 事件流捕获 |
| iproute2 / util-linux / coreutils | ss、lsblk、blockdev、truncate、cmp 等 | WSL 默认自带 |

## 测试前准备

1. **编译**：`cd 项目根 && cmake --build build_wsl2 -j6`（WSL 里执行，构建目录见
   CLAUDE.local.md；禁开满核，最多 -j6）
2. **确认 vhci 模块已加载**：`lsmod | grep vhci`——没加载时 attach 直接失败
3. **确认 sudo 可用且免密**：`sudo -n true` 返回 0 即可
4. **清理环境**（脚本启动时自动做，但手动跑前确认无碍）：
   - 端口 53240 无残留监听（`ss -tln | grep 53240`）
   - `/dev/input/by-path/`、`/dev/ttyACM*`、`/proc/asound/cards` 无残留设备
5. **磁盘安全**：确认 WSL 里挂着的物理硬盘盘符（`lsblk`），测试中 mock_msc
   的新盘由脚本双路确认，但人为干扰（见下）可能破坏确认逻辑

## 运行

```bash
# 单个测试
bash tests/e2e/test_msc.sh

# 全部（失败不中断，逐个跑）
bash tests/e2e/run_all.sh
```

- 每个脚本独立工作目录（`/tmp/usbip_e2e/<测试名>`），日志在
  `<工作目录>/logs/`；服务器以 `setsid` 后台运行，脚本结束自动清理
- 脚本假设能访问本机端口 53240（可用 `PORT` 环境变量覆盖）

## 测试中禁止做的事

1. **禁止混查与写 MSC 设备**：U 盘测试脚本内部严格分步（确认新盘 → 大小校验 →
   写 → 读回），**人为**在 attach 后手写 `lsblk && dd if=/dev/sdX ...` 这类
   一条命令串联查询与写入——WSL 里还有其他物理硬盘，写错盘符会毁数据
2. **禁止 `pkill -f mock_xxx`**：`-f` 按整条命令行匹配，会把自己执行测试的
   shell 杀掉（命令行里含 mock_xxx 字样）。清理用 `pkill -x mock_xxx`
   （可执行名，超 15 字符会被内核截断，先 `ps -eo comm` 确认真实名）
3. **禁止测试中途手动 attach/detach 其他 usbip 设备**：会污染新设备发现
   （块设备对比、dmesg 增量、input 设备枚举），脚本可能误认或超时
4. **禁止占用 53240 端口**：测试期间端口被其他程序占用，服务器起不来，
   attach 会连到旧服务器（表现诡异：attach 成功但设备是旧的）
5. **禁止测试期间插拔物理 USB 设备**：新插入的物理盘/输入设备会被
   lsblk 对比 / by-path 枚举误认成虚拟设备，轻则测试失败，重则（物理盘
   被当成 mock_msc 盘）写入错误设备
6. **禁止手动 kill 测试中的 mock 服务器**：脚本管理服务器生命周期，
   手动 kill 会导致脚本的清理逻辑（detach、等端口释放）错乱

## 风险

- **数据损坏（最高风险）**：MSC 测试向块设备写入。脚本防线：
  ① attach 前后 lsblk 快照对比 ② 内核 dmesg 枚举设备名互证（两路必须一致）
  ③ `blockdev --getsize64` 大小必须等于 mock 镜像（2MiB=2097152），任一不符
  **立即中止，不执行任何写**。人为违反"禁止事项"会绕过这些防线
- **残留服务器/旧会话**：上一次崩溃残留的 mock 进程占着端口，新 attach 连到
  旧设备。脚本已处理（sudo 杀端口进程 + 等端口释放），但手动调试时注意
- **残留假 tty 节点**：设备 detach 后 `tee` 等命令可能把 `/dev/ttyACM0` 建
  成普通文件（不是字符设备），后续 stty/读写全部诡异失败。wait_dev 只认
  字符设备（`-c`）防此问题；手动操作时如遇 stty 报
  "Inappropriate ioctl for device"，先 `ls -la /dev/ttyACM*` 看是不是普通文件
- **串口规范模式陷阱**：串口默认 icanon（规范模式）读按行返回，数据无换行
  时读端永远等不到。测试脚本用 python 关掉 icanon/echo 再读写；手动测试
  串口时先 `stty -F /dev/ttyACM0 -icanon -echo -icrnl -onlcr`
- **声卡资源**：WSL2 无物理声卡，audio/speaker 依赖 vhci 枚举的 USB 声卡；
  多个测试连续跑时声卡号会变，脚本按 `/proc/asound/cards` 实时取号
- **ECM 测试改网络**：test_ecm 会往 enx 网卡加 192.168.53.2/24 地址并 ping，
  测试后脚本清理网卡；期间局域网内如有同名网段设备可能冲突

## 各测试说明

| 脚本 | 设备 | 验证方式 |
|---|---|---|
| test_cdc_acm.sh | 虚拟串口 | python(termios) 单 fd 写读，回显精确一致 |
| test_ecm.sh | 虚拟网卡 | enx 网卡 + ping 192.168.53.1 |
| test_msc.sh | U 盘 | lsblk+dmesg 双路确认新盘 → 大小校验 → 扇区 100 写读 cmp |
| test_keyboard.sh | HID 键盘 | evtest 捕获 KEY_A 事件 |
| test_mouse.sh | HID 鼠标 | evtest 捕获 REL 移动事件 |
| test_gamepad.sh | HID 手柄 | evtest 捕获手柄事件 |
| test_cdc_throttle.sh | 串口（节流） | ttyACM 计数上报 |
| test_pipe.sh | vendor 管道 | pyusb bulk OUT→IN 回显 + 周期发送日志 |
| test_audio.sh | UAC 麦克风 | arecord 实际录音 2 秒 |
| test_speaker.sh | UAC 扬声器 | aplay 播 440Hz 正弦波，收流 WAV 频率/峰谷数内容级比对 |
| test_uvc.sh | UVC 摄像头 | ffmpeg 实际拉 5 帧 |

python 辅助脚本：`cdc_acm_test.py`（串口读写）、`pipe_test.py`（libusb 传输）、
`speaker_verify.py`（音频内容比对）。
