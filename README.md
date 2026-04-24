# usbipdcpp

A C++ library for creating usbip servers

> [中文文档](README-zh.md)

> ✅ USBIP server: Platform-independent implementation via libusb (works wherever libusb is supported)
> ✅ Virtual HID devices: Create virtual USB devices on any platform without libusb dependency (see `examples/`)
> ✅ Hot-plug support: Automatic device insertion/removal detection (LibusbServer)

Contributions welcome! 🚀

> 💡 **Hint**: If this project is useful to you, please consider giving it a ⭐. This can help more people discover it.

---

### Transfer Data Carrier

Transfer data is managed via [`TransferHandle`](include/protocol.h), an RAII wrapper that automatically frees the transfer handle on destruction. Supports move semantics for ownership transfer. Note that `release()` gives up ownership and requires manual cleanup.

---

## Architecture Overview

USB communication and network I/O are both resource-intensive operations. This project implements a fully asynchronous
architecture using:

- **asio** for asynchronous I/O
- **libusb**'s async API for USB communications (physical devices)

### Why not C++20 Coroutines

An earlier version used C++20 coroutines, but they were later removed for the following reasons:

1. **Architecture mismatch**: This project uses a "per-connection-one-thread" model, where each client connection has its own thread and `io_context`. The core advantage of coroutines is "single-threaded multi-tasking", which cannot be leveraged in this architecture.

2. **Code complexity**: The coroutine and non-coroutine versions had nearly identical logic, but maintaining two sets of code increased maintenance burden.

3. **Compilation overhead**: Coroutine-related template instantiation significantly increased compilation time.

4. **ESP32 considerations**: For embedded platforms, FreeRTOS native tasks are preferred over coroutines, or a single-threaded event loop architecture should be used instead.

If future requirements demand supporting hundreds or thousands of concurrent connections, refactoring to a single `io_context` + coroutine model could be considered, where coroutine benefits would truly shine.

### Dependencies

| Dependency | Required | Description |
|------------|----------|-------------|
| asio | ✅ | Asynchronous I/O library |
| spdlog | ✅ | Logging library |
| libusb-1.0 | Optional | For physical USB device forwarding |
| libevdev | Optional (Linux) | For evdev-based input device forwarding |
| GTest | Optional | For building tests |

### Platform Support

| Platform | Virtual Devices | Physical Devices (libusb) | Notes |
|----------|-----------------|---------------------------|-------|
| Windows | ✅ | ⚠️ Requires WinUSB driver | Ideal for virtual HID devices |
| Linux | ✅ | ✅ | Full support |
| macOS | ✅ | ✅ | Full support |
| Android (Termux) | ✅ | ✅ via termux-usb | Non-root access supported |
| ESP32 | ✅ | ✅ | Use ESP-IDF with asio component |

### Core Classes

| Class | Description |
|-------|-------------|
| `Server` | Main server class that manages device list and accepts connections |
| `Session` | Represents a client connection, handles USBIP protocol |
| `UsbDevice` | USB device descriptor and configuration |
| `LibusbServer` | Server wrapper for physical USB device forwarding via libusb |
| `AbstDeviceHandler` | Abstract base class for all device handlers. Provides `is_device_removed()`, `on_device_removed()`, and `trigger_session_stop()` for device lifecycle management |
| `VirtualDeviceHandler` | Base class for implementing virtual USB devices |
| `LibusbDeviceHandler` | Handler for physical USB devices using libusb |
| `VirtualInterfaceHandler` | Base class for implementing virtual USB interfaces |
| `HidVirtualInterfaceHandler` | Base class for HID devices (mouse, keyboard, etc.) |
| `SimpleVirtualDeviceHandler` | Simple device handler with no-op standard request implementations |
| `StringPool` | Manages USB string descriptors (limited to 255 strings) |

### Utility Classes

| Class | Description |
|-------|-------------|
| `ObjectPool<T, PoolSize, ThreadSafe>` | Fixed-size object pool for memory-efficient allocation. Supports pointer validation and duplicate-free detection. alloc O(1), free O(log n). |
| `ConcurrentTransferTracker<TransferPtr, SegmentCount>` | Sharded lock-based transfer tracker for efficient concurrent transfer management. Uses atomic counters for fast-path operations. |

### Class Hierarchy

```
AbstDeviceHandler
├── LibusbDeviceHandler    (physical devices via libusb)
└── VirtualDeviceHandler   (virtual devices)
    └── SimpleVirtualDeviceHandler
```

### Threading Model

Three dedicated threads ensure optimal performance:

1. **Network I/O thread**: Runs `asio::io_context::run()` waiting for client connection
2. **USB transfer thread**: Handles `libusb_handle_events()`
3. **Main thread**: Control the behavior of usbip server, start the server

Each connection starts a separate thread to prevent special synchronization operations on some devices from blocking all
devices.

Starting another thread at this point would feel like a chore. This is also possible given that a single server does not
have a large number of usb devices.

Data flows through the system without blocking:

```
Network thread → libusb_submit_transfer → USB thread → Callback → Network thread
```

This architecture achieves high CPU efficiency by minimizing thread contention.

### Virtual Device Implementation

Virtual device handlers should:

- Avoid blocking the network thread
- Process requests in worker threads
- Submit responses via callbacks

#### Why Request Queues Are Needed

The client only sends URBs, and the server receives them. In the original USBIP, the server stores received URBs and the USB controller sends them to the real device in order, ensuring only one URB is being transferred at a time.

Virtual devices need to simulate this "store URBs in order" behavior, so request queues (`std::deque`) are used to store received requests and process them sequentially.

---

## Getting Started

### Code Note

> 📝 **Language notice**: Comments/logs primarily use Chinese for efficiency.  
> The code structure remains clear and approachable. PRs for English translations appreciated!

### Extending Functionality

To implement custom USB devices:

1. Define descriptors with `usbipdcpp::UsbDevice`
2. Implement device logic via `AbstDeviceHandler` subclass
3. Handle interface-specific operations with `VirtualInterfaceHandler`, and implements the logic of the endpoints inside
   the interface

For simple devices, use `SimpleVirtualDeviceHandler` - it provides no-op implementations for standard requests.

> ⚠️ **Important**: When overriding `VirtualInterfaceHandler::on_new_connection()` and `on_disconnection()`, you **must** call the parent class implementation. The parent class sets/clears the `session` pointer which is required for submitting responses.

---

## ⚠️ Important Windows Notice

Using libusb servers on Windows requires driver replacement:

1. Use [Zadig](https://zadig.akeo.ie/) to install WinUSB driver
    - Select target device (enable "List All Devices" if missing)
    - ⚠️ **WARNING**: Replacing mouse/keyboard drivers may cause input loss
2. After use, revert drivers via:
    - `Win+X` → Device Manager → Select device → Roll back driver

Due to this complexity, we recommend [usbipd-win](https://github.com/dorssel/usbipd-win) for physical devices on
Windows.  
This project is ideal for implementing **virtual USB devices** on Windows.

---

## Examples Introduction

1. libevdev_mouse

   Through libevdev library, in an OS which supports evdev, by reading `/dev/input/event*`, simulate a usbip mouse
   to implement forwarding local mouse signals.
2. mock_mouse

   A mouse demonstration which switches left button statu each second, to introduce how to implement a virtual
   HID device.
3. mock_keyboard

   A keyboard demonstration which simulates pressing and releasing the 'A' key every second.
   Shows how to implement a virtual HID keyboard with standard keyboard report descriptor.
4. multi_devices

   A demonstration with 10 virtual HID devices. Shows how to create multiple devices using a factory pattern.
5. libusb_server

   A usbip server which can forward all local usb devices, has a extremely simple commandline, type `h` for helps
   and can be used to choose which device to forward. By adding virtual usb devices to share the same ubsip server
   with physical usb devices.
6. termux_libusb_server

   A usbip server which can be used at termux in non-root Android device, execute it by
   `termux-usb -e /path/to/termux_libusb_server /dev/bus/usb/xxx/xxx`

   Since termux-usb only supports passing in one fd, multiple servers can be started on different ports to support multiple devices.
   Use the `USBIPDCPP_LISTEN_PORT` environment variable to specify the listening port.

   For the usage of termux-usb, you can refer to the relevant documentation on the official Termux website.

---

## Building

If compiled with gcc, the minimum gcc version is **gcc13**. The C++23 standard support under **gcc14** is broken,
Either `std::println` is not supported or `std::format` is not supported and is not at all comfortable to use.
You have to give up programming experience for compatibility.
So I chose gcc13, which supports `std::format` but still doesn't support `std::println`

There are multiple CMake options to control which parts are compiled:

| Option | Default | Description |
|--------|---------|-------------|
| `USBIPDCPP_ENABLE_BUSY_WAIT` | OFF | Enable busy-wait mode for lower latency |
| `USBIPDCPP_BUILD_LIBUSB_COMPONENTS` | ON | Build libusb-based server components |
| `USBIPDCPP_BUILD_EXAMPLES` | ON (top-level) | Build all example applications |
| `USBIPDCPP_BUILD_TESTS` | ON (top-level) | Build test suite |

See `CMakeLists.txt` for more options and details.

### Full compile commands:

#### Use vcpkg as the package manager:

Please install asio libusb libevdev spdlog in advance

```bash
cmake -B build \
-DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build
cmake --install build
```

#### Use conan as the package manager:

```bash
conan install . --build=missing -s build_type=Release
cmake --preset conan-release
cmake --build build/Release
cmake --install build/Release
```

---

## Usage

```cmake
find_package(usbipdcpp CONFIG REQUIRED)
target_link_libraries(main PRIVATE usbipdcpp::usbipdcpp)

# Or if want to use libusb server

find_package(usbipdcpp CONFIG REQUIRED COMPONENTS libusb)
target_link_libraries(main PRIVATE usbipdcpp::usbipdcpp usbipdcpp::libusb)
```

---

## Acknowledgements

This project builds upon these foundational works:

- [usbipd-libusb](https://github.com/raydudu/usbipd-libusb)
- [usbip](https://github.com/jiegec/usbip)  