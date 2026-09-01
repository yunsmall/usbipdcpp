#include "usbipdcpp/virtual_device/RndisVirtualInterfaceHandler.h"

#include <cstring>

#include "usbipdcpp/DeviceHandler/DeviceHandler.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/virtual_device/CdcAcmConstants.h"
#include "usbipdcpp/virtual_device/RndisConstants.h"

namespace usbipdcpp {

namespace {
// 把字节流偏移处的内容 memcpy 进 packed 结构体：入站 RNDIS 消息是紧贴缓冲的
// 字节序列，直接 reinterpret_cast 访问 packed 成员会触发 UBSan 的
// misaligned 报告，memcpy 到对齐的本地对象再字段访问是安全做法
template <typename T>
bool copy_msg(const data_type &data, std::size_t offset, T &out) {
    if (offset + sizeof(T) > data.size()) {
        return false;
    }
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

// u32 值序列化成 4 字节小端（OID 查询返回数据用）
data_type le32_bytes(std::uint32_t v) {
    data_type d;
    vector_append_to_le(d, v);
    return d;
}

// 组装 INDICATE 状态消息（MEDIA_CONNECT / MEDIA_DISCONNECT，20 字节）
data_type make_indicate(RndisStatus status) {
    data_type resp;
    RndisIndicateStatusMsg{rndis_msg(RndisMessageType::Indicate), 20, rndis_status(status), 0, 0}.append_to(resp);
    return resp;
}

// 包 RNDIS_MSG_PACKET 头（44B）：MessageType=1、MessageLength=44+size、
// DataOffset=36（数据从字节 44 起）、DataLength=size、其余 0。
// 总长是 maxpacket(64) 倍数时补 1 字节防 ZLP（对齐内核 u_ether.c：RNDIS
// 不允许 ZLP；MessageLength 不含填充，主机按消息长度拆包后尾字节被 trim）
data_type wrap_rndis_packet(const std::uint8_t *data, std::size_t size) {
    data_type wrapped;
    wrapped.reserve(44 + size + 1);
    RndisPacketHeader{1, static_cast<std::uint32_t>(44 + size), 36, static_cast<std::uint32_t>(size),
                      0, 0, 0, 0, 0, 0, 0}
            .append_to(wrapped);
    wrapped.insert(wrapped.end(), data, data + size);
    if ((44 + size) % 64 == 0) {
        wrapped.push_back(0);
    }
    return wrapped;
}

// 解析主机发来的 RNDIS_MSG_PACKET 消息流，剥头取出以太网帧列表。
// 一次 bulk OUT 传输可能聚合多条消息（后一条紧跟前一条的 message_length
// 之后），每条消息可能有填充、OOB 字段非 0——按消息长度定位边界、
// 按 DataOffset/DataLength 取帧，OOB 直接忽略（对齐内核 rndis_rm_hdr
// 与 rndis_host.c 的 rndis_rx_fixup）。帧长 < 14 字节（以太网头）丢弃
std::vector<data_type> unwrap_rndis_packets(const data_type &data) {
    std::vector<data_type> frames;
    std::size_t pos = 0;
    while (pos + sizeof(RndisMessageHeader) <= data.size()) {
        RndisMessageHeader hdr{};
        std::memcpy(&hdr, data.data() + pos, sizeof(hdr));
        // 主机方向包头 24 字节、设备方向 44 字节，消息长度下限不假设头长
        //（用 DataOffset/DataLength 边界检查兜底）
        if (hdr.message_length < sizeof(RndisMessageHeader) || pos + hdr.message_length > data.size()) {
            break; // 消息截断/长度非法：本条及之后丢弃
        }
        if (hdr.message_type != rndis_msg(RndisMessageType::Packet)) {
            break; // 数据面出现非包消息：异常主机，丢弃
        }
        RndisPacketHeader ph{};
        std::memcpy(&ph, data.data() + pos, sizeof(ph));
        std::size_t frame_off = pos + 8 + ph.data_offset; // 数据起始 = 消息第 8 字节 + offset
        std::size_t frame_len = ph.data_length;
        if (frame_off + frame_len > data.size()) {
            break;
        }
        if (frame_len >= 14) { // 以太网头最小长度（对齐内核 u_ether 的 ETH_HLEN 校验）
            frames.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(frame_off),
                                data.begin() + static_cast<std::ptrdiff_t>(frame_off + frame_len));
        }
        pos += hdr.message_length;
    }
    return frames;
}
} // namespace

// ==================== RndisCommunicationInterfaceHandler ====================

RndisCommunicationInterfaceHandler::RndisCommunicationInterfaceHandler(
        UsbInterface &handle_interface, StringPool &string_pool, std::array<std::uint8_t, 6> mac_address,
        UsbSpeed speed) :
    VirtualInterfaceHandler(handle_interface, string_pool),
    mac_address_(mac_address), interface_number_(handle_interface.interface_number) {
    // LINK_SPEED 上报（单位 100bps，对齐内核 f_rndis.c 的 bitrate()/100）
    switch (speed) {
        case UsbSpeed::Full:
            link_speed_100bps_ = 97'280; // 19×64×1×1000×8 bps
            break;
        case UsbSpeed::High:
            link_speed_100bps_ = 4'259'840; // 13×512×8×1000×8 bps
            break;
        case UsbSpeed::Super:
            link_speed_100bps_ = 37'500'000; // 3.75 Gbps
            break;
        case UsbSpeed::SuperPlus:
            link_speed_100bps_ = 42'500'000; // 4.25 Gbps
            break;
        default:
            link_speed_100bps_ = 0; // Low/Wireless/Unknown：不上报速度
            break;
    }
    // 通知通道待发缓存上限 30 条：RESPONSE_AVAILABLE 通知只在响应入队时发
    // 一条（握手期间个位数），30 条是防主机长期不读时内存堆积的兜底
    notification_channel.set_max_pending_messages(30);
}

UsbInterface RndisCommunicationInterfaceHandler::make_interface(std::uint8_t interrupt_in_ep) {
    // 对齐内核 f_rndis.c 的控制接口：COMM 类 + ACM 子类 + Vendor 协议。
    // 不用微软 RNDIS 专有描述符——Windows 与 Linux rndis_host 都按
    // 02/02/FF 匹配（rndis_host.c 的 id_table）
    UsbInterface i{
            .interface_class = static_cast<std::uint8_t>(ClassCode::CDC),
            .interface_subclass = 0x02, // ACM（RNDIS 借 ACM 外壳匹配）
            .interface_protocol = 0xFF, // Vendor
            .endpoints = {{UsbEndpoint{.address = interrupt_in_ep,
                                       .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
                                       .max_packet_size = 8, // 状态端点最小 8 字节（cdc_ether 绑定硬性要求）
                                       .interval = 32}}}, // 对齐内核 fs_control_intf_ep（Full speed）
    };
    // IAD：RNDIS 是双接口功能（通信+数据），Windows 靠它把设备识别为复合
    // 设备并做 MS OS 描述符探测（对齐内核 f_rndis.c 的 rndis_iad_descriptor：
    // bFunctionClass=CDC、bFunctionSubClass=ETHERNET(0x06)、bInterfaceCount=2）。
    // bFirstInterface 由配置描述符生成时按所属接口的 interface_number 回填
    i.interface_association_descriptor = IadDesc::make(
            2, static_cast<std::uint8_t>(ClassCode::CDC), 0x06 /* USB_CDC_SUBCLASS_ETHERNET */);
    return i;
}

void RndisCommunicationInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    if (type != RequestType::Class) {
        // 非类请求（标准请求已被基类分发，这里兜底）
        SPDLOG_WARN("RNDIS 通信接口收到非类控制请求 0x{:x}", setup_packet.calc_request_type());
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto request = static_cast<CdcAcmRequest>(setup_packet.request);
    if (request == CdcAcmRequest::SendEncapsulatedCommand && setup_packet.is_out()) {
        // 主机送入一条 RNDIS 命令（数据阶段 = 整条消息），解析并排队响应；
        // 响应由主机稍后 GET_ENCAPSULATED_RESPONSE 取走（对齐内核 f_rndis.c
        // 的 rndis_setup：命令经 ep0 OUT 完整接收后在 completion 里解析）
        auto *trx = GenericTransfer::from_handle(transfer.get());
        process_rndis_message(trx->data);
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), 0));
    }
    else if (request == CdcAcmRequest::GetEncapsulatedResponse && !setup_packet.is_out()) {
        // 主机取走一条排队响应。队列空回 EPIPE（对齐内核 rndis_setup 的
        // STALL——主机 rndis_command 会以 40ms 间隔轮询重试）
        auto *trx = GenericTransfer::from_handle(transfer.get());
        if (response_queue_.empty()) {
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        }
        else {
            trx->data = std::move(response_queue_.front());
            response_queue_.pop_front();
            // 响应超过主机缓冲按缓冲截断（主机 wLength 一般 ≥ 1558，正常不会发生）
            if (trx->data.size() > transfer_buffer_length) {
                trx->data.resize(transfer_buffer_length);
            }
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                    seqnum, static_cast<std::uint32_t>(trx->data.size()), std::move(transfer)));
        }
    }
    else {
        SPDLOG_WARN("RNDIS 通信接口收到意外控制请求 0x{:x} dir={}", setup_packet.request,
                    setup_packet.is_out() ? "OUT" : "IN");
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void RndisCommunicationInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                   std::uint32_t transfer_flags,
                                                                   std::uint32_t transfer_buffer_length,
                                                                   TransferHandle transfer, std::error_code &ec) {
    if (ep.is_in()) {
        // 通道内部处理：缓冲有通知立即应答，否则挂起请求等待响应入队时推入
        notification_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // 中断 OUT：RNDIS 不使用（对齐 ECM 的处理）
        SPDLOG_WARN("RNDIS 通信接口收到意外中断 OUT");
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

data_type RndisCommunicationInterfaceHandler::get_class_specific_descriptor() {
    // Header + CallManagement + ACM + Union（对齐内核 f_rndis.c 的
    // rndis_control_intf 描述符组合，CDC 通用结构体复用 CdcAcmConstants）
    data_type descriptor;

    CdcHeaderFunctionalDesc{0x05, CS_INTERFACE, 0x00, 0x0110 /* bcdCDC: 1.10 */}.append_to(descriptor);

    CdcCallManagementDesc{0x05, CS_INTERFACE, 0x01,
                          0x00, // bmCapabilities: 数据接口不处理呼叫管理
                          0x01} // bDataInterface: 数据接口总是紧邻的下一个接口号
            .append_to(descriptor);

    CdcAcmFunctionalDesc{0x04, CS_INTERFACE, 0x02,
                         0x00} // bmCapabilities 必须为 0：cdc_ether 主机驱动
            // 检查非 0 判定"不是真 RNDIS"拒绝绑定（cdc_ether.c:225-235）
            .append_to(descriptor);

    CdcUnionFunctionalDesc{0x05, CS_INTERFACE, 0x06,
                           0x00, // bMasterInterface: Interface 0（控制接口）
                           0x01} // bSlaveInterface0: Interface 1（数据接口）
            .append_to(descriptor);

    return descriptor;
}

void RndisCommunicationInterfaceHandler::process_rndis_message(const data_type &msg) {
    RndisMessageHeader hdr{};
    if (!copy_msg(msg, 0, hdr)) {
        SPDLOG_WARN("RNDIS 消息过短: {} 字节", msg.size());
        return;
    }
    if (hdr.message_length < sizeof(RndisMessageHeader) || hdr.message_length > msg.size()) {
        SPDLOG_WARN("RNDIS 消息长度非法: 声明 {} 实收 {}", hdr.message_length, msg.size());
        return;
    }

    switch (static_cast<RndisMessageType>(hdr.message_type)) {
        case RndisMessageType::Init: {
            RndisInitMsg init{};
            if (!copy_msg(msg, 0, init)) {
                return;
            }
            state_ = RndisState::Initialized;
            data_type resp;
            RndisInitCmplt{rndis_msg(RndisMessageType::InitComplete), 52, init.request_id,
                           rndis_status(RndisStatus::Success), RNDIS_MAJOR_VERSION, RNDIS_MINOR_VERSION,
                           RNDIS_DF_CONNECTIONLESS, RNDIS_MEDIUM_802_3, 1 /* MaxPacketsPerTransfer */,
                           RNDIS_MAX_TRANSFER_SIZE, 0 /* PacketAlignmentFactor */, 0 /* AFListOffset */,
                           0 /* AFListSize */}
                    .append_to(resp);
            enqueue_response(std::move(resp));
            // 链路 up 通知：对齐内核 rndis_open 的 rndis_signal_connect。
            // 虚拟设备初始化完成即链路可用（Windows 依赖此通知确认连接）
            enqueue_response(make_indicate(RndisStatus::MediaConnect));
            break;
        }
        case RndisMessageType::Halt:
            // HALT 不响应（对齐 rndis.c：主机解绑/断开时的告别消息，状态复位）
            reset_device_state();
            break;
        case RndisMessageType::Query: {
            RndisQueryMsg query{};
            if (!copy_msg(msg, 0, query)) {
                return;
            }
            data_type payload = query_oid(query.oid);
            // 未知/不支持 OID 回 NOT_SUPPORTED 的 QUERY_C（不能 stall 控制请求）
            auto status = payload.empty() ? RndisStatus::NotSupported : RndisStatus::Success;
            data_type resp;
            RndisQueryCmplt{rndis_msg(RndisMessageType::QueryComplete),
                            static_cast<std::uint32_t>(24 + payload.size()), query.request_id,
                            rndis_status(status), static_cast<std::uint32_t>(payload.size()),
                            16 /* InformationBufferOffset: 数据紧跟 24 字节头 */}
                    .append_to(resp);
            resp.insert(resp.end(), payload.begin(), payload.end());
            enqueue_response(std::move(resp));
            break;
        }
        case RndisMessageType::Set: {
            RndisSetMsg set{};
            if (!copy_msg(msg, 0, set)) {
                return;
            }
            // 输入缓冲起点 = 消息第 8 字节 + offset（对齐内核 rndis.c 偏移约定）
            std::size_t buf_off = 8 + set.information_buffer_offset;
            RndisStatus status = RndisStatus::NotSupported;
            switch (set.oid) {
                case OID_GEN_CURRENT_PACKET_FILTER: {
                    if (buf_off + 4 > msg.size()) {
                        status = RndisStatus::InvalidData;
                        break;
                    }
                    std::uint32_t filter_le{};
                    std::memcpy(&filter_le, msg.data() + buf_off, 4);
                    packet_filter_ = static_cast<std::uint16_t>(filter_le); // 低 16 位为过滤位图
                    // 对齐 rndis.c：filter≠0 → DATA_INITIALIZED（carrier on 开始
                    // 收发），filter==0 → 回 INITIALIZED（carrier off）
                    state_ = packet_filter_ != 0 ? RndisState::DataInitialized : RndisState::Initialized;
                    SPDLOG_DEBUG("RNDIS SET_CURRENT_PACKET_FILTER: 0x{:04x}", packet_filter_);
                    status = RndisStatus::Success;
                    break;
                }
                case OID_802_3_MULTICAST_LIST:
                    // 直接接受（忽略内容，对齐内核 gen_ndis_set_resp 的默认接受）
                    SPDLOG_DEBUG("RNDIS SET_802_3_MULTICAST_LIST 接受");
                    status = RndisStatus::Success;
                    break;
                default:
                    // 其余 OID 不支持（对齐内核 SET 未知 OID 回 NOT_SUPPORTED）
                    SPDLOG_WARN("RNDIS 不支持 SET OID 0x{:08X}", set.oid);
                    break;
            }
            data_type resp;
            RndisSetCmplt{rndis_msg(RndisMessageType::SetComplete), 16, set.request_id, rndis_status(status)}
                    .append_to(resp);
            enqueue_response(std::move(resp));
            break;
        }
        case RndisMessageType::Reset:
            // 对齐 rndis.c：清空未取走的响应队列，回 RESET_C（AddressingReset=1）
            response_queue_.clear();
            {
                data_type resp;
                RndisResetCmplt{rndis_msg(RndisMessageType::ResetComplete), 16, rndis_status(RndisStatus::Success),
                                1 /* AddressingReset */}
                        .append_to(resp);
                enqueue_response(std::move(resp));
            }
            break;
        case RndisMessageType::Keepalive: {
            // KEEPALIVE 只有 12 字节头，request_id 在头里（对齐 rndis.c：
            // 主机（Windows）每 5 秒发一次，回 KEEPALIVE_C SUCCESS）
            RndisMessageHeader ka{};
            if (!copy_msg(msg, 0, ka)) {
                return;
            }
            data_type resp;
            RndisKeepaliveCmplt{rndis_msg(RndisMessageType::KeepaliveComplete), 16, ka.request_id,
                                rndis_status(RndisStatus::Success)}
                    .append_to(resp);
            enqueue_response(std::move(resp));
            break;
        }
        default:
            // 未知消息类型：丢弃不响应（对齐 rndis.c 默认分支）
            SPDLOG_WARN("未知 RNDIS 消息类型 0x{:08X}", hdr.message_type);
            break;
    }
}

data_type RndisCommunicationInterfaceHandler::query_oid(std::uint32_t oid) {
    // OID 表对齐内核 rndis.c 的 oid_supported_list 与 rndis_query_response；
    // 返回空表示不支持（组装 NOT_SUPPORTED 的 QUERY_C）
    switch (oid) {
        case OID_GEN_SUPPORTED_LIST: {
            // 支持列表 = 除统计 OID（XMIT_OK/RCV_OK/…）外的全部（对齐内核）
            const std::uint32_t list[] = {
                    OID_GEN_SUPPORTED_LIST,       OID_GEN_HARDWARE_STATUS,  OID_GEN_MEDIA_SUPPORTED,
                    OID_GEN_MEDIA_IN_USE,         OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_LINK_SPEED,
                    OID_GEN_TRANSMIT_BLOCK_SIZE,  OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID,
                    OID_GEN_VENDOR_DESCRIPTION,   OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_CURRENT_PACKET_FILTER,
                    OID_GEN_MAXIMUM_TOTAL_SIZE,   OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAC_OPTIONS,
                    OID_GEN_PHYSICAL_MEDIUM,      OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS,
                    OID_802_3_MULTICAST_LIST,     OID_802_3_MAXIMUM_LIST_SIZE, OID_802_3_MAC_OPTIONS,
            };
            data_type buf;
            for (auto supported : list) {
                vector_append_to_le(buf, supported); // 每条 4 字节
            }
            return buf;
        }
        case OID_GEN_HARDWARE_STATUS:
            return le32_bytes(0); // 硬件就绪
        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            return le32_bytes(RNDIS_MEDIUM_802_3);
        case OID_GEN_MAXIMUM_FRAME_SIZE:
            return le32_bytes(1500); // netdev mtu（对齐内核）
        case OID_GEN_LINK_SPEED:
            return le32_bytes(link_speed_100bps_);
        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            return le32_bytes(1500);
        case OID_GEN_VENDOR_ID:
            return le32_bytes(0); // 厂商 OUI 低 24 位（示例未配置）
        case OID_GEN_VENDOR_DESCRIPTION:
            return le32_bytes(0); // 未配置厂商描述：对齐内核返回空描述
        case OID_GEN_VENDOR_DRIVER_VERSION:
            return le32_bytes(1);
        case OID_GEN_CURRENT_PACKET_FILTER:
            return le32_bytes(packet_filter_);
        case OID_GEN_MAXIMUM_TOTAL_SIZE:
            return le32_bytes(RNDIS_MAX_TOTAL_SIZE);
        case OID_GEN_MEDIA_CONNECT_STATUS:
            return le32_bytes(0); // CONNECTED：usbip 连接即链路 up
        case OID_GEN_MAC_OPTIONS:
            return le32_bytes(RNDIS_MAC_OPTIONS_SERIALIZED_FULL_DUPLEX);
        case OID_GEN_PHYSICAL_MEDIUM:
            return le32_bytes(0); // UNSPECIFIED
        case OID_GEN_XMIT_OK:
        case OID_GEN_RCV_OK:
        case OID_GEN_XMIT_ERROR:
        case OID_GEN_RCV_ERROR:
        case OID_GEN_RCV_NO_BUFFER:
            return le32_bytes(0); // 统计（示例不收集）
        case OID_802_3_PERMANENT_ADDRESS:
        case OID_802_3_CURRENT_ADDRESS: {
            // 6 字节二进制 MAC（Linux 主机绑定强制查询 PERMANENT_ADDRESS，
            // 失败则绑定失败——rndis_host.c generic_rndis_bind）
            return {mac_address_[0], mac_address_[1], mac_address_[2], mac_address_[3], mac_address_[4],
                    mac_address_[5]};
        }
        case OID_802_3_MULTICAST_LIST:
            return le32_bytes(0xE0000000); // 组播基址
        case OID_802_3_MAXIMUM_LIST_SIZE:
            return le32_bytes(1);
        case OID_802_3_MAC_OPTIONS:
            return le32_bytes(0);
        case OID_802_3_RCV_ERROR_ALIGNMENT:
        case OID_802_3_XMIT_ONE_COLLISION:
        case OID_802_3_XMIT_MORE_COLLISIONS:
            return le32_bytes(0);
        default:
            return {}; // 未知 OID → NOT_SUPPORTED
    }
}

void RndisCommunicationInterfaceHandler::enqueue_response(data_type &&response) {
    if (response_queue_.size() >= RNDIS_RESPONSE_QUEUE_LIMIT) {
        response_queue_.pop_front(); // 防恶意主机堆积：丢最旧
    }
    response_queue_.push_back(std::move(response));
    // 通知主机有响应可读（对齐内核 resp_avail 回调：中断 IN 发 8 字节 {1,0}）
    notification_channel.push(make_rndis_response_available_notification());
}

void RndisCommunicationInterfaceHandler::reset_device_state() {
    state_ = RndisState::Uninitialized;
    packet_filter_ = 0;
    response_queue_.clear();
}

void RndisCommunicationInterfaceHandler::on_new_connection(TransferResponder &current_session, std::error_code &ec) {
    // 父类先设 session 指针（通道应答请求要用），再绑定通道并重置断连状态。
    // 新客户端是全新的 RNDIS 会话：状态机从 UNINITIALIZED 开始
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    reset_device_state();
    notification_channel.on_new_connection(&current_session);
}

void RndisCommunicationInterfaceHandler::on_disconnection(std::error_code &ec) {
    // 先清通道（缓冲 + 挂起请求，TransferHandle 析构自动释放）并复位状态机，
    // 再调父类清 session
    notification_channel.on_disconnection();
    reset_device_state();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void RndisCommunicationInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    bool cancelled = notification_channel.cancel_pending(unlink_seqnum);
    // 从队列中真的取消了待处理 URB → 回 -ECONNRESET（URB 被取消，且不再发
    // RET_SUBMIT，请求已从队列移除）；找不到（URB 已完成/不存在）→ 回 0
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

// ==================== RndisDataInterfaceHandler ====================

RndisDataInterfaceHandler::RndisDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                                     NetworkBackend *backend,
                                                     RndisCommunicationInterfaceHandler *comm) :
    VirtualInterfaceHandler(handle_interface, string_pool), backend_(backend), comm_(comm) {
    if (backend_) {
        // 注入发帧回调：后端产生帧时包 RNDIS 头再经通道推给主机
        // （通道线程安全，任意线程可调）
        backend_->set_send_to_host([this](const std::uint8_t *data, std::size_t size) {
            in_channel.push(wrap_rndis_packet(data, size));
        });
    }
}

UsbInterface RndisDataInterfaceHandler::make_interface(std::uint8_t in_ep, std::uint8_t out_ep) {
    // 对齐内核 f_rndis.c 的 rndis_data_intf：只有 alt0（无 NOP altsetting，
    // 这是 RNDIS 相对 ECM 的优势——RNDIS 用 OID 包过滤器控制收发，不需要
    // 通过 altsetting 切换数据面）
    UsbInterface i{
            .interface_class = static_cast<std::uint8_t>(ClassCode::CDCData),
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
    return i;
}

void RndisDataInterfaceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                     std::uint32_t transfer_flags,
                                                     std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                     std::error_code &ec) {
    if (ep.is_in()) {
        // Bulk IN：主机请求帧。通道内部处理：缓冲有帧立即应答（一条消息 =
        // 一帧 RNDIS_MSG_PACKET，帧比请求短发短包），否则挂起等待
        SPDLOG_TRACE("[RNDIS] IN req seq={} len={} buf={}", seqnum, transfer_buffer_length, in_channel.size());
        in_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // Bulk OUT：主机发来 RNDIS_MSG_PACKET 消息流。有 backend 解析剥头后
        // 逐帧交给网络侧消费并立即应答；否则先给子类当场消费机会（true=已
        // 处理），都不行挂起入通道（NAK 背压），子类之后用 take_frame() 取出
        // 数据并应答（挂起的是原始消息，不做剥头）
        auto *trx = GenericTransfer::from_handle(transfer.get());
        auto received_size = static_cast<std::uint32_t>(trx->data.size());
        SPDLOG_TRACE("[RNDIS] OUT raw={} bytes", received_size);
        auto frames = unwrap_rndis_packets(trx->data);
        bool consumed = false;
        if (backend_) {
            for (auto &frame : frames) {
                backend_->send_frame(frame.data(), frame.size());
            }
            consumed = true;
        }
        else if (!frames.empty()) {
            consumed = true;
            for (auto &frame : frames) {
                if (!on_frame_received(std::move(frame))) {
                    consumed = false;
                    break;
                }
            }
        }
        if (consumed) {
            // 解析失败（无有效帧）也正常应答：对齐内核丢包但不 stall URB
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
        }
        else {
            out_channel.on_out_request(ep.address, seqnum, std::move(transfer));
        }
    }
}

void RndisDataInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // 数据接口没有类特定控制请求（RNDIS 控制走通信接口的封装命令）
    SPDLOG_WARN("RNDIS 数据接口收到意外控制请求");
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

data_type RndisDataInterfaceHandler::get_class_specific_descriptor() {
    // 数据接口没有类特定描述符（对齐内核 rndis_data_intf）
    return {};
}

bool RndisDataInterfaceHandler::on_frame_received(data_type &&frame) {
    // 默认行为：总是消费并立即应答（未 override 的子类保持"总是接收"语义）
    return true;
}

std::size_t RndisDataInterfaceHandler::send_frame(const std::uint8_t *data, std::size_t size) {
    // 非阻塞推送（断连时通道内丢弃返回 0），自动包 RNDIS 头；
    // 返回值是帧长（对齐 ECM 的 send_frame 语义）
    if (data == nullptr || size == 0) {
        return 0;
    }
    SPDLOG_TRACE("[RNDIS] push frame size={} buf_before={}", size, in_channel.size());
    in_channel.push(wrap_rndis_packet(data, size));
    return size;
}

std::size_t RndisDataInterfaceHandler::send_frame(const data_type &frame) {
    return send_frame(frame.data(), frame.size());
}

std::optional<OutEndpointChannel::Pending> RndisDataInterfaceHandler::take_frame(std::uint32_t timeout_ms) {
    return out_channel.take(timeout_ms);
}

std::optional<OutEndpointChannel::Pending> RndisDataInterfaceHandler::try_take_frame() {
    return out_channel.try_take();
}

void RndisDataInterfaceHandler::set_tx_max_pending(std::size_t max_pending) {
    // 通道内部锁保护缓冲上限
    in_channel.set_max_pending_messages(max_pending);
}

void RndisDataInterfaceHandler::on_new_connection(TransferResponder &current_session, std::error_code &ec) {
    // 父类先设 session 指针（通道应答请求要用），再绑定通道并重置断连状态
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    in_channel.on_new_connection(&current_session);
    out_channel.on_new_connection(&current_session);
}

void RndisDataInterfaceHandler::on_disconnection(std::error_code &ec) {
    // 先清通道（缓冲 + 挂起请求，TransferHandle 析构时自动释放；唤醒阻塞
    // 的取者让它们按断连返回），后端清缓冲防旧数据残留，再调父类清 session
    out_channel.on_disconnection();
    in_channel.on_disconnection();
    if (backend_) {
        backend_->reset();
    }
    VirtualInterfaceHandler::on_disconnection(ec);
}

void RndisDataInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    // 挂起请求分散在 IN/OUT 两通道里，逐个尝试取消
    bool cancelled = in_channel.cancel_pending(unlink_seqnum);
    if (!cancelled) {
        cancelled = out_channel.cancel_pending(unlink_seqnum);
    }
    // 从队列中真的取消了待处理 URB → 回 -ECONNRESET（URB 被取消，且不再发
    // RET_SUBMIT，请求已从队列移除）；找不到（URB 已完成/不存在）→ 回 0
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}
} // namespace usbipdcpp
