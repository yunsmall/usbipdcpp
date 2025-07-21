# usbipcpp

A C++ library for creating usbip servers.

> Currently in very early development stage.  
> Functional implementation has been achieved for Linux using the libusb-based usbip server,  
> but **Windows support is temporarily unavailable** as control transfers get stuck. Solutions are welcome!

The goal is to enable dynamic addition of virtual USB devices and share them over the network.  
Due to the complexity of the USB protocol, additional work is required to achieve this.

Contributions are welcome!

---

## Building
```bash
cmake -B build
cmake --build build
cmake --install build
```

---

## Acknowledgements
Special thanks to these projects, from which I learned immensely. You may even find similar code patterns in this project:
- [usbipd-libusb](https://github.com/raydudu/usbipd-libusb)
- [usbip](https://github.com/jiegec/usbip)