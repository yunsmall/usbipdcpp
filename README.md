# usbipdcpp

A C++ library for creating usbip servers

> [中文文档](README.zh.md)

> ✅ USBIP server: Platform-independent implementation via libusb (works wherever libusb is supported)
> ✅ All four USB transfer types (control, bulk, interrupt, isochronous) tested and working via libusb backend
> ✅ Virtual devices: HID (mouse, keyboard, gamepad, digitizer), MSC (USB flash drive), CDC ACM (serial port), UVC (camera), UAC (microphone, speaker) — no libusb dependency
> ✅ Hot-plug support: Automatic device insertion/removal detection (LibusbServer)

Contributions welcome! 🚀

> 💡 **Hint**: If this project is useful to you, please consider giving it a ⭐. This can help more people discover it.

---

## Quick Start

Minimal virtual keyboard device (full example in `examples/mock_keyboard`):

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

    // 1. Build the HID keyboard interface descriptor with KeyboardHandler::make_interface
    //    (class/subclass/protocol + interrupt IN endpoint at the given address).
    //    It does NOT bind the handler — binding must happen after the device is
    //    created, when the interface object has a stable address (see below).
    // 2. Build the device from the interface list — UsbDevice::make provides sensible
    //    defaults for the remaining fields (speed=Full, EP0 derived from it, ...).
    //    Bind the interface handler first, then the device-level handler.
    auto device = UsbDevice::make("1-1", 0x1234, 0x5679,
                                  {KeyboardHandler::make_interface(0x81)});
    device->interfaces[0].with_handler<KeyboardHandler>(string_pool);
    device->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();

    // 3. Start the USB/IP server (TCP, listen on port 53240)
    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint ep{asio::ip::tcp::v4(), 53240};
    if (auto ec = server.start(ep); ec) {
        std::cerr << "Failed to start server: " << ec.message() << std::endl;
        return 1;
    }
    // Attach from another machine:  usbip attach -r <host> -b 1-1
    // Once attached, the KeyboardHandler can be driven from any thread,
    // e.g. kb->press_key(HIDKey::A) / kb->release_key(HIDKey::A)

    std::cin.get(); // Run until Enter is pressed
    server.stop();
}
```

The library ships two kinds of devices: **virtual devices** (pure software, as above — see `examples/mock_keyboard`, `mock_mouse`, `mock_msc`, ...) and **libusb devices** (share a physical USB device over the network — see `examples/libusb_server`).

---

## Building

### Pre-built Binaries

Pre-built packages for each platform are available on the [Releases](https://github.com/yunsmall/usbipdcpp/releases) page when a version tag is pushed.

If you need a package without waiting for a release, or want to build from a specific commit:

1. **Fork** this repository
2. Go to the **Actions** tab in your fork → select **"Build packages (manual)"** → **"Run workflow"**
3. After the workflow completes, download the artifacts for your platform

> ⚠️ The `linux-aarch64` job requires an ARM64 runner, which GitHub provides for free only on **public** repositories.
> If your fork is private, that job will be skipped; the other three platforms will still build.

### Compiler Requirements

If compiled with gcc, the minimum gcc version is **gcc13**. gcc13 supports C++23, but
`std::println` was only introduced in gcc14 — with gcc13 you have to use `std::format` instead.
For gcc13 compatibility, the library code consistently uses `std::format` and avoids `std::println`.

### CMake Options

There are multiple CMake options to control which parts are compiled:

| Option | Default | Description |
|--------|---------|-------------|
| `USBIPDCPP_BUILD_LIBUSB_COMPONENTS` | ON | Build libusb-based server components |
| `USBIPDCPP_BUILD_VIRTUAL_DEVICE` | ON | Build virtual device component |
| `USBIPDCPP_BUILD_SHARED_LIBS` | ON | Build as shared library (recommended for LGPL compliance); OFF builds static libraries |
| `USBIPDCPP_BUILD_EXAMPLES` | ON (top-level) | Build all example applications |
| `USBIPDCPP_BUILD_TESTS` | ON (top-level) | Build test suite |

See `CMakeLists.txt` for more options and details.

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

# For mock_audio --audio file playback / mock_speaker local playback (optional, universe repository; skipped automatically when missing)
sudo apt install libminiaudio-dev

# Build
cmake -B build -DUSBIPDCPP_USE_PKGCONF_ASIO=ON
cmake --build build
cmake --install build
```

Skip the corresponding apt packages when disabling features:
- `-DUSBIPDCPP_BUILD_EXAMPLES=OFF` → skip `libcxxopts-dev`
- `-DUSBIPDCPP_BUILD_TESTS=OFF` → skip `libgtest-dev`
- `-DUSBIPDCPP_BUILD_LIBUSB_COMPONENTS=OFF` → skip `libusb-1.0-0-dev`

If `libminiaudio-dev` is not available in your repository, download `miniaudio.h` from
[miniaudio](https://miniaudio.app/) and put it on the include path — when the miniaudio
header is not found, the `--audio` option (`AudioFileSource`) of `mock_audio` is skipped
automatically (configure-time warning); the other audio sources are unaffected.
miniaudio lookup tries two ways: direct header search, then pkgconf.
The stb_vorbis layout difference (include root with vcpkg, `include/stb/` on Termux) is
handled by `__has_include` in the source; without stb only OGG decoding is unavailable
(WAV/MP3/FLAC still work).

#### Windows — vcpkg

Install [Visual Studio](https://visualstudio.microsoft.com/) with the "Desktop development with C++" workload, plus [vcpkg](https://github.com/microsoft/vcpkg).

Install dependencies:

```bash
./vcpkg install asio libusb spdlog cxxopts gtest miniaudio
```

Configure and build — no generator specified, so the Visual Studio generator with MSVC is used:

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
cmake --install build
```

Notes:

- The Visual Studio generator is multi-config: `cmake --build` and `ctest` need `--config Release` (or Debug).
- Dependencies are DLLs on Windows; `cmake --install` copies them next to the executables automatically.
- Skip the corresponding vcpkg packages when disabling features: tests → `gtest`, examples → `cxxopts`, libusb components → `libusb`, mock_audio audio file playback / mock_speaker local playback → `miniaudio`.

#### Termux (Android)

Termux uses clang, so the gcc13 minimum version requirement above does not apply.

Install the dependencies:

```bash
pkg install clang cmake ninja pkg-config libasio libspdlog googletest libusb
```

`cxxopts` is not available in the Termux repositories. To build the examples, install it manually (here it is installed to `$PREFIX/opt/cxxopts`):

```bash
git clone https://github.com/jarro2783/cxxopts.git
cd cxxopts
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX/opt/cxxopts
cmake --build build
cmake --install build
```

Configure and build:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -GNinja \
    -DUSBIPDCPP_USE_PKGCONF_ASIO=ON \
    -Dcxxopts_DIR=$PREFIX/opt/cxxopts/share/cmake/cxxopts
cmake --build build
cmake --install build --prefix $PREFIX
```

Notes:

- `-DUSBIPDCPP_USE_PKGCONF_ASIO=ON` is required: Termux's libasio is built with autotools and only ships `asio.pc`, no CMake config.
- Installing cxxopts is optional — without it all examples are skipped (with a configure-time warning). You can also build just the libraries with `-DUSBIPDCPP_BUILD_EXAMPLES=OFF -DUSBIPDCPP_BUILD_TESTS=OFF`.
- `libevdev_mouse` and `mock_uvc_ffmpeg` depend on libevdev / FFmpeg, which have no dev packages in Termux. They are skipped automatically during configure — no extra options needed.
- miniaudio is not available in the Termux repositories. The `--audio` option (`AudioFileSource`) of `mock_audio` and the local playback of `mock_speaker` are skipped automatically; install the header manually to enable it. The stb headers live in `include/stb/` on Termux (include root with vcpkg); the source adapts to both layouts via `__has_include`.
- To build the `termux_libusb_server` example, add `-DUSBIPDCPP_BUILD_EXAMPLE_TERMUX_LIBUSB_SERVER=ON`. Running it via `termux-usb` requires `pkg install termux-api`.

#### Use vcpkg as the package manager:

Please install asio libusb libevdev spdlog in advance.
To build examples, also install cxxopts:
```bash
./vcpkg install asio libusb libevdev spdlog cxxopts
```
To build tests, also install gtest:
```bash
./vcpkg install gtest
```
For mock_audio audio file playback (`AudioFileSource`) or mock_speaker local playback, also install miniaudio:
```bash
./vcpkg install miniaudio
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

### Running Tests

Build with tests enabled (default at top level) and run:

```bash
ctest --test-dir build --output-on-failure
```

The network test suite covers:

- Server restart cycles (`start` → `stop` → `start`), including 100-round loops and stop/connect races
- Client disconnects at every protocol phase: idle (before any data), half-received requests, garbage data, right after import, during URB transfer — both graceful (FIN) and abrupt (RST)
- Device lifecycle: repeated import/release, device contention between clients, nonexistent device requests

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
- `Session.immediately_stop()` is exposed to Python; normally it is only used internally by the server stop flow

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

---

## Usage

```cmake
find_package(usbipdcpp CONFIG REQUIRED)
target_link_libraries(main PRIVATE usbipdcpp::usbipdcpp)

# Or if want to use libusb server

find_package(usbipdcpp CONFIG REQUIRED COMPONENTS libusb)
target_link_libraries(main PRIVATE usbipdcpp::usbipdcpp usbipdcpp::libusb)
```

### Extending Functionality

For a complete guide with both implementation approaches and full code examples, see
[docs/custom-device.md](docs/custom-device.md).

To implement custom USB devices:

1. Define descriptors with `usbipdcpp::UsbDevice`
2. Implement device logic via `AbstDeviceHandler` subclass
3. Handle interface-specific operations with `VirtualInterfaceHandler`, and implements the logic of the endpoints inside
   the interface

For interface definitions, prefer each handler's `make_interface` factory — it creates the interface with the exact
endpoints the handler expects (a hand-written `UsbInterface` can mismatch internal assumptions of the interface handler).
The factory returns a descriptor template only and does NOT bind the handler: interface handlers hold a reference to
their interface, so binding must happen after the device is created (interface in `device->interfaces`, address stable)
via `device->interfaces[i].with_handler<T>(...)`.
`UsbDevice` itself can always be constructed manually for full control.

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

## Examples

**1. libevdev_mouse**

   Through libevdev library, in an OS which supports evdev, by reading `/dev/input/event*`, simulate a usbip mouse
   to implement forwarding local mouse signals.
**2. mock_mouse**

   A relative mouse example based on `RelativeMouseHandler`. By default, it toggles the left button
   every second. With the `--circle` flag, the cursor traces a circular pattern.
**3. mock_keyboard**

   A keyboard demonstration using the `KeyboardHandler` class which simulates pressing and releasing
   the 'A' key every second. Built-in Consumer Control support (volume, play/pause, etc. media keys).
**4. mock_gamepad**

   A gamepad demonstration using the `GamepadHandler` class. Rotates the D-pad through 8 directions,
   sweeps the left analog stick in a circle, and toggles button 0 on/off.
**5. mock_cdc_acm**

   A virtual serial port (CDC ACM) demonstration. Shows bidirectional data transfer over USB bulk endpoints.
**6. mock_cdc_throttle**

   A virtual serial port (CDC ACM) demonstrating **OUT NAK throttling**: over a fixed `--window-sec`
   window it accepts at most `--limit-bytes` bytes; once the budget is consumed the device stalls
   reception (OUT NAK — the host URB hangs) until the window lapses, then resumes and drains the
   queued requests. The handler also counts received `'1'` characters and reports the running tally
   to the host every half-window as `<count>\n`. Usage: `mock_cdc_throttle -l 64 -w 5`.
**7. multi_devices**

   A demonstration with 10 virtual HID devices. Shows how to create multiple devices using a factory pattern.
**8. absolute_mouse**

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
**9. libusb_server**

   A usbip server which can forward all local usb devices, has a extremely simple commandline, type `h` for helps
   and can be used to choose which device to forward. By adding virtual usb devices to share the same ubsip server
   with physical usb devices.
**10. mock_msc**

   A virtual USB Mass Storage (flash drive) device backed by a disk image file.
   Supports BOT (Bulk-Only Transport) protocol and common SCSI commands (INQUIRY, READ CAPACITY,
   READ(10), WRITE(10), MODE SENSE, etc.). The `StorageBackend` abstraction allows swapping the
   underlying storage — the example uses `RawImageBackend` with memory-mapped file I/O, but
   custom backends (e.g. qcow2) can be plugged in via polymorphism.

   Usage: `mock_msc [disk.img]` (defaults to `disk.img`, 4096 blocks × 512 bytes = 2 MiB)

   **Enabling discard/TRIM (punching holes)**:

   The Linux kernel skips VPD queries for USB storage devices by default, so UNMAP commands are never sent.
   Enable it on the client side:

   ```bash
   # Find the device SCSI path
   ls /sys/class/scsi_disk/
   # Write unmap to the path found (replace X:0:0:0 with the actual value)
   echo unmap > /sys/class/scsi_disk/X:0:0:0/provisioning_mode
   ```

   Or make it persistent with a udev rule:
   ```bash
   # /etc/udev/rules.d/50-usb-ssd-trim.rules
   ACTION=="add|change", SUBSYSTEM=="scsi_disk", ATTR{provisioning_mode}="unmap"
   ```

   After enabling, trigger discard manually with `sudo fstrim -v /mountpoint`,
   or schedule it with `sudo systemctl enable fstrim.timer`.

**11. mock_uvc**

   A virtual UVC camera using `ColorBarSource` to output a 320×240 YUY2 color bar test pattern.
   Demonstrates the `UvcVideoControlHandler` + `UvcVideoStreamingHandler` + `VideoSource` combination.
   Functional on both Linux and Windows.

**12. mock_uvc_ffmpeg**

   A virtual UVC camera that reads video files via FFmpeg as the video source. Supports any format
   decodable by FFmpeg (MP4, MKV, AVI, etc.), with optional MJPEG/H264 passthrough mode.

   Usage: `mock_uvc_ffmpeg --video video.mp4` (add `--passthrough` for MJPEG/H264 passthrough)

   Requires FFmpeg libraries (libavformat, libavcodec, libswscale, libavutil).

**13. mock_audio**

   A virtual USB microphone (UAC 1.0). Demonstrates the
   `UacAudioControlHandler` + `UacAudioStreamingSourceHandler` + `AudioSource` combination
   (Feature Unit mute/volume control, sampling rate negotiation, ISO PCM streaming).

   Three audio sources are available:
   - Sine wave test tone (default): `--freq 440 --amp 50`
   - Fourier series synthesis: `--harmonics "440:50,880:25"` (frequency Hz : amplitude % [: phase in radians])
   - Audio file playback: `--audio music.mp3` (WAV/MP3/FLAC/OGG via miniaudio, loops by default;
     requires miniaudio to be installed, skipped automatically when not found)

   Usage: `mock_audio --rates 48000,16000,8000` (the first is the initial rate)

**14. mock_speaker**

   A virtual USB speaker (UAC 1.0, ISO OUT receive direction). Demonstrates the
   `UacAudioControlHandler` + `UacAudioStreamingSinkHandler` + `AudioSink` combination
   (Feature Unit mute/volume control, sampling rate negotiation, ISO OUT PCM consumption).

   Three consumption modes:
   - Local playback (default): via miniaudio, requires miniaudio to be installed,
     skipped automatically when not found
   - WAV recording: `--output out.wav` (writes received PCM to a WAV file, no miniaudio needed)
   - Discard counting (no miniaudio and no `--output`): only counts received bytes

   Usage: `mock_speaker --rates 48000,44100,96000 --channels 2` (the first is the initial
   rate; playback device via `--device <name>`, default = system default)

**15. termux_libusb_server**

   A usbip server which can be used at termux in non-root Android device, execute it by
   `termux-usb -e /path/to/termux_libusb_server /dev/bus/usb/xxx/xxx`

   Since termux-usb only supports passing in one fd, multiple servers can be started on different ports to support multiple devices.
   Use the `USBIPDCPP_LISTEN_PORT` environment variable to specify the listening port.

   For the usage of termux-usb, you can refer to the relevant documentation on the official Termux website.

**16. multi_interface_hid**

   A demonstration of a composite USB device combining **two HID interfaces** (mouse + keyboard) on a single device.
   Shows how to create multi-interface virtual devices using `SimpleVirtualDeviceHandler`.

**17. mock_pipe**

   A generic virtual pipe device (vendor-specific class, bulk IN + bulk OUT). Demonstrates the
   `PipeDeviceHandler` API: blocking `read()` / `write()` like a file stream over the virtual
   device's endpoints (FunctionFS-style FIFO semantics; control requests are also delivered
   through `read()` as `PipeXfer` with `setup_req`).

**18. mock_pipe_hid**

   A HID keyboard implemented with the generic `PipeDeviceHandler` (no `KeyboardHandler` needed) —
   the tutorial example for "implement any bulk/interrupt device with read/write only".
   The key steps for a HID device:

   ```cpp
   // 1. HID interface descriptor (Boot keyboard, interrupt IN endpoint)
   auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);
   // 2. HID descriptor (0x21) appended after the interface descriptor — required by drivers
   pipe->set_class_specific_descriptor({0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00});
   // 3. Report descriptor (0x22) served on GET_DESCRIPTOR
   pipe->set_custom_descriptor(0x22, keyboard_report_descriptor);
   // 4. Data plane: blocking read/write on the endpoint
   pipe->write(PipeXfer{.ep = 0x81, .data = {0x00, 0x00, 0x04, 0, 0, 0, 0, 0}}); // press 'A'
   ```

   Control requests (GET_REPORT / SET_REPORT etc.) arrive through `read()` as `PipeXfer` with
   `setup_req`; answer IN requests with `write(PipeXfer{.ep = 0, .data = ...})`.

**19. mock_ecm**

   A virtual Ethernet adapter (CDC ECM, shows up as a network card on the host). Two backends:

   - Default: user-space `EthernetEchoBackend` — answers ARP/ICMP/TCP echo in pure user
     space (no root needed, works on all platforms)
   - `--tun <ifname>` (Linux/Android only, needs root): `TunBackend` routes the virtual NIC
     into this host's kernel network stack through a TAP interface (e.g. `usbip%d`), so
     ARP/ICMP/TCP and routing are handled by the real kernel

   Usage: `mock_ecm --tun usbip%d` then on the host `ip addr add 192.168.53.2/24 dev usbX`
   and `ping 192.168.53.1`. macOS has no TAP interface (utun is layer-3 only, tuntaposx is
   disabled on new systems), so `--tun` is not built there.

**20. libusb_windows_service**

   A **Windows Service** wrapper for the libusb server (Windows only). Uses the Windows SCM API to run
   `LibusbServer` as a background service with proper lifecycle management (start/stop via `net start`/`net stop`,
   or `sc.exe`). Supports automatic binding of all connected USB devices on startup.

---

## Architecture Overview

USB communication and network I/O are both resource-intensive operations. The architecture combines:

- **asio** for network I/O — connection acceptance is asynchronous (a coroutine-driven accept loop on the network thread)
- **synchronous blocking socket I/O** inside each session thread — simple to write and easy to reason about
- **libusb**'s async API for USB communications (physical devices)

### Why Session I/O Is Synchronous

An earlier version used C++20 coroutines for session data processing, but they were later replaced with synchronous blocking socket I/O for the following reasons:

1. **Architecture mismatch**: This project uses a "per-connection-one-thread" model, where each client connection has its own thread and `io_context`. The core advantage of coroutines is "single-threaded multi-tasking", which cannot be leveraged in this architecture.

2. **Code complexity**: The coroutine and non-coroutine versions had nearly identical logic, but maintaining two sets of code increased maintenance burden.

3. **Compilation overhead**: Coroutine-related template instantiation significantly increased compilation time.

4. **ESP32 considerations**: For embedded platforms, FreeRTOS native tasks are preferred over coroutines, or a single-threaded event loop architecture should be used instead.

The accept loop on the network thread (`Server::do_accept`) still uses a single coroutine — it is simple and self-contained, so none of the concerns above apply to it.

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
| miniaudio + stb | Optional | For mock_audio audio file playback (`--audio`) and mock_speaker local playback (header-only) |

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

### Transfer Data Carrier

Transfer data is managed via [`TransferHandle`](include/protocol.h), an RAII wrapper that automatically frees the transfer handle on destruction. Supports move semantics for ownership transfer. Note that `release()` gives up ownership and requires manual cleanup.

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
| `UacAudioControlHandler` | UAC AudioControl interface (Feature Unit mute/volume control) |
| `UacAudioStreamingSourceHandler` | UAC AudioStreaming interface (ISO PCM streaming) |
| `AudioSource` | Abstract PCM audio source interface for UAC devices |
| `UacAudioStreamingSinkHandler` | UAC AudioStreaming interface (ISO OUT PCM consumption, speaker direction) |
| `AudioSink` | Abstract PCM sink interface for the virtual UAC speaker |
| `SineWaveSource` | Sine wave test tone audio source |
| `FourierSource` | Fourier series synthesis audio source (multiple harmonics with per-harmonic phase) |
| `AudioFileSource` | Audio file source for the mock_audio example (WAV/MP3/FLAC/OGG via miniaudio, resampling, looping; in examples/mock_audio/) |
| `EcmCommunicationInterfaceHandler` | CDC ECM communication interface (ethernet link/status notifications) |
| `EcmDataInterfaceHandler` | CDC ECM data interface (ethernet frame I/O) |
| `NetworkBackend` | Abstract network backend for the virtual ethernet adapter (frame RX/TX + statistics) |
| `EthernetEchoBackend` | User-space echo backend for mock_ecm (answers ARP/ICMP/TCP echo, no root) |
| `TunBackend` | Linux TAP backend: routes virtual NIC frames into the host kernel network stack (Linux/Android only, needs root) |

### Class Hierarchy

```
AbstDeviceHandler
├── LibusbDeviceHandler    (physical devices via libusb)
└── VirtualDeviceHandler   (virtual devices)
    └── SimpleVirtualDeviceHandler
```

### Threading Model

Threads used by a server instance:

1. **Network I/O thread**: One per `Server` — runs `asio::io_context::run()` with the accept coroutine, waiting for client connections
2. **Session thread**: One per connection — synchronous blocking socket I/O and device handler calls
3. **Sender thread**: One per active transfer — drains the response queue and writes to the socket
4. **libusb event thread**: `libusb_handle_events()` loop, only for the libusb backend (`LibusbServer`)
5. **Main thread**: Calls `start()` / `stop()` and controls the server

Each connection has its own thread so that blocking operations on one connection (e.g. device synchronization) never stall
other connections.

`start()` / `stop()` can be called repeatedly to restart the server. `stop()` closes the listener, interrupts every
active session, and joins all session threads before returning — safe to destroy the `Server` afterwards.

Data flows through the system without blocking:

```
Client → session thread (read URB) → device handler → sender thread (write response) → Client
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

## Platform Notes

### Windows: libusb requires driver replacement

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

## Contributing

### Code Note

> 📝 **Language notice**: Public API documentation (doxygen) and runtime messages are in **English**. Internal implementation comments can be in **Chinese or English** — these are the only two languages used in this project. PRs in other languages will not be accepted.

### Branch Workflow

This project uses a single-`main`-branch workflow:

- Bug fixes and small changes go directly to `main`
- Larger or experimental work is done on a separate branch and merged back to `main` when ready
- External contributors: fork this repository and open a PR against `main`
- Releases: bump the version in `CMakeLists.txt`, commit and push, then push a `v*` tag — CI builds and publishes the packages automatically

---

## License

This project is licensed under [LGPLv3](LICENSE).

> **Attribution required**: No matter in what form you use this library — including source code
> reference, dynamic/static linking, derivative works, or redistribution in any product or
> service — you **must** display a clear attribution in a prominent place (e.g. the About /
> Acknowledgements section of your product, or your documentation):
>
> ```text
> This product uses usbipdcpp (https://github.com/yunsmall/usbipdcpp), licensed under LGPLv3.
> ```

If you modify this library and wish to distribute it closed-source, please contact: yun_small@163.com

---

## Acknowledgements

This project builds upon these foundational works:

- [usbipd-libusb](https://github.com/raydudu/usbipd-libusb)
- [usbip](https://github.com/jiegec/usbip)
