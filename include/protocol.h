//具体定义可查看 https://www.kernel.org/doc/html/latest/usb/usbip_protocol.html

#pragma once

#include <cstdint>
#include <variant>
#include <array>
#include <vector>
#include <system_error>

#include <asio.hpp>

#include "network.h"
#include "Device.h"

// 最大传输缓冲区大小（用于防止恶意大内存分配）
#ifndef USBIPDCPP_MAX_TRANSFER_BUFFER_SIZE
#define USBIPDCPP_MAX_TRANSFER_BUFFER_SIZE (16 * 1024 * 1024) // 16MB
#endif

namespace usbipdcpp {
constexpr std::uint16_t USBIP_VERSION = 0x0111;


class AbstDeviceHandler;  // 前向声明


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
    void from_socket(asio::ip::tcp::socket &sock);

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
    void from_socket(asio::ip::tcp::socket &sock);

    //发送出去的数据是紧凑的，但需要有个信息来确定发送时当前数据在内存中的长度以确定在内存中遍历的步长。
    //这个变量只是为了发送时在内存中处理更加方便，与usbip协议无关
    //对于libusb来说，这个值需要为libusb_iso_packet_descriptor::length
    std::uint32_t length_in_transfer_buffer_only_for_send;
};

static_assert(Serializable<UsbIpIsoPacketDescriptor>);

// 通用传输结构，虚拟设备使用
struct GenericTransfer {
    std::vector<std::uint8_t> data;
    std::vector<UsbIpIsoPacketDescriptor> iso_descriptors;
    std::size_t actual_length = 0;
    std::size_t data_offset = 0;

    static GenericTransfer* from_handle(void* ptr) {
        return static_cast<GenericTransfer*>(ptr);
    }
};

/**
 * @brief RAII 包装类，管理 transfer_handle 的生命周期
 *
 * 该类接管 transfer_handle 的所有权，析构时自动调用 handler_->free_transfer_handle() 释放资源。
 *
 * 使用规则：
 * - 构造时接管所有权：TransferHandle handle(ptr, handler);
 * - 析构时自动释放，无需手动管理
 * - 可以通过 std::move() 转移所有权给另一个 TransferHandle
 * - 调用 release() 会放弃所有权，调用者必须手动释放
 *
 * 典型用法：
 * @code
 *   void* ptr = handler->alloc_transfer_handle(1024, 0);
 *   TransferHandle handle(ptr, handler);  // 接管所有权
 *   // ... 使用 handle ...
 *   // 函数结束时 handle 析构，自动调用 handler->free_transfer_handle(ptr)
 * @endcode
 */
class TransferHandle {
    void* handle_ = nullptr;
    AbstDeviceHandler* handler_ = nullptr;

public:
    TransferHandle() = default;

    /**
     * @brief 构造并接管所有权
     * @param handle 由 handler->alloc_transfer_handle() 返回的指针
     * @param handler 设备处理器，用于释放 handle
     */
    TransferHandle(void* handle, AbstDeviceHandler* handler);

    // 禁止拷贝（所有权唯一）
    TransferHandle(const TransferHandle&) = delete;
    TransferHandle& operator=(const TransferHandle&) = delete;

    /**
     * @brief 移动构造，转移所有权
     * @param other 源对象，移动后变为空状态
     */
    TransferHandle(TransferHandle&& other) noexcept;
    TransferHandle& operator=(TransferHandle&& other) noexcept;

    /**
     * @brief 析构时自动释放 handle
     *
     * 如果 handle_ 和 handler_ 都非空，调用 handler_->free_transfer_handle(handle_)
     */
    ~TransferHandle();

    /**
     * @brief 释放当前持有的 handle 并置空
     *
     * 调用 handler_->free_transfer_handle(handle_)，然后将 handle_ 和 handler_ 置空。
     * 对空对象调用此函数是安全的（无操作）。
     */
    void reset();

    /**
     * @brief 获取原始指针（不转移所有权）
     * @return 原始指针，可能为 nullptr
     *
     * 注意：返回的指针生命周期由 TransferHandle 管理，不要在外部释放。
     */
    [[nodiscard]] void* get() const { return handle_; }

    /**
     * @brief 获取关联的 handler
     * @return 设备处理器指针，可能为 nullptr
     */
    [[nodiscard]] AbstDeviceHandler* handler() const { return handler_; }

    /**
     * @brief 检查是否持有有效 handle
     * @return true 表示持有有效 handle
     */
    explicit operator bool() const { return handle_ != nullptr; }

    /**
     * @brief 设置 handler（用于 from_socket 前设置）
     * @param handler 设备处理器
     *
     * 通常与 set_handle() 配合使用，在反序列化时填充。
     */
    void set_handler(AbstDeviceHandler* handler) { handler_ = handler; }

    /**
     * @brief 设置 handle（用于 from_socket 中填充）
     * @param handle 原始指针
     *
     * 警告：调用此函数前确保当前没有持有其他 handle，否则会泄漏。
     */
    void set_handle(void* handle) { handle_ = handle; }

    /**
     * @brief 释放所有权，返回原始指针
     * @return 原始指针，调用者必须手动释放
     *
     * 警告：调用此函数后，TransferHandle 不再管理该 handle，
     * 调用者必须确保调用 handler->free_transfer_handle() 释放资源，
     * 否则会导致内存泄漏！
     *
     * @code
     *   void* ptr = handle.release();
     *   // ... 使用 ptr ...
     *   handler->free_transfer_handle(ptr);  // 必须手动释放！
     * @endcode
     */
    void* release();
};

namespace UsbIpCommand {
    struct OpReqDevlist {
        std::uint32_t status;

        [[nodiscard]] array_data_type<calculate_total_size_with_array<
            decltype(USBIP_VERSION), decltype(OP_REQ_DEVLIST), decltype(status)
        >()> to_bytes() const;
        void from_socket(asio::ip::tcp::socket &sock);
    };

    static_assert(Serializable<OpReqDevlist>);

    struct OpReqImport {
        std::uint32_t status;
        array_data_type<32> busid;

        [[nodiscard]] array_data_type<calculate_total_size_with_array<
            decltype(USBIP_VERSION), decltype(OP_REQ_DEVLIST), decltype(status), decltype(busid)
        >()> to_bytes() const;
        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        void from_socket(asio::ip::tcp::socket &sock);
    };

    static_assert(SerializableFromSocket<OpReqImport>);

    struct UsbIpCmdSubmit {
        UsbIpHeaderBasic header;
        std::uint32_t transfer_flags;
        //表明了传输数据的最大值
        std::uint32_t transfer_buffer_length;
        std::uint32_t start_frame;
        //等时传输包数量
        std::uint32_t number_of_packets;
        std::uint32_t interval;
        SetupPacket setup;
        //IN方向transfer_buffer_length==data.size()，OUT方向IN方向transfer_buffer_length=0

        // RAII 包装的 transfer_handle
        mutable TransferHandle transfer;

        [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;

        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        //这个函数只读取部分数值，后面的数据部分不读取，一个对象只能调用一次
        void from_socket(asio::ip::tcp::socket &sock);
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
        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        //一个对象只能调用一次
        void from_socket(asio::ip::tcp::socket &sock);
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
    usbipdcpp::UsbIpCommand::OpCmdVariant get_op_from_socket(
            asio::ip::tcp::socket &sock, usbipdcpp::error_code &ec);


    /**
     * @brief 该函数只有ec有值则返回值为空，无ec则一定有值，无需二次判断
     * @param sock
     * @param handler 用于创建 transfer_handle
     * @param ec
     * @return 获取到的命令
     */
    usbipdcpp::UsbIpCommand::CmdVariant get_cmd_from_socket(
            asio::ip::tcp::socket &sock, AbstDeviceHandler* handler, usbipdcpp::error_code &ec);

    std::vector<std::uint8_t> to_bytes(const AllCmdVariant &cmd);
}

namespace UsbIpResponse {
    struct OpRepDevlist {
        std::uint32_t status;
        std::uint32_t device_count;
        std::vector<UsbDevice> devices;

        [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;
        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        void from_socket(asio::ip::tcp::socket &sock);

        static OpRepDevlist create_from_devices(const std::vector<std::shared_ptr<UsbDevice>> &devices);
    };

    static_assert(SerializableFromSocket<OpRepDevlist>);

    struct OpRepImport {
        std::uint32_t status;
        std::shared_ptr<UsbDevice> device;

        [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;
        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        void from_socket(asio::ip::tcp::socket &sock);

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

        // RAII 包装的 transfer_handle
        // 如果没有数据阶段（actual_length == 0），请勿赋值
        mutable TransferHandle transfer;

        // 发送配置，用于控制发送时的行为
        struct SendConfig {
            std::uint32_t data_offset = 0; // 数据偏移量 (控制传输为8，其他为0)
        } send_config{};

        [[nodiscard]] data_type to_bytes() const;
        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        void from_socket(asio::ip::tcp::socket &sock);

        /**
         * @brief 创建 RET_SUBMIT 响应（接管 transfer 所有权）
         */
        static UsbIpRetSubmit create_ret_submit(
                std::uint32_t seqnum,
                std::uint32_t status,
                std::uint32_t actual_length,
                std::uint32_t start_frame,
                std::uint32_t number_of_packets,
                TransferHandle transfer
                );
        /**
         * @brief 创建成功的 RET_SUBMIT 响应（无数据，不接管 transfer）
         */
        static UsbIpRetSubmit create_ret_submit_ok_without_data(std::uint32_t seqnum, std::uint32_t actual_length);

        /**
         * @brief 创建带状态的 RET_SUBMIT 响应（无数据，不接管 transfer）
         */
        static UsbIpRetSubmit create_ret_submit_with_status_and_no_data(std::uint32_t seqnum, std::uint32_t status,
                                                                        std::uint32_t actual_length);
        /**
         * @brief 创建带状态的 RET_SUBMIT 响应（无等时包，接管 transfer 所有权）
         */
        static UsbIpRetSubmit create_ret_submit_with_status_and_no_iso(std::uint32_t seqnum, std::uint32_t status,
                                                                       std::uint32_t actual_length,
                                                                       TransferHandle transfer);
        /**
         * @brief 创建 EPIPE 状态的 RET_SUBMIT 响应（接管 transfer 所有权）
         */
        static UsbIpRetSubmit create_ret_submit_epipe_no_iso(std::uint32_t seqnum,
                                                             std::uint32_t actual_length,
                                                             TransferHandle transfer);
        /**
         * @brief 创建 EPIPE 状态的 RET_SUBMIT 响应（无数据，不接管 transfer）
         */
        static UsbIpRetSubmit create_ret_submit_epipe_without_data(std::uint32_t seqnum, std::uint32_t actual_length);
        /**
         * @brief 创建成功的 RET_SUBMIT 响应（无等时包，接管 transfer 所有权）
         */
        static UsbIpRetSubmit create_ret_submit_ok_with_no_iso(std::uint32_t seqnum,
                                                               std::uint32_t actual_length,
                                                               TransferHandle transfer);
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
        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        void from_socket(asio::ip::tcp::socket &sock);

        static UsbIpRetUnlink create_ret_unlink(std::uint32_t seqnum, std::uint32_t status);
        static UsbIpRetUnlink create_ret_unlink_success(std::uint32_t seqnum);
    };

    static_assert(SerializableFromSocket<UsbIpRetUnlink>);

    using OpRepVariant = std::variant<OpRepDevlist, OpRepImport>;
    using RetVariant = std::variant<UsbIpRetSubmit, UsbIpRetUnlink>;
    using AllRepVariant = std::variant<OpRepDevlist, OpRepImport, UsbIpRetSubmit, UsbIpRetUnlink>;
}


}
