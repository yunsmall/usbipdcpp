// [UAC-AC]/[UAC-AS]/[ISO] 调试日志默认编译期裁掉（TRACE 级别）：
// 排查主机驱动协商/流问题时定义 USBIPDCPP_STRACE 保留
#ifndef USBIPDCPP_STRACE
    #undef SPDLOG_ACTIVE_LEVEL
    #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#else
    #undef SPDLOG_ACTIVE_LEVEL
    #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "usbipdcpp/Device.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/protocol.h"
#include "spdlog/spdlog.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"

namespace usbipdcpp {

// ==================== UacAudioControlHandler ====================

UacAudioControlHandler::UacAudioControlHandler(UsbInterface &handle_interface, StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
    change_string_interface(L"Usbipdcpp Audio Control");
}

void UacAudioControlHandler::set_config(const UacDeviceConfig &new_config) {
    config = new_config;
    // 声道数仅支持 1 或 2（bControlSize=1）
    if (config.channels != 1 && config.channels != 2) {
        config.channels = 1;
    }
}

void UacAudioControlHandler::on_setup_interface_handlers() {
    build_class_descriptor();
}

void UacAudioControlHandler::set_as_handler(UacAudioStreamingSourceHandler *handler) {
    as_interface_number = static_cast<std::uint8_t>(handler->get_interface().interface_number);
}

void UacAudioControlHandler::set_as_handler(UacAudioStreamingSinkHandler *handler) {
    as_interface_number = static_cast<std::uint8_t>(handler->get_interface().interface_number);
}

void UacAudioControlHandler::build_class_descriptor() {
    if (desc_built)
        return;
    desc_built = true;

    // UAC 1.0：Header(9) + Input Terminal(12) + Feature Unit(7+ch+1) + Output Terminal(9)
    // 麦克风拓扑：IT(MIC) → FU → OT(USB streaming)；扬声器拓扑：IT(USB streaming) → FU
    // → OT(Speaker)（对齐内核 gadget f_uac1.c：usb_out_it_desc/io_out_ot_desc）。
    // 方向由 input_terminal_type 判断：数据从 USB 流入（IT=TT_USB_STREAMING）即扬声器
    bool is_speaker = (config.input_terminal_type == TT_USB_STREAMING);
    // Header 的 baInterfaceNr 是音频流（AS）接口号：装配时由 set_as_handler
    // 显式写入（复合设备里 AC 不从接口 0 起，硬编码会让驱动找不到 AS 接口）
    auto as_if_num = as_interface_number;
    auto fu_len = static_cast<std::uint8_t>(AC_FEATURE_UNIT_FIXED_LEN + config.channels + 1);

    // Feature Unit bmaControls：按配置组合
    std::uint8_t bma_controls = 0;
    if (config.feature_unit_mute)
        bma_controls |= 0x01;
    if (config.feature_unit_volume)
        bma_controls |= 0x02;

    data_type d;

    // AC Header（wTotalLength 占位，构建完成后回填实际总长）
    AcHeaderDesc{AC_HEADER_LEN, CS_INTERFACE, AC_DESC_HEADER,
                 UAC_BCD_1_00, 0x00,
                 0x01, as_if_num}
            .append_to(d);

    // Input Terminal：麦克风方向类型来自配置（默认麦克风），扬声器方向为 USB streaming
    auto channel_config = (config.channels == 2) ? CHANNEL_CONFIG_STEREO : CHANNEL_CONFIG_MONO;
    AcInputTerminalDesc{AC_INPUT_TERMINAL_LEN, CS_INTERFACE, AC_DESC_INPUT_TERMINAL,
                        UAC_ENTITY_INPUT_TERMINAL, config.input_terminal_type,
                        0x00, config.channels, channel_config,
                        0x00, 0x00}
            .append_to(d);

    // Feature Unit：bmaControls 数组含 ch+1 个元素（master + 每个逻辑通道），所有声道共享同一组控制
    AcFeatureUnitHead{fu_len, CS_INTERFACE, AC_DESC_FEATURE_UNIT,
                      UAC_ENTITY_FEATURE_UNIT, UAC_ENTITY_INPUT_TERMINAL,
                      0x01}
            .append_to(d, bma_controls, config.channels);

    // Output Terminal: 麦克风方向为 USB streaming，扬声器方向为 output_terminal_type
    auto output_type = is_speaker ? config.output_terminal_type : TT_USB_STREAMING;
    AcOutputTerminalDesc{AC_OUTPUT_TERMINAL_LEN, CS_INTERFACE, AC_DESC_OUTPUT_TERMINAL,
                         UAC_ENTITY_OUTPUT_TERMINAL, output_type,
                         0x00, UAC_ENTITY_FEATURE_UNIT,
                         0x00}
            .append_to(d);

    // 回填 AC Header 的 wTotalLength（offset 5-6）为描述符实际总长
    d[5] = static_cast<std::uint8_t>(d.size() & 0xFF);
    d[6] = static_cast<std::uint8_t>((d.size() >> 8) & 0xFF);

    class_desc = std::move(d);
}

data_type UacAudioControlHandler::get_class_specific_descriptor() {
    return class_desc;
}

void UacAudioControlHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    if (type != RequestType::Class) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());

    // 调试用：打印收到的 AC 控制请求（主机驱动的流启动/音量初始化排查）
    SPDLOG_TRACE("[UAC-AC] ctrl: bmReqType=0x{:02x} bRequest=0x{:02x} wValue=0x{:04x} wIndex=0x{:04x} len={}",
                setup_packet.calc_request_type(), setup_packet.request, setup_packet.value, setup_packet.index,
                transfer_buffer_length);

    // 处理类特定 GET_DESCRIPTOR（CS_INTERFACE=0x24）
    if (setup_packet.request == static_cast<std::uint8_t>(StandardRequest::GetDescriptor)) {
        auto desc_type = setup_packet.value >> 8;
        if (desc_type == CS_INTERFACE) {
            auto resp = class_desc;
            auto act_len = std::min(resp.size(), static_cast<std::size_t>(transfer_buffer_length));
            trx->data.assign(resp.begin(), resp.begin() + act_len);
            trx->actual_length = act_len;
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // UAC 1.0 §5.2.3：wValue 高字节为控制选择器，低字节为声道号（master=0），wIndex 高字节为 entity
    auto entity = setup_packet.index >> 8;
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;

    // Input/Output Terminal 无控制属性，ACK 空数据防止主机 STALL（同 UVC 对 IT/OT 的做法）
    if (entity == UAC_ENTITY_INPUT_TERMINAL || entity == UAC_ENTITY_OUTPUT_TERMINAL) {
        trx->actual_length = 0;
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), 0, std::move(transfer)));
        return;
    }

    if (entity != UAC_ENTITY_FEATURE_UNIT) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // ===== Feature Unit =====
    switch (control_selector) {
        case FU_MUTE_CONTROL: {
            if (!config.feature_unit_mute) {
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                return;
            }
            switch (request) {
                case GET_CUR:
                    trx->data = {static_cast<std::uint8_t>(mute ? 1 : 0)};
                    trx->actual_length = 1;
                    break;
                case SET_CUR:
                    if (!trx->data.empty()) {
                        auto new_mute = (trx->data[0] != 0);
                        if (new_mute != mute) {
                            mute = new_mute;
                            // 状态变化经 AC 中断端点推送（对齐内核 u_audio_mute_put 的 notify）
                            send_ac_status({UAC1_STATUS_TYPE_IRQ_PENDING | UAC1_STATUS_TYPE_ORIG_AUDIO_CONTROL_IF,
                                            UAC_ENTITY_FEATURE_UNIT});
                        }
                    }
                    trx->actual_length = 0;
                    break;
                default:
                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
            }
            break;
        }
        case FU_VOLUME_CONTROL: {
            if (!config.feature_unit_volume) {
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                return;
            }
            switch (request) {
                case GET_CUR: {
                    auto v = volume_db;
                    trx->data = {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF)};
                    trx->actual_length = 2;
                    break;
                }
                case GET_MIN: {
                    auto v = config.volume_min_db256;
                    trx->data = {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF)};
                    trx->actual_length = 2;
                    break;
                }
                case GET_MAX: {
                    auto v = config.volume_max_db256;
                    trx->data = {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF)};
                    trx->actual_length = 2;
                    break;
                }
                case GET_DEF: {
                    std::int16_t v = 0;
                    trx->data = {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF)};
                    trx->actual_length = 2;
                    break;
                }
                case GET_RES: {
                    // 1dB 步进
                    std::int16_t v = 0x0100;
                    trx->data = {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF)};
                    trx->actual_length = 2;
                    break;
                }
                case SET_CUR: {
                    if (trx->data.size() >= 2) {
                        auto v = static_cast<std::int16_t>(trx->data[0] | (trx->data[1] << 8));
                        // clamp 到配置的音量范围
                        auto clamped = std::clamp(v, config.volume_min_db256, config.volume_max_db256);
                        if (clamped != volume_db) {
                            volume_db = clamped;
                            // Q16 线性增益：10^(dB/20)，低于 -120dB 视为静音
                            double linear = std::pow(10.0, (volume_db / 256.0) / 20.0);
                            if (volume_db <= -0x7800)
                                linear = 0.0;
                            gain_q16 = static_cast<std::uint32_t>(linear * 65536.0 + 0.5);
                            // 状态变化经 AC 中断端点推送（对齐内核 u_audio_volume_put 的 notify）
                            send_ac_status({UAC1_STATUS_TYPE_IRQ_PENDING | UAC1_STATUS_TYPE_ORIG_AUDIO_CONTROL_IF,
                                            UAC_ENTITY_FEATURE_UNIT});
                        }
                    }
                    trx->actual_length = 0;
                    break;
                }
                default:
                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
            }
            break;
        }
        default:
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
    }

    // 调试用：打印提交的响应（数据前 2 字节 + 长度），排查主机驱动的初始化失败
    SPDLOG_TRACE("[UAC-AC] 提交响应 seq={} actual={} d0=0x{:02x} d1=0x{:02x}", seqnum, trx->actual_length,
                trx->data.empty() ? 0 : trx->data[0], trx->data.size() < 2 ? 0 : trx->data[1]);

    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), static_cast<std::uint32_t>(trx->actual_length),
            std::move(transfer)));
}

void UacAudioControlHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                       std::uint32_t transfer_flags,
                                                       std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                       std::error_code &ec) {
    if (ep.is_in()) {
        // 通道内部处理：缓冲有状态立即应答，否则挂起请求等待 send_ac_status()
        status_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void UacAudioControlHandler::send_ac_status(data_type status) {
    // 通道内部处理：有挂起请求直接应答，否则入缓冲（对齐内核 f_uac1 audio_notify）
    status_channel.push(std::move(status));
}

void UacAudioControlHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    // 父类先设 session 指针（通道应答请求要用），再绑定通道并重置断连状态
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    status_channel.on_new_connection(&current_session);
}

void UacAudioControlHandler::on_disconnection(std::error_code &ec) {
    // 先清通道（缓冲 + 挂起请求，TransferHandle 析构自动释放），再调父类清 session
    status_channel.on_disconnection();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void UacAudioControlHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    bool cancelled = status_channel.cancel_pending(unlink_seqnum);
    // 从队列中真的取消了待处理 URB → 回 -ECONNRESET（URB 被取消，且不再发
    // RET_SUBMIT，请求已从队列移除）；找不到（URB 已完成/不存在）→ 回 0。
    // 与内核 stub_tx.c 及本项目 LibusbDeviceHandler 的 unlink 范本一致
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

data_type UacAudioControlHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                          std::uint16_t descriptor_length, std::uint32_t *p_status) {
    if (type == CS_INTERFACE) {
        return class_desc;
    }
    return VirtualInterfaceHandler::request_get_descriptor(type, language_id, descriptor_length, p_status);
}

// ==================== UacAudioStreamingSourceHandler ====================

UacAudioStreamingSourceHandler::UacAudioStreamingSourceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                                   std::unique_ptr<AudioSource> source) :
    VirtualInterfaceHandler(handle_interface, string_pool), source(std::move(source)) {
    change_string_interface(L"Usbipdcpp Microphone");

    // 按初始格式计算包调度参数（采样率/声道数变化时由 SET_CUR 重算）
    update_packet_bytes();
}

void UacAudioStreamingSourceHandler::update_packet_bytes() {
    auto fmt = source->current_format();
    // 高速等时端点 bInterval=4（对齐内核 gadget f_uac1.c 的 as_in_ep_desc）：
    // 每 1ms 一个包，每秒 1000 包。
    // 基准包长 = 帧大小×(采样率/1000)；采样率对 1000 的余数折算成帧数逐包累加，
    // 累加值够一帧时本包补一帧。包长恒为帧大小整数倍，平均速率精确等于采样率
    packet_framesize = static_cast<std::size_t>(fmt.channels) * (fmt.bits_per_sample / 8);
    packet_interval = 1000;
    auto rate = static_cast<std::size_t>(fmt.sample_rate);
    packet_base = packet_framesize * (rate / packet_interval);
    packet_residue_step = packet_framesize * (rate % packet_interval);
    packet_residue_acc = 0;
}

void UacAudioStreamingSourceHandler::on_setup_interface_handlers() {
    build_class_descriptor();
}

std::vector<std::uint32_t> UacAudioStreamingSourceHandler::supported_sample_rates(const AudioFormatInfo &fmt) const {
    // 收集当前声道数/位深下 source 支持的所有采样率，去重升序
    std::vector<std::uint32_t> rates;
    for (auto &f: source->supported_formats()) {
        if (f.channels == fmt.channels && f.bits_per_sample == fmt.bits_per_sample) {
            rates.push_back(f.sample_rate);
        }
    }
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

void UacAudioStreamingSourceHandler::build_class_descriptor() {
    auto fmt = source->current_format();
    auto rates = supported_sample_rates(fmt);

    data_type d;
    // AS General: bTerminalLink 指向 AC 的 Output Terminal（USB streaming）
    AsGeneralDesc{AS_GENERAL_LEN, CS_INTERFACE, AS_DESC_GENERAL,
                  UAC_ENTITY_OUTPUT_TERMINAL, 0x01, AUDIO_FORMAT_PCM}
            .append_to(d);

    // Format Type I: 16 位 PCM，采样率列表来自 source（UAC 1.0 允许最多 255 个）
    AsFormatTypeIHead{static_cast<std::uint8_t>(AS_FORMAT_TYPE_I_BASE_LEN + rates.size() * AS_SAMFREQ_ENTRY_LEN),
                      CS_INTERFACE, AS_DESC_FORMAT_TYPE,
                      0x01, // bFormatType: FORMAT_TYPE_I
                      static_cast<std::uint8_t>(fmt.channels),
                      0x02, // bSubframeSize
                      static_cast<std::uint8_t>(fmt.bits_per_sample),
                      static_cast<std::uint8_t>(rates.size())}
            .append_to(d, rates);

    class_desc = std::move(d);
}

data_type UacAudioStreamingSourceHandler::get_class_specific_descriptor() {
    return class_desc;
}

void UacAudioStreamingSourceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    if (type != RequestType::Class) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());

    // 调试用：打印收到的 AS 控制请求（主机驱动的流启动排查）
    SPDLOG_TRACE("[UAC-AS] ctrl: bmReqType=0x{:02x} bRequest=0x{:02x} wValue=0x{:04x} wIndex=0x{:04x} len={}",
                setup_packet.calc_request_type(), setup_packet.request, setup_packet.value, setup_packet.index,
                transfer_buffer_length);

    // 处理类特定 GET_DESCRIPTOR（CS_INTERFACE=0x24）
    if (setup_packet.request == static_cast<std::uint8_t>(StandardRequest::GetDescriptor)) {
        auto desc_type = setup_packet.value >> 8;
        if (desc_type == CS_INTERFACE) {
            SPDLOG_TRACE("[UAC-AS] 返回 CS_INTERFACE 类描述符 {} 字节", class_desc.size());
            auto resp = class_desc;
            auto act_len = std::min(resp.size(), static_cast<std::size_t>(transfer_buffer_length));
            trx->data.assign(resp.begin(), resp.begin() + act_len);
            trx->actual_length = act_len;
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // AS 接口控制 entity 固定为接口自身（0）
    auto entity = setup_packet.index >> 8;
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;

    if (entity != 0 || control_selector != AS_SAMPLING_FREQ_CONTROL) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    if (!handle_sampling_freq_control(seqnum, request, trx, transfer, transfer_buffer_length)) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void UacAudioStreamingSourceHandler::handle_non_standard_request_type_control_urb_to_endpoint(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // Linux snd-usb-audio 对 UAC1 的采样率控制发给端点：
    // wValue 高字节为控制选择器，wIndex 为端点地址（recpient=Endpoint）
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;
    auto *trx = GenericTransfer::from_handle(transfer.get());

    if (control_selector != AS_SAMPLING_FREQ_CONTROL ||
        !handle_sampling_freq_control(seqnum, request, trx, transfer, transfer_buffer_length)) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

bool UacAudioStreamingSourceHandler::handle_sampling_freq_control(std::uint32_t seqnum, std::uint8_t request,
                                                            GenericTransfer *trx, TransferHandle &transfer,
                                                            std::uint32_t transfer_buffer_length) {
    auto fmt = source->current_format();
    switch (request) {
        case GET_CUR: {
            auto rate = fmt.sample_rate;
            trx->data = {static_cast<std::uint8_t>(rate & 0xFF), static_cast<std::uint8_t>((rate >> 8) & 0xFF),
                         static_cast<std::uint8_t>((rate >> 16) & 0xFF)};
            trx->actual_length = 3;
            break;
        }
        case GET_MIN:
        case GET_MAX: {
            // 返回支持列表中的最小/最大采样率
            auto rates = supported_sample_rates(fmt);
            auto rate = (request == GET_MIN) ? rates.front() : rates.back();
            trx->data = {static_cast<std::uint8_t>(rate & 0xFF), static_cast<std::uint8_t>((rate >> 8) & 0xFF),
                         static_cast<std::uint8_t>((rate >> 16) & 0xFF)};
            trx->actual_length = 3;
            break;
        }
        case SET_CUR: {
            if (trx->data.size() < 3) {
                return false;
            }
            auto rate = static_cast<std::uint32_t>(trx->data[0] | (trx->data[1] << 8) | (trx->data[2] << 16));
            // UAC 1.0 §5.2.3.2.3.1: 离散采样率端点收到不支持的值时应四舍五入到最近的支持值
            auto rates = supported_sample_rates(fmt);
            auto nearest = *std::min_element(rates.begin(), rates.end(), [rate](std::uint32_t a, std::uint32_t b) {
                return std::abs(static_cast<std::int64_t>(a) - rate) <
                       std::abs(static_cast<std::int64_t>(b) - rate);
            });
            if (nearest != rate) {
                SPDLOG_INFO("采样率 SET_CUR {} 不在支持列表，四舍五入到 {}", rate, nearest);
            }
            rate = nearest;
            if (!source->set_format(fmt.channels, fmt.bits_per_sample, rate)) {
                SPDLOG_WARN("采样率 SET_CUR {} 被 source 拒绝", rate);
                return false;
            }
            update_packet_bytes();
            SPDLOG_INFO("采样率 SET_CUR: {} → source 切换成功，基准包长={} 残差步进={}", rate,
                        packet_base, packet_residue_step);
            chunk_data = nullptr;
            chunk_size = 0;
            chunk_offset = 0;
            build_class_descriptor();
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
            return true;
        }
        default:
            return false;
    }

    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), static_cast<std::uint32_t>(trx->actual_length),
            std::move(transfer)));
    return true;
}

void UacAudioStreamingSourceHandler::fill_pcm(std::uint8_t *dst, std::size_t n) {
    // 静音直接填 0
    if (ac_handler && ac_handler->is_muted()) {
        std::memset(dst, 0, n);
        return;
    }

    auto scale = ac_handler ? ac_handler->volume_scale_q16() : 65536;

    while (n > 0) {
        if (chunk_offset >= chunk_size) {
            AudioChunk chunk;
            if (!source->get_chunk(chunk)) {
                // 源无数据：剩余填静音
                std::memset(dst, 0, n);
                return;
            }
            chunk_data = chunk.data;
            chunk_size = chunk.size;
            chunk_offset = 0;
        }
        auto take = std::min(n, chunk_size - chunk_offset);
        if (scale == 65536) {
            // 0dB 快路径：直接拷贝
            std::memcpy(dst, chunk_data + chunk_offset, take);
        }
        else if (scale == 0) {
            std::memset(dst, 0, take);
        }
        else {
            // 逐样本 Q16 缩放（16 位有符号小端）
            const auto *src = reinterpret_cast<const std::int16_t *>(chunk_data + chunk_offset);
            auto *out = reinterpret_cast<std::int16_t *>(dst);
            for (std::size_t i = 0; i < take / 2; ++i) {
                out[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(src[i]) * scale) >> 16);
            }
        }
        dst += take;
        chunk_offset += take;
        n -= take;
    }
}

void UacAudioStreamingSourceHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                           std::uint32_t transfer_flags,
                                                           std::uint32_t transfer_buffer_length,
                                                           TransferHandle transfer, int num_iso_packets,
                                                           std::error_code &ec) {
    if (!streaming) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());
    // 响应延迟 = 主机声明的各包长度之和对应的音频时长（iso IN 缓冲按各包
    // length 紧凑填充，sum(length) 即本次数据量；数据量在处理时才知道，
    // 这里用声明值提前折算，与处理时的 total_sent 只差截断的零头）。
    // 数据填充挪到 TransferScheduler 的服务时刻做（vudc 语义：处理完当场
    // 发 RET，限速靠"延迟处理时机"——每 URB 处理完当场发 → 主机立即重提交
    // → 下一 URB 再等数据时长 → 完成节奏恒等于数据速率，等效 URB 的 N 个包
    // 分布在 N 帧完成。立即处理会让主机驱动的"完成→重提交"循环失去节流，
    // 实测超发 158 倍）
    std::chrono::microseconds data_duration{};
    auto fmt = source->current_format();
    auto bytes_per_second = static_cast<std::uint64_t>(fmt.sample_rate) * (fmt.bits_per_sample / 8) * fmt.channels;
    std::uint32_t declared = 0;
    for (auto &iso: trx->iso_descriptors)
        declared += iso.length;
    if (bytes_per_second > 0 && declared > 0) {
        data_duration = std::chrono::microseconds(
                static_cast<std::int64_t>(declared) * 1000000 / static_cast<std::int64_t>(bytes_per_second));
    }
    device_handler->get_transfer_scheduler().submit(
            *responder, ep, EndpointAttributes::Isochronous, data_duration, seqnum, std::move(transfer),
            [this, num_iso_packets](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                    TransferHandle &&transfer) {
                process_iso_in(responder, ep, seqnum, num_iso_packets, std::move(transfer));
            });
}

void UacAudioStreamingSourceHandler::process_iso_in(TransferResponder &responder, const UsbEndpoint &ep,
                                                    std::uint32_t seqnum, int num_iso_packets,
                                                    TransferHandle transfer) {
    auto *trx = GenericTransfer::from_handle(transfer.get());
    auto &data = trx->data;
    auto &iso_descs = trx->iso_descriptors;

    // 调试用：确认 ISO URB 是否到达、每包填多少字节（Windows 录音无数据排查）
    SPDLOG_TRACE("[ISO] seq={} num_packets={} packet_base={} residue_step={}",
                seqnum, num_iso_packets, packet_base, packet_residue_step);

    // 内核 usb_submit_urb 会把 iso_frame_desc[n].status 初始化为 -EXDEV，
    // 必须清零，否则内核音频驱动会跳过所有包。
    for (auto &iso: iso_descs)
        iso.status = 0;

    // 每包字节数按内核 gadget u_audio.c（u_audio_iso_complete 的 PLAYBACK 分支）的
    // 残差累加算法逐包计算：基准包长 + 残差累积够一帧时补一帧。
    // 包长恒为帧大小整数倍，平均速率精确匹配采样率——非 1kHz 整数倍采样率
    // （如 44100）下每 ms 88.2 字节由 88/90 字节包交替实现，
    // 短包合法（CS_ENDPOINT bmAttributes D7=0，MaxPacketsOnly 未置位）
    std::uint32_t total_sent = 0;

    for (int i = 0; i < num_iso_packets; ++i) {
        auto &iso = iso_descs[i];
        packet_residue_acc += packet_residue_step;
        auto packet_bytes = packet_base;
        if (packet_residue_acc >= packet_framesize * packet_interval) {
            packet_bytes += packet_framesize;
            packet_residue_acc -= packet_framesize * packet_interval;
        }
        auto want = std::min(static_cast<std::size_t>(iso.length), packet_bytes);
        if (want == 0)
            continue;

        fill_pcm(&data[iso.offset], want);
        iso.actual_length = static_cast<std::uint32_t>(want);
        total_sent += iso.actual_length;
    }

    // 调试用：ISO 响应信息（每包实际字节数分布）
    SPDLOG_TRACE("[ISO] 处理完成 seq={} num_packets={} total={}B", seqnum, num_iso_packets, total_sent);
    // 处理完自行发送（通知语义，与 handler 其他响应路径一致），再上报
    // 完成：调度器推进串行点、服务下一个 URB（同步处理，先发后报）
    responder.submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), total_sent, 0,
            static_cast<std::uint32_t>(iso_descs.size()), std::move(transfer)));
    device_handler->get_transfer_scheduler().on_urb_done(ep.address, seqnum);
}

void UacAudioStreamingSourceHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    streaming = false;
    chunk_data = nullptr;
    chunk_size = 0;
    chunk_offset = 0;
}

void UacAudioStreamingSourceHandler::on_disconnection(error_code &ec) {
    streaming = false;
    chunk_data = nullptr;
    chunk_size = 0;
    chunk_offset = 0;
    VirtualInterfaceHandler::on_disconnection(ec);
}

void UacAudioStreamingSourceHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting == 0) {
        streaming = false;
        *p_status = 0;
    }
    else if (alternate_setting == 1) {
        streaming = true;
        chunk_offset = 0; // 重新开始拉数据，保证流起点干净
        packet_residue_acc = 0; // 残差清零（对齐 gadget 的 uac_pcm_open 重置 p_residue）
        *p_status = 0;
    }
    else {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

data_type UacAudioStreamingSourceHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                            std::uint16_t descriptor_length, std::uint32_t *p_status) {
    if (type == CS_INTERFACE) {
        return class_desc;
    }
    return VirtualInterfaceHandler::request_get_descriptor(type, language_id, descriptor_length, p_status);
}

// ==================== UacAudioStreamingSinkHandler ====================

UacAudioStreamingSinkHandler::UacAudioStreamingSinkHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                                           std::unique_ptr<AudioSink> sink) :
    VirtualInterfaceHandler(handle_interface, string_pool), sink(std::move(sink)) {
    change_string_interface(L"Usbipdcpp Speaker");
}

void UacAudioStreamingSinkHandler::on_setup_interface_handlers() {
    build_class_descriptor();
}

void UacAudioStreamingSinkHandler::build_class_descriptor() {
    auto fmt = sink->current_format();
    std::vector<std::uint32_t> rates;
    for (auto &f: sink->supported_formats()) {
        if (f.channels == fmt.channels && f.bits_per_sample == fmt.bits_per_sample) {
            rates.push_back(f.sample_rate);
        }
    }
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());

    data_type d;
    // AS General: bTerminalLink 指向 AC 的 Input Terminal（USB streaming，扬声器方向
    // 数据从该终端流入，对齐内核 gadget f_uac1.c 的 as_out_header_desc 动态终端链接）
    AsGeneralDesc{AS_GENERAL_LEN, CS_INTERFACE, AS_DESC_GENERAL,
                  UAC_ENTITY_INPUT_TERMINAL, 0x01, AUDIO_FORMAT_PCM}
            .append_to(d);

    // Format Type I: 16 位 PCM，采样率列表来自 sink（UAC 1.0 允许最多 255 个）
    AsFormatTypeIHead{static_cast<std::uint8_t>(AS_FORMAT_TYPE_I_BASE_LEN + rates.size() * AS_SAMFREQ_ENTRY_LEN),
                      CS_INTERFACE, AS_DESC_FORMAT_TYPE,
                      0x01, // bFormatType: FORMAT_TYPE_I
                      static_cast<std::uint8_t>(fmt.channels),
                      0x02, // bSubframeSize
                      static_cast<std::uint8_t>(fmt.bits_per_sample),
                      static_cast<std::uint8_t>(rates.size())}
            .append_to(d, rates);

    class_desc = std::move(d);
}

data_type UacAudioStreamingSinkHandler::get_class_specific_descriptor() {
    return class_desc;
}

void UacAudioStreamingSinkHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    if (type != RequestType::Class) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());

    // 调试用：打印收到的 AS 控制请求（主机驱动的流启动排查）
    SPDLOG_TRACE("[UAC-AS-SINK] ctrl: bmReqType=0x{:02x} bRequest=0x{:02x} wValue=0x{:04x} wIndex=0x{:04x} len={}",
                setup_packet.calc_request_type(), setup_packet.request, setup_packet.value, setup_packet.index,
                transfer_buffer_length);

    // 处理类特定 GET_DESCRIPTOR（CS_INTERFACE=0x24）
    if (setup_packet.request == static_cast<std::uint8_t>(StandardRequest::GetDescriptor)) {
        auto desc_type = setup_packet.value >> 8;
        if (desc_type == CS_INTERFACE) {
            auto resp = class_desc;
            auto act_len = std::min(resp.size(), static_cast<std::size_t>(transfer_buffer_length));
            trx->data.assign(resp.begin(), resp.begin() + act_len);
            trx->actual_length = act_len;
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // AS 接口控制 entity 固定为接口自身（0）
    auto entity = setup_packet.index >> 8;
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;

    if (entity != 0 || control_selector != AS_SAMPLING_FREQ_CONTROL) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    if (!handle_sampling_freq_control(seqnum, request, trx, transfer, transfer_buffer_length)) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void UacAudioStreamingSinkHandler::handle_non_standard_request_type_control_urb_to_endpoint(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // Linux snd-usb-audio 对 UAC1 的采样率控制发给端点：
    // wValue 高字节为控制选择器，wIndex 为端点地址（recpient=Endpoint）
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;
    auto *trx = GenericTransfer::from_handle(transfer.get());

    if (control_selector != AS_SAMPLING_FREQ_CONTROL ||
        !handle_sampling_freq_control(seqnum, request, trx, transfer, transfer_buffer_length)) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

bool UacAudioStreamingSinkHandler::handle_sampling_freq_control(std::uint32_t seqnum, std::uint8_t request,
                                                                GenericTransfer *trx, TransferHandle &transfer,
                                                                std::uint32_t transfer_buffer_length) {
    auto fmt = sink->current_format();
    switch (request) {
        case GET_CUR: {
            auto rate = fmt.sample_rate;
            trx->data = {static_cast<std::uint8_t>(rate & 0xFF), static_cast<std::uint8_t>((rate >> 8) & 0xFF),
                         static_cast<std::uint8_t>((rate >> 16) & 0xFF)};
            trx->actual_length = 3;
            break;
        }
        case GET_MIN:
        case GET_MAX: {
            // 返回支持列表中的最小/最大采样率
            std::vector<std::uint32_t> rates;
            for (auto &f: sink->supported_formats()) {
                if (f.channels == fmt.channels && f.bits_per_sample == fmt.bits_per_sample) {
                    rates.push_back(f.sample_rate);
                }
            }
            std::sort(rates.begin(), rates.end());
            auto rate = (request == GET_MIN) ? rates.front() : rates.back();
            trx->data = {static_cast<std::uint8_t>(rate & 0xFF), static_cast<std::uint8_t>((rate >> 8) & 0xFF),
                         static_cast<std::uint8_t>((rate >> 16) & 0xFF)};
            trx->actual_length = 3;
            break;
        }
        case SET_CUR: {
            if (trx->data.size() < 3) {
                return false;
            }
            auto rate = static_cast<std::uint32_t>(trx->data[0] | (trx->data[1] << 8) | (trx->data[2] << 16));
            // UAC 1.0 §5.2.3.2.3.1: 离散采样率端点收到不支持的值时应四舍五入到最近的支持值
            std::vector<std::uint32_t> rates;
            for (auto &f: sink->supported_formats()) {
                if (f.channels == fmt.channels && f.bits_per_sample == fmt.bits_per_sample) {
                    rates.push_back(f.sample_rate);
                }
            }
            std::sort(rates.begin(), rates.end());
            auto nearest = *std::min_element(rates.begin(), rates.end(), [rate](std::uint32_t a, std::uint32_t b) {
                return std::abs(static_cast<std::int64_t>(a) - rate) <
                       std::abs(static_cast<std::int64_t>(b) - rate);
            });
            if (nearest != rate) {
                SPDLOG_INFO("采样率 SET_CUR {} 不在支持列表，四舍五入到 {}", rate, nearest);
            }
            rate = nearest;
            if (!sink->set_format(fmt.channels, fmt.bits_per_sample, rate)) {
                SPDLOG_WARN("采样率 SET_CUR {} 被 sink 拒绝", rate);
                return false;
            }
            SPDLOG_INFO("采样率 SET_CUR: {} → sink 切换成功", rate);
            build_class_descriptor();
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
            return true;
        }
        default:
            return false;
    }

    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), static_cast<std::uint32_t>(trx->actual_length),
            std::move(transfer)));
    return true;
}

void UacAudioStreamingSinkHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                               std::uint32_t transfer_flags,
                                                               std::uint32_t transfer_buffer_length,
                                                               TransferHandle transfer, int num_iso_packets,
                                                               std::error_code &ec) {
    if (!streaming) {
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());
    // 响应延迟 = 主机声明的各包长度之和对应的音频时长（OUT 数据按各包 length
    // 紧凑排列，sum(length) 即本次实际数据量，处理时才统计，这里用声明值提前
    // 折算）+ 收流速率闭环修正。延迟跟随数据量 → 完成速率恒等于主机数据速率
    //（对齐真实自适应 OUT 端点的跟随行为；本地时钟固定延迟会与主机时钟频偏
    // 累积，实测 0.2% 级失步 → usbaudio rate matching 缓冲水位缓降 → 周期性
    // 欠载沙沙）。数据处理（音量/写 sink/闭环统计）挪到 TransferScheduler 的
    // 服务时刻做（vudc 语义：处理完当场发 RET，限速靠"延迟处理时机"——每
    // URB 处理完当场发 → 主机立即重提交 → 下一 URB 再等数据时长 → 完成节奏
    // 恒等于主机数据速率。立即应答会触发超发，实测 5 倍速灌入，消费跟不上
    // 被迫丢 80% 数据 → 播放快进/断续）
    std::chrono::microseconds data_duration{};
    auto fmt = sink->current_format();
    auto bytes_per_second = static_cast<std::uint64_t>(fmt.sample_rate) * (fmt.bits_per_sample / 8) * fmt.channels;
    std::uint32_t declared = 0;
    for (auto &iso: trx->iso_descriptors)
        declared += iso.length;
    if (bytes_per_second > 0 && declared > 0) {
        data_duration = std::chrono::microseconds(
                static_cast<std::int64_t>(declared) * 1000000 / static_cast<std::int64_t>(bytes_per_second));
        data_duration += std::chrono::microseconds(pacing_delta_us.load());
    }
    device_handler->get_transfer_scheduler().submit(
            *responder, ep, EndpointAttributes::Isochronous, data_duration, seqnum, std::move(transfer),
            [this, num_iso_packets](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                    TransferHandle &&transfer) {
                process_iso_out(responder, ep, seqnum, num_iso_packets, std::move(transfer));
            });
}

void UacAudioStreamingSinkHandler::process_iso_out(TransferResponder &responder, const UsbEndpoint &ep,
                                                   std::uint32_t seqnum, int num_iso_packets,
                                                   TransferHandle transfer) {
    auto *trx = GenericTransfer::from_handle(transfer.get());
    auto &data = trx->data;
    auto &iso_descs = trx->iso_descriptors;

    // iso 描述符 status 初始为 -EXDEV（对齐内核 usb_submit_urb 初始化），
    // 必须清零，否则内核音频驱动认为所有包失败
    for (auto &iso: iso_descs)
        iso.status = 0;

    // OUT 数据在缓冲中按各包 length 累加紧凑排列（描述符 offset 是客户端本地布局，
    // 不可信，见 LibusbTransferOperator recv_transfer_data 注释），逐包读出交给
    // sink 消费
    std::uint32_t total_received = 0;
    std::size_t data_pos = 0;
    for (auto &iso: iso_descs) {
        auto take = std::min(static_cast<std::size_t>(iso.length), data.size() - data_pos);
        if (take > 0) {
            // 应用 AC 音量控制（mute/volume）——对齐麦克风 fill_pcm 的增益处理。
            // Windows 音量滑块走 FU 的 SET_CUR，音量状态在 AC handler，收流侧
            // 不应用则调节无效。16 位 PCM 逐采样乘 Q16 增益，字节运算避免未对齐
            if (ac_handler && ac_handler->is_muted()) {
                std::memset(&data[data_pos], 0, take);
            } else if (ac_handler) {
                auto scale = ac_handler->volume_scale_q16();
                if (scale != 65536) {
                    auto n = take / 2 * 2; // 只处理完整采样
                    for (std::size_t i = 0; i < n; i += 2) {
                        auto s = static_cast<std::int32_t>(
                                static_cast<std::int16_t>(data[data_pos + i] | (data[data_pos + i + 1] << 8)));
                        s = (s * static_cast<std::int32_t>(scale)) >> 16;
                        data[data_pos + i] = static_cast<std::uint8_t>(s & 0xFF);
                        data[data_pos + i + 1] = static_cast<std::uint8_t>((s >> 8) & 0xFF);
                    }
                }
            }
            sink->write_pcm(&data[data_pos], take);
            iso.actual_length = static_cast<std::uint32_t>(take);
            total_received += iso.actual_length;
        }
        data_pos += iso.length;
    }
    // 收流速率闭环（无条件启用，对所有 sink 生效）：
    // 实测 usbaudio 每 URB 数据恒为 10ms 音频、从不加减数据量，主机提交节奏 =
    // 我们的响应延迟 ± 小开销 → 接收速率完全由响应延迟决定。反馈用墙钟窗口
    // 测接收速率（received 累计增量/真实时间）——不能用 URB 到达间隔（usbip-win
    // 池机制下 URB 突发到达，间隔法窗口边界误差会把速率高估 ~1%，闭环误判
    // 已收敛导致持续欠载），也不能用瞬时水位（回调相位偏差把修正量推死限幅）。
    // R 快 → Δ 正（延迟响应）→ R 慢，反之亦然，平衡点自动补偿主机固定提交
    // 开销（实测 ~60µs，Δ 收敛到 -60µs 附近）。播放 sink 靠它防欠载沙沙；
    // 文件 sink（WavFileSink）靠它锁接收速率 = 标称采样率，否则录出的文件
    // 样本数/秒偏少，按标称采样率播放会变速
    auto fmt = sink->current_format();
    auto frame_bytes = static_cast<std::int64_t>(fmt.bits_per_sample / 8) * fmt.channels;
    urb_stat_count++;
    urb_stat_bytes += total_received;
    if (urb_stat_count >= 100) {
        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
        // 显式 int64_t：duration 的 rep 在 Android LP64 是 long long、int64_t
        // 是 long，auto 推导混用会让 clamp 的类型推导冲突（Windows 上两者
        // 都是 long long 不触发）
        auto time_delta = static_cast<std::int64_t>(now_us - urb_stat_last_time_us);
        auto bytes_delta = static_cast<std::int64_t>(urb_stat_bytes) - urb_stat_last_bytes;
        if (time_delta > 0 && frame_bytes > 0) {
            auto r_hz = bytes_delta * 1000000 / time_delta / frame_bytes;
            auto target_hz = static_cast<std::int64_t>(fmt.sample_rate);
            // 增益：1% 速率差 → 每轮修正 48µs（约 10 秒收敛）；
            // 限幅 ±600µs（±6%），防网络尖峰把响应延迟打飞。
            // 单写者（调度线程）多读者（网络线程提交时 load），用原子读写
            auto delta = pacing_delta_us.load() + (r_hz - target_hz) * 50 / 1000;
            pacing_delta_us.store(std::clamp(delta, std::int64_t{-600}, std::int64_t{600}));
        }
        urb_stat_count = 0;
        urb_stat_last_bytes = static_cast<std::int64_t>(urb_stat_bytes);
        urb_stat_last_time_us = now_us;
    }
    // 处理完自行发送（通知语义，与 handler 其他响应路径一致），再上报
    // 完成：调度器推进串行点、服务下一个 URB（同步处理，先发后报）
    responder.submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), total_received, 0,
            static_cast<std::uint32_t>(iso_descs.size()), std::move(transfer)));
    device_handler->get_transfer_scheduler().on_urb_done(ep.address, seqnum);
}

void UacAudioStreamingSinkHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    streaming = false;
    sink->reset();
    // 闭环状态从零开始：新连接的水位/节奏与上次无关（统计字段全清，
    // 否则残留的字节数/墙钟时刻会污染重连后第一个测速窗口）
    pacing_delta_us = 0;
    urb_stat_count = 0;
    urb_stat_bytes = 0;
    urb_stat_last_bytes = 0;
    urb_stat_last_time_us = 0;
}

void UacAudioStreamingSinkHandler::on_disconnection(error_code &ec) {
    streaming = false;
    sink->reset();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void UacAudioStreamingSinkHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting == 0) {
        streaming = false;
        *p_status = 0;
    }
    else if (alternate_setting == 1) {
        streaming = true;
        sink->reset(); // 清空缓冲，保证新流起点干净
        *p_status = 0;
    }
    else {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

data_type UacAudioStreamingSinkHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                                std::uint16_t descriptor_length, std::uint32_t *p_status) {
    if (type == CS_INTERFACE) {
        return class_desc;
    }
    return VirtualInterfaceHandler::request_get_descriptor(type, language_id, descriptor_length, p_status);
}

// ==================== UacDeviceHelper ====================

std::error_code UacDeviceHelper::setup_microphone(std::shared_ptr<UsbDevice> device,
                                                  std::uint8_t ac_interface_number, StringPool &string_pool,
                                                  std::unique_ptr<AudioSource> source,
                                                  const UacDeviceConfig &config) {
    // 按 interface_number 定位 AC/AS 两个相邻接口（不依赖数组下标：复合设备
    // 里 UAC 接口可能不在 interfaces 开头，与其他功能交错）
    auto found = device->find_interfaces_by_number<2>(ac_interface_number);
    if (!found[0] || !found[1]) {
        return make_error_code(ErrorType::INVALID_ARG);
    }
    auto *ac_interface = found[0];
    auto *as_interface = found[1];

    auto ac = ac_interface->with_handler<UacAudioControlHandler>(string_pool);
    auto as = as_interface->with_handler<UacAudioStreamingSourceHandler>(string_pool, std::move(source));

    // 声道数：config 未指定（0）时从 source 推断
    auto resolved = config;
    if (resolved.channels == 0) {
        resolved.channels = as->get_source()->current_format().channels;
    }
    ac->set_config(resolved);
    as->set_ac_handler(ac.get());
    ac->set_as_handler(as.get());
    // UAC 功能由 AC 接口声明 IAD（iFunction = AC 的 iInterface，同
    // UvcDeviceHelper 的声明方式，bFirstInterface 由配置描述符生成时按接口号
    // 回填）：复合设备（如 UVC+UAC 一体摄像头）里 usbccgp 只按 IAD 归并接口
    // 组，UAC 组无 IAD 时 AC/AS 被拆成两个独立功能分别启动，usbaudio.sys
    // 凑不齐 AC+AS 组合而启动失败（Windows 实测 CM_PROB_FAILED_START）。
    // 音频没有 UVC 那种 "interface collection" 子类概念，bFunctionSubClass 填 0
    ac_interface->interface_association_descriptor = IadDesc::make(
            2, CC_AUDIO, 0, 0, ac->get_string_interface_value());

    auto dh = device->handler ? std::dynamic_pointer_cast<VirtualDeviceHandler>(device->handler)
                              : device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    // UAC 走等时帧调度（ISO URB 按帧节奏延迟响应），启用设备级调度器
    dh->set_use_transfer_scheduler(true);
    dh->setup_interface_handlers();
    return {};
}

std::error_code UacDeviceHelper::setup_speaker(std::shared_ptr<UsbDevice> device, std::uint8_t ac_interface_number,
                                               StringPool &string_pool, std::unique_ptr<AudioSink> sink,
                                               const UacDeviceConfig &config) {
    // 按 interface_number 定位 AC/AS 两个相邻接口（不依赖数组下标，同 setup_microphone）
    auto found = device->find_interfaces_by_number<2>(ac_interface_number);
    if (!found[0] || !found[1]) {
        return make_error_code(ErrorType::INVALID_ARG);
    }
    auto *ac_interface = found[0];
    auto *as_interface = found[1];

    auto ac = ac_interface->with_handler<UacAudioControlHandler>(string_pool);
    auto as = as_interface->with_handler<UacAudioStreamingSinkHandler>(string_pool, std::move(sink));

    // 声道数：config 未指定（0）时从 sink 推断
    auto resolved = config;
    // 扬声器工厂强制 USB streaming 输入终端：AC 拓扑方向由 input_terminal_type
    // 决定（build_class_descriptor 按它判断扬声器），调用方漏传会生成麦克风拓扑
    // （IT=mic/OT=USB streaming）配 OUT 数据端点，Windows usbaudio 启动失败
    resolved.input_terminal_type = TT_USB_STREAMING;
    if (resolved.channels == 0) {
        resolved.channels = as->get_sink()->current_format().channels;
    }
    ac->set_config(resolved);
    as->set_ac_handler(ac.get());
    ac->set_as_handler(as.get());
    // UAC 功能由 AC 接口声明 IAD（iFunction = AC 的 iInterface，同
    // UvcDeviceHelper 的声明方式，bFirstInterface 由配置描述符生成时按接口号
    // 回填）：复合设备（如 UVC+UAC 一体摄像头）里 usbccgp 只按 IAD 归并接口
    // 组，UAC 组无 IAD 时 AC/AS 被拆成两个独立功能分别启动，usbaudio.sys
    // 凑不齐 AC+AS 组合而启动失败（Windows 实测 CM_PROB_FAILED_START）。
    // 音频没有 UVC 那种 "interface collection" 子类概念，bFunctionSubClass 填 0
    ac_interface->interface_association_descriptor = IadDesc::make(
            2, CC_AUDIO, 0, 0, ac->get_string_interface_value());

    auto dh = device->handler ? std::dynamic_pointer_cast<VirtualDeviceHandler>(device->handler)
                              : device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    // 扬声器收流（OUT）同样需要帧调度：立即回 RET 会让主机（usbip-win vhci
    // 的发送节奏被 RET 反馈驱动）以为管道随时空闲而超发（实测 5 倍速灌入），
    // 延迟响应把发送速率限制回 1 倍。Linux vhci 按自己的帧时钟发不受影响
    dh->set_use_transfer_scheduler(true);
    dh->setup_interface_handlers();
    return {};
}

} // namespace usbipdcpp
