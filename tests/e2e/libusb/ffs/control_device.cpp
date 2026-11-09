#include "ffs_helper.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include <linux/usb/ch9.h>

namespace usbipdcpp::e2e {

namespace {

usb_interface_descriptor make_interface(std::uint8_t alternate_setting) {
    usb_interface_descriptor descriptor {};
    descriptor.bLength = sizeof(descriptor);
    descriptor.bDescriptorType = USB_DT_INTERFACE;
    descriptor.bInterfaceNumber = 0;
    descriptor.bAlternateSetting = alternate_setting;
    descriptor.bNumEndpoints = 0;
    descriptor.bInterfaceClass = USB_CLASS_VENDOR_SPEC;
    descriptor.bInterfaceSubClass = 0;
    descriptor.bInterfaceProtocol = 0;
    descriptor.iInterface = 0;
    return descriptor;
}

/**
 * @brief 按 (seed + i) 填充递增模式
 */
void fill_pattern(std::vector<std::uint8_t> &buffer, std::uint8_t seed) {
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<std::uint8_t>(seed + i);
    }
}

}  // namespace

/**
 * @brief 纯控制传输设备
 *
 * 只有接口没有数据端点，专门覆盖控制传输的各种行为：各方向、各长度、
 * 短包、零长包、stall、延时、超时后的恢复、alt setting 切换。
 */
class ControlDevice : public FfsDevice {
public:
    using FfsDevice::FfsDevice;

protected:
    FfsDescriptors make_descriptors() const override {
        FfsDescriptors descriptors;
        descriptors.full_speed.add(make_interface(0));
        descriptors.full_speed.add(make_interface(1));
        descriptors.high_speed.add(make_interface(0));
        descriptors.high_speed.add(make_interface(1));
        return descriptors;
    }

    void handle_setup(const usb_ctrlrequest &setup) override {
        const std::uint8_t type = setup.bRequestType & USB_TYPE_MASK;
        if (type != USB_TYPE_VENDOR) {
            // 标准请求由 composite 处理；走到这里说明不支持
            stall_ep0();
            return;
        }

        const auto request = static_cast<proto::Request>(setup.bRequest);
        const bool in = setup_is_in();
        const std::uint16_t length = setup_length();
        const std::uint16_t value = setup_value(setup);

        switch (request) {
            case proto::Request::get_info: {
                if (!in) {
                    stall_ep0();
                    return;
                }
                std::uint32_t payload = proto::version;
                send_ep0(&payload, sizeof(payload));
                break;
            }

            case proto::Request::echo_in: {
                if (!in) {
                    stall_ep0();
                    return;
                }
                std::vector<std::uint8_t> buffer(length);
                fill_pattern(buffer, static_cast<std::uint8_t>(value & 0xff));
                send_ep0(buffer.data(), buffer.size());
                break;
            }

            case proto::Request::echo_out: {
                if (in) {
                    stall_ep0();
                    return;
                }
                std::vector<std::uint8_t> buffer(length);
                const ssize_t received = recv_ep0(buffer.data(), buffer.size());
                if (received >= 0) {
                    last_out_length = static_cast<std::uint32_t>(received);
                    last_out_sum = 0;
                    for (ssize_t i = 0; i < received; ++i) {
                        last_out_sum += buffer[static_cast<std::size_t>(i)];
                    }
                }
                break;
            }

            case proto::Request::get_last_out: {
                if (!in) {
                    stall_ep0();
                    return;
                }
                std::uint32_t payload[2] = {last_out_length, last_out_sum};
                send_ep0(payload, sizeof(payload));
                break;
            }

            case proto::Request::stall:
                stall_ep0();
                break;

            case proto::Request::short_in: {
                if (!in) {
                    stall_ep0();
                    return;
                }
                const std::uint32_t reply = std::min<std::uint32_t>(length, value);
                std::vector<std::uint8_t> buffer(reply);
                fill_pattern(buffer, 0x55);
                send_ep0(buffer.data(), buffer.size());
                break;
            }

            case proto::Request::zero_in: {
                if (!in) {
                    stall_ep0();
                    return;
                }
                // wLength 非 0 但一个字节都不回，主机应收到零长数据阶段
                send_ep0(nullptr, 0);
                break;
            }

            case proto::Request::delay_in: {
                if (!in) {
                    stall_ep0();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(value));
                const std::uint8_t payload = 0xaa;
                send_ep0(&payload, sizeof(payload));
                break;
            }

            case proto::Request::delay_out: {
                if (in || length != 0) {
                    stall_ep0();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(value));
                complete_ep0_no_data();
                break;
            }

            case proto::Request::stall_out_data:
                stall_ep0();
                break;

            default:
                stall_ep0();
                break;
        }
    }

private:
    std::uint32_t last_out_length = 0;
    std::uint32_t last_out_sum = 0;
};

}  // namespace usbipdcpp::e2e

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "用法: %s <functionfs 挂载目录>\n", argv[0]);
        return 2;
    }
    usbipdcpp::e2e::ControlDevice device(argv[1]);
    return device.run();
}
