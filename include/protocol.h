#pragma once

#include <cstdint>
#include <variant>
#include <array>
#include <vector>
#include <system_error>

#include <asio/awaitable.hpp>

#include "network.h"
#include "device.h"


namespace usbipdcpp {
    constexpr std::uint16_t USBIP_VERSION = 0x0111;


    constexpr std::uint16_t OP_REQ_DEVLIST = 0x8005;
    constexpr std::uint16_t OP_REP_DEVLIST = 0x0005;
    constexpr std::uint16_t OP_REQ_IMPORT = 0x8003;
    constexpr std::uint16_t OP_REP_IMPORT = 0x0003;


    constexpr std::uint32_t USBIP_CMD_SUBMIT = 0x0001;
    constexpr std::uint32_t USBIP_CMD_UNLINK = 0x0002;
    constexpr std::uint32_t USBIP_RET_SUBMIT = 0x0003;
    constexpr std::uint32_t USBIP_RET_UNLINK = 0x0004;

    enum UsbIpDirection {
        Out = 0,
        In = 1,
    };

    enum class ErrorType {
        OK = 0,
        UNKNOWN_VERSION,
        UNKNOWN_CMD,
        PROTOCOL_ERROR,
        NO_DEVICE,
        SOCKET_EOF,
        SOCKET_ERR,
        INTERNAL_ERROR,
        INVALID_ARG,
        UNIMPLEMENTED,
        TRANSFER_ERROR,
    };

    //tools/usbip_common.h
    enum class OperationStatuType {
        //Request Completed Successfully
        OK = 0,
        //Request Failed
        NA,
        //Device busy (exported)
        DevBusy,
        //Device in error state
        DevError,
        //Device not found
        NoDev,
        //Unexpected response
        Error
    };

    enum class UrbStatusType {
        StatusOK = -0,
        StatusECONNRESET = -104,
        StatusEPIPE = -32,
        StatusESHUTDOWN = -108,
        StatusENODEV = -19,
        StatusENOENT = -2,
        StatusETIMEDOUT = -110,
        StatusEEOVERFLOW = -75
    };

    class TransferErrorCategory : public std::error_category {
    public:
        [[nodiscard]] const char *name() const noexcept override;
        [[nodiscard]] std::string message(int _Errval) const override;
    };

    const TransferErrorCategory g_error_category;
    std::error_code make_error_code(ErrorType e);


    struct UsbIpHeaderBasic {
        /**
         * 这个字段并不需要从socket里面读，由子命令设置。
         * 根据先读的字段判断应该创建哪个包
        */
        std::uint32_t command;
        std::uint32_t seqnum;
        std::uint32_t devid;
        std::uint32_t direction;
        std::uint32_t ep;

        [[nodiscard]] array_data_type<calculate_total_size_with_array<
            decltype(command), decltype(seqnum), decltype(devid), decltype(direction), decltype(ep)
        >()>
        to_bytes() const;
        [[nodiscard]] asio::awaitable<void> from_socket(asio::ip::tcp::socket &sock);

        bool operator==(const UsbIpHeaderBasic &other) const = default;

        void set_as_server() {
            devid = 0;
            direction = 0;
            ep = 0;
        }

        static UsbIpHeaderBasic get_server_header(std::uint32_t command, std::uint32_t seqnum) {
            return UsbIpHeaderBasic{
                    .command = command,
                    .seqnum = seqnum,
                    .devid = 0,
                    .direction = 0,
                    .ep = 0
            };
        }

    };

    static_assert(Serializable<UsbIpHeaderBasic>);


    struct UsbIpIsoPacketDescriptor {
        std::uint32_t offset;
        std::uint32_t length;
        std::uint32_t actual_length;
        std::uint32_t status;

        [[nodiscard]] array_data_type<calculate_total_size_with_array<
            decltype(offset), decltype(length), decltype(actual_length), decltype(status)
        >()> to_bytes() const;
        [[nodiscard]] asio::awaitable<void> from_socket(asio::ip::tcp::socket &sock);

        bool operator==(const UsbIpIsoPacketDescriptor &other) const = default;
    };

    static_assert(Serializable<UsbIpIsoPacketDescriptor>);

    namespace UsbIpCommand {
        struct OpReqDevlist {
            std::uint32_t status;

            [[nodiscard]] array_data_type<calculate_total_size_with_array<
                decltype(USBIP_VERSION), decltype(OP_REQ_DEVLIST), decltype(status)
            >()> to_bytes() const;
            [[nodiscard]] asio::awaitable<void> from_socket(asio::ip::tcp::socket &sock);
            bool operator==(const OpReqDevlist &) const = default;
        };

        static_assert(Serializable<OpReqDevlist>);

        struct OpReqImport {
            std::uint32_t status;
            array_data_type<32> busid;

            [[nodiscard]] array_data_type<calculate_total_size_with_array<
                decltype(USBIP_VERSION), decltype(OP_REQ_DEVLIST), decltype(status), decltype(busid)
            >()> to_bytes() const;
            [[nodiscard]] asio::awaitable<void> to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
            [[nodiscard]] asio::awaitable<void> from_socket(asio::ip::tcp::socket &sock);

            bool operator==(const OpReqImport &other) const = default;
        };

        static_assert(SerializableFromSocket<OpReqImport>);

        struct UsbIpCmdSubmit {
            UsbIpHeaderBasic header;
            std::uint32_t transfer_flags;
            std::uint32_t transfer_buffer_length;
            std::uint32_t start_frame;
            std::uint32_t number_of_packets;
            std::uint32_t interval;
            SetupPacket setup;
            // std::array<std::uint8_t, 8> setup;
            std::vector<std::uint8_t> data;
            std::vector<UsbIpIsoPacketDescriptor> iso_packet_descriptor;

            [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;
            [[nodiscard]] asio::awaitable<void> to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
            //这个函数只读取部分数值，后面的数据部分不读取
            [[nodiscard]] asio::awaitable<void> from_socket(asio::ip::tcp::socket &sock);


            bool operator==(const UsbIpCmdSubmit &other) const;
        };

        static_assert(SerializableFromSocket<UsbIpCmdSubmit>);

        struct UsbIpCmdUnlink {
            UsbIpHeaderBasic header;
            std::uint32_t unlink_seqnum;

            [[nodiscard]] array_data_type<
                calculate_total_size_with_array<
                    decltype(UsbIpHeaderBasic{}.to_bytes()), decltype(unlink_seqnum)
                >() + 24
            > to_bytes() const;
            [[nodiscard]] asio::awaitable<void> to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
            [[nodiscard]] asio::awaitable<void> from_socket(asio::ip::tcp::socket &sock);

            bool operator==(const UsbIpCmdUnlink &other) const = default;
        };

        static_assert(SerializableFromSocket<UsbIpCmdUnlink>);

        using OpCmdVariant = std::variant<OpReqDevlist, OpReqImport>;
        using CmdVariant = std::variant<UsbIpCmdSubmit, UsbIpCmdUnlink>;


        /**
         * @brief 该函数只有ec有值则返回值为空，无ec则一定有值，无需二次判断
         * @param sock
         * @param ec
         * @return 获取到的命令
         */
        asio::awaitable<usbipdcpp::UsbIpCommand::OpCmdVariant> get_op_from_socket(
                asio::ip::tcp::socket &sock, usbipdcpp::error_code &ec);
        /**
         * @brief 该函数只有ec有值则返回值为空，无ec则一定有值，无需二次判断
         * @param sock
         * @param ec
         * @return 获取到的命令
         */
        asio::awaitable<usbipdcpp::UsbIpCommand::CmdVariant> get_cmd_from_socket(
                asio::ip::tcp::socket &sock, usbipdcpp::error_code &ec);

        std::vector<std::uint8_t> to_bytes(const AllCmdVariant &cmd);
    }

    namespace UsbIpResponse {
        struct OpRepDevlist {
            std::uint32_t status;
            std::uint32_t device_count;
            std::vector<UsbDevice> devices;

            [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;
            [[nodiscard]] asio::awaitable<void> to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
            [[nodiscard]] asio::awaitable<void> from_socket(asio::ip::tcp::socket &sock);

            bool operator==(const OpRepDevlist &other) const = default;

            static OpRepDevlist create_from_devices(const std::vector<std::shared_ptr<UsbDevice>> &devices);
        };

        static_assert(SerializableFromSocket<OpRepDevlist>);

        struct OpRepImport {
            std::uint32_t status;
            std::shared_ptr<UsbDevice> device;

            [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;
            [[nodiscard]] asio::awaitable<void> to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
            [[nodiscard]] asio::awaitable<void> from_socket(asio::ip::tcp::socket &sock);

            bool operator==(const OpRepImport &other) const {
                return status == other.status &&
                       device && other.device &&
                       *device == *(other.device);
            };
            static OpRepImport create_on_failure_with_status(std::uint32_t status);
            static OpRepImport create_on_failure();
            /**
             * @brief A shared pointer copy construct that shares the same pointed object
             * @param device
             * @return
             */
            static OpRepImport create_on_success(std::shared_ptr<UsbDevice> device);
        };

        static_assert(SerializableFromSocket<OpRepImport>);

        struct UsbIpRetSubmit {
            UsbIpHeaderBasic header;
            std::uint32_t status;
            std::uint32_t actual_length;
            std::uint32_t start_frame;
            std::uint32_t number_of_packets;
            std::uint32_t error_count;
            data_type transfer_buffer;
            std::vector<UsbIpIsoPacketDescriptor> iso_packet_descriptor;

            [[nodiscard]] data_type to_bytes() const;
            [[nodiscard]] asio::awaitable<void> to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
            [[nodiscard]] asio::awaitable<void> from_socket(asio::ip::tcp::socket &sock);

            bool operator==(const UsbIpRetSubmit &other) const = default;

            static UsbIpRetSubmit usbip_ret_submit_fail_with_status(std::uint32_t seqnum, std::uint32_t status);
            static UsbIpRetSubmit create_ret_submit(
                    std::uint32_t seqnum,
                    std::uint32_t status,
                    std::uint32_t start_frame,
                    std::uint32_t number_of_packets,
                    std::vector<std::uint8_t> &&transfer_buffer,
                    const std::vector<UsbIpIsoPacketDescriptor> &
                    iso_packet_descriptor
                    );
            static UsbIpRetSubmit create_ret_submit_ok_without_data(std::uint32_t seqnum);
            static UsbIpRetSubmit create_ret_submit_with_status_and_no_data(std::uint32_t seqnum, std::uint32_t status);
            static UsbIpRetSubmit create_ret_submit_with_status_and_no_iso(std::uint32_t seqnum, std::uint32_t status,
                                                                           const data_type &transfer_buffer);
            static UsbIpRetSubmit create_ret_submit_epipe_no_iso(std::uint32_t seqnum,
                                                                 const data_type &transfer_buffer);
            static UsbIpRetSubmit create_ret_submit_epipe_without_data(std::uint32_t seqnum);
            static UsbIpRetSubmit create_ret_submit_ok_with_no_iso(std::uint32_t seqnum,
                                                                   const data_type &transfer_buffer);
        };

        static_assert(SerializableFromSocket<UsbIpRetSubmit>);

        struct UsbIpRetUnlink {
            UsbIpHeaderBasic header;
            std::uint32_t status;

            [[nodiscard]] array_data_type<
                calculate_total_size_with_array<
                    decltype(UsbIpHeaderBasic{}.to_bytes()), decltype(status)
                >() + 24
            > to_bytes() const;
            [[nodiscard]] asio::awaitable<void> to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
            [[nodiscard]] asio::awaitable<void> from_socket(asio::ip::tcp::socket &sock);

            bool operator==(const UsbIpRetUnlink &other) const = default;

            static UsbIpRetUnlink create_ret_unlink(std::uint32_t seqnum, std::uint32_t status);
            static UsbIpRetUnlink create_ret_unlink_success(std::uint32_t seqnum);
        };

        static_assert(SerializableFromSocket<UsbIpRetUnlink>);

        using OpRepVariant = std::variant<OpRepDevlist, OpRepImport>;
        using RetVariant = std::variant<UsbIpRetSubmit, UsbIpRetUnlink>;
        using AllRepVariant = std::variant<OpRepDevlist, OpRepImport, UsbIpRetSubmit, UsbIpRetUnlink>;
    }


}
