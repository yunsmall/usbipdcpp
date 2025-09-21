#pragma once

#include "InterfaceHandler/InterfaceHandler.h"

namespace usbipdcpp {

    class VirtualInterfaceHandler : public InterfaceHandlerBase {
    public:
        explicit VirtualInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
            InterfaceHandlerBase(handle_interface), string_pool(string_pool) {

            string_interface = string_pool.new_string(L"Usbipdcpp Virtual Interface");
        }

        void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                  std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                  const data_type &out_data,
                                  error_code &ec) override;
        void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                       std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                       const data_type &out_data,
                                       std::error_code &ec) override;
        void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                         std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                         const data_type &out_data,
                                         const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
                                         std::error_code &ec) override;

        virtual void handle_non_standard_request_type_control_urb(std::uint32_t seqnum,
                                                                  const UsbEndpoint &ep,
                                                                  std::uint32_t transfer_flags,
                                                                  std::uint32_t transfer_buffer_length,
                                                                  const SetupPacket &setup,
                                                                  const data_type &out_data, std::error_code &ec);
        virtual void handle_non_standard_request_type_control_urb_to_endpoint(std::uint32_t seqnum,
                                                                              const UsbEndpoint &ep,
                                                                              std::uint32_t transfer_flags,
                                                                              std::uint32_t transfer_buffer_length,
                                                                              const SetupPacket &setup,
                                                                              const data_type &out_data,
                                                                              std::error_code &ec);
        /**
         * @brief 新的客户端连接时会调这个函数
         * @param current_session
         * @param ec 发生的ec
         */
        void on_new_connection(Session &current_session, error_code &ec) override;
        /**
         * @brief 当发生错误、客户端detach、主动关闭服务器等情况需要完全终止传输时会调用这个函数。被调用后不可以再提交消息
         */
        void on_disconnection(error_code &ec) override;

        virtual void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) =0;
        virtual void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                    std::uint32_t *p_status) =0;

        virtual std::uint8_t request_get_interface(std::uint32_t *p_status) =0;
        virtual void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) =0;

        virtual std::uint16_t request_get_status(std::uint32_t *p_status) =0;
        virtual std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) =0;

        /**
         * @brief this function is not necessary for all device,
         * HID device is required to implement this function
         * @param type
         * @param language_id
         * @param descriptor_length
         * @param p_status
         * @return
         */
        virtual data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                 std::uint16_t descriptor_length, std::uint32_t *p_status);

        virtual void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) =0;
        virtual void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                  std::uint32_t *p_status) =0;

        /**
         * @brief Only use for isochronous transfer, so give a default empty implement.
         * @param ep_address
         * @param p_status
         * @return
         */
        virtual std::uint16_t request_endpoint_sync_frame(std::uint8_t ep_address, std::uint32_t *p_status) {
            return 0;
        }


        [[nodiscard]] virtual data_type get_class_specific_descriptor() =0;

        [[nodiscard]] virtual std::uint8_t get_string_interface_value() const {
            return string_interface;
        }

        [[nodiscard]] virtual std::wstring get_string_interface() const {
            return string_pool.get_string(string_interface).value_or(L"");
        }

    protected:
        std::atomic<Session *> session = nullptr;

        std::uint8_t string_interface;

        StringPool &string_pool;
    };

}
