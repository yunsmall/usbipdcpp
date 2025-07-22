#pragma once

#include "StringPool.h"
#include "DeviceHandler.h"

namespace usbipdcpp {

    class VirtualDeviceHandler : public DeviceHandlerBase {
    public:
        explicit VirtualDeviceHandler(UsbDevice &handle_device, StringPool &string_pool,
                                      const Version &usb_version = {2, 0, 0}) :
            DeviceHandlerBase(handle_device), string_pool(string_pool), usb_version(usb_version) {
            change_device_ep0_max_size_by_speed();

            string_configuration_value = string_pool.new_string(L"Default Configuration");
            string_manufacturer_value = string_pool.new_string(L"Usbipcpp");
            string_product_value = string_pool.new_string(L"Usbipcpp Virtual Device");
            string_serial_value = string_pool.new_string(L"Usbipcpp Serial");
        }

        void dispatch_urb(Session &session, const UsbIpCommand::UsbIpCmdSubmit &cmd, std::uint32_t seqnum,
                          const UsbEndpoint &ep, std::optional<UsbInterface> &interface, std::uint32_t transfer_flags,
                          std::uint32_t transfer_buffer_length, const SetupPacket &setup_packet,
                          const data_type &out_data,
                          const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
                          usbipdcpp::error_code &ec) override;


        void handle_unlink_seqnum(std::uint32_t seqnum) override;
        void stop_transfer() override;

    protected:
        void change_device_ep0_max_size_by_speed();

        void handle_control_urb(Session &session,
                                std::uint32_t seqnum, const UsbEndpoint &ep,
                                std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                const SetupPacket &setup_packet, const data_type &out_data,
                                std::error_code &ec) override;
        void handle_bulk_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                  UsbInterface &interface, std::uint32_t transfer_flags,
                                  std::uint32_t transfer_buffer_length, const data_type &out_data,
                                  std::error_code &ec) override;
        void handle_interrupt_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                       UsbInterface &interface, std::uint32_t transfer_flags,
                                       std::uint32_t transfer_buffer_length, const data_type &out_data,
                                       std::error_code &ec) override;

        void handle_isochronous_transfer(Session &session, std::uint32_t seqnum,
                                         const UsbEndpoint &ep, UsbInterface &interface,
                                         std::uint32_t transfer_flags,
                                         std::uint32_t transfer_buffer_length, const data_type &out_data,
                                         const std::vector<UsbIpIsoPacketDescriptor> &
                                         iso_packet_descriptors, std::error_code &ec) override;

        virtual void handle_non_standard_request_type_control_urb(Session &session,
                                                                  std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                  std::uint32_t transfer_flags,
                                                                  std::uint32_t transfer_buffer_length,
                                                                  const SetupPacket &setup_packet,
                                                                  const data_type &out_data,
                                                                  std::error_code &ec) =0;
        virtual void handle_non_standard_control_urb_to_endpoint(Session &session,
                                                                 std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                 std::uint32_t transfer_flags,
                                                                 std::uint32_t transfer_buffer_length,
                                                                 const SetupPacket &setup_packet,
                                                                 const data_type &out_data,
                                                                 std::error_code &ec) =0;

        virtual void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) =0;

        virtual std::uint8_t request_get_configuration(std::uint32_t *p_status);

        virtual std::uint16_t request_get_status(std::uint32_t *p_status) =0;
        virtual void request_set_address(std::uint16_t address, std::uint32_t *status) =0;
        virtual void request_set_configuration(std::uint16_t configuration_value, std::uint32_t *p_status) =0;
        virtual void request_set_descriptor(std::uint8_t desc_type, std::uint8_t desc_index,
                                            std::uint16_t language_id, std::uint16_t descriptor_length,
                                            const data_type &descriptor, std::uint32_t *p_status) =0;

        virtual void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) =0;

        data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                         std::uint32_t *p_status);
        std::uint8_t request_get_interface(std::uint16_t intf, std::uint32_t *p_status);
        void request_set_interface(std::uint16_t alternate_setting, std::uint16_t intf, std::uint32_t *p_status);


        virtual data_type get_device_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length, std::uint32_t* p_status);
        virtual data_type get_bos_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length, std::uint32_t* p_status);
        virtual data_type get_configuration_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length, std::uint32_t* p_status);
        virtual data_type get_string_descriptor(std::uint8_t language_id, std::uint16_t descriptor_length, std::uint32_t* p_status);
        virtual data_type get_device_qualifier_descriptor(std::uint8_t language_id, std::uint16_t descriptor_length, std::uint32_t* p_status);
        virtual data_type get_other_speed_descriptor(std::uint8_t language_id, std::uint16_t descriptor_length, std::uint32_t* p_status) =0;

        /**
         * @brief If host wants a descriptor which is not in enum DescriptorType, then this function will be called
         * @param type descriptor tye
         * @param language_id which language
         * @param descriptor_length descriptor_length
         * @param p_status
         * @return descriptor
         */
        virtual data_type get_custom_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                std::uint16_t descriptor_length, std::uint32_t* p_status);

        virtual void set_descriptor(std::uint16_t configuration_value) =0;

        void change_string_configuration(const std::wstring &new_str) {
            auto ret = string_pool.get_string(string_configuration_value);
            if (ret) {
                string_pool.remove_string(string_configuration_value);
                string_configuration_value = string_pool.new_string(new_str);
                return;
            }
            SPDLOG_CRITICAL("string_configuration_value无效");
            throw std::system_error(std::make_error_code(std::errc::invalid_argument));
        }

        void change_string_manufacturer(const std::wstring &new_str) {
            auto ret = string_pool.get_string(string_manufacturer_value);
            if (ret) {
                string_pool.remove_string(string_manufacturer_value);
                string_manufacturer_value = string_pool.new_string(new_str);
                return;
            }
            SPDLOG_CRITICAL("string_manufacturer_value无效");
            throw std::system_error(std::make_error_code(std::errc::invalid_argument));
        }

        void change_string_product(const std::wstring &new_str) {
            auto ret = string_pool.get_string(string_product_value);
            if (ret) {
                string_pool.remove_string(string_product_value);
                string_product_value = string_pool.new_string(new_str);
                return;
            }
            SPDLOG_CRITICAL("string_product_value无效");
            throw std::system_error(std::make_error_code(std::errc::invalid_argument));
        }

        void change_string_serial(const std::wstring &new_str) {
            auto ret = string_pool.get_string(string_serial_value);
            if (ret) {
                string_pool.remove_string(string_serial_value);
                string_serial_value = string_pool.new_string(new_str);
                return;
            }
            SPDLOG_CRITICAL("string_serial_value无效");
            throw std::system_error(std::make_error_code(std::errc::invalid_argument));
        }

    protected:
        std::uint8_t string_configuration_value;
        std::uint8_t string_manufacturer_value;
        std::uint8_t string_product_value;
        std::uint8_t string_serial_value;

        StringPool &string_pool;

        Version usb_version;
        std::shared_mutex data_mutex;
    };
}
