#pragma once

#include "type.h"
#include "StringPool.h"

namespace usbipdcpp {
    struct UsbIpIsoPacketDescriptor;
    struct UsbEndpoint;
    struct SetupPacket;
    struct UsbInterface;

    class Session;


    /**
     * @brief 继承 InterfaceHandlerBase 类，不要继承这个类
     */
    class AbstInterfaceHandler {
    public:
        explicit AbstInterfaceHandler(UsbInterface &handle_interface):
            handle_interface(handle_interface) {
        }

        virtual void handle_bulk_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                          std::uint32_t transfer_flags,
                                          std::uint32_t transfer_buffer_length, const data_type &out_data,
                                          std::error_code &ec) =0;
        virtual void handle_interrupt_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                               std::uint32_t transfer_flags,
                                               std::uint32_t transfer_buffer_length, const data_type &out_data,
                                               std::error_code &ec) =0;

        virtual void handle_isochronous_transfer(Session &session, std::uint32_t seqnum,
                                                 const UsbEndpoint &ep,
                                                 std::uint32_t transfer_flags,
                                                 std::uint32_t transfer_buffer_length, const data_type &out_data,
                                                 const std::vector<UsbIpIsoPacketDescriptor> &
                                                 iso_packet_descriptors, std::error_code &ec) =0;

        /**
         * @brief 新的客户端连接时会调这个函数
         * @param ec 发生的ec
         */
        virtual void on_new_connection(error_code &ec) =0;

        /**
         * @brief 当发生错误、客户端detach、主动关闭服务器等情况需要完全终止传输时会调用这个函数。被调用后不可以再提交消息
         */
        virtual void on_disconnection(error_code &ec) =0;

        virtual ~AbstInterfaceHandler() = default;

    protected:
        UsbInterface &handle_interface;
    };

    class InterfaceHandlerBase : public AbstInterfaceHandler {
    public:
        explicit InterfaceHandlerBase(UsbInterface &handle_interface) :
            AbstInterfaceHandler(handle_interface) {
        }
    };

}
