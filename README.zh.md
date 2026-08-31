# usbipdcpp

一个用于创建 usbip 服务器的 C++ 库

> [English Documentation](README.md)

## 功能特性

- ✅ **USBIP 服务器**: 基于 libusb 实现，支持所有 libusb 兼容平台
- ✅ **四种 USB 传输类型**（控制、批量、中断、同步）均通过 libusb 后端测试
- ✅ **虚拟设备**: HID（鼠标、键盘、手柄、触摸屏）、MSC（U盘）、CDC ACM（串口）、UVC（摄像头）、UAC（麦克风、扬声器）—— 无需 libusb
- 🔌 **热插拔支持**: 自动检测设备插入/拔出（LibusbServer）
- 🧩 **可扩展设计**: 提供完善的抽象接口供开发者扩展

欢迎贡献代码！🚀

> 💡 **提示**: 如果这个项目对你有用，请考虑给它一个 ⭐，这能帮助更多人发现它。

---

## 快速开始

最简单的虚拟键盘设备（完整示例见 `examples/mock_keyboard`）：

```cpp
#include <cstdint>
#include <iostream>
#include <vector>

#include "usbipdcpp/Server.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/KeyboardHandler.h"

using namespace usbipdcpp;

int main() {
    StringPool string_pool;

    // 1. 用 KeyboardHandler::make_interface 建 HID 键盘接口描述符模板
    //    （类/子类/协议 + 给定地址的中断 IN 端点）；make_interface 不绑 handler，
    //    绑定须在设备创建后（接口入设备、地址稳定，见下）
    // 2. 从接口列表创建设备——UsbDevice::make 为其余字段提供合理默认
    //    （speed=Full、EP0 按它自动生成等）；先绑接口 handler 再绑设备级 handler
    auto device = UsbDevice::make("1-1", 0x1234, 0x5679,
                                  {KeyboardHandler::make_interface(0x81)});
    device->interfaces[0].with_handler<KeyboardHandler>(string_pool);
    device->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();

    // 3. 启动 USB/IP 服务器（TCP，监听 53240 端口）
    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint ep{asio::ip::tcp::v4(), 53240};
    if (auto ec = server.start(ep); ec) {
        std::cerr << "服务器启动失败: " << ec.message() << std::endl;
        return 1;
    }
    // 客户端连接: usbip attach -r <主机> -b 1-1
    // 连接后可在任意线程驱动 KeyboardHandler，
    // 例如 kb->press_key(HIDKey::A) / kb->release_key(HIDKey::A)

    std::cin.get(); // 回车退出
    server.stop();
}
```

本库支持两类设备：**虚拟设备**（纯软件，如上所示——见 `examples/mock_keyboard`、`mock_mouse`、`mock_msc` 等）和 **libusb 设备**（通过网络共享物理 USB 设备——见 `examples/libusb_server`）。

---

## 编译安装

### 预编译二进制

每次推送版本 tag 后，各平台的预编译包都会发布到 [Releases](https://github.com/yunsmall/usbipdcpp/releases) 页面。

如果你不想等版本发布就想拿到包，或者想从某个特定提交构建：

1. **Fork** 本仓库
2. 在 fork 仓库的 **Actions** 页签中选择 **"Build packages (manual)"** → **"Run workflow"**
3. 工作流跑完后，下载对应平台的构建产物

> ⚠️ `linux-aarch64` 任务需要 ARM64 runner，GitHub 仅对**公开**仓库免费提供。
> 如果你的 fork 是私有的，该任务会被跳过，其余三个平台仍会正常构建。

### 编译器要求

若使用gcc编译，最低gcc版本为**gcc13**。gcc13 虽然支持 C++23，但
`std::println` 是从 gcc14 才开始支持的，gcc13 下只能用 `std::format`。
为了兼容 gcc13，本库代码统一使用 `std::format`，不用 `std::println`。

### CMake 选项

有多个CMake选项用于控制相应模块是否编译：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `USBIPDCPP_BUILD_LIBUSB_COMPONENTS` | ON | 编译基于libusb的服务器组件 |
| `USBIPDCPP_BUILD_VIRTUAL_DEVICE` | ON | 编译虚拟设备组件 |
| `USBIPDCPP_BUILD_SHARED_LIBS` | ON | 编译为动态库（符合 LGPL 合规要求，推荐）；设为 OFF 编译静态库 |
| `USBIPDCPP_BUILD_EXAMPLES` | ON (顶级项目) | 编译所有示例程序 |
| `USBIPDCPP_BUILD_TESTS` | ON (顶级项目) | 编译测试套件 |

更多选项详见 `CMakeLists.txt`

### 完整编译命令：

#### Linux (Ubuntu/Debian) — 不使用 vcpkg

直接通过 apt 安装依赖，无需 vcpkg：

```bash
# 必需
sudo apt install libasio-dev libspdlog-dev

# 如果需要编译示例（默认 USBIPDCPP_BUILD_EXAMPLES=ON）
sudo apt install libcxxopts-dev

# 如果需要编译测试（默认 USBIPDCPP_BUILD_TESTS=ON）
sudo apt install libgtest-dev

# 如果需要 libusb 转发物理设备（默认 USBIPDCPP_BUILD_LIBUSB_COMPONENTS=ON）
sudo apt install libusb-1.0-0-dev

# 如果需要 mock_audio 的 --audio 音频文件播放 / mock_speaker 的本机播放（可选，位于 universe 软件源；不装则自动跳过）
sudo apt install libminiaudio-dev

# 编译
cmake -B build -DUSBIPDCPP_USE_PKGCONF_ASIO=ON
cmake --build build
cmake --install build
```

禁用对应功能时可跳过相应的 apt 包和 cmake 选项：
- `-DUSBIPDCPP_BUILD_EXAMPLES=OFF` → 可不装 `libcxxopts-dev`
- `-DUSBIPDCPP_BUILD_TESTS=OFF` → 可不装 `libgtest-dev`
- `-DUSBIPDCPP_BUILD_LIBUSB_COMPONENTS=OFF` → 可不装 `libusb-1.0-0-dev`

如果你的软件源里没有 `libminiaudio-dev`，可以从 [miniaudio](https://miniaudio.app/) 官网
下载 `miniaudio.h` 放到 include 路径——找不到 miniaudio 头文件时
mock_audio 的 `--audio` 选项（`AudioFileSource`）会自动跳过（configure 时有 WARNING），
其他音源不受影响。miniaudio 查找支持两种方式：直接找头文件，找不到走 pkgconf。
stb_vorbis 的布局差异（vcpkg 在 include 根、Termux 在 include/stb/）由源码
`__has_include` 自适应，缺 stb 时仅 OGG 解码不可用（WAV/MP3/FLAC 正常）。

#### Windows — vcpkg

安装 [Visual Studio](https://visualstudio.microsoft.com/)（勾选"使用 C++ 的桌面开发"工作负载）和 [vcpkg](https://github.com/microsoft/vcpkg)。

安装依赖：

```bash
./vcpkg install asio libusb spdlog cxxopts gtest miniaudio
```

配置并编译（不指定生成器，默认使用 Visual Studio 生成器和 MSVC）：

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
cmake --install build
```

注意事项：

- Visual Studio 生成器是多配置的：`cmake --build` 和 `ctest` 需要加 `--config Release`（或 Debug）。
- Windows 下依赖库是 DLL，`cmake --install` 会自动把依赖 DLL 拷贝到可执行文件旁。
- 禁用对应功能时可跳过相应的 vcpkg 包：测试 → `gtest`、示例 → `cxxopts`、libusb 组件 → `libusb`、mock_audio 的音频文件播放 / mock_speaker 的本机播放 → `miniaudio`。

#### Termux (Android)

Termux 使用 clang 编译，不受上文 gcc13 版本限制。

安装依赖：

```bash
pkg install clang cmake ninja pkg-config libasio libspdlog googletest libusb
```

Termux 仓库中没有 `cxxopts`。如需编译示例，请手动安装（以下以安装到 `$PREFIX/opt/cxxopts` 为例）：

```bash
git clone https://github.com/jarro2783/cxxopts.git
cd cxxopts
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX/opt/cxxopts
cmake --build build
cmake --install build
```

配置并编译：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -GNinja \
    -DUSBIPDCPP_USE_PKGCONF_ASIO=ON \
    -Dcxxopts_DIR=$PREFIX/opt/cxxopts/share/cmake/cxxopts
cmake --build build
cmake --install build --prefix $PREFIX
```

注意事项：

- 必须开启 `-DUSBIPDCPP_USE_PKGCONF_ASIO=ON`：Termux 的 libasio 是 autotools 构建，只提供 `asio.pc`，没有 CMake config。
- 不装 cxxopts 也可以，examples 会被整块跳过（configure 时会有 WARNING）；也可通过 `-DUSBIPDCPP_BUILD_EXAMPLES=OFF -DUSBIPDCPP_BUILD_TESTS=OFF` 只编译库。
- `libevdev_mouse` 和 `mock_uvc_ffmpeg` 依赖的 libevdev / FFmpeg 在 Termux 没有 dev 包，configure 时自动跳过，无需额外选项。
- Termux 仓库没有 miniaudio，mock_audio 的 `--audio` 选项（`AudioFileSource`）与 mock_speaker 的本机播放自动跳过；可手动安装头文件启用。stb 头文件在 Termux 放在 `include/stb/` 子目录（vcpkg 在 include 根目录），源码用 `__has_include` 自适应两种布局。
- 如需编译 termux_libusb_server 示例，添加 `-DUSBIPDCPP_BUILD_EXAMPLE_TERMUX_LIBUSB_SERVER=ON`；运行它需要 `pkg install termux-api`（提供 termux-usb 命令）。

#### 使用vcpkg包管理器
请提前装好asio libusb libevdev spdlog等库。
如需编译示例，还需安装 cxxopts：
```bash
./vcpkg install asio libusb libevdev spdlog cxxopts
```
如需编译测试，还需安装 gtest：
```bash
./vcpkg install gtest
```
如需 mock_audio 的音频文件播放（`AudioFileSource`）或 mock_speaker 的本机播放，还需安装 miniaudio：
```bash
./vcpkg install miniaudio
```

```bash
cmake -B build \
-DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build
cmake --install build
```

#### 使用conan作为包管理器：
```bash
conan install . --build=missing -s build_type=Release
cmake --preset conan-release
cmake --build build/Release
cmake --install build/Release
```

### 运行测试

开启测试编译（顶级项目默认开启）后运行：

```bash
ctest --test-dir build --output-on-failure
```

网络测试套件覆盖：

- 服务器重启循环（`start` → `stop` → `start`），包括 100 轮循环和 stop 与连接并发竞争
- 客户端在各个协议阶段的断连：空闲期（未发数据）、请求收到一半、垃圾数据、import 后立即断连、URB 传输中断连——优雅断开（FIN）和异常断开（RST）两种方式
- 设备生命周期：反复 import/释放、多客户端抢占同一设备、import 不存在的设备

### Python 绑定

Python 绑定使用 pybind11，需要先安装 `pybind11`（vcpkg 或 pip 均可）。

**编译：**
```bash
cmake -B build -DUSBIPDCPP_BUILD_PYTHON_BINDINGS=ON
cmake --build build
```

编译后设置 PYTHONPATH 并测试：
```bash
# Windows
set PYTHONPATH=build/python_package
python examples/python/test_absolute_mouse.py
# Linux/macOS
export PYTHONPATH=build/python_package
python examples/python/test_absolute_mouse.py
```

**依赖：**
- pybind11（vcpkg 安装：`./vcpkg install pybind11`，或 pip：`pip install pybind11`）
- 可选，用于生成 `.pyi` 存根文件：`pip install pybind11-stubgen`

**注意事项：**
- `send_input_report()` 接收 `bytes`：`handler.send_input_report(b'\x01\x00\x00\x00')`
- Python 可继承 `HidVirtualInterfaceHandler` 实现自定义 HID 设备（参考 `examples/python/flip_left_button.py`）
- `start()` 和 `stop()` 会释放 Python GIL，可在主线程安全调用
- Python 可调用 `Session.immediately_stop()`；正常情况下该接口仅供服务器停止流程内部使用

### Python 绑定开发状态 🚧

Python 绑定正在**积极开发中**，可能存在 bug 或崩溃问题。

如果你遇到报错：

1. **提 issue**，附上完整的 Python 回溯（traceback）
2. **获取 C++ 堆栈**：在 CLion 中调试原生应用程序
   - Run → Edit Configurations → Add New → **Native Application**
   - Target：选择 `usbipdcpp_python`
   - Executable：选择 `python.exe` 路径
   - Program arguments：目标脚本路径（如 `examples/python/flip_left_button.py`）
   - Working directory：项目根目录
   
   CLion 会在崩溃时断住，显示完整的 C++ 调用栈。

3. 请提供：
   - Python 版本和操作系统
   - 使用的构建配置
   - 完整的错误输出（Python 回溯 + C++ 堆栈）
   - 最小可复现代码

---

## 使用

```cmake
find_package(usbipdcpp CONFIG REQUIRED)
target_link_libraries(main PRIVATE usbipdcpp::usbipdcpp)

# 或者想使用libusb功能

find_package(usbipdcpp CONFIG REQUIRED COMPONENTS libusb)
target_link_libraries(main PRIVATE usbipdcpp::usbipdcpp usbipdcpp::libusb)
```

### 扩展功能

实现自定义设备完整指南（含两种实现方法与完整代码示例）见 [docs/custom-device.md](docs/custom-device.md)。

实现自定义 USB 设备：

1. 使用 `usbipdcpp::UsbDevice` 定义设备描述符
2. 继承 `AbstDeviceHandler` 实现设备逻辑
3. 使用 `VirtualInterfaceHandler` 处理接口操作，同时实现接口内的端点的逻辑

接口定义建议用各 handler 的 `make_interface` 工厂——它创建的接口与 handler 内部
预期的端点结构一致（手写 `UsbInterface` 可能和接口 handler 的硬编码假设不符）。
工厂只返回描述符模板、不绑定 handler：接口 handler 构造持接口引用，须在设备创建
完成后（接口入 `device->interfaces`、地址稳定）用 `device->interfaces[i].with_handler<T>(...)` 绑定。
`UsbDevice` 本身则完全可以手动构造，方便更灵活的配置。

简单设备可直接使用 `SimpleVirtualDeviceHandler`，它为标准请求提供了空实现

> ⚠️ **重要提示**：重写 `VirtualInterfaceHandler::on_new_connection()` 和 `on_disconnection()` 时，**必须**调用父类实现。父类会设置/清除 `session` 指针，该指针用于提交响应数据。

### 自定义 USB 字符串

在启动服务器**之前**调用 `change_string_*` 修改设备字符串：

**设备级字符串**（通过 `VirtualDeviceHandler`）：
```cpp
device_handler->change_string_manufacturer(L"我的公司");
device_handler->change_string_product(L"我的 USB 设备");
device_handler->change_string_serial(L"1234567890");
device_handler->change_string_configuration(L"我的配置");
```

**接口字符串**（通过 `VirtualInterfaceHandler`）：
```cpp
interface_handler->change_string_interface(L"我的 HID 接口");
```

所有 `change_string_*` 方法最终调用 `StringPool::change_string()`，若字符串索引无效会抛异常。

---

## 例子

**1. libevdev_mouse**

   使用libevdev库，在支持evdev的系统上，通过读取`/dev/input/event*`，模拟一个usbip的鼠标，实现转发本地的鼠标信号
**2. mock_mouse**

   基于 `RelativeMouseHandler` 的相对鼠标示例。默认每秒切换左键状态，
   加 `--circle` 参数则控制光标绕圈移动。
**3. mock_keyboard**

   一个键盘示例，基于 `KeyboardHandler` 类，内置 Consumer Control（音量、播放暂停等媒体键）。
   每秒模拟按下和释放'A'键。
**4. mock_gamepad**

   一个游戏手柄示例，基于 `GamepadHandler` 类。D-pad 八方向旋转，左摇杆画圆，按钮 0 交替开关。
**5. mock_cdc_acm**

   虚拟串口（CDC ACM）示例，展示 USB Bulk 端点的双向数据传输。
**6. mock_cdc_throttle**

   限流虚拟串口（CDC ACM）示例，演示 **OUT NAK 背压**：固定窗口 `--window-sec` 秒内最多接收
   `--limit-bytes` 字节；额度用完即停止接收（OUT NAK——主机 URB 挂起），窗口结束恢复并排空
   挂起的请求。数据处理器同时统计收到的 `'1'` 字符数，每半个窗口把累计数发回主机
   （`<数字>\n`）。使用：`mock_cdc_throttle -l 64 -w 5`。
**7. multi_devices**

   包含10个虚拟HID设备的示例。展示了如何使用工厂模式创建多个设备。
**8. absolute_mouse**

   绝对坐标鼠标虚拟设备示例，提供完整的鼠标操作API：
   - **屏幕坐标API**：使用像素坐标定位，通过 `set_screen_bounds()` 设置屏幕边界
   - **HID原始坐标API**：使用 `_raw` 后缀的方法直接操作HID坐标（0-32767）
   - **移动函数**：`move(from, to)` 和 `humanized_move(from, to)` 接受起点终点参数
   - **拖动功能**：`drag(from, to)` 和 `humanized_drag(from, to)` 按下左键移动
   - **按钮操作**：左键、右键、中键、点击、双击
   
   `set_screen_bounds(x1, y1, x2, y2)` 工作原理：
   - 定义屏幕坐标的有效范围边界，如 `bounds(0, 0, 1920, 1080)` 表示屏幕范围 [0, 1920] × [0, 1080]
   - 屏幕坐标通过线性映射转换为 HID 坐标 [0, 32767]
   - 超出 bounds 的坐标会被 clamp 到边界值
   - 注意：Windows 主机不接受 HID (0, 0)，建议屏幕坐标避开 (x1, y1) 边界
**9. libusb_server**

   转发本机的usb设备，带一个非常简陋的命令行，输入`h`查看用法，可自行选择转发哪些设备。
   通过添加虚拟usb设备可实现和真实设备共享同一个usbip server
**10. mock_msc**

   虚拟 USB 大容量存储（U盘）设备，用磁盘镜像文件作为存储介质。
   支持 BOT (Bulk-Only Transport) 协议和常见 SCSI 命令（INQUIRY、READ CAPACITY、
   READ(10)、WRITE(10)、MODE SENSE 等）。通过 `StorageBackend` 抽象接口可替换底层存储
   —— 示例使用 `RawImageBackend` 以内存映射文件实现，也可通过多态接入 qcow2 等自定义后端。

   使用：`mock_msc [disk.img]`（默认 `disk.img`，4096 块 × 512 字节 = 2 MiB）

   **启用 discard/TRIM（打洞）**：

   Linux 内核默认对 USB 存储设备跳过 VPD 查询，导致 UNMAP 命令无法下发。需在客户端启用：

   ```bash
   # 查看设备 SCSI 路径
   ls /sys/class/scsi_disk/
   # 对找到的路径写入 unmap（替换 X:0:0:0 为实际值）
   echo unmap > /sys/class/scsi_disk/X:0:0:0/provisioning_mode
   ```

   或创建 udev 规则持久生效：
   ```bash
   # /etc/udev/rules.d/50-usb-ssd-trim.rules
   ACTION=="add|change", SUBSYSTEM=="scsi_disk", ATTR{provisioning_mode}="unmap"
   ```

   启用后通过 `sudo fstrim -v /mountpoint` 手动触发 discard，
   或 `sudo systemctl enable fstrim.timer` 开启定时 trim。

**11. mock_uvc**

   虚拟 UVC 摄像头，使用 `ColorBarSource` 输出 320×240 YUY2 彩条测试图。
   演示如何实现 `UvcVideoControlHandler` + `UvcVideoStreamingHandler` + `VideoSource` 组合。
   Linux 和 Windows 均可使用。

**12. mock_uvc_ffmpeg**

   虚拟 UVC 摄像头，通过 FFmpeg 读取视频文件作为视频源。支持 FFmpeg 能解码的所有格式
   （MP4、MKV、AVI 等），可选 MJPEG/H264 透传模式。

   使用：`mock_uvc_ffmpeg --video video.mp4`（添加 `--passthrough` 启用 MJPEG/H264 透传）

   需要 FFmpeg 库（libavformat、libavcodec、libswscale、libavutil）。

**13. mock_audio**

   虚拟 USB 麦克风（UAC 1.0）。演示
   `UacAudioControlHandler` + `UacAudioStreamingSourceHandler` + `AudioSource` 组合
   （Feature Unit 静音/音量控制、采样率协商、ISO PCM 推流）。

   支持三种音源：
   - 正弦波测试音（默认）：`--freq 440 --amp 50`
   - 傅里叶级数合成：`--harmonics "440:50,880:25"`（频率Hz : 幅度% [: 相位弧度]）
   - 音频文件播放：`--audio music.mp3`（WAV/MP3/FLAC/OGG，基于 miniaudio，默认循环播放；
     需安装 miniaudio，未找到时自动跳过）

   使用：`mock_audio --rates 48000,16000,8000`（首个为初始采样率）

**14. mock_speaker**

   虚拟 USB 扬声器（UAC 1.0，ISO OUT 收流方向）。演示
   `UacAudioControlHandler` + `UacAudioStreamingSinkHandler` + `AudioSink` 组合
   （Feature Unit 静音/音量控制、采样率协商、ISO OUT PCM 收流消费）。

   三种消费方式：
   - 本机播放（默认）：基于 miniaudio，需安装 miniaudio，未找到时自动跳过
   - WAV 落盘：`--output out.wav`（收下的 PCM 写入 WAV 文件，无需 miniaudio）
   - 丢弃计数（无 miniaudio 且未指定 `--output`）：仅统计接收字节

   使用：`mock_speaker --rates 48000,44100,96000 --channels 2`（首个为初始采样率；
   播放设备用 `--device <名称>` 指定，默认系统默认设备）

**15. termux_libusb_server**

   可在非root安卓设备的termux中使用的libusb server，通过
   `termux-usb -e /path/to/termux_libusb_server /dev/bus/usb/xxx/xxx`启动。

   由于termux-usb只支持传入一个fd，因此可使用不同端口启动多个服务器以支持多个设备。
   使用`USBIPDCPP_LISTEN_PORT`环境变量来指定监听端口

   termux-usb的使用可查看termux官方的相关文档

**16. multi_interface_hid**

   复合 USB 设备示例，在单个设备上同时实现**两个 HID 接口**（鼠标 + 键盘）。
   展示如何使用 `SimpleVirtualDeviceHandler` 创建多接口虚拟设备。

**17. mock_pipe**

   通用虚拟管道设备（vendor 类接口，bulk IN + bulk OUT）。展示 `PipeDeviceHandler` 的
   read()/write() 接口：像读写文件一样操作虚拟设备的端点数据流（FunctionFS 风格的
   FIFO 阻塞语义；非标准控制请求也通过 read() 以带 setup_req 的 PipeXfer 返回）。

**18. mock_pipe_hid**

   用通用 `PipeDeviceHandler` 实现的 **HID 键盘**（无需 `KeyboardHandler`）——"任意
   bulk/interrupt 设备只需 read/write 就能实现"的教程示例。HID 设备的关键步骤：

   ```cpp
   // 1. 构造 HID 接口描述符（Boot 键盘，中断 IN 端点）
   auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);
   // 2. 设置 HID 描述符（0x21）：追加在接口描述符后，驱动加载必需
   pipe->set_class_specific_descriptor({0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00});
   // 3. 设置报告描述符（0x22）：GET_DESCRIPTOR 时返回
   pipe->set_custom_descriptor(0x22, keyboard_report_descriptor);
   // 4. 数据面就是阻塞 read/write
   pipe->write(PipeXfer{.ep = 0x81, .data = {0x00, 0x00, 0x04, 0, 0, 0, 0, 0}}); // 按下 A 键
   ```

   控制请求（GET_REPORT / SET_REPORT 等 class 请求）经 read() 以带 setup_req 的
   PipeXfer 返回，IN 方向用 `write(PipeXfer{.ep = 0, .data = ...})` 应答。

**19. mock_ecm**

   虚拟以太网卡（CDC ECM，在主机上表现为一块网卡）。两种后端：

   - 默认：纯用户态 `EthernetEchoBackend`——应答 ARP/ICMP/TCP echo（不需要 root，
     所有平台可用）
   - `--tun <接口名>`（仅 Linux/Android，需要 root）：`TunBackend` 经 TAP 接口把虚拟
     网卡接入本机内核协议栈（如 `usbip%d`），ARP/ICMP/TCP 与路由全由真实内核处理

   用法：`mock_ecm --tun usbip%d`，主机侧 `ip addr add 192.168.53.2/24 dev usbX` 后
   `ping 192.168.53.1`。macOS 没有 TAP 接口（utun 是三层设备，tuntaposx 驱动在新系统
   已禁用），因此 macOS 不编译 `--tun` 选项。

**20. libusb_windows_service**

   将 libusb 服务器包装为 **Windows 服务**（仅 Windows）。使用 Windows SCM API 运行
   `LibusbServer`，支持完整的服务生命周期管理（通过 `net start`/`net stop` 或
   `sc.exe` 启停）。支持启动时自动绑定所有已连接的 USB 设备。

---

## 架构设计

USB 通信和网络通信都是 I/O 密集型任务，本项目的架构组合如下：

- 使用 **asio** 处理网络 I/O——连接接收为异步（网络线程上由协程驱动的 accept 循环）
- 每个 session 线程内使用**同步阻塞 socket I/O**——编写简单、易于推理
- 使用 libusb 异步接口处理 USB 通信（物理设备）

### 为什么 Session I/O 采用同步

早期版本曾使用 C++20 协程处理 session 数据，但后来改成了同步阻塞 socket I/O，原因如下：

1. **架构不匹配**：本项目采用"每连接一线程"模型，每个客户端连接有独立的线程和 `io_context`。协程的核心优势是"单线程处理多任务"，在这种架构下无法发挥。

2. **代码复杂度**：协程版本和非协程版本的逻辑几乎相同，但需要维护两套代码，增加了维护成本。

3. **编译开销**：协程相关的模板实例化会显著增加编译时间。

4. **ESP32 考量**：对于嵌入式平台，更推荐使用 FreeRTOS 原生任务而非协程，或者改用单线程事件循环架构。

网络线程上的 accept 循环（`Server::do_accept`）仍保留单个协程——它足够简单、自成一体，上述问题都不适用于它。

如果未来需要支持大量并发连接（数百/数千），可以考虑重构为单 `io_context` + 协程模型，届时协程优势才能真正体现。

### 依赖项

| 依赖 | 是否必需 | 说明 |
|------|----------|------|
| asio | ✅ 必需 | 异步I/O库 |
| spdlog | ✅ 必需 | 日志库 |
| libusb-1.0 | 可选 | 用于物理USB设备转发 |
| libevdev | 可选 (仅Linux) | 用于evdev输入设备转发 |
| cxxopts | 可选 | 用于编译示例程序 |
| GTest | 可选 | 用于编译测试 |
| miniaudio + stb | 可选 | mock_audio 的音频文件播放（--audio）与 mock_speaker 的本机播放（纯头文件） |

### 平台支持

| 平台 | 虚拟设备 | 物理设备 (libusb) | 备注 |
|------|----------|-------------------|------|
| Windows | ✅ | ⚠️ 需安装WinUSB驱动 | 适合虚拟HID设备 |
| Linux | ✅ | ✅ | 完全支持 |
| macOS | ✅ | ✅ | 完全支持 |
| Android (Termux) | ✅ | ✅ 通过termux-usb | 支持非root访问 |
| ESP32 | ✅ | ✅ | 使用ESP-IDF和asio组件 |

### 核心类

| 类 | 说明 |
|----|------|
| `Server` | 主服务器类，管理设备列表并接受连接 |
| `Session` | 表示客户端连接，处理USBIP协议 |
| `UsbDevice` | USB设备描述符和配置 |
| `AbstDeviceHandler` | 所有设备处理器的抽象基类 |
| `LibusbServer` | 物理USB设备转发服务器封装（基于libusb） |
| `StringPool` | 管理USB字符串描述符（最多255个） |

### 传输数据载体

传输数据通过 [`TransferHandle`](include/protocol.h) 类管理，这是一个 RAII 包装类，析构时自动释放传输句柄。支持移动语义转移所有权，注意 `release()` 方法会放弃所有权，需手动释放。

### 工具类

| 类 | 说明 |
|----|------|
| `ObjectPool<T, PoolSize, ThreadSafe, LifeManager, Reset>` | 固定大小对象池，支持自定义创建/销毁/重置策略。alloc O(1)，free O(log n)。 |

### 虚拟设备类

| 类 | 说明 |
|----|------|
| `VirtualDeviceHandler` | 虚拟USB设备的基类 |
| `SimpleVirtualDeviceHandler` | 简单设备处理器，提供标准请求的空实现 |
| `VirtualInterfaceHandler` | 实现虚拟USB接口的基类 |
| `HidVirtualInterfaceHandler` | HID设备基类（鼠标、键盘等） |
| `AbsoluteMouseHandler` | 绝对坐标鼠标，支持屏幕坐标映射和人性化移动 |
| `RelativeMouseHandler` | 相对坐标鼠标，偏移量累积发送，5 键 + 滚轮 |
| `KeyboardHandler` | USB HID 键盘，内置 Consumer Control 媒体键支持 |
| `GamepadHandler` | USB HID 游戏手柄，16 按钮 + 十字键 + 4 模拟轴 |
| `DigitizerHandler` | USB HID 触摸屏，支持按压力度 |
| `MscBulkOnlyHandler` | USB 大容量存储 BOT 协议处理器，实现 SCSI 命令处理 |
| `StorageBackend` | 块存储后端抽象接口，为 MSC 设备提供读写能力 |
| `RawImageBackend` | 基于内存映射的磁盘镜像文件后端（跨平台） |
| `MemoryBackend` | 基于内存的块存储后端，用于 MSC 测试 |
| `CdcAcmCommunicationInterfaceHandler` | CDC ACM 通信接口处理器 |
| `CdcAcmDataInterfaceHandler` | CDC ACM 数据接口处理器 |
| `UvcVideoControlHandler` | UVC VideoControl 接口（摄像头控制、状态中断） |
| `UvcVideoStreamingHandler` | UVC VideoStreaming 接口（PROBE/COMMIT、ISO 视频流） |
| `VideoSource` | UVC 虚拟摄像头视频源抽象接口 |
| `ColorBarSource` | 彩条测试图视频源 |
| `UacAudioControlHandler` | UAC AudioControl 接口（Feature Unit 静音/音量控制） |
| `UacAudioStreamingSourceHandler` | UAC AudioStreaming 接口（ISO PCM 推流） |
| `AudioSource` | UAC 虚拟麦克风 PCM 音频源抽象接口 |
| `UacAudioStreamingSinkHandler` | UAC AudioStreaming 接口（ISO OUT 收流消费，扬声器方向） |
| `AudioSink` | UAC 虚拟扬声器 PCM 消费端抽象接口 |
| `SineWaveSource` | 正弦波测试音源 |
| `FourierSource` | 傅里叶级数合成音源（多谐波叠加，各谐波独立相位） |
| `AudioFileSource` | mock_audio 示例的音源（WAV/MP3/FLAC/OGG，基于 miniaudio，支持重采样和循环播放；在 examples/mock_audio/） |
| `EcmCommunicationInterfaceHandler` | CDC ECM 通信接口（以太网连接/状态通知） |
| `EcmDataInterfaceHandler` | CDC ECM 数据接口（以太网帧收发） |
| `NetworkBackend` | 虚拟以太网卡的网络后端抽象（帧收发 + 统计） |
| `EthernetEchoBackend` | mock_ecm 的纯用户态 echo 后端（应答 ARP/ICMP/TCP echo，不需要 root） |
| `TunBackend` | Linux TAP 后端：把虚拟网卡帧接入本机内核协议栈（仅 Linux/Android，需要 root） |

### 类继承关系

```
AbstDeviceHandler
├── LibusbDeviceHandler    (通过libusb的物理设备)
└── VirtualDeviceHandler   (虚拟设备)
    └── SimpleVirtualDeviceHandler
```

### 线程模型

一个服务器实例使用到的线程：

1. **网络 I/O 线程**：每个 `Server` 一个——运行 `asio::io_context::run()` 和 accept 协程，等待客户端连接
2. **Session 线程**：每个连接一个——同步阻塞 socket I/O 和设备处理器调用
3. **Sender 线程**：每次传输一个——从响应队列取数据写入 socket
4. **libusb 事件线程**：运行 `libusb_handle_events()` 循环，仅 libusb 后端（`LibusbServer`）使用
5. **主线程**：调用 `start()` / `stop()`，控制服务器

每个连接使用独立线程，保证一个连接上的阻塞操作（如设备同步）不会卡住其他连接。

`start()` / `stop()` 可以反复调用以重启服务器。`stop()` 会关闭监听、打断所有活跃 session，并在返回前 join 所有 session 线程——返回后可以安全析构 `Server`。

数据传输流程：

```
客户端 → session 线程（读取 URB）→ 设备处理器 → sender 线程（写响应）→ 客户端
```

该架构通过最小化线程竞争实现高 CPU 效率。

### 虚拟设备实现

开发虚拟设备时需注意：

- 避免阻塞网络线程
- 在工作线程中处理请求
- 通过回调提交响应数据

#### 为什么需要请求队列

客户端只负责发送 URB，服务端负责接收。在原版 USBIP 中，服务端收到 URB 后会存储起来，然后由 USB 控制器按顺序发送给真实设备，这保证了同时只有一个 URB 在传输。

虚拟设备需要模拟这个"按顺序存储 URB"的行为，因此使用请求队列（`std::deque`）来存储收到的请求，按顺序逐个处理。

## 平台说明

### Windows：libusb 需要替换驱动

在 Windows 使用 libusb 服务器需要替换驱动：

1. 使用 [Zadig](https://zadig.akeo.ie/) 安装 WinUSB 驱动
    - 选择目标设备（找不到设备时启用"列出所有设备"）
    - **警告**：替换鼠标/键盘驱动会导致输入失效
2. 使用后通过设备管理器回滚驱动：
    - `Win+X` → 设备管理器 → 选择设备 → 回滚驱动程序

对于物理设备，推荐使用 [usbipd-win](https://github.com/dorssel/usbipd-win) ，
该项目使用VBoxUSB从驱动层面实现上述功能
本项目更适合在 Windows 实现**虚拟 USB 设备**

---

## 贡献指南

### 代码说明

> 📝 **语言说明**：公开 API 文档（doxygen）和运行时消息使用**英语**。内部实现注释可使用**中文或英语**——这是项目中仅允许的两种语言，其他语言的 PR 不予接受。

### 分支工作流

本项目采用单 main 分支工作流：

- 修复和小改动直接提交到 main
- 较大的或实验性改动在独立分支开发，完成后合并回 main
- 外部贡献者：fork 本仓库并针对 main 提交 PR
- 发布版本：更新 CMakeLists.txt 中的版本号并提交推送，然后推送 v* tag，CI 自动构建并发布包

---

## 许可证

本项目使用 [LGPLv3](LICENSE) 许可证。

> **必须署名**：无论以任意形式使用本库——包括源码引用、动态/静态链接、修改后的
> 衍生作品，以及在任意产品或服务中分发——都**必须**在醒目位置（如产品的"关于/
> 致谢"页面或文档中）明确标识使用了本库：
>
> ```text
> 本产品使用了 usbipdcpp（https://github.com/yunsmall/usbipdcpp），遵循 LGPLv3 许可证。
> ```

修改本库代码后如需以闭源方式分发，请联系：yun_small@163.com

---

## 致谢

本项目受益于以下开源项目：

- [usbipd-libusb](https://github.com/raydudu/usbipd-libusb)
- [usbip](https://github.com/jiegec/usbip)
