# libusb 后端真机 e2e（WSL2，不用 Windows）

这套流程用内核自带的 `dummy_hcd` + configfs 造一个“真 USB 设备”，让 libusb 后端像访问物理
设备一样访问它，再由另一个 WSL 发行版通过 USB/IP 导入并做读写验证。全程不经过 Windows 的
usbip-win2（那个驱动 bug 多、容易蓝屏）。

## 拓扑

```
Ubuntu 24.04（导出侧）                         Kali（导入侧）
┌──────────────────────────────┐              ┌──────────────────────────────┐
│ dummy_hcd + libcomposite     │              │ vhci_hcd                     │
│   gadget: mass_storage       │              │   usbip client               │
│   host 侧枚举成 /dev/sdg     │              │   导入后 usbcore 再枚举      │
│                              │              │                              │
│ libusb_server -d -p 53241 ───┼── TCP ──────>│ usbip --tcp-port 53241        │
│   (打开 /dev/bus/usb/...)    │  127.0.0.1   │   attach -r 127.0.0.1 -b 3-1 │
└──────────────────────────────┘              └──────────────────────────────┘
```

关键前提：**所有 WSL2 发行版运行在同一个轻量 VM、同一个内核实例里**，网络命名空间和内核
对象（模块、`/sys`、`/dev`、`dmesg`）都是共享的。因此：

- 一个发行版里 `modprobe dummy_hcd`、创建 gadget，另一个发行版立刻能看到同一个 UDC 和设备
- 导出侧监听 `127.0.0.1`，导入侧就能直接连上（两边 `hostname -I` 相同）
- 这测试的是 libusb 后端 + USB/IP + vhci 链路，**不能**用来测不同内核版本的兼容性

## 自动化用例（FunctionFS 设备 + libusb 客户端）

`tests/e2e/libusb/` 下有一套可重复运行的 e2e：control/bulk/interrupt 的设备侧用
FunctionFS（configfs 的 `functions/ffs.*`）实现，客户端是 libusb 程序；iso 用项目
的虚拟等时设备走两跳链路（dummy_hcd 不支持 iso，见下文）。**这些用例手动运行，
不注册进 ctest**。

```bash
# 在 WSL2 Ubuntu 里、以 root 运行（创建 gadget、打开 usbfs 设备、attach vhci 都需要）
cd <项目根目录>
cmake --build <构建目录> -j6            # 先编译 e2e 目标
sudo tests/e2e/libusb/run_one.sh bulk   # 单个用例
sudo tests/e2e/libusb/run_all.sh        # 五个用例依次跑
```

`run_one.sh --help` / `run_all.sh --help` 列出可用用例、前置条件和环境变量
（`BUILD_DIR`、`USBIP`）。

每个用例的链路：

```
e2e_ffs_<profile>（设备守护进程）
  └─ configfs gadget ffs.<name> → dummy_hcd 的 dummy_udc.0
       └─ 同内核 host 侧枚举出 /dev/bus/usb/... → libusb_server（交互模式，FIFO 发 b <busid>）
            └─ USB/IP（127.0.0.1:5326x）→ vhci_hcd attach
                 └─ e2e_libusb_<profile>_client（libusb 客户端）
```

用例划分（一个设备对应一个客户端）：

| 用例 | VID:PID | 设备侧 | 客户端覆盖 |
|---|---|---|---|
| control | 1d6b:0104 | 接口 0 有 alt 0/alt 1（无端点），厂商请求实现 IN/OUT/STALL/短包/零长/延迟 | 35 项：各长度控制 IN/OUT、STALL、短包、零长包、异步取消、标准描述符读取、GET/SET_CONFIGURATION、GET/SET_INTERFACE 切 alt |
| bulk | 1d6b:0105 | 接口 0 alt 0（0x81/0x02）、alt 1（0x83/0x04）；接口 1 alt 0（无端点）/alt 1（0x85/0x06） | 45 项：IN 各长度（512 整数倍到 1MiB）、零长包、OUT 各长度、16 条并发异步 IN、取消、超时、IN/OUT halt 与 clear_halt 恢复、接口/alt 切换 10 轮（每轮都用当前 alt 的端点收发） |
| interrupt | 1d6b:0106 | 一个接口，interrupt IN 0x81/OUT 0x02，IN 持续输出 8 字节计数块 | 18 项：各长度、跨请求计数器连续性、并发、取消、超时、IN/OUT halt 与 clear_halt 恢复 |
| iso | 1d6b:0107 | 项目虚拟等时设备（不用 gadget）：alt 0 无端点，alt 1 是 1024B iso IN/OUT，alt 2 是 512B iso IN；厂商请求提供统计、IN 延迟和失败包掩码 | 38 项：描述符布局、alt 0/1/2 切换、IN 8/32/64 包与变长/零长包、4 条并发、OUT 三种长度与设备侧字节校验、vendor 请求、不存在的 alt 被拒绝、alt 1↔0 切换 5 轮、alt 0 提交被拒、挂起中 UNLINK 取消、部分包失败（逐包状态 + 设备统计） |
| raw | 1d6b:0107 | 与 iso 共用虚拟设备；不经过第二个 vhci，直接按 USB/IP 线协议连 libusb_server | 19 项：当前 alt 不存在的端点被静默丢弃、wLength 大于 transfer_buffer_length、CLEAR_FEATURE 降级提交、重复 seqnum、ISO number_of_packets=0、非法 ISO 描述符断连；每条错误后验证会话/设备可恢复，以及挂起传输随客户端异常断开被清理 |

客户端不假设端点地址，而是从实际枚举出的配置描述符里按接口/alt 取端点——UDC 自动分配
端点时会改写地址（例如请求 0x83 实际拿到 0x86）。

`run_one.sh` 自动完成 configfs 建 gadget、挂 functionfs、等设备守护进程、启动
`libusb_server`（交互模式，避免 `-d` 的自动绑定把 vhci 导入出来的设备再导出一次）、
attach、跑客户端以及退出清理（trap 里 detach、退出服务端、拆 gadget）。

### iso 用例（虚拟等时设备 + 双跳）

dummy_hcd 不支持 iso，所以 iso 用例不经过 gadget，链路是：

```
e2e_vdev_iso（USB/IP 服务器，项目虚拟设备）
  └─ vhci 控制器 0（端口 0）导入 → libusb_server 用 libusb 打开并再导出
       └─ vhci 控制器 1（端口 16）再导入 → e2e_libusb_iso_client
```

**两个 hop 必须落在不同的 vhci 控制器上。** Linux 内核 `usb_set_interface()`
（`drivers/usb/core/message.c`）在持有 `hcd->bandwidth_mutex` 的情况下同步发送
SET_INTERFACE；同一个 `usb_hcd` 上的两个导入设备共用这把锁，hop2 的 SET_INTERFACE
会等 libusb_server 转发到 hop1 的那次 SET_INTERFACE，而后者又要同一把锁，形成 ABBA
死锁（客户端卡在 `usb_kill_urb`、整台 vhci 设备失联，只能重启 WSL 虚拟机恢复）。
分到两个控制器后是两把锁，alt 切换才能正常测。

内核需要 `CONFIG_USBIP_VHCI_NR_HCS>=2`（本用例用到控制器 0 和 1），
`CONFIG_USBIP_VHCI_HC_PORTS=8`。端口号是全局编号：0–7 是控制器 0 的 hs，8–15 是
控制器 0 的 ss，16–23 是控制器 1 的 hs，以此类推。

`usbip attach` 命令行没有选控制器/端口的选项（`usbip_vhci_get_free_port()` 永远按
顺序取第一个匹配速度的空闲端口，也就是控制器 0），所以 hop2 用 `attach_port.py`
自己完成 `OP_REQ_IMPORT` 握手后把 `"16 <sockfd> <devid> <speed>"` 写进
`/sys/devices/platform/vhci_hcd.0/attach`；hop1 仍走 usbip 命令行。

只编了一个控制器的内核对这个用例会直接报错退出（`run_one.sh` 会检查）。

### raw 用例（直接走 USB/IP 协议的负路径）

libusb 客户端在本地就会拒绝的请求（当前 alt 不存在的端点、`wLength` 超过缓冲区、
`number_of_packets=0`、非法 ISO 描述符）根本到不了服务端，服务端自己的校验、
应答和清理路径因此没有覆盖。`raw_client.py` 用 `usbip_raw.py` 直接连
`libusb_server`，按线协议构造这些请求并断言：

- 当前 alt 不存在的端点按内核 stub 语义静默丢弃（不回任何应答），之后正常请求仍工作
- `wLength > transfer_buffer_length` 回 EPIPE 且不断连（防 libusb 按 wLength 越界写）
- 对当前 alt 里存在但不可 halt 的等时端点发 CLEAR_FEATURE：`libusb_clear_halt` 失败后
  服务端降级为普通控制传输，设备按 Request Error 回 EPIPE，会话不断（不能改用完全不存在的
  端点：usbfs 的 `check_ctrlrecip()` 会在提交阶段按描述符校验 wIndex，返回 ENOENT）
- 重复 seqnum 回 EPIPE，原传输仍正常完成
- ISO `number_of_packets=0` 按策略断开会话：严格策略下与内核 `SDEV_EVENT_ERROR_SUBMIT`
  一致，直接丢连接、不保证回 RET_SUBMIT；设备回到可用列表后可重新导入
- 非法 ISO 描述符（长度和超过缓冲区）直接断开会话，而不是越界读写
- 传输挂起时客户端直接断开 socket（不 detach、不发 UNLINK）：服务端取消在途传输、设备回到
  可用列表，之后可以重新导入并正常收发

它只需要虚拟设备本身，不需要第二个 vhci 控制器。

## 前置条件

- WSL2 内核包含 `dummy_hcd`、`libcomposite`、`usb_f_fs`、`vhci_hcd`。手动 mass_storage
  示例另需 `usb_f_mass_storage`，自动化用例不需要
- iso 用例另需 `CONFIG_USBIP_VHCI_NR_HCS>=2`（两个以上 vhci 控制器，见“iso 用例”）
- `/sys/kernel/config` 已挂载（configfs）
- 导出侧有 `libusb_server`（默认在 `<构建目录>/libusb_server`）
- 导入侧有 usbip 客户端（Kali：`apt install usbip`，端口参数是 `--tcp-port`）
- root 权限（创建 gadget、libusb 打开设备、vhci attach/detach 都需要）

## 步骤

### 1. 导出侧创建 gadget（root）

```bash
modprobe dummy_hcd
modprobe libcomposite

cd /sys/kernel/config/usb_gadget
mkdir g1
cd g1
echo 0x1d6b > idVendor
echo 0x0104 > idProduct
mkdir -p strings/0x409
echo "usbipdcpp-gadget" > strings/0x409/serialnumber
echo "usbipdcpp" > strings/0x409/manufacturer
echo "Gadget MSC" > strings/0x409/product

mkdir -p functions/mass_storage.0
truncate -s 2M /tmp/gadget_msc.img
echo /tmp/gadget_msc.img > functions/mass_storage.0/lun.0/file

mkdir -p configs/c.1
ln -sf functions/mass_storage.0 configs/c.1/
echo dummy_udc.0 > UDC
```

`dummy_hcd` 同时提供虚拟 UDC（设备端）和虚拟主机控制器（host 端），所以 `echo UDC` 之后
同一个内核的 host 侧会立刻枚举它，`usb-storage` 绑定出 `/dev/sdX`。这一步不需要写 C 代码，
configfs 的文件操作就是内核暴露的接口。

### 2. 确认 host 侧枚举

```bash
lsusb | grep 1d6b:0104
lsblk -o NAME,SIZE,MODEL,TRAN | grep -i gadget
```

### 3. 导出侧启动 libusb 服务端

```bash
cd <项目根目录>
setsid ./build/libusb_server -d -p 53241 < /dev/null \
    > tmp/libusb_server_gadget.log 2>&1 &
```

`-d` 是 daemon 模式：自动绑定所有非 hub 设备并等待信号退出。日志里应看到 gadget 的 busid
（如 `3-1`）被加入可用列表。

### 4. 导入侧 list + attach

```bash
usbip --tcp-port 53241 list -r 127.0.0.1
usbip --tcp-port 53241 attach -r 127.0.0.1 -b 3-1
usbip port
```

attach 成功后 `usbip port` 会显示 `High Speed(480Mbps)` 和 `Multifunction Composite Gadget`。

### 5. 导入侧验证身份并读写

先确认设备身份和大小，再写。下面用 2MiB 临时镜像，写第 100 扇区再读回比对：

```bash
dev=sdg
model=$(lsblk -no MODEL /dev/$dev | tr -d ' ')
size=$(blockdev --getsize64 /dev/$dev)
[ "$size" = "2097152" ] || { echo ABORT_SIZE; exit 1; }
case "$model" in *File-Stor*Gadget*) ;; *) echo ABORT_MODEL; exit 1 ;; esac

head -c 512 /dev/urandom > /tmp/payload.bin
dd if=/tmp/payload.bin of=/dev/$dev bs=512 seek=100 conv=fsync status=none
dd if=/dev/$dev of=/tmp/back.bin bs=512 skip=100 count=1 status=none
cmp /tmp/payload.bin /tmp/back.bin && echo IO_OK
```

### 6. 清理（顺序不能乱）

```bash
# 导入侧：先 detach，再停服务端（否则会留下异常状态的 vhci/会话）
usbip detach -p 0

# 导出侧：优雅停止服务端
kill $(pgrep -x libusb_server)

# 导出侧：拆 gadget
cd /sys/kernel/config/usb_gadget/g1
echo '' > UDC
rm -f configs/c.1/mass_storage.0
rmdir configs/c.1 strings/0x409 functions/mass_storage.0
cd .. && rmdir g1
rm -f /tmp/gadget_msc.img
```

## 安全注意

- gadget 后端只用 `/tmp` 下的一次性镜像，绝不接物理盘
- 写盘前必须校验 `size` 和 `model`，不符立即中止
- **做 e2e 期间不要插拔、添加或移除任何磁盘**（包括在 WSL 里 `mount` 新盘、改挂载点）：
  `/dev/sdX` 的设备名会随磁盘增删变化，写盘目标可能指到别的盘上。确需改动，先停掉测试
- attach 时服务端会 detach host 侧的内核驱动（如 `usb-storage`），导出侧原来的 `/dev/sdX`
  会消失；导入设备随后接管同一个空闲的 sd 名字。`dmesg` 里能看到旧设备
  `Synchronizing SCSI cache` 和新设备枚举，这是正常现象，不是两个独立设备
- 两端共享同一个内核：一侧的 `/sys`、`/dev`、`dmesg` 变化另一侧也能看到，排查时不要误判

## 已知现象

- vhci 枚举阶段服务端可能打印几条 `error on endpoint 129`（0x81 Bulk IN），属于 Bulk-Only
  传输的短包路径，设备仍能正常枚举和读写；如果后续出现真实 I/O 失败再单独排查
- 导入侧 `usbip` 的端口参数：发行版自带包（如 Kali）用 `--tcp-port`；自行编译的新版
  usbip 用 `-t`，两者不要混用

### dummy_hcd 的行为限制（写用例时必须知道）

- **不支持 iso**：`ep_info[]` 里的 ISO 条目被注释掉，ISO URB 直接返回 `-EINVAL`
- **bulk IN 的非整包短读**：主机请求长度不是 maxpacket 整数倍、而设备侧还有更多排队数据时，
  dummy_hcd 返回 `-EOVERFLOW`（服务端映射成 PIPE）。所以 bulk 的整包读取只测 512 的整数倍，
  “设备回短包”用专门的短包端点覆盖
- **OUT 请求长度必须控制在 maxpacket**：gadget 侧一次 read 的请求长度大于一包时，dummy_hcd
  会把多个主机 URB 攒进同一个请求，read 迟迟不返回，统计和上层流控都会滞后
- **UDC 自动分配端点会改写地址**：设备侧要以 `FUNCTIONFS_ENDPOINT_DESC` 查到的实际地址为准
- **usbfs 控制传输上限 4096 字节**（`MAX_CTRL_BUFFER_LENGTH`），超过返回
  `LIBUSB_ERROR_INVALID_PARAM`
- **端点 halt 的 SET_FEATURE/CLEAR_FEATURE 被 dummy_hcd 内部处理**，FunctionFS 的
  userspace 收不到这两个请求（真实 UDC 会转发，设备代码里保留了处理路径）
- **切 alt setting 会让整个 function 的端点先禁用再启用**，只发一个 FUNCTIONFS_ENABLE
  事件；被禁用的端点会把阻塞中的请求以 `ESHUTDOWN` 取消，设备 worker 必须重试等待重启用

## 范围

覆盖 libusb 后端对真实 USB 设备的打开/claim/控制与 bulk/interrupt 传输、ISO 传输
（经虚拟设备双跳链路）、USB/IP 导出、vhci 导入、接口/alt setting 切换和块设备读写。
不覆盖：dummy_hcd 上的 iso（内核 gadget 不支持，iso 用虚拟设备覆盖）、不同内核版本差异
（WSL2 各发行版内核相同）、Windows 客户端、热插拔。
