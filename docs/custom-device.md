# 如何实现一个自定义 USB 虚拟设备

本文讲解如何在 usbipdcpp 中实现自定义的 USB 虚拟设备（不需要真实硬件），包含两种方法：

| 方法 | 需要了解的 USB 知识 | 适用场景 |
|---|---|---|
| [方法一：PipeDeviceHandler](#方法一pipedevicehandler通用管道) | 几乎为零 | 只要一个「数据流」设备：主机写进来、设备读走；设备写出去、主机读走 |
| [方法二：继承 VirtualInterfaceHandler](#方法二继承virtualinterfacehandler完全自定义) | 需要了解端点、传输类型、控制请求 | 需要实现特定 USB 类协议（HID/UAC/UVC/MSC），或对控制请求有特殊处理 |

两种方法的共同骨架是一样的（定义设备模型 → 绑定 handler → 启动服务器），区别只在「handler 怎么实现」。

## 背景：虚拟设备的三层结构

虚拟设备按 USB 设备的三级结构组织：

```
UsbDevice（设备描述符 + 接口列表）
 ├── UsbInterface[0] ──── VirtualInterfaceHandler（接口级 handler）
 ├── UsbInterface[1] ──── VirtualInterfaceHandler
 └── ...
 + 设备级 handler：VirtualDeviceHandler / SimpleVirtualDeviceHandler
```

**数据流**（客户端 `usbip attach` 之后）：

```
主机发 CMD_SUBMIT（一次 USB 传输）
  → Session::receiver 线程读取
  → 设备级 handler 的 receive_urb()
  → 按端点类型分发：
     控制传输 → 设备级 handle_control_urb（标准请求内部消化，非标准转接口 handler）
     Bulk/Interrupt/Iso → 对应接口的 handler
  → handler 通过 session->submit_ret_submit() 提交应答
  → Session::sender 线程把 RET_SUBMIT 发回主机
```

**描述符是自动生成的**：设备级 handler 根据你定义的 `UsbDevice` / `UsbInterface` / `UsbEndpoint` 结构自动拼出设备描述符、配置描述符、字符串描述符，主机枚举时直接使用，无需手工构造字节流。

## 第一步：定义设备模型

> **库内置 handler 的接口不要手写**：每个内置 InterfaceHandler 都提供同名静态工厂
> `make_interface(StringPool&, <每个端点一个地址参数>...)`，直接返回已绑好 handler 的
> 完整 `UsbInterface`（如 `KeyboardHandler::make_interface(pool, 0x81)`）。接口 handler
> 内部可能对接口结构有硬编码假设（如构造函数里记录端点地址、假定端点数量/属性），
> 手写接口与 handler 的假设不符会导致枚举或数据传输异常，因此 **UsbInterface 一律
> 建议用工厂创建**（自定义 handler 也照此提供自己的工厂，见 [make_interface 工厂](#make_interface-工厂)）。
> `UsbDevice` 则没有此约束——想更灵活地配置时完全可以手动构造（所有成员都可改）。
> 下面的手写 `UsbInterface`/`UsbEndpoint` 仅用于理解底层结构。

所有设备都从定义描述符结构开始：

```cpp
#include "usbipdcpp/Server.h"
#include "usbipdcpp/Device.h"
#include "usbipdcpp/utils/StringPool.h"

using namespace usbipdcpp;

StringPool string_pool;

// 接口定义：class/subclass/protocol 是 USB 类标识（0xFF = vendor 自定义类）
std::vector<UsbInterface> interfaces = {
        UsbInterface{
                .interface_class = 0xFF,        // vendor specific
                .interface_subclass = 0x00,
                .interface_protocol = 0x00,
                .endpoints = {{                 // 外层 = alternate setting 列表，内层 = 该 alt 的端点
                        UsbEndpoint{
                                .address = 0x81,          // 0x80 | 端点号 1 = IN（设备 → 主机）
                                .attributes = 0x02,       // Bulk（0x03 = Interrupt，0x01 = Isochronous）
                                .max_packet_size = 64,    // Full speed 的 bulk 上限
                                .interval = 0,
                        },
                        UsbEndpoint{
                                .address = 0x02,          // OUT（主机 → 设备）
                                .attributes = 0x02,
                                .max_packet_size = 64,
                                .interval = 0,
                        },
                }},
        },
};

// 设备定义：VID/PID、速度、接口列表、EP0
auto device = std::make_shared<UsbDevice>(UsbDevice{
        .path = "/usbipdcpp/my_device",   // 设备路径（usbip list 显示用）
        .busid = "1-1",                   // busid（usbip attach -b 用的标识）
        .bus_num = 1,
        .dev_num = 1,
        .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
        .vendor_id = 0x1234,
        .product_id = 0x5678,
        .device_bcd = 0x0100,
        .device_class = 0x00,             // 0 = 在接口层定义类
        .device_subclass = 0x00,
        .device_protocol = 0x00,
        .configuration_value = 1,
        .num_configurations = 1,
        .interfaces = interfaces,
        .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
        .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
});
```

关键点：

- **端点地址**：高位置 1 是 IN（设备→主机），低 7 位是端点号。EP0 是控制端点，不用手动定义（上面的 `ep0_in`/`ep0_out` 字段）
- **`attributes`**：低 2 位是传输类型——`0x00` 控制、`0x01` 等时、`0x02` Bulk、`0x03` 中断。HID 用中断端点，存储/MSC 用 Bulk
- **`endpoints` 是 `vector<vector<UsbEndpoint>>`**：外层下标是 alternate setting（alt 0 必须有）
- **`speed`** 决定 EP0 大小（Full=64，Low=8）和描述符里的 bMaxPacketSize0，也决定了 `max_packet_size` 的合理取值

## 公共步骤：绑定 handler 并启动服务器

两种方法共用的骨架（方法一/二各自替换 `with_handler` 那一行）：

```cpp
// 1. 绑定设备级 handler（关键一步）
auto handler = device->with_handler<XXXDeviceHandler>(string_pool);
handler->setup_interface_handlers();   // 把接口 handler 的端点注册进设备级路由表

// 2. 注册到服务器并启动
Server server;
server.add_device(std::move(device));

asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 53240};
if (auto ec = server.start(endpoint); ec) {
    SPDLOG_ERROR("服务器启动失败：{}", ec.message());
    return 1;
}
// 连接：usbip attach -r <主机> -b 1-1
```

---

## 方法一：PipeDeviceHandler（通用管道）

`PipeDeviceHandler` 把设备实现成一组 FIFO：**不需要实现任何 USB 协议细节**，所有端点自动管道化，标准控制请求自动处理。业务代码只面对两个阻塞函数：

```cpp
#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

struct PipeXfer {
    std::uint8_t ep;                  // 端点地址（含方向位）；控制请求时为 0
    std::optional<SetupPacket> setup_req; // 仅控制请求（ep==0）时有效
    data_type data;                   // OUT 数据或控制请求数据
};

bool read(PipeXfer &xfer, std::uint32_t timeout_ms = 0);       // 等一个 OUT 传输或非标准控制请求
std::size_t write(const PipeXfer &xfer, std::uint32_t timeout_ms = 0); // 发数据到 IN 端点（FIFO 满则阻塞）
```

- `read()`：阻塞等待主机发来的 OUT 数据（或 class/vendor 控制请求，此时 `setup_req` 有值）。`timeout_ms = 0` 无限等；断连返回 false
- `write()`：把数据写入指定 IN 端点的 FIFO。**FIFO 满时阻塞**（等主机把数据读走），`timeout_ms = 0` 无限等；断连返回 0
- 退出语义：断连时 `read` 返回 false、`write` 返回 0；`read` 会先把已排队的 OUT 数据消费完再返回 false

### 完整示例（回显设备）

```cpp
#include <atomic>
#include <thread>

#include "usbipdcpp/Server.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

using namespace usbipdcpp;

int main() {
    StringPool string_pool;

    // （第一步里的 interfaces 定义，bulk IN 0x81 + bulk OUT 0x02，vendor 类）

    auto device = std::make_shared<UsbDevice>(UsbDevice{ /* 第一步里的设备定义 */ });
    auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);
    pipe->setup_interface_handlers();

    Server server;
    server.add_device(std::move(device));
    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 53240};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    std::atomic<bool> running{true};
    // 业务线程：收到的 OUT 数据原样发回 IN 端点
    std::thread echo_thread([&]() {
        PipeXfer xfer;
        while (running) {
            if (pipe->read(xfer, 200)) {               // 200ms 超时轮询，便于退出
                pipe->write(PipeXfer{.ep = 0x81, .data = std::move(xfer.data)}, 200);
            }
        }
    });

    std::cin.get();
    running = false;      // 先停业务线程
    echo_thread.join();
    server.stop();        // 再停服务器
    return 0;
}
```

完整可编译版本见 `examples/mock_pipe/mock_pipe_main.cpp`（含回显 + 周期消息两个线程）。

**退出顺序有讲究**：业务线程必须**先退出**（不再调用 `read`/`write`），再
`server.stop()`——否则 handler 随会话析构时，业务线程还阻塞在 read/write 上
就是 use-after-free（示例里用 `running` 标志 + 短超时轮询实现）。

### 需要 USB 类识别符时

管道设备默认是 vendor 类（接口 class=0xFF），**主机端没有驱动会加载**。要让主机按特定设备类识别（如 HID 鼠标），两个步骤：

1. 接口的 `interface_class` 改成对应类（如 `ClassCode::HID`），并把端点类型改成该类的规范（HID 用中断端点）
2. 提供类描述符——`PipeDeviceHandler` 本身不生成类描述符，需要手动设置（在连接前）：

```cpp
// HID 设备：HID 描述符（type 0x21）挂在接口描述符之后
pipe->set_class_specific_descriptor({
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3E, 0x00,  // HID 描述符（9 字节）
});
// HID 报告描述符（type 0x22）：主机 GET_DESCRIPTOR 时按类型返回
pipe->set_custom_descriptor(0x22, report_descriptor_bytes);
```

参考 `examples/mock_pipe_hid/` 的完整例子。

### 标准请求行为可配置

管道设备的接口级标准请求默认行为：接受接口定义里存在的 alt（SET_INTERFACE，
其余拒绝）、其余请求接受并回成功。需要特殊行为时，在连接前设置回调结构体
`standard_request_handler`——8 个回调对应 8 个接口/端点级标准请求，未设置的回调
保持默认行为。回调在 session receiver 线程调用，第一个参数是 handler 自身的引用：

```cpp
// 演示：拒绝接口级 SET_FEATURE（该设备不支持任何接口级 feature）
PipeStandardRequestHandler handler;
handler.set_feature = [](PipeDeviceHandler &pipe, std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
};
pipe->set_standard_request_handler(std::move(handler));
```

| 回调成员 | 对应请求 | 回调参数（除 pipe 引用外） |
|---|---|---|
| `clear_feature` / `set_feature` | 接口级 CLEAR/SET_FEATURE | feature_selector, p_status |
| `endpoint_clear_feature` / `endpoint_set_feature` | 端点级 CLEAR/SET_FEATURE | feature_selector, ep_address, p_status |
| `get_interface` | GET_INTERFACE | p_status（返回当前 alt） |
| `set_interface` | SET_INTERFACE | alternate_setting, p_status |
| `get_status` / `endpoint_get_status` | 接口/端点级 GET_STATUS | (ep_address), p_status（返回状态值） |

`p_status` 是应答状态（0 = 成功），非 0 时主机收到对应的 USB 错误。

### 适用边界

- 数据面只有「读 OUT 流 + 写 IN 流」两种操作——所有端点共享同一个数据流，没有按端点区分的语义
- 标准控制请求（枚举）自动处理；**非标准**控制请求（class/vendor setup）会出现在 `read()` 返回的 `PipeXfer` 里（`ep==0`、`setup_req` 有值），应答用 `write({.ep = 0, .data = ...})`
- 不适合：需要响应主机的带状态查询/配置类协议（如音频的采样率协商、UVC 的流参数协商）——这些控制请求又多又杂，管道模型处理起来不自然

---

## 方法二：继承 VirtualInterfaceHandler（完全自定义）

**这是实现自定义设备的推荐方式。** 需要实现类特定协议时，继承接口级 handler。先看继承关系：

```
AbstInterfaceHandler
 └── VirtualInterfaceHandler      ← 虚拟设备的接口级基类
      ├── HidVirtualInterfaceHandler   （HID 类已实现）
      ├── CdcAcmVirtualInterfaceHandler（CDC ACM 串口类已实现）
      └── 你自己的 handler             ← 从这里继承
```

设备级 handler 用 `SimpleVirtualDeviceHandler`（标准请求默认实现都回 OK），接口 handler 的标准请求回调也都有默认实现。

### 标准请求：库内部处理，默认行为已够用

**标准控制请求（枚举/状态查询）的解析和应答全部在库内部完成**（设备级
`VirtualDeviceHandler::handle_control_urb`）：

```
主机发标准控制请求（bmRequestType 低 5 位 = 0，如 GET_DESCRIPTOR / SET_ADDRESS /
SET_CONFIGURATION / GET_STATUS / GET_INTERFACE / SET_INTERFACE / SET_FEATURE ...）
  └→ 设备级 handle_control_urb 解析并应答
       ├─ recipient = 设备   → 设备级内部处理（描述符、地址、配置……全部自动）
       └─ recipient = 接口/端点 → 回调本 handler 的 request_*（8 个标准请求回调）
```

继承 `VirtualInterfaceHandler` 后**不需要强制实现任何函数**，基类默认行为已是最简
正确行为，只有特殊需求的设备才重写：

| 回调 | 默认行为 |
|---|---|
| 8 个标准请求回调（`request_*`） | 接受并回成功；`request_set_interface` 只接受设备定义里存在的 alt（其余回 EPIPE）；`request_get_interface` 返回当前 alt |
| `get_class_specific_descriptor` | 返回空（没有类描述符） |
| 数据面 5 个（`handle_bulk_transfer` / `handle_interrupt_transfer` / `handle_isochronous_transfer` / `handle_non_standard_request_type_control_urb`（及 to_endpoint）） | 回 EPIPE——**必须重写**，否则主机发的传输全部报错 |

重写示例（接口级 SET_FEATURE 一律拒绝）：

```cpp
void request_set_feature(std::uint16_t, std::uint32_t *p_status) override {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}
```

**只有非标准请求（CLASS/VENDOR 类型）才需要你自己解析**——回调是
`handle_non_standard_request_type_control_urb`（默认回 EPIPE）。例如 HID 的
GET_REPORT/SET_REPORT 是 class 请求，由 `HidVirtualInterfaceHandler` 在这个回调里解析。

### 核心机制：IN 数据面的「挂起-应答」模式

USB 设备协议是**异步**的：主机发 IN 请求时设备可能没有数据，设备有数据时主机可能没在请求。库提供 `InEndpointChannel` 组件（`include/usbipdcpp/virtual_device/InEndpointChannel.h`）封装这种错峰：

```
主机发 IN 请求（handle_bulk_transfer，ep 是 IN）
  ├─ 数据缓冲非空 → 立即填 transfer 回 RET_SUBMIT
  └─ 无数据 → 请求挂起（按端点排队），等数据

业务线程产生数据（push / write）
  ├─ 有挂起的请求 → 从队列取出应答
  └─ 无挂起请求 → 数据进缓冲，等主机下次请求
```

组件内部完成请求排队/应答、双锁（数据侧阻塞等空间时不占用请求队列锁）、断连
清理与唤醒；数据填充走 `TransferHandle` 持有的 `TransferOperator::set_transfer_data`
（组件不假设 transfer 内部结构，GenericTransfer / libusb 等任意实现都可以）。
按消费语义分两种模式：

| 模式 | 类 | 适用场景 | 生产数据入口 |
|---|---|---|---|
| 消息模式 | `MessageInChannel` | 一条数据 = 一个完整消息：HID 输入报告、UVC 状态通知 | `push(data)` 非阻塞，缓冲超上限丢最旧 |
| 字节流模式 | `ByteStreamInChannel` | 数据是连续字节流按请求长度分片：CDC 数据接口、通用管道 | `write(data, timeout_ms)` 阻塞，缓冲满等宿主取走（对齐内核 FIFO） |

**完整示例：一个回显接口 handler**（OUT 收什么，IN 还什么，顺带响应一个 vendor 控制请求）：

```cpp
#include "usbipdcpp/Session.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/VirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
// 以上是接口 handler 类所需的 includes；整个程序还需要第一步的 includes
// （StringPool / Device / Server）

class EchoInterfaceHandler : public VirtualInterfaceHandler {
public:
    EchoInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
        VirtualInterfaceHandler(handle_interface, string_pool) {
        // 缓冲上限：主机长期不读时丢最旧保最新，防止内存无限增长
        echo_channel.set_max_pending(1024);
    }

    /**
     * @brief 创建回显接口（已绑定本 handler）。库内置 handler 都提供同名工厂，
     * 自定义 handler 可选提供（约定见下文「make_interface 工厂」小节）：
     *
     * 形参 = 该接口每类端点一个地址（参数名即用途，调用方可显式指定任意地址），
     * 端点属性/方向由工厂给定合理默认，只暴露地址可变；返回的接口已绑好 handler。
     * @param string_pool 字符串池（需活得比 handler 久）
     * @param in_ep Bulk IN 端点地址（设备→主机）
     * @param out_ep Bulk OUT 端点地址（主机→设备）
     */
    static UsbInterface make_interface(StringPool &string_pool, std::uint8_t in_ep, std::uint8_t out_ep) {
        UsbInterface i{
                .interface_class = static_cast<std::uint8_t>(ClassCode::VendorSpecific),
                .interface_subclass = 0x00,
                .interface_protocol = 0x00,
                .endpoints = {{UsbEndpoint{.address = in_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                           .max_packet_size = 64,
                                           .interval = 0},
                               UsbEndpoint{.address = out_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                           .max_packet_size = 64,
                                           .interval = 0}}},
        };
        i.with_handler<EchoInterfaceHandler>(string_pool);
        return i;
    }

    // ===== 生命周期：会话绑定与断连清理（通道内部状态必须在这两个回调里维护） =====
    void on_new_connection(Session &current_session, error_code &ec) override {
        // 父类先设 session 指针（通道应答请求要用），再绑定通道并重置断连状态
        VirtualInterfaceHandler::on_new_connection(current_session, ec);
        echo_channel.on_new_connection(&current_session);
    }

    void on_disconnection(error_code &ec) override {
        // 先清通道（缓冲 + 挂起请求，TransferHandle 析构时自动释放），再调父类清 session
        echo_channel.on_disconnection();
        VirtualInterfaceHandler::on_disconnection(ec);
    }

    // ===== 数据面 =====
    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                              error_code &ec) override {
        if (ep.is_in()) {
            // ===== IN：主机请求数据，全部交给通道（有数据立即应答，无数据挂起等 push） =====
            echo_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
        }
        else {
            // ===== OUT：取出主机数据 =====
            auto *trx = GenericTransfer::from_handle(transfer.get());
            // 主机的 OUT 请求变成了设备要产生的数据，故需要通过 IN 请求发回
            // （有挂起请求时通道直接应答）
            echo_channel.push(data_type(trx->data.begin(), trx->data.end()));
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, 0));
        }
    }

    // ===== 非标准控制请求（class/vendor setup）：这里回应 vendor 请求 0xAA =====
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup, TransferHandle transfer,
                                                      error_code &ec) override {
        if (setup.request == 0xAA && !setup.is_out()) {
            auto *trx = GenericTransfer::from_handle(transfer.get());
            trx->data = {0x42, 0x00};  // 应答数据
            trx->actual_length = 2;
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                    seqnum, 2, std::move(transfer)));
        }
        else {
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        }
    }

    // ===== UNLINK：主机取消挂起的传输 =====
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override {
        bool cancelled = echo_channel.cancel_pending(unlink_seqnum);
        session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
    }

private:
    // 回显通道（消息模式）：OUT 收到的数据经它发回，一条 OUT 数据 = 一条消息
    MessageInChannel echo_channel;
};
```

> 注意：
> - **生命周期回调是必须的**：`on_new_connection` 里绑定会话并重置断连状态，
>   `on_disconnection` 里清缓冲与挂起请求（漏掉会用悬垂 session 指针或残留
>   上一会话的状态）
> - 多端点场景：一个通道管一个端点（`on_in_request` 传端点地址，同端点的请求
>   保持 FIFO 顺序），按端点懒建通道的参考见 `PipeDeviceHandler::get_in_channel`
> - 字节流模式：把上面的 `MessageInChannel` 换成 `ByteStreamInChannel`、`push`
>   换成 `write`（可带 `timeout_ms` 阻塞等宿主取走）即可。字节流还支持 pull
>   模型：`set_pull_callback` 在 IN 请求到达且缓冲/队列全空时现场生成数据
>   （CDC 数据接口的 `on_data_requested` 就是这个用途）。回调在锁内调用，
>   回调里不要再调用本通道的写方法（会死锁）
> - 完整参照：`src/virtual_device/HidVirtualInterfaceHandler.cpp`（消息模式 +
>   push）、`src/virtual_device/CdcAcmVirtualInterfaceHandler.cpp`（字节流模式 +
>   阻塞写 + pull）、`src/virtual_device/PipeDeviceHandler.cpp`（按端点懒建通道）

### 完整文件：绑定与使用

两个绑定入口的区别：

- `device->with_handler<T>(...)`——绑定**设备级** handler（每个虚拟设备一个）
- `interfaces[i].with_handler<T>(...)`——给**单个接口**绑定接口级 handler（方法二手动
  绑，或用该 handler 的 `make_interface` 自动绑）。
  `setup_interface_handlers()` 只注册绑定了 handler 的接口的端点，**没绑 handler 的接口
  主机访问不到**（设备级 handler 不会自动创建接口 handler；方法一的 PipeDeviceHandler
  会在 setup_interface_handlers 时给所有接口自动绑管道接口 handler，所以方法一不需要手动绑）

### make_interface 工厂

库内置 handler（`KeyboardHandler`/`RelativeMouseHandler`/`AbsoluteMouseHandler`/
`GamepadHandler`/`DigitizerHandler`/`MscBulkOnlyHandler`/`CdcAcmCommunicationInterfaceHandler`/
`CdcAcmDataInterfaceHandler`）都提供同名静态工厂：

```cpp
// 形参 = 该接口每个端点一个地址（参数名即用途），调用方可显式指定任意地址；
// 端点属性（attributes/max_packet_size/interval/方向）由工厂给定合理默认，
// 返回值是已绑好 handler 的完整 UsbInterface
static UsbInterface make_interface(StringPool &string_pool, std::uint8_t in_ep, std::uint8_t out_ep);
```

约定：

- **端点地址必须显式传**（不能省）——外界要能控制端点号，参数名即该端点的用途
- **handler 构造带业务参数**（如 AbsoluteMouseHandler 的屏幕尺寸、DigitizerHandler 的
  坐标上限）：工厂把这些参数透传（带默认值），只暴露真正需要改的
- **同方向多端点需要区分"作用"**（如同一接口两个 IN 端点用途不同）：工厂的地址参数
  不足以表达语义，由该 handler 的**构造函数**额外收一份「用途端点地址」，工厂在
  with_handler 时透传，handler 内部按用途记住地址、按 ep 分流。框架层不做映射猜测
- 自定义 handler 可选提供同名工厂（上面 EchoInterfaceHandler 就是范例），不提供也不
  影响——`UsbInterface` 仍可手写聚合 + `with_handler` 直接绑定
- **UsbDevice 可以手动构造**：需要比 make 参数更灵活的配置时（如接口数量动态变化），
  直接 `UsbDevice{...}` 聚合初始化即可（所有成员都可改）；**UsbInterface 则建议一律
  走工厂**——接口 handler 内部可能硬编码了接口的端点结构（构造时记录端点地址、
  假定端点数量/属性），手写接口与 handler 的假设不一致会在枚举或数据传输时出问题
- 设备级同理：`UsbDevice::make(busid, vid, pid, interfaces, ...)` 一步建好设备——
  默认参数按改动频率从高到低排列（`bus_num`/`dev_num`/`device_class`/`path`/`speed` 在前，
  后面是 `device_bcd`/`device_subclass`/`device_protocol`/`configuration_value`/
  `num_configurations`），EP0 按 speed 自动生成；返回后仍可改任意字段再绑定设备级 handler

把上面各段拼起来，一个完整的自定义设备文件长这样（接口定义用
`EchoInterfaceHandler::make_interface`；数据面在接口 handler 内部，设备不需要业务线程）：

```cpp
#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/VirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"

using namespace usbipdcpp;

// 这里放上面定义的 EchoInterfaceHandler 类

int main() {
    StringPool string_pool;

    // EchoInterfaceHandler::make_interface 返回已绑定 EchoInterfaceHandler 的
    // 完整接口（vendor 类，bulk IN 0x81 + bulk OUT 0x02）
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678,
                                  {EchoInterfaceHandler::make_interface(string_pool, 0x81, 0x02)});
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();   // 必须：注册端点路由 + 接口 handler

    Server server;
    server.add_device(std::move(device));
    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 53240};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    std::cin.get();
    server.stop();
    return 0;
}
```

### 更真实的参考实现

| 文件 | 特点 |
|---|---|
| `include/usbipdcpp/virtual_device/devices/KeyboardHandler.h` + 实现 | 继承 `HidVirtualInterfaceHandler` 的完整设备：报告描述符、报告数据面、LED 状态、`wait_for_client` |
| `examples/mock_keyboard/mock_keyboard_main.cpp` | 上述 handler 的完整用法（含 `press_key` 业务线程） |
| `src/virtual_device/HidVirtualInterfaceHandler.cpp` | 中断端点的挂起-应答（`MessageInChannel`）+ `send_input_report` push 数据 + UNLINK，最干净的参照 |
| `src/virtual_device/CdcAcmVirtualInterfaceHandler.cpp` | 多端点 + 阻塞写（`ByteStreamInChannel`，缓冲满等宿主取走）+ pull 回调（`on_data_requested`），接口有 alt 时的样例 |
| `src/virtual_device/PipeDeviceHandler.cpp` | 设备级聚合多个接口 handler 的样例（也证明了 handler 可以转发给设备级） |

## 两种方法对比

| | PipeDeviceHandler | 继承 VirtualInterfaceHandler |
|---|---|---|
| 学习成本 | 低：read/write 两个函数 | 高：要懂端点/传输类型/控制请求 |
| 数据面 | 统一数据流（所有端点共享） | 每端点/每接口独立语义 |
| 控制请求 | 标准请求自动处理；非标准的经 read() 透出 | 完全自己实现（可精确应答每种 class/vendor 请求） |
| 典型用途 | 通用管道、串口转发、简单回显、HID 快速原型 | HID/UAC/UVC/MSC 等真实 USB 类、带状态协商的设备 |
| 主机驱动 | 默认 vendor 类无驱动；需类描述符时手动 set_* | 接口 class 正确 + 类描述符齐全，主机驱动直接加载 |

**怎么选：实现自定义设备推荐方法二（继承 `VirtualInterfaceHandler`）**。
方法一适合「整设备就是一个字节管道」的场景（如把虚拟设备当文件读写），
一旦要按端点区分语义、或实现特定 USB 类协议（HID 报告、CDC 控制线状态、
UVC 流协商等），管道模型就不够用——方法二的回调里可以精确控制每个端点的
行为，标准请求仍由库消化。项目内所有类设备（HID / CDC ACM / UVC）都是
方法二实现，`PipeDeviceHandler` 本身也是通过内部接口 handler 走同一套回调。

## 验证

1. 服务器启动后，主机端 `usbip list -r <服务器>` 能看到设备
2. `usbip attach -r <服务器> -b <busid>` 连接（本项目的 usbip 工具用法：`usbip -t <端口> attach -r ...`）
3. 连接后按你的设备类型操作（写数据/读数据/看 `dmesg` 的枚举日志）
4. Windows 客户端：`usbipd` / wsl 里 attach 后看设备管理器是否枚举出对应驱动

调试提示：`spdlog::set_level(spdlog::level::trace)` 可打开每个 CMD_SUBMIT/RET_SUBMIT 的流转日志（`src/virtual_device/VirtualDeviceHandler.cpp` 顶部定义 `USBIPDCPP_STRACE` 还能看到所有控制请求明细，排查枚举卡住很有用）。
