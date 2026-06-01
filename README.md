# usbipdcpp

A C++ library for creating usbip servers

> [中文文档](README-zh.md)

> ✅ USBIP server: Platform-independent implementation via libusb (works wherever libusb is supported)
> ✅ All four USB transfer types (control, bulk, interrupt, isochronous) tested and working via libusb backend
> ✅ Virtual devices: HID (mouse, keyboard, gamepad, digitizer), MSC (USB flash drive), CDC ACM (serial port) — no libusb dependency
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
| cxxopts | Optional | For building example applications |
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
| `AbstDeviceHandler` | Abstract base class for all device handlers |
| `LibusbServer` | Server wrapper for physical USB device forwarding via libusb |
| `StringPool` | Manages USB string descriptors (limited to 255 strings) |

### Utility Classes

| Class | Description |
|-------|-------------|
| `ObjectPool<T, PoolSize, ThreadSafe, LifeManager, Reset>` | Fixed-size object pool. Supports custom create/destroy/reset policies. alloc O(1), free O(log n). |

### Virtual Device Classes

| Class | Description |
|-------|-------------|
| `VirtualDeviceHandler` | Base class for implementing virtual USB devices |
| `SimpleVirtualDeviceHandler` | Simple device handler with no-op standard request implementations |
| `VirtualInterfaceHandler` | Base class for implementing virtual USB interfaces |
| `HidVirtualInterfaceHandler` | Base class for HID devices (mouse, keyboard, etc.) |
| `AbsoluteMouseHandler` | Absolute-coordinate mouse with screen-to-HID mapping and smooth movement |
| `RelativeMouseHandler` | Relative-coordinate mouse with delta accumulation, 5-button + wheel |
| `KeyboardHandler` | USB HID keyboard with media keys (Consumer Control) |
| `GamepadHandler` | USB HID gamepad: 16 buttons, D-pad, 4 analog axes |
| `DigitizerHandler` | USB HID touchscreen with pressure support |
| `MscBulkOnlyHandler` | USB Mass Storage BOT handler with SCSI command support |
| `StorageBackend` | Abstract block storage backend interface for MSC devices |
| `RawImageBackend` | Memory-mapped file storage backend (cross-platform) |
| `MemoryBackend` | In-memory block storage backend for MSC testing |
| `CdcAcmCommunicationInterfaceHandler` | CDC ACM communication interface handler |
| `CdcAcmDataInterfaceHandler` | CDC ACM data interface handler |
| `UvcVideoControlHandler` | UVC VideoControl interface (camera controls, status interrupt) |
| `UvcVideoStreamingHandler` | UVC VideoStreaming interface (PROBE/COMMIT, ISO video streaming) |
| `VideoSource` | Abstract video source interface for UVC devices |
| `ColorBarSource` | Test pattern video source (color bars) |

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

### UVC Virtual Camera (experimental)

A virtual UVC (USB Video Class) camera implementation is provided:

- **Linux**: Fully functional — PROBE/COMMIT negotiation, ISO streaming via vhci-hcd + uvcvideo
- **Windows**: Currently **fails with Code 10** (STATUS_IO_DEVICE_ERROR). Standard USB enumeration completes successfully, but usbvideo.sys fails during StartDevice before sending any UVC class-specific requests to the server. See the comment in `include/virtual_device/UvcVirtualInterfaceHandler.h` for details. **Help wanted — if you can debug or fix the Windows compatibility issue, PRs are very welcome!**

Example: `mock_uvc` — a 320×240 YUY2 virtual camera with color bar test pattern.

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

### Customizing USB Strings

Call `change_string_*` on your device handler or interface handler **before** starting the server:

**Device-level strings** (via `VirtualDeviceHandler`):
```cpp
device_handler->change_string_manufacturer(L"My Company");
device_handler->change_string_product(L"My USB Device");
device_handler->change_string_serial(L"1234567890");
device_handler->change_string_configuration(L"My Configuration");
```

**Interface string** (via `VirtualInterfaceHandler`):
```cpp
interface_handler->change_string_interface(L"My HID Interface");
```

All `change_string_*` methods delegate to `StringPool::change_string()` and will throw if the string index is invalid.

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

   A relative mouse example based on `RelativeMouseHandler`. By default, it toggles the left button
   every second. With the `--circle` flag, the cursor traces a circular pattern.
3. mock_keyboard

   A keyboard demonstration using the `KeyboardHandler` class which simulates pressing and releasing
   the 'A' key every second. Built-in Consumer Control support (volume, play/pause, etc. media keys).
4. mock_gamepad

   A gamepad demonstration using the `GamepadHandler` class. Rotates the D-pad through 8 directions,
   sweeps the left analog stick in a circle, and toggles button 0 on/off.
5. mock_cdc_acm

   A virtual serial port (CDC ACM) demonstration. Shows bidirectional data transfer over USB bulk endpoints.
6. multi_devices

   A demonstration with 10 virtual HID devices. Shows how to create multiple devices using a factory pattern.
7. absolute_mouse

   Absolute coordinate mouse virtual device example providing complete mouse operation API:
   - **Screen coordinate API**: Position using pixel coordinates, set screen bounds via `set_screen_bounds()`
   - **HID raw coordinate API**: Methods with `_raw` suffix for direct HID coordinate manipulation (0-32767)
   - **Movement functions**: `move(from, to)` and `humanized_move(from, to)` accept start and end points
   - **Drag functionality**: `drag(from, to)` and `humanized_drag(from, to)` with left button pressed
   - **Button operations**: Left, right, middle button, click, double-click
   
   `set_screen_bounds(x1, y1, x2, y2)` working principle:
   - Defines screen coordinate boundary range, e.g. `bounds(0, 0, 1920, 1080)` means screen range [0, 1920] × [0, 1080]
   - Screen coordinates are linearly mapped to HID coordinates [0, 32767]
   - Coordinates outside bounds are clamped to boundary values
   - Note: Windows host doesn't accept HID (0, 0), avoid screen coordinates at (x1, y1) boundary
8. libusb_server

   A usbip server which can forward all local usb devices, has a extremely simple commandline, type `h` for helps
   and can be used to choose which device to forward. By adding virtual usb devices to share the same ubsip server
   with physical usb devices.
9. mock_msc

   A virtual USB Mass Storage (flash drive) device backed by a disk image file.
   Supports BOT (Bulk-Only Transport) protocol and common SCSI commands (INQUIRY, READ CAPACITY,
   READ(10), WRITE(10), MODE SENSE, etc.). The `StorageBackend` abstraction allows swapping the
   underlying storage — the example uses `RawImageBackend` with memory-mapped file I/O, but
   custom backends (e.g. qcow2) can be plugged in via polymorphism.

   Usage: `mock_msc [disk.img]` (defaults to `disk.img`, 4096 blocks × 512 bytes = 2 MiB)

10. mock_uvc

   A virtual UVC camera using `ColorBarSource` to output a 320×240 YUY2 color bar test pattern.
   Demonstrates the `UvcVideoControlHandler` + `UvcVideoStreamingHandler` + `VideoSource` combination.
   Currently functional on Linux; Windows has known issues (see UVC section above).

11. mock_uvc_ffmpeg

   A virtual UVC camera that reads video files via FFmpeg as the video source. Supports any format
   decodable by FFmpeg (MP4, MKV, AVI, etc.), with optional MJPEG/H264 passthrough mode.

   Usage: `mock_uvc_ffmpeg --video video.mp4` (add `--passthrough` for MJPEG/H264 passthrough)

   Requires FFmpeg libraries (libavformat, libavcodec, libswscale, libavutil).

12. termux_libusb_server

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
| `USBIPDCPP_BUILD_LIBUSB_COMPONENTS` | ON | Build libusb-based server components |
| `USBIPDCPP_BUILD_EXAMPLES` | ON (top-level) | Build all example applications |
| `USBIPDCPP_BUILD_TESTS` | ON (top-level) | Build test suite |

See `CMakeLists.txt` for more options and details.

### Python Bindings

Python bindings use pybind11 and require `pybind11` installed via vcpkg or pip.

**Build:**
```bash
cmake -B build -DUSBIPDCPP_BUILD_PYTHON_BINDINGS=ON
cmake --build build
```

After build, set PYTHONPATH and test:
```bash
# Windows
set PYTHONPATH=build/python_package
python examples/python/test_absolute_mouse.py
# Linux/macOS
export PYTHONPATH=build/python_package
python examples/python/test_absolute_mouse.py
```

**Dependencies:**
- pybind11 (via vcpkg `./vcpkg install pybind11` or `pip install pybind11`)
- For `.pyi` stub generation (optional): `pip install pybind11-stubgen`

**Usage notes:**
- `send_input_report()` takes `bytes`: `handler.send_input_report(b'\x01\x00\x00\x00')`
- Python can inherit from `HidVirtualInterfaceHandler` for custom HID devices (see `examples/python/flip_left_button.py`)
- `start()` and `stop()` release the Python GIL, safe to call from main thread

### Python Bindings Status 🚧

Python bindings are **under active development** and may have bugs or crashes.

If you encounter an error:

1. **Report an issue** with the full Python traceback
2. **Get a C++ stack trace** by debugging in CLion:
   - Run → Edit Configurations → Add New → **Native Application**
   - Target: `usbipdcpp_python`
   - Executable: path to `python.exe`
   - Program arguments: path to your script (e.g., `examples/python/flip_left_button.py`)
   - Working directory: project root
   
   CLion will break on crash and show the full C++ call stack.

3. Include:
   - Python version and OS
   - Build configuration used
   - Full error output (both Python and C++ stack trace)
   - Minimal reproduction script

### Full compile commands:

#### Linux (Ubuntu/Debian) — without vcpkg

Install dependencies directly via apt:

```bash
# Required
sudo apt install libasio-dev libspdlog-dev

# If building examples (USBIPDCPP_BUILD_EXAMPLES=ON by default)
sudo apt install libcxxopts-dev

# If building tests (USBIPDCPP_BUILD_TESTS=ON by default)
sudo apt install libgtest-dev

# If building libusb components (USBIPDCPP_BUILD_LIBUSB_COMPONENTS=ON by default)
sudo apt install libusb-1.0-0-dev

# Build
cmake -B build -DUSBIPDCPP_USE_PKGCONF_ASIO=ON
cmake --build build
cmake --install build
```

Skip the corresponding apt packages when disabling features:
- `-DUSBIPDCPP_BUILD_EXAMPLES=OFF` → skip `libcxxopts-dev`
- `-DUSBIPDCPP_BUILD_TESTS=OFF` → skip `libgtest-dev`
- `-DUSBIPDCPP_BUILD_LIBUSB_COMPONENTS=OFF` → skip `libusb-1.0-0-dev`

#### Use vcpkg as the package manager:

Please install asio libusb libevdev spdlog in advance.
To build examples, also install cxxopts:
```bash
./vcpkg install asio libusb libevdev spdlog cxxopts
```

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

## License

This project is licensed under [LGPLv3](LICENSE).

For closed-source or proprietary use, please contact: yun_small@163.com

---

## Acknowledgements

This project builds upon these foundational works:

- [usbipd-libusb](https://github.com/raydudu/usbipd-libusb)
- [usbip](https://github.com/jiegec/usbip)  