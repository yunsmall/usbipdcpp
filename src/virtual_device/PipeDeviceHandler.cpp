#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

#include <algorithm>

#include "usbipdcpp/Session.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

using namespace usbipdcpp;

namespace usbipdcpp {

/**
 * @brief 管道接口 handler：接口内所有端点统一转发到设备级 PipeDeviceHandler
 * （数据面状态集中在设备级，read/write 从任意接口拿到统一的数据流）
 * 内部实现类：声明在源文件，头文件的 friend 声明与之匹配
 */
class PipeInterfaceHandler : public VirtualInterfaceHandler {
public:
    PipeInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool, PipeDeviceHandler *pipe) :
        VirtualInterfaceHandler(handle_interface, string_pool), pipe(pipe) {
    }

    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                              std::error_code &ec) override {
        forward_transfer(seqnum, ep, transfer_buffer_length, std::move(transfer));
    }

    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override {
        forward_transfer(seqnum, ep, transfer_buffer_length, std::move(transfer));
    }

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override {
        pipe->on_pipe_control_request(setup_packet, seqnum, transfer_buffer_length, std::move(transfer));
    }

    void handle_non_standard_request_type_control_urb_to_endpoint(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                  std::uint32_t transfer_flags,
                                                                  std::uint32_t transfer_buffer_length,
                                                                  const SetupPacket &setup_packet,
                                                                  TransferHandle transfer,
                                                                  std::error_code &ec) override {
        pipe->on_pipe_control_request(setup_packet, seqnum, transfer_buffer_length, std::move(transfer));
    }

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override {
        pipe->on_pipe_unlink(unlink_seqnum, cmd_seqnum);
    }

    // ========== 标准请求回调（未设置的回调用基类默认行为：feature/status 回成功，未实现的查询类回 EPIPE）==========

    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.clear_feature) {
            pipe->standard_request_handler.clear_feature(*pipe, feature_selector, p_status);
        }
        else {
            VirtualInterfaceHandler::request_clear_feature(feature_selector, p_status);
        }
    }

    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.endpoint_clear_feature) {
            pipe->standard_request_handler.endpoint_clear_feature(*pipe, feature_selector, ep_address, p_status);
        }
        else {
            VirtualInterfaceHandler::request_endpoint_clear_feature(feature_selector, ep_address, p_status);
        }
    }

    std::uint8_t request_get_interface(std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.get_interface) {
            return pipe->standard_request_handler.get_interface(*pipe, p_status);
        }
        return VirtualInterfaceHandler::request_get_interface(p_status);
    }

    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.set_interface) {
            pipe->standard_request_handler.set_interface(*pipe, alternate_setting, p_status);
        }
        else {
            VirtualInterfaceHandler::request_set_interface(alternate_setting, p_status);
        }
    }

    std::uint16_t request_get_status(std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.get_status) {
            return pipe->standard_request_handler.get_status(*pipe, p_status);
        }
        return VirtualInterfaceHandler::request_get_status(p_status);
    }

    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.endpoint_get_status) {
            return pipe->standard_request_handler.endpoint_get_status(*pipe, ep_address, p_status);
        }
        return VirtualInterfaceHandler::request_endpoint_get_status(ep_address, p_status);
    }

    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.set_feature) {
            pipe->standard_request_handler.set_feature(*pipe, feature_selector, p_status);
        }
        else {
            VirtualInterfaceHandler::request_set_feature(feature_selector, p_status);
        }
    }

    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.endpoint_set_feature) {
            pipe->standard_request_handler.endpoint_set_feature(*pipe, feature_selector, ep_address, p_status);
        }
        else {
            VirtualInterfaceHandler::request_endpoint_set_feature(feature_selector, ep_address, p_status);
        }
    }

    [[nodiscard]] data_type get_class_specific_descriptor() override {
        // 通用管道本身没有类描述符；HID 等需要特定 USB 类的设备通过
        // set_class_specific_descriptor 提供（如 HID 描述符 0x21）
        return pipe->class_specific_descriptor;
    }

    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override {
        // GET_DESCRIPTOR（recipient=接口）：自定义描述符按类型返回（如 HID
        // 报告描述符 type=0x22），未设置的类型回 EPIPE（基类默认行为）
        auto it = pipe->custom_descriptors.find(type);
        if (it != pipe->custom_descriptors.end()) {
            return it->second;
        }
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return {};
    }

private:
    void forward_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_buffer_length,
                          TransferHandle transfer) {
        if (ep.is_in()) {
            // IN：主机请求数据，转发设备级（FIFO 匹配或挂起）
            pipe->on_pipe_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
        }
        else {
            // OUT：整个请求挂起（数据留在 transfer），设备级 read() 取走时应答
            // （NAK 背压：业务层不读则主机 URB 挂着，对齐内核 vudc）
            pipe->on_pipe_out_transfer(ep.address, seqnum, std::move(transfer));
        }
    }

    PipeDeviceHandler *pipe;
};

} // namespace usbipdcpp

PipeDeviceHandler::PipeDeviceHandler(UsbDevice &handle_device, StringPool &string_pool) :
    SimpleVirtualDeviceHandler(handle_device, string_pool) {
}

void PipeDeviceHandler::setup_interface_handlers() {
    // 强制所有接口都改为管道接口 handler：数据面/标准请求统一走管道转发
    // （read/write 拿到全部端点数据流），不允许混合使用其他接口 handler，
    // 否则 read/write 会漏掉部分端点。创建放在这个阶段而不是构造函数里，
    // 因为接口 handler 持有本对象指针，需等对象完整构造
    for (auto &intf: handle_device.interfaces) {
        intf.with_handler<PipeInterfaceHandler>(string_pool, this);
    }
    VirtualDeviceHandler::setup_interface_handlers();
}

bool PipeDeviceHandler::read(PipeXfer &xfer, std::uint32_t timeout_ms) {
    // 从各端点 OUT 通道取（挂起的请求取走时应答）。取回的数据包括：
    // bulk OUT 负载、控制 OUT 数据（带 setup_req）、控制 IN 的纯通知
    // （带 setup_req、无数据）。取序 = 主机请求的全局到达顺序：内核 vhci
    // 的 seqnum 全局单调递增，跨端点取 seqnum 最旧的那条（seqnum_newer
    // 环形比较防回绕，每端点内严格 FIFO）
    std::unique_lock lock(pipe_mutex);
    auto take_one = [&]() -> bool {
        // 找到 seqnum 最旧的通道（全局最旧请求）
        OutEndpointChannel *oldest = nullptr;
        for (auto &[ep, ch]: out_channels) {
            auto sn = ch.front_seqnum();
            if (!sn) {
                continue;
            }
            if (!oldest || seqnum_newer(*oldest->front_seqnum(), *sn)) {
                oldest = &ch;
            }
        }
        if (!oldest) {
            return false;
        }
        auto p = oldest->try_take();
        if (p) {
            xfer.ep = p->ep;
            xfer.setup_req = p->setup_req;
            xfer.data = std::move(p->data);
            return true;
        }
        return false;
    };

    while (!disconnected) {
        if (take_one()) {
            return true;
        }
        // 等待数据或断连（断连时把已排队的消费完再返回 false）
        if (timeout_ms == 0) {
            out_cv.wait(lock);
        }
        else {
            if (out_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms)) == std::cv_status::timeout) {
                // 超时时刻数据可能恰好入队：跳出循环走统一判断，避免丢数据
                break;
            }
        }
    }
    // 断连或超时：把已排队的数据消费完再返回 false
    if (take_one()) {
        return true;
    }
    return false;
}

std::size_t PipeDeviceHandler::write(const PipeXfer &xfer, std::uint32_t timeout_ms) {
    if (xfer.ep != 0 && (xfer.ep & 0x80) == 0) {
        // 目标不是 IN 端点（含方向位）也不是控制应答：宿主不会对它发 IN 请求，
        // 数据会滞留 FIFO，属用户误用，防御性提示
        SPDLOG_WARN("PipeDeviceHandler::write 目标端点 {:02x} 不是 IN 端点", xfer.ep);
    }
    ByteStreamInChannel *channel;
    {
        // 只在取通道引用时持 pipe_mutex：阻塞写期间必须释放，否则宿主 IN
        // 请求路径（on_pipe_in_request 也要 pipe_mutex）无法应答请求腾出
        // 空间，写者永远等不到唤醒（死锁）
        std::lock_guard lock(pipe_mutex);
        if (disconnected) {
            return 0;
        }
        channel = &get_in_channel(xfer.ep);
    }
    // 阻塞写：通道内部等空间（缓冲满时宿主取走，timeout_ms=0 无限等），断连
    // 返回已写入量。引用稳定性：断连时 on_disconnection 先遍历通道置断连
    // 标记并唤醒（写者醒来检查断连返回，不再访问通道），之后才清 map
    return channel->write(xfer.data.data(), xfer.data.size(), timeout_ms);
}

ByteStreamInChannel &PipeDeviceHandler::get_in_channel(std::uint8_t ep_addr) {
    // 调用者必须已持有 pipe_mutex
    auto it = in_channels.find(ep_addr);
    if (it == in_channels.end()) {
        it = in_channels.try_emplace(ep_addr).first;
        auto &ch = it->second;
        // 新会话懒创建：容量取当前配置，绑定会话并重置断连状态
        ch.set_capacity(fifo_capacity);
        ch.on_new_connection(responder);
    }
    return it->second;
}

OutEndpointChannel &PipeDeviceHandler::get_out_channel(std::uint8_t ep_addr) {
    // 调用者必须已持有 pipe_mutex
    auto it = out_channels.find(ep_addr);
    if (it == out_channels.end()) {
        it = out_channels.try_emplace(ep_addr).first;
        auto &ch = it->second;
        // 新会话懒创建：绑定会话并重置断连状态
        ch.on_new_connection(responder);
    }
    return it->second;
}

void PipeDeviceHandler::set_in_fifo_capacity(std::size_t capacity) {
    std::lock_guard lock(pipe_mutex);
    fifo_capacity = capacity;
}

void PipeDeviceHandler::set_class_specific_descriptor(data_type descriptor) {
    class_specific_descriptor = std::move(descriptor);
}

void PipeDeviceHandler::set_custom_descriptor(std::uint8_t type, data_type descriptor) {
    custom_descriptors[type] = std::move(descriptor);
}

void PipeDeviceHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    // 父类注册 session 并遍历接口建连
    VirtualDeviceHandler::on_new_connection(current_session, ec);
    if (ec) {
        return;
    }
    // 新会话从干净状态开始：IN/OUT 通道由端点首次访问时懒重建
    std::lock_guard lock(pipe_mutex);
    disconnected = false;
    in_channels.clear();
    out_channels.clear();
}

void PipeDeviceHandler::on_disconnection(error_code &ec) {
    {
        std::lock_guard lock(pipe_mutex);
        disconnected = true;
        // 每通道清缓冲 + 挂起请求（TransferHandle 析构时自动释放）+ 唤醒
        // 阻塞的写者让它们以断连返回
        for (auto &[ep, ch]: in_channels) {
            ch.on_disconnection();
        }
        for (auto &[ep, ch]: out_channels) {
            ch.on_disconnection();
        }
        in_channels.clear();
        out_channels.clear();
    }
    // 唤醒阻塞的 read，让它以"断连"返回
    out_cv.notify_all();
    // 父类遍历接口 on_disconnection 并清除 session 指针
    VirtualDeviceHandler::on_disconnection(ec);
}

void PipeDeviceHandler::on_pipe_in_request(std::uint8_t ep_addr, std::uint32_t seqnum, std::uint32_t length,
                                           TransferHandle transfer) {
    std::lock_guard lock(pipe_mutex);
    // 通道内部处理：缓冲有数据立即应答，否则挂起请求等待 write
    get_in_channel(ep_addr).on_in_request(ep_addr, seqnum, length, std::move(transfer));
}

void PipeDeviceHandler::on_pipe_out_transfer(std::uint8_t ep_addr, std::uint32_t seqnum,
                                             TransferHandle transfer) {
    // 请求挂起到该端点通道（数据留在 transfer），read() 取走时读出并应答；
    // 业务层不读则主机该端点 URB 挂着（NAK 背压，逐端点独立），读走后恢复
    std::lock_guard lock(pipe_mutex);
    get_out_channel(ep_addr).on_out_request(ep_addr, seqnum, std::move(transfer));
    out_cv.notify_all();
}

void PipeDeviceHandler::on_pipe_control_request(const SetupPacket &setup, std::uint32_t seqnum,
                                                std::uint32_t transfer_buffer_length, TransferHandle transfer) {
    std::lock_guard lock(pipe_mutex);
    if (setup.is_out()) {
        // OUT：数据已随控制传输到达，整体挂起（read() 取走时读数据并应答）
        get_out_channel(0).on_out_request(0, seqnum, std::move(transfer), setup);
    }
    else {
        // IN：请求挂起（等 write({ep=0, data}) 应答），并透出 setup 通知业务层：
        // read() 取到纯通知（带 setup_req、无数据）后知道有控制请求待应答
        get_in_channel(0).on_in_request(0, seqnum, transfer_buffer_length, std::move(transfer));
        get_out_channel(0).on_out_request(0, seqnum, TransferHandle{}, setup);
    }
    out_cv.notify_all();
}

void PipeDeviceHandler::on_pipe_unlink(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    bool cancelled = false;
    {
        // 挂起请求分两处：IN 请求按端点分散在各通道里，OUT 请求同理，
        // 逐个尝试取消
        std::lock_guard lock(pipe_mutex);
        for (auto &[ep, ch]: in_channels) {
            if (ch.cancel_pending(unlink_seqnum)) {
                cancelled = true;
                break;
            }
        }
        if (!cancelled) {
            for (auto &[ep, ch]: out_channels) {
                if (ch.cancel_pending(unlink_seqnum)) {
                    cancelled = true;
                    break;
                }
            }
        }
    }
    // 从队列中真的取消了待处理 URB → 回 -ECONNRESET（URB 被取消，且不再发
    // RET_SUBMIT，请求已从队列移除）；找不到（URB 已完成/不存在）→ 回 0。
    // 与内核 stub_tx.c 及本项目 HID/CdcAcm 的 unlink 范本一致
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}
