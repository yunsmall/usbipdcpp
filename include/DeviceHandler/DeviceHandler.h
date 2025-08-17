#pragma once

#include <vector>
#include <cstdint>
#include <optional>
#include <system_error>
#include <spdlog/spdlog.h>

#include "device.h"
#include "type.h"
#include "protocol.h"


namespace usbipdcpp {
    struct UsbEndpoint;
    struct UsbInterface;
    struct SetupPacket;

    class Session;

    class AbstDeviceHandler {
    public:
        explicit AbstDeviceHandler(UsbDevice &handle_device):
            handle_device(handle_device) {
        }

        AbstDeviceHandler(AbstDeviceHandler &&other) noexcept;

        virtual void dispatch_urb(
                Session &session,
                const UsbIpCommand::UsbIpCmdSubmit &cmd,
                std::uint32_t seqnum,
                const UsbEndpoint &ep,
                std::optional<UsbInterface> &interface,
                std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length, const SetupPacket &setup_packet,
                const data_type &out_data, const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
                usbipdcpp::error_code
                &ec
                );
        /**
         * @brief 完全终止传输
         */
        virtual void cancer_all_transfer() =0;
        /**
         * @brief 处理unlink。传入想要取消的序号。默认是空实现，就当全部发得太快了来不及取消了
         * @param seqnum 包序号
         */
        virtual void handle_unlink_seqnum(std::uint32_t seqnum) =0;

    protected:
        // 对于Out传输，transfer_buffer_length必须要等于out_data.size()
        // In传输out_data为空，transfer_buffer_length不是0
        // 因此函数内部请使用transfer_buffer_length获取buffer长度
        // 无论发生什么错误都请使用session提交一个返回包，不然session会视为当前urb未处理结束，除非发生无法恢复的错误
        virtual void handle_control_urb(
                Session &session,
                std::uint32_t seqnum,
                const UsbEndpoint &ep,
                std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length, const SetupPacket &setup_packet,
                const data_type &out_data, std::error_code &ec
                ) =0;
        // 对于Out传输，transfer_buffer_length必须要等于out_data.size()
        // In传输out_data为空，transfer_buffer_length不是0
        // 因此函数内部请使用transfer_buffer_length获取buffer长度
        // 无论发生什么错误都请使用session提交一个返回包，不然session会视为当前urb未处理结束，除非发生无法恢复的错误
        virtual void handle_bulk_transfer(
                Session &session,
                std::uint32_t seqnum,
                const UsbEndpoint &ep,
                UsbInterface &interface,
                std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                const data_type &out_data, std::error_code &ec
                ) =0;
        // 对于Out传输，transfer_buffer_length必须要等于out_data.size()
        // In传输out_data为空，transfer_buffer_length不是0
        // 因此函数内部请使用transfer_buffer_length获取buffer长度
        // 无论发生什么错误都请使用session提交一个返回包，不然session会视为当前urb未处理结束，除非发生无法恢复的错误
        virtual void handle_interrupt_transfer(
                Session &session,
                std::uint32_t seqnum,
                const UsbEndpoint &ep,
                UsbInterface &interface,
                std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                const data_type &out_data, std::error_code &ec
                ) =0;
        // 对于Out传输，transfer_buffer_length必须要等于out_data.size()
        // In传输out_data为空，transfer_buffer_length不是0
        // 因此函数内部请使用transfer_buffer_length获取buffer长度
        // 无论发生什么错误都请使用session提交一个返回包，不然session会视为当前urb未处理结束，除非发生无法恢复的错误
        virtual void handle_isochronous_transfer(
                Session &session,
                std::uint32_t seqnum,
                const UsbEndpoint &ep,
                UsbInterface &interface,
                std::uint32_t transfer_flags,
                std::uint32_t transfer_buffer_length, const data_type &out_data,
                const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors, std::error_code &ec
                ) =0;


        virtual ~AbstDeviceHandler() = default;

    protected:
        UsbDevice &handle_device;

    private:
        std::mutex self_mutex;
    };

    class DeviceHandlerBase : public AbstDeviceHandler {
    public:
        explicit DeviceHandlerBase(UsbDevice &handle_device) :
            AbstDeviceHandler(handle_device) {
        }
    };
}
