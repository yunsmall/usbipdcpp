# usbipdcpp

A C++ library for creating usbip servers

> ✅ USBIP server: Platform-independent implementation via libusb (works wherever libusb is supported)  
> ✅ Virtual HID devices: Create virtual USB devices on any platform without libusb dependency (see `examples/`)

Contributions welcome! 🚀

---

## Architecture Overview

USB communication and network I/O are both resource-intensive operations. This project implements a fully asynchronous architecture using:
- **C++20 coroutines** for network operations
- **asio** for asynchronous I/O
- **libusb**'s async API for USB communications (physical devices)

### Threading Model
Three dedicated threads ensure optimal performance:
1. **Network I/O thread**: Runs `asio::io_context::run()`
2. **USB transfer thread**: Handles `libusb_handle_events()`
3. **Main thread**: Control the behavior of usbip server, start the server

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

---

## Getting Started

### Code Note
> 📝 **Language notice**: Comments/logs primarily use Chinese for efficiency.  
> The code structure remains clear and approachable. PRs for English translations appreciated!

### Extending Functionality
To implement custom USB devices:
1. Define descriptors with `usbipdcpp::UsbDevice`
2. Implement device logic via `AbstDeviceHandler` subclass
3. Handle interface-specific operations with `VirtualInterfaceHandler`, and implements the logic of the endpoints inside the interface

For simple devices, use `SimpleVirtualDeviceHandler` - it provides no-op implementations for standard requests.

---

## ⚠️ Important Windows Notice

Using libusb servers on Windows requires driver replacement:
1. Use [Zadig](https://zadig.akeo.ie/) to install WinUSB driver
    - Select target device (enable "List All Devices" if missing)
    - ⚠️ **WARNING**: Replacing mouse/keyboard drivers may cause input loss
2. After use, revert drivers via:
    - `Win+X` → Device Manager → Select device → Roll back driver

Due to this complexity, we recommend [usbipd-win](https://github.com/dorssel/usbipd-win) for physical devices on Windows.  
This project is ideal for implementing **virtual USB devices** on Windows.

---

## Examples Introduction
1. libevdev_mouse
   
   Through libevdev library, in an OS which supports evdev, by reading `/dev/input/event*`, simulate a usbip mouse 
   to implement forwarding local mouse signals.
2. mock_mouse

   A mouse demonstration which switches left button statu each second, to introduce how to implement a virtual
   HID device.
3. empty_server

   A usbip server that has only one device which has no functions and will not response to any input data.
4. libusb_server

   A usbip server which can forward all local usb devices, has a extremely simple commandline, type `h` for helps 
   and can be used to choose which device to forward. By adding virtual usb devices to share the same ubsip server 
   with physical usb devices.

---

## Building
```bash
cmake -B build
cmake --build build
cmake --install build
```

---

## Usage
```cmake
find_package(usbipdcpp CONFIG REQUIRED)
target_link_libraries(main PRIVATE usbipdcpp::usbipdcpp)

# Or if want to use libusb server

find_package(usbipdcpp CONFIG REQUIRED COMPONENTS libusb)
target_link_libraries(main PRIVATE usbipdcpp::usbipdcpp usbipdcpp::usbipdcpp_libusb)
```

---

## Acknowledgements
This project builds upon these foundational works:
- [usbipd-libusb](https://github.com/raydudu/usbipd-libusb)
- [usbip](https://github.com/jiegec/usbip)  