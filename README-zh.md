# usbipdcpp

一个用于创建 usbip 服务器的 C++ 库

## 功能特性

- ✅ **USBIP 服务器**: 基于 libusb 实现，支持所有 libusb 兼容平台
- ✅ **虚拟 HID 设备**: 跨平台虚拟 USB 设备支持（无需 libusb），详见 `examples` 目录
- 🚀 **异步架构**: 使用 C++20 协程和 asio 实现高效 I/O
- 🧩 **可扩展设计**: 提供完善的抽象接口供开发者扩展

欢迎贡献代码！🚀

---

## 架构设计

USB 通信和网络通信都是 I/O 密集型任务，本项目采用全异步架构：
- 使用 C++20 协程处理网络通信
- 使用 libusb 异步接口处理 USB 通信

### 线程模型
系统包含三个核心线程：
1. **网络 I/O 线程**: 运行 `asio::io_context::run()`
2. **USB 传输线程**: 处理 `libusb_handle_events()`
3. **主线程**: 控制服务器的行为，以及用于启动服务器

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
3. empty_server

   一个只有一个设备的usbip服务器。设备无任何功能，不会对数据做相应。
4. libusb_server

   转发本机的usb设备，带一个非常简陋的命令行，输入`h`查看用法，可自行选择转发哪些设备。
   通过添加虚拟usb设备可实现和真实设备共享同一个usbip server
---

## 编译安装
有三个选项

`USBIPDCPP_BUILD_EXAMPLES`
`USBIPDCPP_BUILD_LIBUSB_COMPONENTS`
`USBIPDCPP_BUILD_TESTS`

控制相应模块是否编译。

完整编译命令：
```bash
cmake -B build
cmake --build build
cmake --install build
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