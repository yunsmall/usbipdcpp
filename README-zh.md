# usbipdcpp

一个用于创建 usbip 服务器的 C++ 库

> ✅ 已在 Linux 平台实现基于 libusb 的 usbip 服务器功能  
> ✅ 已经实现了创建虚拟 HID usb设备，详细可见example目录，可在任意平台创建虚拟usb设备  
> ⚠️ **Windows 平台 libusb server 暂不可用**：控制传输存在阻塞问题，欢迎提供解决方案。

欢迎大佬贡献代码！🚀

## 介绍

usb通信和网络通信都是IO密集型任务，因此本项目全程采取异步架构，使用C++20和asio的协程来进行网络通信，
而对于USB通信则使用libusb框架提供的异步接口通信。

libusb的服务器中一共有三个线程， 用来运行asio io_context.run的基础的网络io线程，
运行libusb的libusb_handle_events的usb传输线程。网络线程收到消息后将消息跨线程使用
libusb_submit_transfer提交到usb通信线程，libusb在设备返回数据后异步调用回调再将传输的数据根据是否unlink，
发还到网络io线程将其发出。通过上述流程可以看出其有着极高的cpu利用效率。

对于用户定义的虚拟usb设备，同样希望在接收到消息后不要阻塞当前线程，
通过将当前收到的任务提交到其他线程处理或快速处理以防止阻塞网络通信线程，再通过回调方式提交返回数据。

## 帮助
由于经历有限，里面的注释、日志都采取的中文，部分采用英文，但代码逻辑还是挺好懂的，
认真看一下就能看懂。另外欢迎提交英文翻译的PR。

对于更多的虚拟usb设备，由于usb设备的种类繁杂，想实现的可使用这个库自行创建。

设备描述使用usbipdcpp::UsbDevice类，设备的使用逻辑由AbstDeviceHandler负责，
对于虚拟设备，具体接口的逻辑交由VirtualInterfaceHandler类负责，端点的具体逻辑也交由接口负责。
上述Handler类都是抽象类，想自行创建设备逻辑需要继承上述类。

对于简单设备可使用SimpleVirtualDeviceHandler类，该类对所有发往设备的标准请求提供了不报错的空实现。


---

## 编译指南
```bash
cmake -B build
cmake --build build
cmake --install build
```

## 感谢 
非常感谢下面的项目，我从中学到了很多。在本项目中能看到这些项目的痕迹:  
- [usbipd-libusb](https://github.com/raydudu/usbipd-libusb)  
- [usbip](https://github.com/jiegec/usbip)  