# usbipcpp

A C++ library for creating usbip servers

> ✅ Linux support: Implemented libusb-based usbip server functionality  
> ✅ Virtual HID device: Added virtual HID USB device creation (see `examples/` directory)  
> ⚠️ **Windows not supported**: Control transfers currently hang - solutions welcome!

## Getting Help
> 📝 **Note on language**: Due to time constraints, code comments and logs primarily use Chinese with some English.  
> The code logic remains clear and understandable - with careful reading you should grasp the implementation.  
> PRs for English translations are welcome!

To implement additional virtual USB devices (given USB protocol complexity):
1. Define device descriptors using `usbipcpp::UsbDevice`
2. Implement device logic by inheriting from `AbstDeviceHandler`
3. For virtual devices, implement interface logic via `VirtualDeviceHandler` subclassing

Contribute your device implementations! 🚀

---

## Building
```bash
cmake -B build
cmake --build build
cmake --install build
```

---

## Acknowledgements
Special thanks to these projects - I learned immensely from them. You may recognize similar patterns:
- [usbipd-libusb](https://github.com/raydudu/usbipd-libusb)
- [usbip](https://github.com/jiegec/usbip)  