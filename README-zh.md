# usbipdcpp

一个用于创建 usbip 服务器的 C++ 库

## 功能特性

- ✅ **USBIP 服务器**: 基于 libusb 实现，支持所有 libusb 兼容平台
- ✅ **虚拟 HID 设备**: 跨平台虚拟 USB 设备支持（无需 libusb），详见 `examples` 目录
- 🔌 **热插拔支持**: 自动检测设备插入/拔出（LibusbServer）
- 🚀 **异步架构**: 使用 C++20 协程和 asio 实现高效 I/O
- 🧩 **可扩展设计**: 提供完善的抽象接口供开发者扩展

欢迎贡献代码！🚀

> 💡 **提示**: 如果这个项目对你有用，请考虑给它一个 ⭐，这能帮助更多人发现它。

---

## 架构设计

USB 通信和网络通信都是 I/O 密集型任务，本项目采用全异步架构：

- 使用 C++20 协程处理网络通信
- 使用 asio 异步通信
- 使用 libusb 异步接口处理 USB 通信

### 依赖项

| 依赖 | 是否必需 | 说明 |
|------|----------|------|
| asio | ✅ 必需 | 异步I/O库 |
| spdlog | ✅ 必需 | 日志库 |
| libusb-1.0 | 可选 | 用于物理USB设备转发 |
| libevdev | 可选 (仅Linux) | 用于evdev输入设备转发 |
| GTest | 可选 | 用于编译测试 |

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
| `LibusbServer` | 物理USB设备转发服务器封装（基于libusb） |
| `AbstDeviceHandler` | 所有设备处理器的抽象基类。提供 `is_device_removed()`、`on_device_removed()`、`trigger_session_stop()` 用于设备生命周期管理 |
| `DeviceHandlerBase` | 提供通用功能的设备处理器中间基类 |
| `VirtualDeviceHandler` | 虚拟USB设备的基类 |
| `LibusbDeviceHandler` | 基于libusb的物理设备处理器 |
| `VirtualInterfaceHandler` | 实现虚拟USB接口的基类 |
| `HidVirtualInterfaceHandler` | HID设备基类（鼠标、键盘等） |
| `SimpleVirtualDeviceHandler` | 简单设备处理器，提供标准请求的空实现 |
| `StringPool` | 管理USB字符串描述符（最多255个） |

### 工具类

| 类 | 说明 |
|----|------|
| `ObjectPool<T, InitialSize, MaxSize, ThreadSafe>` | 模板化对象池，用于高效内存分配。支持初始固定大小和可选的动态扩容（有上限）。专为嵌入式平台（ESP32）设计。 |
| `ConcurrentTransferTracker<TransferPtr, SegmentCount>` | 基于分段锁的传输追踪器，用于高效管理并发传输。使用原子计数器实现快速路径操作。 |

### 类继承关系

```
AbstDeviceHandler
└── DeviceHandlerBase
    ├── LibusbDeviceHandler    (通过libusb的物理设备)
    └── VirtualDeviceHandler   (虚拟设备)
        └── SimpleVirtualDeviceHandler
```

### 线程模型

系统包含三个核心线程：

1. **网络 I/O 线程**: 运行 `asio::io_context::run()`等待客户端连接
2. **USB 传输线程**: 处理 `libusb_handle_events()`
3. **主线程**: 控制服务器的行为，以及用于启动服务器

每个连接会启动一个单独线程，防止某些设备的一些特殊同步操作阻塞所有设备。

如果此时又启动一个线程会感觉多此一举。
鉴于本身单个服务器的usb设备不会特别多，这种设计也是可行的。

数据传输流程：

```
网络线程 → libusb_submit_transfer → USB线程 → 回调 → 网络线程
```

该架构通过最小化线程竞争实现高 CPU 效率

### 虚拟设备实现

开发虚拟设备时需注意：

- 避免阻塞网络线程
- 在工作线程中处理请求
- 通过回调提交响应数据

---

## 使用指南

### 代码说明

> 📝 注释和日志主要使用中文，代码结构清晰易读  
> 欢迎提交英文翻译的 PR！

### 扩展功能

实现自定义 USB 设备：

1. 使用 `usbipdcpp::UsbDevice` 定义设备描述符
2. 继承 `AbstDeviceHandler` 实现设备逻辑
3. 使用 `VirtualInterfaceHandler` 处理接口操作，同时实现接口内的端点的逻辑

简单设备可直接使用 `SimpleVirtualDeviceHandler`，它为标准请求提供了空实现

> ⚠️ **重要提示**：重写 `VirtualInterfaceHandler::on_new_connection()` 和 `on_disconnection()` 时，**必须**调用父类实现。父类会设置/清除 `session` 指针，该指针用于提交响应数据。

---

## ⚠️ Windows 使用提示

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

## 例子介绍

1. libevdev_mouse

   使用libevdev库，在支持evdev的系统上，通过读取`/dev/input/event*`，模拟一个usbip的鼠标，实现转发本地的鼠标信号
2. mock_mouse

   一个每隔一秒就切换鼠标左键状态的鼠标示例。用以介绍虚拟HID设备的写法
3. mock_keyboard

   一个键盘示例，每秒模拟按下和释放'A'键。展示了如何实现标准键盘报告描述符的虚拟HID键盘。
4. multi_devices

   包含10个虚拟HID设备的示例。展示了如何使用工厂模式创建多个设备。
5. empty_server

   一个只有一个设备的usbip服务器。设备无任何功能，不会对数据做相应。
6. libusb_server

   转发本机的usb设备，带一个非常简陋的命令行，输入`h`查看用法，可自行选择转发哪些设备。
   通过添加虚拟usb设备可实现和真实设备共享同一个usbip server
7. termux_libusb_server

   可在非root安卓设备的termux中使用的libusb server，通过
   `termux-usb -e /path/to/termux_libusb_server /dev/bus/usb/xxx/xxx`启动。

   由于termux-usb只支持传入一个fd，因此可使用不同端口启动多个服务器以支持多个设备。
   使用`USBIPDCPP_LISTEN_PORT`环境变量来指定监听端口

   termux-usb的使用可查看termux官方的相关文档
---

## 编译安装

若使用gcc编译，最低gcc版本为**gcc13**。**gcc14**之下的C++23标准支持都是残的，
不是`std::println`不支持就是`std::format`不支持，用得一点也不舒服。
为了兼容性只能放弃编程体验。所以我选择了支持`std::format`但仍不支持`std::println`的gcc13

有多个CMake选项用于控制相应模块是否编译：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `USBIPDCPP_USE_COROUTINE` | ON | 使用C++20协程实现 |
| `USBIPDCPP_ENABLE_BUSY_WAIT` | ON | 启用busy-wait模式以降低延时（仅在非协程模式下有效） |
| `USBIPDCPP_BUILD_LIBUSB_COMPONENTS` | ON | 编译基于libusb的服务器组件 |
| `USBIPDCPP_BUILD_EXAMPLES` | ON (顶级项目) | 编译所有示例程序 |
| `USBIPDCPP_BUILD_TESTS` | ON (顶级项目) | 编译测试套件 |

更多选项详见 `CMakeLists.txt`

### 完整编译命令：

#### 使用vcpkg包管理器
请提前装好asio libusb libevdev spdlog等库
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

---

## 使用

```cmake
find_package(usbipdcpp CONFIG REQUIRED)
target_link_libraries(main PRIVATE usbipdcpp::usbipdcpp)

# 或者想使用libusb功能

find_package(usbipdcpp CONFIG REQUIRED COMPONENTS libusb)
target_link_libraries(main PRIVATE usbipdcpp::usbipdcpp usbipdcpp::usbipdcpp_libusb)
```

---

## 致谢

本项目受益于以下开源项目：

- [usbipd-libusb](https://github.com/raydudu/usbipd-libusb)
- [usbip](https://github.com/jiegec/usbip)