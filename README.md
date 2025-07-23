# usbipdcpp

A C++ library for creating usbip servers

> ✅ Linux server: libusb-based usbip implementation  
> ✅ Cross-platform virtual HID: Create virtual USB devices on any OS (see `examples/`)  
> ⚠️ **Windows libusb server unavailable**: Control transfers hang - solutions welcome!

Contributions welcome! 🚀

## Architecture Overview

USB communication and network I/O are both resource-intensive operations. This project implements a fully asynchronous architecture using:
- **C++20 coroutines** for network operations
- **asio** for asynchronous I/O
- **libusb**'s async API for USB communications

### Threading Model
Three dedicated threads ensure optimal performance:
1. **Network I/O thread**: Runs `asio::io_context::run()`
2. **USB transfer thread**: Handles `libusb_handle_events()`
3. **Worker thread pool**: Processes device logic

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
3. Handle interface-specific operations with `VirtualInterfaceHandler`
4. Manage endpoint logic within interfaces

For simple devices, use `SimpleVirtualDeviceHandler` - it provides no-op implementations for standard requests.

---

## Building
```bash
cmake -B build
cmake --build build
cmake --install build
```

---

## Acknowledgements
This project stands on the shoulders of giants. Special thanks to:
- [usbipd-libusb](https://github.com/raydudu/usbipd-libusb)
- [usbip](https://github.com/jiegec/usbip)  