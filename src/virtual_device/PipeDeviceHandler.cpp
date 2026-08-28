#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

#include <algorithm>

#include "usbipdcpp/Session.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"
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

    // ========== 标准请求回调（未设置的回调用基类默认行为：接受并回成功）==========

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
            // OUT：主机发来数据，取出负载交给设备级（回 OK 由设备级完成）
            auto *trx = GenericTransfer::from_handle(transfer.get());
            auto received_size = static_cast<std::uint32_t>(trx->data.size());
            pipe->on_pipe_out_transfer(ep.address, seqnum, received_size, std::move(trx->data));
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
    std::unique_lock lock(pipe_mutex);
    // 等待数据或断连（断连时把已排队的数据消费完再返回 false）
    while (out_queue.empty() && !disconnected) {
        if (timeout_ms == 0) {
            read_cv.wait(lock);
        }
        else {
            if (read_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms)) == std::cv_status::timeout) {
                // 超时时刻数据可能恰好入队：跳出循环走统一判断，避免丢数据
                break;
            }
        }
    }
    if (out_queue.empty()) {
        return false; // 断连且无剩余数据
    }
    xfer = std::move(out_queue.front());
    out_queue.pop_front();
    return true;
}

std::size_t PipeDeviceHandler::write(const PipeXfer &xfer, std::uint32_t timeout_ms) {
    if (xfer.ep != 0 && (xfer.ep & 0x80) == 0) {
        // 目标不是 IN 端点（含方向位）也不是控制应答：宿主不会对它发 IN 请求，
        // 数据会滞留 FIFO，属用户误用，防御性提示
        SPDLOG_WARN("PipeDeviceHandler::write 目标端点 {:02x} 不是 IN 端点", xfer.ep);
    }
    const auto *data = xfer.data.data();
    std::size_t size = xfer.data.size();
    std::size_t total_written = 0;
    std::size_t offset = 0;

    while (offset < size) {
        // 阶段1：等待 FIFO 有空间（对齐内核 FIFO 的阻塞写语义）
        {
            std::unique_lock lock(pipe_mutex);
            if (disconnected) {
                return total_written;
            }
            // 懒创建该端点的 FIFO（容量取当前配置）
            auto &fifo = in_fifos.try_emplace(xfer.ep, fifo_capacity).first->second;

            if (fifo.available() == 0) {
                // 先尝试应答挂起的 IN 请求腾出空间
                {
                    std::lock_guard queue_lock(requests_mutex);
                    try_send_pending_locked();
                }
                if (disconnected) {
                    return total_written;
                }
                // 仍满则等待：宿主 IN 请求取走数据后由 send_from_fifo_locked 唤醒
                while (fifo.available() == 0 && !disconnected) {
                    if (timeout_ms == 0) {
                        write_cv.wait(lock);
                    }
                    else {
                        if (write_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms)) ==
                            std::cv_status::timeout) {
                            // 超时时刻空间可能恰好腾出：跳出循环走统一判断，
                            // 能写则继续写，避免少写
                            break;
                        }
                    }
                }
                if (disconnected) {
                    return total_written;
                }
                if (fifo.available() == 0) {
                    return total_written; // 超时且仍未腾出空间
                }
            }

            std::size_t written = fifo.write(data + offset, size - offset);
            total_written += written;
            offset += written;
        }

        // 阶段2：FIFO 有数据了，尝试应答挂起的 IN 请求
        {
            std::lock(pipe_mutex, requests_mutex);
            std::lock_guard lock1(pipe_mutex, std::adopt_lock);
            std::lock_guard lock2(requests_mutex, std::adopt_lock);
            try_send_pending_locked();
        }
    }
    return total_written;
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

void PipeDeviceHandler::on_new_connection(Session &current_session, error_code &ec) {
    // 父类注册 session 并遍历接口建连
    VirtualDeviceHandler::on_new_connection(current_session, ec);
    if (ec) {
        return;
    }
    // 新会话从干净状态开始：FIFO/队列由各端点按需重建
    std::lock_guard lock(pipe_mutex);
    disconnected = false;
    out_queue.clear();
    in_fifos.clear();
}

void PipeDeviceHandler::on_disconnection(error_code &ec) {
    {
        std::lock_guard lock(pipe_mutex);
        disconnected = true;
        out_queue.clear();
        in_fifos.clear();
    }
    {
        std::lock_guard lock(requests_mutex);
        // TransferHandle 析构时会自动释放
        endpoint_requests.clear();
    }
    // 唤醒阻塞的 read/write，让它们以"断连"返回
    read_cv.notify_all();
    write_cv.notify_all();
    // 父类遍历接口 on_disconnection 并清除 session 指针
    VirtualDeviceHandler::on_disconnection(ec);
}

void PipeDeviceHandler::on_pipe_in_request(std::uint8_t ep_addr, std::uint32_t seqnum, std::uint32_t length,
                                           TransferHandle transfer) {
    std::lock(pipe_mutex, requests_mutex);
    std::lock_guard lock1(pipe_mutex, std::adopt_lock);
    std::lock_guard lock2(requests_mutex, std::adopt_lock);
    // 请求先入队再尝试匹配 FIFO 数据：无数据时请求留在队列等待 write
    endpoint_requests.enqueue(ep_addr, {seqnum, length, std::move(transfer)});
    try_send_pending_locked();
}

void PipeDeviceHandler::on_pipe_out_transfer(std::uint8_t ep_addr, std::uint32_t seqnum,
                                             std::uint32_t received_size, data_type &&data) {
    {
        std::lock_guard lock(pipe_mutex);
        out_queue.emplace_back(PipeXfer{.ep = ep_addr, .data = std::move(data)});
    }
    read_cv.notify_one();
    // 数据已取出，transfer 由调用方析构释放，此处只回 OK
    session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
}

void PipeDeviceHandler::on_pipe_control_request(const SetupPacket &setup, std::uint32_t seqnum,
                                                std::uint32_t transfer_buffer_length, TransferHandle transfer) {
    if (setup.is_out()) {
        // OUT：数据已随控制传输到达，整体交给用户
        auto *trx = GenericTransfer::from_handle(transfer.get());
        data_type data(trx->data.begin(), trx->data.begin() + transfer_buffer_length);
        {
            std::lock_guard lock(pipe_mutex);
            out_queue.emplace_back(PipeXfer{.ep = 0, .setup_req = setup, .data = std::move(data)});
        }
        read_cv.notify_one();
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, transfer_buffer_length));
    }
    else {
        // IN：请求挂起，用户 read() 拿到 setup_req 后 write({ep=0, data}) 应答
        // （对齐 VirtualUSBDevice：ep0 IN 请求挂起，write(0, ...) 匹配发送）
        {
            std::lock_guard lock(requests_mutex);
            endpoint_requests.enqueue(0, {seqnum, transfer_buffer_length, std::move(transfer)});
        }
        {
            std::lock_guard lock(pipe_mutex);
            out_queue.emplace_back(PipeXfer{.ep = 0, .setup_req = setup});
        }
        read_cv.notify_one();
    }
}

void PipeDeviceHandler::on_pipe_unlink(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    std::lock_guard lock(requests_mutex);
    bool cancelled = endpoint_requests.cancel_by_seqnum(unlink_seqnum);
    // 从队列中真的取消了待处理 URB → 回 -ECONNRESET（URB 被取消，且不再发
    // RET_SUBMIT，请求已从队列移除）；找不到（URB 已完成/不存在）→ 回 0。
    // 与内核 stub_tx.c 及本项目 HID/CdcAcm 的 unlink 范本一致
    session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

void PipeDeviceHandler::try_send_pending_locked() {
    // 调用者必须已持有 pipe_mutex 和 requests_mutex
    // 按端点匹配：只服务 FIFO 有数据的端点，避免 dequeue_any 取到空 FIFO 端点的
    // 请求放回队尾导致乱序（多端点场景下同端点请求必须保持 FIFO 顺序）
    for (auto &[ep_addr, fifo]: in_fifos) {
        while (!fifo.empty()) {
            auto req_opt = endpoint_requests.dequeue(ep_addr);
            if (!req_opt.has_value()) {
                break;
            }
            auto &req = req_opt.value();
            send_from_fifo_locked(fifo, req.seqnum, req.length, std::move(req.transfer));
        }
    }
}

void PipeDeviceHandler::send_from_fifo_locked(RingBuffer &fifo, std::uint32_t seqnum, std::uint32_t max_length,
                                              TransferHandle transfer) {
    // 调用者必须已持有 pipe_mutex 和 requests_mutex，且 fifo 非空
    std::size_t send_len = std::min(fifo.size(), static_cast<std::size_t>(max_length));
    auto *trx = GenericTransfer::from_handle(transfer.get());
    trx->data.resize(send_len);
    fifo.read(trx->data.data(), send_len);
    trx->actual_length = send_len;
    // FIFO 腾出空间，唤醒阻塞等待的 write
    write_cv.notify_one();
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
            seqnum, static_cast<std::uint32_t>(send_len), std::move(transfer)));
}
