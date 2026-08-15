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

void UacAudioControlHandler::build_class_descriptor() {
    if (desc_built)
        return;
    desc_built = true;

    // UAC 1.0：Header(9) + Input Terminal(12) + Feature Unit(7+ch+1) + Output Terminal(9)
    // AS interface 固定跟在 AC 之后（interface 1）
    std::uint8_t as_if_num = 1;
    auto fu_len = static_cast<std::uint8_t>(AC_FEATURE_UNIT_FIXED_LEN + config.channels + 1);
    auto total_ac_size = AC_HEADER_LEN + AC_INPUT_TERMINAL_LEN + fu_len + AC_OUTPUT_TERMINAL_LEN;

    // Feature Unit bmaControls：按配置组合
    std::uint8_t bma_controls = 0;
    if (config.feature_unit_mute)
        bma_controls |= 0x01;
    if (config.feature_unit_volume)
        bma_controls |= 0x02;

    data_type d;

    // AC Header
    append_descriptor(d, AcHeaderDesc{
                                 AC_HEADER_LEN, CS_INTERFACE, AC_DESC_HEADER,
                                 UAC_BCD_1_00, static_cast<std::uint16_t>(total_ac_size),
                                 0x01, as_if_num});

    // Input Terminal：类型来自配置（默认麦克风）
    auto channel_config = (config.channels == 2) ? CHANNEL_CONFIG_STEREO : CHANNEL_CONFIG_MONO;
    append_descriptor(d, AcInputTerminalDesc{
                                 AC_INPUT_TERMINAL_LEN, CS_INTERFACE, AC_DESC_INPUT_TERMINAL,
                                 UAC_ENTITY_INPUT_TERMINAL, config.input_terminal_type,
                                 0x00, config.channels, channel_config,
                                 0x00, 0x00});

    // Feature Unit：bmaControls 数组含 ch+1 个元素（master + 每个逻辑通道），所有声道共享同一组控制
    append_descriptor(d, AcFeatureUnitHead{
                                 fu_len, CS_INTERFACE, AC_DESC_FEATURE_UNIT,
                                 UAC_ENTITY_FEATURE_UNIT, UAC_ENTITY_INPUT_TERMINAL,
                                 0x01});
    for (int i = 0; i < config.channels + 1; ++i)
        d.push_back(bma_controls);
    d.push_back(0x00); // iFeature

    // Output Terminal: USB streaming
    append_descriptor(d, AcOutputTerminalDesc{
                                 AC_OUTPUT_TERMINAL_LEN, CS_INTERFACE, AC_DESC_OUTPUT_TERMINAL,
                                 UAC_ENTITY_OUTPUT_TERMINAL, TT_USB_STREAMING,
                                 0x00, UAC_ENTITY_FEATURE_UNIT,
                                 0x00});

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
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());

    // 处理类特定 GET_DESCRIPTOR（CS_INTERFACE=0x24）
    if (setup_packet.request == static_cast<std::uint8_t>(StandardRequest::GetDescriptor)) {
        auto desc_type = setup_packet.value >> 8;
        if (desc_type == CS_INTERFACE) {
            auto resp = class_desc;
            auto act_len = std::min(resp.size(), static_cast<std::size_t>(transfer_buffer_length));
            trx->data.assign(resp.begin(), resp.begin() + act_len);
            trx->actual_length = act_len;
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // UAC 1.0 §5.2.3：wValue 高字节为控制选择器，低字节为声道号（master=0），wIndex 高字节为 entity
    auto entity = setup_packet.index >> 8;
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;

    // Input/Output Terminal 无控制属性，ACK 空数据防止主机 STALL（同 UVC 对 IT/OT 的做法）
    if (entity == UAC_ENTITY_INPUT_TERMINAL || entity == UAC_ENTITY_OUTPUT_TERMINAL) {
        trx->actual_length = 0;
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), 0, std::move(transfer)));
        return;
    }

    if (entity != UAC_ENTITY_FEATURE_UNIT) {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // ===== Feature Unit =====
    switch (control_selector) {
        case FU_MUTE_CONTROL: {
            if (!config.feature_unit_mute) {
                session->submit_ret_submit(
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
                        mute = (trx->data[0] != 0);
                    }
                    trx->actual_length = 0;
                    break;
                default:
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
            }
            break;
        }
        case FU_VOLUME_CONTROL: {
            if (!config.feature_unit_volume) {
                session->submit_ret_submit(
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
                        volume_db = std::clamp(v, config.volume_min_db256, config.volume_max_db256);
                        // Q16 线性增益：10^(dB/20)，低于 -120dB 视为静音
                        double linear = std::pow(10.0, (volume_db / 256.0) / 20.0);
                        if (volume_db <= -0x7800)
                            linear = 0.0;
                        gain_q16 = static_cast<std::uint32_t>(linear * 65536.0 + 0.5);
                    }
                    trx->actual_length = 0;
                    break;
                }
                default:
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
            }
            break;
        }
        default:
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
    }

    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), static_cast<std::uint32_t>(trx->actual_length),
            std::move(transfer)));
}

void UacAudioControlHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    *p_status = (alternate_setting == 0) ? 0 : static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}
std::uint8_t UacAudioControlHandler::request_get_interface(std::uint32_t *p_status) {
    return 0;
}
void UacAudioControlHandler::request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = 0;
}
void UacAudioControlHandler::request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                          std::uint32_t *p_status) {
    *p_status = 0;
}
void UacAudioControlHandler::request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = 0;
}
void UacAudioControlHandler::request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                            std::uint32_t *p_status) {
    *p_status = 0;
}
std::uint16_t UacAudioControlHandler::request_get_status(std::uint32_t *p_status) {
    return 0;
}
std::uint16_t UacAudioControlHandler::request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) {
    return 0;
}

data_type UacAudioControlHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                          std::uint16_t descriptor_length, std::uint32_t *p_status) {
    if (type == CS_INTERFACE) {
        return class_desc;
    }
    return VirtualInterfaceHandler::request_get_descriptor(type, language_id, descriptor_length, p_status);
}

// ==================== UacAudioStreamingHandler ====================

UacAudioStreamingHandler::UacAudioStreamingHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                                   std::unique_ptr<AudioSource> source) :
    VirtualInterfaceHandler(handle_interface, string_pool), source(std::move(source)) {
    change_string_interface(L"Usbipdcpp Microphone");

    // 每 URB 期望 1ms PCM 字节数（采样率需为 8kHz 整数倍，高速等时每 microframe 一包恰好整除）
    auto fmt = this->source->current_format();
    bytes_per_ms = static_cast<std::size_t>(fmt.sample_rate) * fmt.channels * (fmt.bits_per_sample / 8) / 1000;
}

void UacAudioStreamingHandler::on_setup_interface_handlers() {
    build_class_descriptor();
}

std::vector<std::uint32_t> UacAudioStreamingHandler::supported_sample_rates(const AudioFormatInfo &fmt) const {
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

void UacAudioStreamingHandler::build_class_descriptor() {
    auto fmt = source->current_format();
    auto rates = supported_sample_rates(fmt);

    data_type d;
    // AS General: bTerminalLink 指向 AC 的 Output Terminal（USB streaming）
    append_descriptor(d, AsGeneralDesc{
                                 AS_GENERAL_LEN, CS_INTERFACE, AS_DESC_GENERAL,
                                 UAC_ENTITY_OUTPUT_TERMINAL, 0x01, AUDIO_FORMAT_PCM});

    // Format Type I: 16 位 PCM，采样率列表来自 source（UAC 1.0 允许最多 255 个）
    append_descriptor(d, AsFormatTypeIHead{
                                 static_cast<std::uint8_t>(AS_FORMAT_TYPE_I_BASE_LEN +
                                                           rates.size() * AS_SAMFREQ_ENTRY_LEN),
                                 CS_INTERFACE, AS_DESC_FORMAT_TYPE,
                                 0x01, // bFormatType: FORMAT_TYPE_I
                                 static_cast<std::uint8_t>(fmt.channels),
                                 0x02, // bSubframeSize
                                 static_cast<std::uint8_t>(fmt.bits_per_sample),
                                 static_cast<std::uint8_t>(rates.size())});
    for (auto rate: rates) {
        d.insert(d.end(),
                 {static_cast<std::uint8_t>(rate & 0xFF), static_cast<std::uint8_t>((rate >> 8) & 0xFF),
                  static_cast<std::uint8_t>((rate >> 16) & 0xFF)});
    }

    class_desc = std::move(d);
}

data_type UacAudioStreamingHandler::get_class_specific_descriptor() {
    return class_desc;
}

void UacAudioStreamingHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    if (type != RequestType::Class) {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());

    // 处理类特定 GET_DESCRIPTOR（CS_INTERFACE=0x24）
    if (setup_packet.request == static_cast<std::uint8_t>(StandardRequest::GetDescriptor)) {
        auto desc_type = setup_packet.value >> 8;
        if (desc_type == CS_INTERFACE) {
            auto resp = class_desc;
            auto act_len = std::min(resp.size(), static_cast<std::size_t>(transfer_buffer_length));
            trx->data.assign(resp.begin(), resp.begin() + act_len);
            trx->actual_length = act_len;
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // AS 接口控制 entity 固定为接口自身（0）
    auto entity = setup_packet.index >> 8;
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;

    if (entity != 0 || control_selector != AS_SAMPLING_FREQ_CONTROL) {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    if (!handle_sampling_freq_control(seqnum, request, trx, transfer, transfer_buffer_length)) {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void UacAudioStreamingHandler::handle_non_standard_request_type_control_urb_to_endpoint(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // Linux snd-usb-audio 对 UAC1 的采样率控制发给端点：
    // wValue 高字节为控制选择器，wIndex 为端点地址（recpient=Endpoint）
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;
    auto *trx = GenericTransfer::from_handle(transfer.get());

    if (control_selector != AS_SAMPLING_FREQ_CONTROL ||
        !handle_sampling_freq_control(seqnum, request, trx, transfer, transfer_buffer_length)) {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

bool UacAudioStreamingHandler::handle_sampling_freq_control(std::uint32_t seqnum, std::uint8_t request,
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
            bytes_per_ms = static_cast<std::size_t>(rate) * fmt.channels * (fmt.bits_per_sample / 8) / 1000;
            SPDLOG_INFO("采样率 SET_CUR: {} → source 切换成功，bytes_per_ms={}", rate, bytes_per_ms);
            chunk_data = nullptr;
            chunk_size = 0;
            chunk_offset = 0;
            build_class_descriptor();
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
            return true;
        }
        default:
            return false;
    }

    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), static_cast<std::uint32_t>(trx->actual_length),
            std::move(transfer)));
    return true;
}

void UacAudioStreamingHandler::fill_pcm(std::uint8_t *dst, std::size_t n) {
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

void UacAudioStreamingHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                           std::uint32_t transfer_flags,
                                                           std::uint32_t transfer_buffer_length,
                                                           TransferHandle transfer, int num_iso_packets,
                                                           std::error_code &ec) {
    if (!streaming) {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());
    auto &data = trx->data;
    auto &iso_descs = trx->iso_descriptors;

    // 内核 usb_submit_urb 会把 iso_frame_desc[n].status 初始化为 -EXDEV，
    // 必须清零，否则内核音频驱动会跳过所有包。
    for (auto &iso: iso_descs)
        iso.status = 0;

    // 每个 URB 恰好携带 1ms 音频。
    // 高速等时每 microframe 一个包：每包实际字节数 = bytes_per_ms / 8（采样率为 8kHz 整数倍时整除）。
    // 端点 wMaxPacketSize 按最高采样率预留，低采样率下每包只填 packet_bytes，
    // 驱动按当前采样率的每包字节数取数据，填多了会造成样本错乱。
    std::size_t packet_bytes = bytes_per_ms / 8;
    std::size_t remaining = bytes_per_ms;
    std::uint32_t total_sent = 0;

    for (int i = 0; i < num_iso_packets && remaining > 0; ++i) {
        auto &iso = iso_descs[i];
        auto want = std::min({static_cast<std::size_t>(iso.length), packet_bytes, remaining});
        if (want == 0)
            continue;

        fill_pcm(&data[iso.offset], want);
        iso.actual_length = static_cast<std::uint32_t>(want);
        total_sent += iso.actual_length;
        remaining -= want;
    }

    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), total_sent, 0,
            static_cast<std::uint32_t>(iso_descs.size()), std::move(transfer)));
}

void UacAudioStreamingHandler::on_new_connection(Session &current_session, error_code &ec) {
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    streaming = false;
    chunk_data = nullptr;
    chunk_size = 0;
    chunk_offset = 0;
}

void UacAudioStreamingHandler::on_disconnection(error_code &ec) {
    streaming = false;
    chunk_data = nullptr;
    chunk_size = 0;
    chunk_offset = 0;
    VirtualInterfaceHandler::on_disconnection(ec);
}

void UacAudioStreamingHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting == 0) {
        streaming = false;
        *p_status = 0;
    }
    else if (alternate_setting == 1) {
        streaming = true;
        chunk_offset = 0; // 重新开始拉数据，保证流起点干净
        *p_status = 0;
    }
    else {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

std::uint8_t UacAudioStreamingHandler::request_get_interface(std::uint32_t *p_status) {
    return streaming ? 1 : 0;
}

void UacAudioStreamingHandler::request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = 0;
}
void UacAudioStreamingHandler::request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                            std::uint32_t *p_status) {
    *p_status = 0;
}
void UacAudioStreamingHandler::request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = 0;
}
void UacAudioStreamingHandler::request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                              std::uint32_t *p_status) {
    *p_status = 0;
}
std::uint16_t UacAudioStreamingHandler::request_get_status(std::uint32_t *p_status) {
    return 0;
}
std::uint16_t UacAudioStreamingHandler::request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) {
    return 0;
}

data_type UacAudioStreamingHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                            std::uint16_t descriptor_length, std::uint32_t *p_status) {
    if (type == CS_INTERFACE) {
        return class_desc;
    }
    return VirtualInterfaceHandler::request_get_descriptor(type, language_id, descriptor_length, p_status);
}

// ==================== UacDeviceHelper ====================

void UacDeviceHelper::setup(std::shared_ptr<UsbDevice> device, StringPool &string_pool,
                            std::unique_ptr<AudioSource> source, const UacDeviceConfig &config) {
    auto ac = std::make_shared<UacAudioControlHandler>(device->interfaces[0], string_pool);
    auto as = std::make_shared<UacAudioStreamingHandler>(device->interfaces[1], string_pool, std::move(source));

    device->interfaces[0].handler = ac;
    device->interfaces[1].handler = as;

    // 声道数：config 未指定（0）时从 source 推断
    auto resolved = config;
    if (resolved.channels == 0) {
        resolved.channels = as->get_source()->current_format().channels;
    }
    ac->set_config(resolved);
    as->set_ac_handler(ac.get());

    auto dh = device->handler ? std::dynamic_pointer_cast<VirtualDeviceHandler>(device->handler)
                              : device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    dh->setup_interface_handlers();
}

} // namespace usbipdcpp
