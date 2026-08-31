#include "usbipdcpp/virtual_device/EcmVirtualInterfaceHandler.h"

#include <cstdio>
#include <string>

#include "usbipdcpp/DeviceHandler/DeviceHandler.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/virtual_device/EcmConstants.h"

namespace usbipdcpp {

// ==================== EcmCommunicationInterfaceHandler ====================

EcmCommunicationInterfaceHandler::EcmCommunicationInterfaceHandler(UsbInterface &handle_interface,
                                                                   StringPool &string_pool,
                                                                   std::array<std::uint8_t, 6> mac_address) :
    VirtualInterfaceHandler(handle_interface, string_pool), interface_number_(handle_interface.interface_number) {
    // MAC 地址转 12 个 hex 字符存字符串描述符（cdc_ether 主机驱动按此格式解析，
    // 要求 Unicode 字符为大写 0-9A-F，长度恰好 12）
    char hex[13];
    std::snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X", mac_address[0], mac_address[1], mac_address[2],
                  mac_address[3], mac_address[4], mac_address[5]);
    mac_string_index_ = string_pool.new_string(std::wstring(hex, hex + 12));
    // 通知通道待发缓存上限 30 条：网络状态通知低频（连接/速率变化才发一条），
    // 正常使用缓存几乎不会超过个位数；30 条是防主机长期不读时内存堆积的
    // 兜底，同时保留足够余量不丢低频通知（超限丢最旧，对齐 CdcAcm 通知通道）
    notification_channel.set_max_pending(30);
}

UsbInterface EcmCommunicationInterfaceHandler::make_interface(std::uint8_t interrupt_in_ep) {
    UsbInterface i{
            .interface_class = static_cast<std::uint8_t>(ClassCode::CDC),
            .interface_subclass = 0x06, // Ethernet Networking Control Model
            .interface_protocol = 0x00,
            .endpoints = {{UsbEndpoint{.address = interrupt_in_ep,
                                       .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
                                       .max_packet_size = 16, // ECM_STATUS_BYTECOUNT：容纳头+SPEED_CHANGE 数据
                                       .interval = 32}}}, // 对齐内核 ECM_STATUS_INTERVAL_MS=32ms（Full speed）
    };
    return i;
}

void EcmCommunicationInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    std::uint32_t status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);

    if (type == RequestType::Class) {
        auto request = static_cast<EcmRequest>(setup_packet.request);

        if (!setup_packet.is_out()) {
            // IN 请求：统计/电源过滤均在描述符里声明了不支持，对齐内核
            // f_ecm.c 的 default（STALL）一律回 EPIPE
            SPDLOG_WARN("Unsupported ECM IN request 0x{:x}", setup_packet.request);
            status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        }
        else {
            // OUT 请求
            switch (request) {
                case EcmRequest::SetEthernetPacketFilter: {
                    // ECM120 §6.2.4：无数据，wValue=过滤位图，wIndex=接口号。
                    // 对齐内核 ecm_setup：wLength != 0 视为非法（STALL）
                    if (setup_packet.length != 0) {
                        SPDLOG_WARN("SET_ETHERNET_PACKET_FILTER with data, wLength={}", setup_packet.length);
                        status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                        break;
                    }
                    packet_filter_ = setup_packet.value;
                    on_set_packet_filter(packet_filter_);
                    SPDLOG_DEBUG("SET_ETHERNET_PACKET_FILTER: 0x{:04x}", packet_filter_);
                    break;
                }
                default: {
                    // 多播过滤/电源模式过滤：描述符声明了不支持，对齐内核 STALL
                    SPDLOG_WARN("Unsupported ECM OUT request 0x{:x}", setup_packet.request);
                    status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                }
            }
        }
        // transfer 析构时自动释放
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                seqnum, status, 0));
    }
    else {
        // 非类请求（标准请求已被基类分发，这里兜底）
        SPDLOG_WARN("Unhandled request type 0x{:x} in ECM communication interface", setup_packet.calc_request_type());
        // transfer 析构时自动释放
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void EcmCommunicationInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                 std::uint32_t transfer_flags,
                                                                 std::uint32_t transfer_buffer_length,
                                                                 TransferHandle transfer, std::error_code &ec) {
    if (ep.is_in()) {
        // 通道内部处理：缓冲有通知立即应答，否则挂起请求等待
        // send_network_connection / send_speed_change 推入
        notification_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // 中断 OUT：ECM 不使用（对齐 CdcAcm 的处理）
        // transfer 析构时自动释放
        SPDLOG_WARN("ECM communication interface received unexpected interrupt OUT");
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

data_type EcmCommunicationInterfaceHandler::get_class_specific_descriptor() {
    // Header + Union + Ethernet Networking（对齐内核 f_ecm.c 的 ecm_*_descriptor）
    data_type descriptor;

    CdcHeaderFunctionalDesc{0x05, CS_INTERFACE, 0x00, 0x0110 /* bcdCDC: 1.10 */}.append_to(descriptor);

    CdcUnionFunctionalDesc{0x05, CS_INTERFACE, 0x06,
                           0x00, // bMasterInterface: Interface 0
                           0x01} // bSlaveInterface0: Interface 1
            .append_to(descriptor);

    CdcEthernetFunctionalDesc{0x0D, CS_INTERFACE, 0x0F /* EthernetNetworking */,
                              mac_string_index_,
                              0, // bmEthernetStatistics: 不收集统计
                              1514, // wMaxSegmentSize: 对齐内核 ETH_FRAME_LEN
                              0, // wNumberMCFilters
                              0} // bNumberPowerFilters
            .append_to(descriptor);

    return descriptor;
}

void EcmCommunicationInterfaceHandler::on_set_packet_filter(std::uint16_t filter) {
    // 默认空实现，子类可重写
}

void EcmCommunicationInterfaceHandler::send_network_connection(bool up) {
    // 状态放 wValue（0/1）、无数据字节，对齐内核 ecm_do_notify
    notification_channel.push(make_cdc_notification(EcmNotification::NetworkConnection, up ? 1 : 0,
                                                    interface_number_, nullptr, 0));
}

void EcmCommunicationInterfaceHandler::send_speed_change(std::uint32_t up_speed, std::uint32_t down_speed) {
    // 数据 8 字节 = up/down 各 4 字节小端，对齐内核 ecm_do_notify
    std::uint8_t speed_data[8];
    speed_data[0] = static_cast<std::uint8_t>(up_speed & 0xFF);
    speed_data[1] = static_cast<std::uint8_t>((up_speed >> 8) & 0xFF);
    speed_data[2] = static_cast<std::uint8_t>((up_speed >> 16) & 0xFF);
    speed_data[3] = static_cast<std::uint8_t>((up_speed >> 24) & 0xFF);
    speed_data[4] = static_cast<std::uint8_t>(down_speed & 0xFF);
    speed_data[5] = static_cast<std::uint8_t>((down_speed >> 8) & 0xFF);
    speed_data[6] = static_cast<std::uint8_t>((down_speed >> 16) & 0xFF);
    speed_data[7] = static_cast<std::uint8_t>((down_speed >> 24) & 0xFF);
    notification_channel.push(make_cdc_notification(EcmNotification::ConnectionSpeedChange, 0, interface_number_,
                                                    speed_data, sizeof(speed_data)));
}

void EcmCommunicationInterfaceHandler::on_new_connection(Session &current_session, std::error_code &ec) {
    // 父类先设 session 指针（通道应答请求要用），再绑定通道并重置断连状态
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    notification_channel.on_new_connection(&current_session);
}

void EcmCommunicationInterfaceHandler::on_disconnection(std::error_code &ec) {
    // 先清通道（缓冲 + 挂起请求，TransferHandle 析构自动释放），再调父类清 session
    notification_channel.on_disconnection();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void EcmCommunicationInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    bool cancelled = notification_channel.cancel_pending(unlink_seqnum);
    // 从队列中真的取消了待处理 URB → 回 -ECONNRESET（URB 被取消，且不再发
    // RET_SUBMIT，请求已从队列移除）；找不到（URB 已完成/不存在）→ 回 0。
    // 与内核 stub_tx.c 及本项目 CdcAcm 的 unlink 范本一致
    session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

// ==================== EcmDataInterfaceHandler ====================

EcmDataInterfaceHandler::EcmDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                                 NetworkBackend *backend) :
    VirtualInterfaceHandler(handle_interface, string_pool), backend_(backend) {
    if (backend_) {
        // 注入发帧回调：后端产生帧时经通道推给主机（通道线程安全，任意线程可调）
        backend_->set_send_to_host([this](const std::uint8_t *data, std::size_t size) {
            in_channel.push(data_type(data, data + size));
        });
    }
}

UsbInterface EcmDataInterfaceHandler::make_interface(std::uint8_t in_ep, std::uint8_t out_ep) {
    // alt0 无端点 / alt1 两个 bulk：endpoints 外层下标即 alternate setting 号
    UsbInterface i{
            .interface_class = static_cast<std::uint8_t>(ClassCode::CDCData),
            .interface_subclass = 0x00,
            .interface_protocol = 0x00,
            .endpoints = {{},
                          {UsbEndpoint{.address = in_ep,
                                       .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                       .max_packet_size = 64,
                                       .interval = 0},
                           UsbEndpoint{.address = out_ep,
                                       .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                       .max_packet_size = 64,
                                       .interval = 0}}},
    };
    return i;
}

void EcmDataInterfaceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                   std::uint32_t transfer_flags,
                                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                   std::error_code &ec) {
    if (ep.is_in()) {
        // Bulk IN：主机请求帧。通道内部处理：缓冲有帧立即应答（一条消息 = 一帧，
        // 帧比请求短发短包，主机 usbnet 以短包/整包边界定帧），否则挂起等待
        SPDLOG_TRACE("[ECM] IN req seq={} len={} buf={}", seqnum, transfer_buffer_length, in_channel.size());
        in_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // Bulk OUT：主机发来一帧。有 backend 直接交给网络侧消费并立即应答；
        // 否则先给子类当场消费机会（true=已处理），都不行挂起入通道（NAK
        // 背压），子类之后用 take_frame() 取出数据并应答
        auto *trx = GenericTransfer::from_handle(transfer.get());
        auto received_size = static_cast<std::uint32_t>(trx->data.size());
        SPDLOG_TRACE("[ECM] OUT frame size={}", received_size);
        if (backend_) {
            backend_->send_frame(trx->data.data(), trx->data.size());
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
        }
        else if (on_frame_received(std::move(trx->data))) {
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
        }
        else {
            out_channel.on_out_request(ep.address, seqnum, std::move(transfer));
        }
    }
}

void EcmDataInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // 数据接口没有类特定控制请求
    SPDLOG_WARN("ECM data interface received unexpected control request");
    // transfer 析构时自动释放
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

data_type EcmDataInterfaceHandler::get_class_specific_descriptor() {
    // 数据接口没有类特定描述符
    return {};
}

bool EcmDataInterfaceHandler::on_frame_received(data_type &&frame) {
    // 默认行为：总是消费并立即应答（未 override 的子类保持"总是接收"语义）
    return true;
}

std::size_t EcmDataInterfaceHandler::send_frame(const std::uint8_t *data, std::size_t size) {
    // 非阻塞推送（断连时通道内丢弃返回 0），内部会应答挂起的 IN 请求
    if (data == nullptr || size == 0) {
        return 0;
    }
    SPDLOG_TRACE("[ECM] push frame size={} buf_before={}", size, in_channel.size());
    in_channel.push(data_type(data, data + size));
    SPDLOG_TRACE("[ECM] pushed, buf_after={}", in_channel.size());
    return size;
}

std::size_t EcmDataInterfaceHandler::send_frame(const data_type &frame) {
    return send_frame(frame.data(), frame.size());
}

std::optional<OutEndpointChannel::Pending> EcmDataInterfaceHandler::take_frame(std::uint32_t timeout_ms) {
    return out_channel.take(timeout_ms);
}

std::optional<OutEndpointChannel::Pending> EcmDataInterfaceHandler::try_take_frame() {
    return out_channel.try_take();
}

void EcmDataInterfaceHandler::set_tx_max_pending(std::size_t max_pending) {
    // 通道内部锁保护缓冲上限
    in_channel.set_max_pending(max_pending);
}

void EcmDataInterfaceHandler::on_new_connection(Session &current_session, std::error_code &ec) {
    // 父类先设 session 指针（通道应答请求要用），再绑定通道并重置断连状态
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    in_channel.on_new_connection(&current_session);
    out_channel.on_new_connection(&current_session);
}

void EcmDataInterfaceHandler::on_disconnection(std::error_code &ec) {
    // 先清通道（缓冲 + 挂起请求，TransferHandle 析构时自动释放；唤醒阻塞
    // 的取者让它们按断连返回），后端清缓冲防旧数据残留，再调父类清 session
    out_channel.on_disconnection();
    in_channel.on_disconnection();
    if (backend_) {
        backend_->reset();
    }
    VirtualInterfaceHandler::on_disconnection(ec);
}

void EcmDataInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    // 挂起请求分散在 IN/OUT 两通道里，逐个尝试取消
    bool cancelled = in_channel.cancel_pending(unlink_seqnum);
    if (!cancelled) {
        cancelled = out_channel.cancel_pending(unlink_seqnum);
    }
    // 从队列中真的取消了待处理 URB → 回 -ECONNRESET（URB 被取消，且不再发
    // RET_SUBMIT，请求已从队列移除）；找不到（URB 已完成/不存在）→ 回 0。
    // 与内核 stub_tx.c 及本项目 CdcAcm 的 unlink 范本一致
    session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}
} // namespace usbipdcpp
