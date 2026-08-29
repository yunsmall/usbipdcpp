#pragma once

#include <filesystem>
#include <variant>
#include <memory>

#include "usbipdcpp/Version.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/network.h"
#include "usbipdcpp/Interface.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/Export.h"


namespace usbipdcpp {
class Session;

class AbstDeviceHandler;

struct UsbIpIsoPacketDescriptor;

namespace UsbIpCommand {
    struct OpReqDevlist;
    struct OpReqImport;
    struct UsbIpCmdSubmit;
    struct UsbIpCmdUnlink;
    using AllCmdVariant = std::variant<OpReqDevlist, OpReqImport, UsbIpCmdSubmit, UsbIpCmdUnlink>;
}

struct USBIPDCPP_API UsbDevice {
    std::filesystem::path path{};
    std::string busid{};
    std::uint32_t bus_num;
    std::uint32_t dev_num;
    std::uint32_t speed;
    std::uint16_t vendor_id;
    std::uint16_t product_id;
    Version device_bcd{0, 0, 0};
    std::uint8_t device_class;
    std::uint8_t device_subclass;
    std::uint8_t device_protocol;
    std::uint8_t configuration_value;
    std::uint8_t num_configurations;
    std::vector<UsbInterface> interfaces{};


    UsbEndpoint ep0_in;
    UsbEndpoint ep0_out;

    /**
     * @brief 设备处理器
     * 必须在 Server::add_device 调用之前设置，推荐使用 with_handler 函数。
     * 如果调用时 handler 为空，属于未定义行为。
     */
    std::shared_ptr<AbstDeviceHandler> handler;

    /**
     * @brief 创建并设置 handler
     * 推荐使用此函数设置 handler。
     * @tparam T handler 类型
     * @tparam args 传递给 handler 构造函数的参数
     * @return 创建的 handler
     */
    template<typename T, typename... Args>
    std::shared_ptr<T> with_handler(Args &&... args) {
        auto new_handler = std::make_shared<T>(*this, std::forward<Args>(args)...);
        handler = std::static_pointer_cast<AbstDeviceHandler>(new_handler);
        return new_handler;
    }

    /**
     * @brief 创建一台基本的虚拟设备：接口列表（通常由各接口 handler 的
     * make_interface 创建，已绑好 handler）与易变字段当参数，其余字段给默认值；
     * EP0 最大包大小按 speed 自动生成。
     *
     * 参数按改动频率从高到低排列（前面的参数改动时无需填后面的）。
     * 返回后仍可改任意字段，随后用 with_handler 绑定设备级 handler。
     * @param busid 设备 busid（如 "1-1"），devlist/attach 用它标识
     * @param vendor_id 厂商 ID
     * @param product_id 产品 ID
     * @param interfaces 设备接口列表
     * @param bus_num 总线编号（默认 1）
     * @param dev_num 设备编号（默认 1）
     * @param device_class 设备级类标识（默认 0 = 在接口级定义；IAD 复合设备如
     *        CDC 需设 0x02）
     * @param path 设备路径（devlist 显示用）；默认空 = "/usbipdcpp/<busid>"
     * @param speed 设备速度（默认 Full）
     * @param device_bcd 设备版本号（BCD，默认 1.0）
     * @param device_subclass / device_protocol 设备级子类/协议（默认 0）
     * @param configuration_value / num_configurations 配置序号与配置数（默认 1）
     */
    static std::shared_ptr<UsbDevice> make(std::string busid, std::uint16_t vendor_id, std::uint16_t product_id,
                                           std::vector<UsbInterface> interfaces,
                                           std::uint32_t bus_num = 1, std::uint32_t dev_num = 1,
                                           std::uint8_t device_class = 0,
                                           std::filesystem::path path = {},
                                           UsbSpeed speed = UsbSpeed::Full,
                                           Version device_bcd = Version{0, 1, 0},
                                           std::uint8_t device_subclass = 0, std::uint8_t device_protocol = 0,
                                           std::uint8_t configuration_value = 1, std::uint8_t num_configurations = 1) {
        auto device = std::make_shared<UsbDevice>();
        device->path = path.empty() ? std::filesystem::path("/usbipdcpp/" + busid) : path;
        device->busid = std::move(busid);
        device->bus_num = bus_num;
        device->dev_num = dev_num;
        device->speed = static_cast<std::uint32_t>(speed);
        device->vendor_id = vendor_id;
        device->product_id = product_id;
        device->device_bcd = device_bcd;
        device->device_class = device_class;
        device->device_subclass = device_subclass;
        device->device_protocol = device_protocol;
        device->configuration_value = configuration_value;
        device->num_configurations = num_configurations;
        device->interfaces = std::move(interfaces);
        device->ep0_in = UsbEndpoint::get_ep0_in(speed);
        device->ep0_out = UsbEndpoint::get_ep0_out(speed);
        return device;
    }

    static constexpr std::size_t bytes_without_interfaces_num = calculate_total_size_with_array<
        array_data_type<256>,
        array_data_type<32>,
        decltype(bus_num),
        decltype(dev_num),
        decltype(speed),
        decltype(vendor_id),
        decltype(product_id),
        std::uint16_t,
        decltype(device_class),
        decltype(device_subclass),
        decltype(device_protocol),
        decltype(configuration_value),
        decltype(num_configurations),
        std::uint8_t
    >();

    [[nodiscard]] std::vector<std::uint8_t> to_bytes_with_interfaces() const;
    [[nodiscard]] array_data_type<bytes_without_interfaces_num> to_bytes_without_interfaces() const;

    //devlist请求的时候要发送接口信息，import请求时不发送接口信息
    [[nodiscard]] array_data_type<bytes_without_interfaces_num> to_bytes() const;
    void from_socket(asio::ip::tcp::socket &sock);

    std::optional<std::pair<UsbEndpoint, std::optional<UsbInterface>>> find_ep(std::uint8_t ep);

    bool operator==(const UsbDevice &other) const {
        return path == other.path &&
               busid == other.busid &&
               bus_num == other.bus_num &&
               dev_num == other.dev_num &&
               speed == other.speed &&
               vendor_id == other.vendor_id &&
               product_id == other.product_id &&
               device_bcd == other.device_bcd &&
               device_class == other.device_class &&
               device_subclass == other.device_subclass &&
               device_protocol == other.device_protocol &&
               configuration_value == other.configuration_value &&
               num_configurations == other.num_configurations &&
               interfaces == other.interfaces;
    }
};

static_assert(Serializable<UsbDevice>);
}
