#include "virtual_device/UvcVirtualInterfaceHandler.h"

#include <algorithm>
#include <cstring>

#include "Device.h"
#include "Session.h"
#include "protocol.h"
#include "spdlog/spdlog.h"
#include "virtual_device/SimpleVirtualDeviceHandler.h"

namespace usbipdcpp {

// ==================== UvcStreamingControl ====================

data_type UvcStreamingControl::serialize() const {
    data_type d(SIZE, 0);
    d[0] = static_cast<std::uint8_t>(bmHint & 0xFF);
    d[1] = static_cast<std::uint8_t>((bmHint >> 8) & 0xFF);
    d[2] = bFormatIndex;
    d[3] = bFrameIndex;
    d[4] = static_cast<std::uint8_t>(dwFrameInterval & 0xFF);
    d[5] = static_cast<std::uint8_t>((dwFrameInterval >> 8) & 0xFF);
    d[6] = static_cast<std::uint8_t>((dwFrameInterval >> 16) & 0xFF);
    d[7] = static_cast<std::uint8_t>((dwFrameInterval >> 24) & 0xFF);
    d[8] = static_cast<std::uint8_t>(wKeyFrameRate & 0xFF);
    d[9] = static_cast<std::uint8_t>((wKeyFrameRate >> 8) & 0xFF);
    d[10] = static_cast<std::uint8_t>(wPFrameRate & 0xFF);
    d[11] = static_cast<std::uint8_t>((wPFrameRate >> 8) & 0xFF);
    d[12] = static_cast<std::uint8_t>(wCompQuality & 0xFF);
    d[13] = static_cast<std::uint8_t>((wCompQuality >> 8) & 0xFF);
    d[14] = static_cast<std::uint8_t>(wCompWindowSize & 0xFF);
    d[15] = static_cast<std::uint8_t>((wCompWindowSize >> 8) & 0xFF);
    d[16] = static_cast<std::uint8_t>(wDelay & 0xFF);
    d[17] = static_cast<std::uint8_t>((wDelay >> 8) & 0xFF);
    d[18] = static_cast<std::uint8_t>(dwMaxVideoFrameSize & 0xFF);
    d[19] = static_cast<std::uint8_t>((dwMaxVideoFrameSize >> 8) & 0xFF);
    d[20] = static_cast<std::uint8_t>((dwMaxVideoFrameSize >> 16) & 0xFF);
    d[21] = static_cast<std::uint8_t>((dwMaxVideoFrameSize >> 24) & 0xFF);
    d[22] = static_cast<std::uint8_t>(dwMaxPayloadTransferSize & 0xFF);
    d[23] = static_cast<std::uint8_t>((dwMaxPayloadTransferSize >> 8) & 0xFF);
    d[24] = static_cast<std::uint8_t>((dwMaxPayloadTransferSize >> 16) & 0xFF);
    d[25] = static_cast<std::uint8_t>((dwMaxPayloadTransferSize >> 24) & 0xFF);
    // UVC 1.1 fields (offsets 26–33)
    d[26] = static_cast<std::uint8_t>(dwClockFrequency & 0xFF);
    d[27] = static_cast<std::uint8_t>((dwClockFrequency >> 8) & 0xFF);
    d[28] = static_cast<std::uint8_t>((dwClockFrequency >> 16) & 0xFF);
    d[29] = static_cast<std::uint8_t>((dwClockFrequency >> 24) & 0xFF);
    d[30] = bmFramingInfo;
    d[31] = bPreferredVersion;
    d[32] = bMinVersion;
    d[33] = bMaxVersion;
    // UVC 1.5 fields (offsets 34–47)
    d[34] = bUsage;
    d[35] = bBitDepthLuma;
    d[36] = bmSettings;
    d[37] = bMaxNumberOfRefFramesPlus1;
    d[38] = static_cast<std::uint8_t>(bmRateControlModes & 0xFF);
    d[39] = static_cast<std::uint8_t>((bmRateControlModes >> 8) & 0xFF);
    d[40] = static_cast<std::uint8_t>(bmLayoutPerStream & 0xFF);
    d[41] = static_cast<std::uint8_t>((bmLayoutPerStream >> 8) & 0xFF);
    d[42] = static_cast<std::uint8_t>((bmLayoutPerStream >> 16) & 0xFF);
    d[43] = static_cast<std::uint8_t>((bmLayoutPerStream >> 24) & 0xFF);
    d[44] = static_cast<std::uint8_t>((bmLayoutPerStream >> 32) & 0xFF);
    d[45] = static_cast<std::uint8_t>((bmLayoutPerStream >> 40) & 0xFF);
    d[46] = static_cast<std::uint8_t>((bmLayoutPerStream >> 48) & 0xFF);
    d[47] = static_cast<std::uint8_t>((bmLayoutPerStream >> 56) & 0xFF);
    return d;
}

void UvcStreamingControl::deserialize(const std::uint8_t *data, std::size_t len) {
    if (len < 26)
        return;
    bmHint = static_cast<std::uint16_t>(data[0] | (data[1] << 8));
    bFormatIndex = data[2];
    bFrameIndex = data[3];
    dwFrameInterval = static_cast<std::uint32_t>(data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24));
    wKeyFrameRate = static_cast<std::uint16_t>(data[8] | (data[9] << 8));
    wPFrameRate = static_cast<std::uint16_t>(data[10] | (data[11] << 8));
    wCompQuality = static_cast<std::uint16_t>(data[12] | (data[13] << 8));
    wCompWindowSize = static_cast<std::uint16_t>(data[14] | (data[15] << 8));
    wDelay = static_cast<std::uint16_t>(data[16] | (data[17] << 8));
    dwMaxVideoFrameSize = static_cast<std::uint32_t>(data[18] | (data[19] << 8) | (data[20] << 16) | (data[21] << 24));
    dwMaxPayloadTransferSize =
            static_cast<std::uint32_t>(data[22] | (data[23] << 8) | (data[24] << 16) | (data[25] << 24));
    if (len >= 34) {
        dwClockFrequency = static_cast<std::uint32_t>(data[26] | (data[27] << 8) | (data[28] << 16) | (data[29] << 24));
        bmFramingInfo = data[30];
        bPreferredVersion = data[31];
        bMinVersion = data[32];
        bMaxVersion = data[33];
    }
    if (len >= 48) { // UVC 1.5 extension fields
        bUsage = data[34];
        bBitDepthLuma = data[35];
        bmSettings = data[36];
        bMaxNumberOfRefFramesPlus1 = data[37];
        bmRateControlModes = static_cast<std::uint16_t>(data[38] | (data[39] << 8));
        bmLayoutPerStream =
                static_cast<std::uint64_t>(data[40]) | (static_cast<std::uint64_t>(data[41]) << 8) |
                (static_cast<std::uint64_t>(data[42]) << 16) | (static_cast<std::uint64_t>(data[43]) << 24) |
                (static_cast<std::uint64_t>(data[44]) << 32) | (static_cast<std::uint64_t>(data[45]) << 40) |
                (static_cast<std::uint64_t>(data[46]) << 48) | (static_cast<std::uint64_t>(data[47]) << 56);
    }
}

// ==================== UvcVideoControlHandler ====================

UvcVideoControlHandler::UvcVideoControlHandler(UsbInterface &handle_interface, StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
}

void UvcVideoControlHandler::on_setup_interface_handlers() {
    build_class_descriptor();
}

void UvcVideoControlHandler::build_class_descriptor() {
    if (desc_built_)
        return;
    desc_built_ = true;

    // VS interface is always at index 1 in UVC setup
    std::uint8_t vs_if_num = 1;

    // VC Header(13) + CameraTerminal(18) + PU(13) + OT(9) = 53
    // UVC 1.5: Processing Unit bControlSize=3 + bmVideoStandards(1)
    static constexpr std::uint8_t cam_term_len = 18; // 15 + bControlSize(3)
    static constexpr std::uint8_t pu_len = 13;       // 10 + bControlSize(3) + bmVideoStandards(1)
    data_type d;
    auto total_vc_size = VC_HEADER_1ITF_LEN + cam_term_len + pu_len + OUTPUT_TERM_LEN;

    d.insert(d.end(),
             {VC_HEADER_1ITF_LEN, 0x24, VC_DESC_HEADER, 0x50, 0x01, // bcdUVC 0x0150 (UVC 1.5)
              static_cast<std::uint8_t>(total_vc_size & 0xFF), static_cast<std::uint8_t>((total_vc_size >> 8) & 0xFF),
              0xC0, 0xFC, 0x9B, 0x01, // dwClockFrequency = 27000000 (27 MHz)
              0x01, // bInCollection
              vs_if_num});

    // Camera Terminal: 18 bytes, bControlSize=3
    d.insert(d.end(),
             {cam_term_len, 0x24, VC_DESC_INPUT_TERMINAL,
              0x01, // bTerminalID
              static_cast<std::uint8_t>(ITT_CAMERA & 0xFF), static_cast<std::uint8_t>((ITT_CAMERA >> 8) & 0xFF),
              0x00, // bAssocTerminal
              0x00, // iTerminal
              0x00, 0x00, // wObjectiveFocalLengthMin
              0x00, 0x00, // wObjectiveFocalLengthMax
              0x00, 0x00, // wOcularFocalLength
              0x03, // bControlSize
              0x00, 0x00, 0x00}); // bmControls[3]: no camera terminal controls

    // Processing Unit: 13 bytes, bControlSize=3 + bmVideoStandards (UVC 1.5 Table 3-8)
    d.insert(d.end(), {pu_len, 0x24, VC_DESC_PROCESSING_UNIT,
                       0x02, // bUnitID
                       0x01, // bSourceID → IT
                       0x00, 0x00, // wMaxMultiplier
                       0x03, // bControlSize = 3
                       0x1F, 0x02, 0x00, // bmControls[3]: Brightness|Contrast|Hue|Saturation|Sharpness|Gain
                       0x00, // iProcessing
                       0x00}); // bmVideoStandards: no analog video

    d.insert(d.end(),
             {OUTPUT_TERM_LEN, 0x24, VC_DESC_OUTPUT_TERMINAL,
              0x03, // bTerminalID
              static_cast<std::uint8_t>(TT_STREAMING & 0xFF), static_cast<std::uint8_t>((TT_STREAMING >> 8) & 0xFF),
              0x00, // bAssocTerminal
              0x02, // bSourceID → PU
              0x00}); // iTerminal

    class_desc_ = std::move(d);
}

data_type UvcVideoControlHandler::get_class_specific_descriptor() {
    return class_desc_;
}

void UvcVideoControlHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    if (type != RequestType::Class) {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto entity = setup_packet.index >> 8;
    auto control_selector = setup_packet.value >> 8;
    auto request = setup_packet.request;

    auto *trx = GenericTransfer::from_handle(transfer.get());

    // 处理类特定 GET_DESCRIPTOR（如 CS_INTERFACE=0x24）
    if (request == static_cast<std::uint8_t>(StandardRequest::GetDescriptor)) {
        auto desc_type = setup_packet.value >> 8;
        if (desc_type == CS_INTERFACE) {
            auto resp = class_desc_;
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

    // ===== VC 接口级控制（entity=0）=====
    if (entity == ENTITY_VC_INTERFACE) {
        if (control_selector == VC_VIDEO_POWER_MODE_CONTROL) {
            switch (request) {
                case GET_CUR:
                    trx->data = {static_cast<std::uint8_t>(power_on_ ? 1 : 0)};
                    trx->actual_length = 1;
                    break;
                case GET_INFO:
                    trx->data = {0x03}; // GET | SET
                    trx->actual_length = 1;
                    break;
                case SET_CUR:
                    if (!trx->data.empty()) {
                        power_on_ = (trx->data[0] != 0);
                        if (!power_on_ && vs_handler_)
                            vs_handler_->notify_stop_streaming();
                    }
                    trx->actual_length = 0;
                    break;
                default:
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
            }
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }
        if (control_selector == VC_REQUEST_ERROR_CODE_CONTROL) {
            switch (request) {
                case GET_CUR:
                    trx->data = {0x00}; // no error
                    trx->actual_length = 1;
                    break;
                case GET_INFO:
                    trx->data = {0x01}; // GET only
                    trx->actual_length = 1;
                    break;
                default:
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
            }
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // entity != 0: TinyUSB 对描述符中存在的 entity 直接返回成功（ACK，无数据）
    // IT=0x01, OT=0x03 无实际控制逻辑，但需要 ACK 防止 Windows STALL
    if (entity == ENTITY_INPUT_TERMINAL || entity == ENTITY_OUTPUT_TERMINAL) {
        trx->actual_length = 0;
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), 0, std::move(transfer)));
        return;
    }

    // ===== Processing Unit（entity=2）=====
    if (entity != ENTITY_PROCESSING_UNIT) {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    struct PuControl {
        std::uint8_t selector;
        std::int16_t cur, min, max, def, step;
    };
    static const PuControl controls[] = {
            {PU_BRIGHTNESS, 128, 0, 255, 128, 1}, {PU_CONTRAST, 128, 0, 255, 128, 1}, {PU_HUE, 0, -180, 180, 0, 1},
            {PU_SATURATION, 128, 0, 255, 128, 1}, {PU_SHARPNESS, 2, 0, 15, 2, 1},     {PU_GAIN, 64, 0, 255, 64, 1},
    };

    const PuControl *ctrl = nullptr;
    for (auto &c: controls) {
        if (c.selector == control_selector) {
            ctrl = &c;
            break;
        }
    }

    std::int16_t val = 0;

    switch (request) {
        case GET_CUR:
            val = ctrl ? ctrl->cur : 0;
            goto respond_16;
        case GET_MIN:
            val = ctrl ? ctrl->min : 0;
            goto respond_16;
        case GET_MAX:
            val = ctrl ? ctrl->max : 0;
            goto respond_16;
        case GET_DEF:
            val = ctrl ? ctrl->def : 0;
            goto respond_16;
        case GET_RES:
            val = ctrl ? ctrl->step : 1;
            goto respond_16;
        respond_16:
            trx->data = {static_cast<std::uint8_t>(val & 0xFF), static_cast<std::uint8_t>((val >> 8) & 0xFF)};
            trx->actual_length = 2;
            break;
        case GET_LEN:
            trx->data = {0x02, 0x00};
            trx->actual_length = 2;
            break;
        case GET_INFO:
            trx->data = {static_cast<std::uint8_t>(ctrl ? 0x03 : 0x00)};
            trx->actual_length = 1;
            break;
        case SET_CUR:
            trx->actual_length = 0;
            break;
        default:
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
    }

    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), static_cast<std::uint32_t>(trx->actual_length),
            std::move(transfer)));
}

void UvcVideoControlHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                       std::uint32_t transfer_flags,
                                                       std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                       std::error_code &ec) {
    if (ep.is_in()) {
        std::lock(status_mutex_, endpoint_requests_mutex_);
        std::lock_guard lock1(status_mutex_, std::adopt_lock);
        std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);

        if (endpoint_requests_.empty(ep.address) && !pending_status_.empty()) {
            auto status = std::move(pending_status_.front());
            pending_status_.pop_front();
            auto *trx = GenericTransfer::from_handle(transfer.get());
            auto send_len = std::min(status.size(), static_cast<std::size_t>(transfer_buffer_length));
            trx->data.assign(status.begin(), status.begin() + send_len);
            trx->actual_length = send_len;
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                    seqnum, static_cast<std::uint32_t>(send_len), std::move(transfer)));
        }
        else {
            endpoint_requests_.enqueue(ep.address, {seqnum, transfer_buffer_length, std::move(transfer)});
        }
    }
    else {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void UvcVideoControlHandler::send_vc_status(data_type status) {
    std::lock(status_mutex_, endpoint_requests_mutex_);
    std::lock_guard lock1(status_mutex_, std::adopt_lock);
    std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);

    auto req_opt = endpoint_requests_.dequeue_any();
    if (req_opt.has_value()) {
        auto &[ep_addr, req] = req_opt.value();
        auto *trx = GenericTransfer::from_handle(req.transfer.get());
        auto send_len = std::min(status.size(), static_cast<std::size_t>(req.length));
        trx->data.assign(status.begin(), status.begin() + send_len);
        trx->actual_length = send_len;
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                req.seqnum, static_cast<std::uint32_t>(send_len), std::move(req.transfer)));
    }
    else {
        pending_status_.push_back(std::move(status));
    }
}

void UvcVideoControlHandler::on_disconnection(std::error_code &ec) {
    {
        std::lock_guard lock(status_mutex_);
        pending_status_.clear();
    }
    {
        std::lock_guard lock(endpoint_requests_mutex_);
        endpoint_requests_.clear();
    }
    VirtualInterfaceHandler::on_disconnection(ec);
}

void UvcVideoControlHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    std::lock_guard lock(endpoint_requests_mutex_);
    endpoint_requests_.cancel_by_seqnum(unlink_seqnum);
    session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(cmd_seqnum));
}

void UvcVideoControlHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    *p_status = (alternate_setting == 0) ? 0 : static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}
std::uint8_t UvcVideoControlHandler::request_get_interface(std::uint32_t *p_status) {
    return 0;
}
void UvcVideoControlHandler::request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = 0;
}
void UvcVideoControlHandler::request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                          std::uint32_t *p_status) {
    *p_status = 0;
}
void UvcVideoControlHandler::request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = 0;
}
void UvcVideoControlHandler::request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                            std::uint32_t *p_status) {
    *p_status = 0;
}
std::uint16_t UvcVideoControlHandler::request_get_status(std::uint32_t *p_status) {
    return 0;
}
std::uint16_t UvcVideoControlHandler::request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) {
    return 0;
}

data_type UvcVideoControlHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                          std::uint16_t descriptor_length, std::uint32_t *p_status) {
    if (type == CS_INTERFACE) {
        return class_desc_;
    }
    return VirtualInterfaceHandler::request_get_descriptor(type, language_id, descriptor_length, p_status);
}

// ==================== UvcVideoStreamingHandler ====================

UvcVideoStreamingHandler::UvcVideoStreamingHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                                   std::unique_ptr<VideoSource> source) :
    VirtualInterfaceHandler(handle_interface, string_pool), source_(std::move(source)) {
    probe_data_.dwMaxVideoFrameSize = static_cast<std::uint32_t>(source_->max_frame_size());
    probe_data_.dwMaxPayloadTransferSize = 512; // wMaxPacketSize of ISO endpoint
    probe_data_.dwFrameInterval = source_->frame_interval();
}

void UvcVideoStreamingHandler::on_setup_interface_handlers() {
    build_class_descriptor();
}

void UvcVideoStreamingHandler::build_class_descriptor() {
    auto fmt = source_->current_format();

    std::uint8_t ep_addr = 0x81;
    // 从 streaming alt (alt 1) 获取端点地址
    if (handle_interface.endpoints.size() > 1 && !handle_interface.endpoints[1].empty())
        ep_addr = handle_interface.endpoints[1][0].address;
    else if (!handle_interface.endpoints.empty() && !handle_interface.endpoints[0].empty())
        ep_addr = handle_interface.endpoints[0][0].address;

    bool is_mjpeg = (fmt.fourcc == UvcFourCC::MJPEG);
    std::uint8_t format_subtype = is_mjpeg ? VS_DESC_FORMAT_MJPEG : VS_DESC_FORMAT_UNCOMPRESSED;
    std::uint8_t frame_subtype = is_mjpeg ? VS_DESC_FRAME_MJPEG : VS_DESC_FRAME_UNCOMPRESSED;
    auto guid = UvcGuid::from_fourcc(fmt.fourcc);

    // Format descriptor: 27 bytes
    data_type format(27, 0);
    format[0] = 27;
    format[1] = 0x24;
    format[2] = format_subtype;
    format[3] = 0x01; // bFormatIndex
    format[4] = 0x01; // bNumFrameDescriptors

    if (is_mjpeg) {
        format[5] = 0x01; // bmFlags
        std::memcpy(&format[6], guid.data, 16);
        format[22] = 0x01; // bDefaultFrameIndex
    }
    else {
        std::memcpy(&format[5], guid.data, 16);
        format[21] = static_cast<std::uint8_t>(fmt.bits_per_pixel);
        format[22] = 0x01; // bDefaultFrameIndex
    }
    format[23] = 0x00; // bAspectRatioX: 0 for non-interlaced
    format[24] = 0x00; // bAspectRatioY: 0 for non-interlaced
    // [25] bmInterlaceFlags = 0
    // [26] bCopyProtect = 0

    // Frame descriptor: continuous (bFrameIntervalType=0) — 26 fixed + 3*4 intervals = 38 bytes
    auto fps_val = 10'000'000ULL / fmt.default_frame_interval;
    auto bit_rate = static_cast<std::uint32_t>(fmt.max_frame_size * 8 * fps_val);
    auto frame_interval = static_cast<std::uint32_t>(fmt.default_frame_interval);

    data_type frame(VS_FRM_UNCOMPR_CONT_LEN, 0);
    frame[0] = VS_FRM_UNCOMPR_CONT_LEN;
    frame[1] = 0x24;
    frame[2] = frame_subtype;
    frame[3] = 0x01; // bFrameIndex
    frame[4] = 0x00; // bmCapabilities
    frame[5] = static_cast<std::uint8_t>(fmt.width & 0xFF);
    frame[6] = static_cast<std::uint8_t>((fmt.width >> 8) & 0xFF);
    frame[7] = static_cast<std::uint8_t>(fmt.height & 0xFF);
    frame[8] = static_cast<std::uint8_t>((fmt.height >> 8) & 0xFF);
    for (int i = 0; i < 4; ++i) {
        frame[9 + i] = static_cast<std::uint8_t>((bit_rate >> (i * 8)) & 0xFF);
        frame[13 + i] = static_cast<std::uint8_t>((bit_rate >> (i * 8)) & 0xFF);
    }
    frame[17] = static_cast<std::uint8_t>(fmt.max_frame_size & 0xFF);
    frame[18] = static_cast<std::uint8_t>((fmt.max_frame_size >> 8) & 0xFF);
    frame[19] = static_cast<std::uint8_t>((fmt.max_frame_size >> 16) & 0xFF);
    frame[20] = static_cast<std::uint8_t>((fmt.max_frame_size >> 24) & 0xFF);
    frame[21] = static_cast<std::uint8_t>(frame_interval & 0xFF);
    frame[22] = static_cast<std::uint8_t>((frame_interval >> 8) & 0xFF);
    frame[23] = static_cast<std::uint8_t>((frame_interval >> 16) & 0xFF);
    frame[24] = static_cast<std::uint8_t>((frame_interval >> 24) & 0xFF);
    frame[25] = 0x00; // bFrameIntervalType = 0 (continuous)
    // continuous: dwFrameInterval[0]=min, [1]=max, [2]=step
    //
    // usbvideo.sys!DumpAndValidateFrameUncompressed 对连续帧间隔有三项整除检查：
    //   if (step != 0) {
    //       if (max <= min)                → STATUS_INVALID_PARAMETER
    //       if ((max - min) % step != 0)   → STATUS_INVALID_PARAMETER
    //   }
    // 因此 max 必须是 min + N*step（N为正整数），否则 Windows 直接 Code 10。
    // 这里取 N=9，即最慢帧率为默认的 1/10。
    auto max_frame_interval = frame_interval * 10;
    for (int i = 0; i < 4; ++i)
        frame[26 + i] = static_cast<std::uint8_t>((frame_interval >> (i * 8)) & 0xFF); // min
    for (int i = 0; i < 4; ++i)
        frame[30 + i] = static_cast<std::uint8_t>((max_frame_interval >> (i * 8)) & 0xFF); // max
    for (int i = 0; i < 4; ++i)
        frame[34 + i] = static_cast<std::uint8_t>((frame_interval >> (i * 8)) & 0xFF); // step

    // Color Matching descriptor: 6 bytes
    data_type color = {VS_COLOR_MATCHING_LEN,     0x24,
                       VS_DESC_COLORFORMAT,       VIDEO_COLOR_PRIMARIES_BT709,
                       VIDEO_COLOR_XFER_CH_BT709, VIDEO_COLOR_COEF_SMPTE170M};

    // VS Input Header: 13 + bControlSize(1) = 14
    static constexpr std::uint8_t bControlSize = 1;
    auto total_vs_len = VS_INPUT_HEADER_LEN + VS_FMT_UNCOMPR_LEN + VS_FRM_UNCOMPR_CONT_LEN + VS_COLOR_MATCHING_LEN;

    data_type header(VS_INPUT_HEADER_LEN, 0);
    header[0] = VS_INPUT_HEADER_LEN;
    header[1] = 0x24;
    header[2] = VS_DESC_INPUT_HEADER;
    header[3] = 0x01; // bNumFormats
    header[4] = static_cast<std::uint8_t>(total_vs_len & 0xFF);
    header[5] = static_cast<std::uint8_t>((total_vs_len >> 8) & 0xFF);
    header[6] = ep_addr;
    header[7] = 0x00; // bmInfo
    header[8] = 0x03; // bTerminalLink → Output Terminal ID 3
    header[9] = 0x00; // bStillCaptureMethod
    header[10] = 0x00; // bTriggerSupport
    header[11] = 0x00; // bTriggerUsage
    header[12] = bControlSize;

    data_type d;
    d.insert(d.end(), header.begin(), header.end());
    d.insert(d.end(), format.begin(), format.end());
    d.insert(d.end(), frame.begin(), frame.end());
    d.insert(d.end(), color.begin(), color.end());

    class_desc_ = std::move(d);
}

data_type UvcVideoStreamingHandler::get_class_specific_descriptor() {
    return class_desc_;
}

void UvcVideoStreamingHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    auto ctrl_code = setup_packet.value >> 8;

    if (type != RequestType::Class) {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto request = setup_packet.request;
    auto *trx = GenericTransfer::from_handle(transfer.get());

    // 处理类特定 GET_DESCRIPTOR（如 CS_INTERFACE=0x24）
    if (request == static_cast<std::uint8_t>(StandardRequest::GetDescriptor)) {
        auto desc_type = setup_packet.value >> 8;
        if (desc_type == CS_INTERFACE) {
            auto resp = class_desc_;
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

    if (!(ctrl_code == VS_PROBE_CONTROL || ctrl_code == VS_COMMIT_CONTROL ||
          ctrl_code == VS_STREAM_ERROR_CODE_CONTROL)) {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    // Stream Error Code Control
    if (ctrl_code == VS_STREAM_ERROR_CODE_CONTROL) {
        switch (request) {
            case GET_CUR:
                trx->data = {0x00}; // no error
                trx->actual_length = 1;
                break;
            case GET_INFO:
                trx->data = {0x01}; // GET only
                trx->actual_length = 1;
                break;
            default:
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                return;
        }
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
        return;
    }

    bool is_commit = (ctrl_code == VS_COMMIT_CONTROL);

    if (request == GET_CUR || request == GET_MIN || request == GET_MAX || request == GET_DEF) {
        auto fmt = source_->current_format();
        auto max_frame_size = static_cast<std::uint32_t>(source_->max_frame_size());
        auto ctrl = probe_data_;
        // 设备能力字段 — 内核可能发来全 0 的 SET_CUR，必须每次都设对
        ctrl.bmFramingInfo = 0x03;     // 支持动态帧率 + 帧 ID
        ctrl.bPreferredVersion = 1;
        ctrl.bMinVersion = 1;
        ctrl.bMaxVersion = 1;
        ctrl.dwClockFrequency = 27000000;
        auto interval = fmt.default_frame_interval;
        if (request == GET_MIN) {
            ctrl.dwMaxVideoFrameSize = 0;
            ctrl.dwMaxPayloadTransferSize = 0;
            ctrl.dwFrameInterval = interval * 4; // 最低帧率 = 1/4 默认
        } else if (request == GET_MAX) {
            ctrl.dwMaxVideoFrameSize = max_frame_size;
            ctrl.dwMaxPayloadTransferSize = max_frame_size;
            ctrl.dwFrameInterval = interval / 4; // 最高帧率 = 4x 默认
        } else if (request == GET_DEF) {
            ctrl.bFormatIndex = 1;
            ctrl.bFrameIndex = 1;
            ctrl.dwFrameInterval = interval;
            ctrl.dwMaxVideoFrameSize = max_frame_size;
            ctrl.dwMaxPayloadTransferSize = max_frame_size;
        } else {
            // GET_CUR: 用上次协商的值，未协商过时用默认值
            if (ctrl.dwMaxVideoFrameSize == 0)
                ctrl.dwMaxVideoFrameSize = max_frame_size;
            if (ctrl.dwMaxPayloadTransferSize == 0)
                ctrl.dwMaxPayloadTransferSize = max_frame_size;
            if (ctrl.dwFrameInterval == 0)
                ctrl.dwFrameInterval = interval;
        }
        auto resp = ctrl.serialize();
        auto act_len = std::min(resp.size(), static_cast<std::size_t>(transfer_buffer_length));
        trx->data.assign(resp.begin(), resp.begin() + act_len);
        trx->actual_length = act_len;
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
    }
    else if (request == GET_LEN) {
        std::uint16_t len = UvcStreamingControl::SIZE;
        trx->data = {static_cast<std::uint8_t>(len & 0xFF), static_cast<std::uint8_t>((len >> 8) & 0xFF)};
        trx->actual_length = 2;
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
    }
    else if (request == GET_INFO) {
        trx->data = {0x03}; // GET | SET
        trx->actual_length = 1;
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
    }
    else if (request == SET_CUR && !is_commit) {
        if (trx->data.size() >= 26) {
            probe_data_.deserialize(trx->data.data(), trx->data.size());
            probe_data_.bmFramingInfo = 0x03;
            probe_data_.bPreferredVersion = 1;
            probe_data_.bMinVersion = 1;
            probe_data_.bMaxVersion = 1;
            probe_data_.dwClockFrequency = 27000000;
            // UVC 1.5 fields: device capabilities
            probe_data_.bUsage = 0;
            probe_data_.bBitDepthLuma = 0;
            probe_data_.bmSettings = 0;
            probe_data_.bMaxNumberOfRefFramesPlus1 = 0;
            probe_data_.bmRateControlModes = 0;
            probe_data_.bmLayoutPerStream = 0;
        }
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
    }
    else if (request == SET_CUR && is_commit) {
        if (trx->data.size() >= 26) {
            probe_data_.deserialize(trx->data.data(), trx->data.size());
            probe_data_.bmFramingInfo = 0x03;
            probe_data_.bPreferredVersion = 1;
            probe_data_.bMinVersion = 1;
            probe_data_.bMaxVersion = 1;
            probe_data_.dwClockFrequency = 27000000;
            probe_data_.bUsage = 0;
            probe_data_.bBitDepthLuma = 0;
            probe_data_.bmSettings = 0;
            probe_data_.bMaxNumberOfRefFramesPlus1 = 0;
            probe_data_.bmRateControlModes = 0;
            probe_data_.bmLayoutPerStream = 0;
        }
        auto fmt = source_->current_format();
        source_->set_format(fmt.fourcc, fmt.width, fmt.height, probe_data_.dwFrameInterval);
        committed_ = true;
        frame_buffer_.resize(source_->max_frame_size());
        frame_offset_ = 0;
        current_fid_ = false;
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
    }
    else {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void UvcVideoStreamingHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                           std::uint32_t transfer_flags,
                                                           std::uint32_t transfer_buffer_length,
                                                           TransferHandle transfer, int num_iso_packets,
                                                           std::error_code &ec) {
    if (!streaming_ || !committed_) {
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        return;
    }

    auto *trx = GenericTransfer::from_handle(transfer.get());
    auto &data = trx->data;
    auto &iso_descs = trx->iso_descriptors;

    if (frame_offset_ == 0) {
        VideoFrame vf{};
        if (!source_->get_frame(vf)) {
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
        }
        frame_buffer_.assign(vf.data, vf.data + vf.size);
        current_fid_ = !current_fid_;
    }

    std::uint32_t total_sent = 0;
    std::size_t frame_remaining = frame_buffer_.size() - frame_offset_;

    // 内核 usb_submit_urb 会把 iso_frame_desc[n].status 初始化为 -EXDEV，
    // 我们必须清零，否则内核 UVC 驱动会跳过所有包。
    for (auto &iso: iso_descs)
        iso.status = 0;

    for (int i = 0; i < num_iso_packets && frame_remaining > 0; ++i) {
        auto &iso = iso_descs[i];
        if (iso.length < UVC_PAYLOAD_HEADER_SIZE) {
            iso.actual_length = 0;
            continue;
        }

        auto chunk = std::min(static_cast<std::size_t>(iso.length - UVC_PAYLOAD_HEADER_SIZE), frame_remaining);

        std::uint8_t header_info = current_fid_ ? UVC_PAYLOAD_HEADER_FID : 0;
        if (frame_offset_ + chunk >= frame_buffer_.size())
            header_info |= 0x02; // EOF

        auto *dst = &data[iso.offset];
        dst[0] = UVC_PAYLOAD_HEADER_SIZE;
        dst[1] = header_info;
        std::memcpy(dst + UVC_PAYLOAD_HEADER_SIZE, frame_buffer_.data() + frame_offset_, chunk);

        iso.actual_length = static_cast<std::uint32_t>(UVC_PAYLOAD_HEADER_SIZE + chunk);
        total_sent += iso.actual_length;
        frame_offset_ += chunk;
        frame_remaining -= chunk;
    }

    if (frame_offset_ >= frame_buffer_.size())
        frame_offset_ = 0;

    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), total_sent, 0,
            static_cast<std::uint32_t>(iso_descs.size()), std::move(transfer)));
}

void UvcVideoStreamingHandler::on_new_connection(Session &current_session, error_code &ec) {
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    committed_ = false;
    streaming_ = false;
    frame_offset_ = 0;
    current_fid_ = false;
}

void UvcVideoStreamingHandler::on_disconnection(error_code &ec) {
    streaming_ = false;
    committed_ = false;
    VirtualInterfaceHandler::on_disconnection(ec);
}

void UvcVideoStreamingHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting == 0) {
        streaming_ = false;
        *p_status = 0;
    }
    else if (alternate_setting == 1) {
        if (committed_) {
            streaming_ = true;
            frame_offset_ = 0;
            current_fid_ = false;
        }
        *p_status = 0;
    }
    else {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

std::uint8_t UvcVideoStreamingHandler::request_get_interface(std::uint32_t *p_status) {
    return streaming_ ? 1 : 0;
}

void UvcVideoStreamingHandler::request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = 0;
}
void UvcVideoStreamingHandler::request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                            std::uint32_t *p_status) {
    *p_status = 0;
}
void UvcVideoStreamingHandler::request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = 0;
}
void UvcVideoStreamingHandler::request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                              std::uint32_t *p_status) {
    *p_status = 0;
}
std::uint16_t UvcVideoStreamingHandler::request_get_status(std::uint32_t *p_status) {
    return 0;
}
std::uint16_t UvcVideoStreamingHandler::request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) {
    return 0;
}

data_type UvcVideoStreamingHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                            std::uint16_t descriptor_length, std::uint32_t *p_status) {
    if (type == CS_INTERFACE) {
        return class_desc_;
    }
    return VirtualInterfaceHandler::request_get_descriptor(type, language_id, descriptor_length, p_status);
}

// ==================== UvcDeviceHelper ====================

void UvcDeviceHelper::setup(std::shared_ptr<UsbDevice> device, StringPool &string_pool,
                            std::unique_ptr<VideoSource> source) {
    auto vc = std::make_shared<UvcVideoControlHandler>(device->interfaces[0], string_pool);
    auto vs = std::make_shared<UvcVideoStreamingHandler>(device->interfaces[1], string_pool, std::move(source));

    device->interfaces[0].handler = vc;
    device->interfaces[1].handler = vs;

    vc->set_vs_handler(vs.get());
    vs->set_vc_handler(vc.get());

    // VC/VS 用相同 iInterface：USBCCGP 会从子 PDO 配置描述符中移除 IAD，
    // usbvideo.sys 依靠相同 iInterface 将 VC 和 VS 识别为同一功能
    vs->sync_string_interface_from(*vc);

    auto dh = device->handler ? std::dynamic_pointer_cast<VirtualDeviceHandler>(device->handler)
                              : device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    dh->setup_interface_handlers();
}

} // namespace usbipdcpp
