# usbipdcpp

一个用于创建 usbip 服务器的 C++ 库

> ✅ 已在 Linux 平台实现基于 libusb 的 usbip 服务器功能  
> ✅ 已经实现了创建虚拟 HID usb设备，详细可见example目录  
> ⚠️ **Windows 平台暂不可用**：控制传输存在阻塞问题，欢迎提供解决方案

## 帮助
由于经历有限，里面的注释、日志都采取的中文，部分采用英文，但代码逻辑还是挺好懂的，
认真看一下就能看懂。欢迎提交英文翻译的PR。

对于更多的虚拟usb设备，由于usb设备的种类繁杂，想实现的可使用这个库自行创建。

通过设备描述使用usbipdcpp::UsbDevice类，设备的使用逻辑由AbstDeviceHandler负责，
对于虚拟设备，具体接口的逻辑交由VirtualInterfaceHandler类负责。上述Handler类都是抽象类，
想自行创建设备逻辑需要继承上述类。

欢迎贡献代码！🚀

---

## 编译指南
```bash
cmake -B build
cmake --build build
cmake --install build
```

## 感谢 
非常感谢下面的项目，我从中学到了很多。在本项目中甚至能看到相似的代码:  
- [usbipd-libusb](https://github.com/raydudu/usbipd-libusb)  
- [usbip](https://github.com/jiegec/usbip)  