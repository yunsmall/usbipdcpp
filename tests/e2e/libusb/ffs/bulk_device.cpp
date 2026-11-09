#include "ffs_helper.hpp"
#include "protocol.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <endian.h>
#include <thread>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

namespace usbipdcpp::e2e {

namespace {

usb_interface_descriptor make_interface(std::uint8_t interface_number,
                                        std::uint8_t alternate_setting,
                                        std::uint8_t endpoint_count) {
    usb_interface_descriptor descriptor {};
    descriptor.bLength = sizeof(descriptor);
    descriptor.bDescriptorType = USB_DT_INTERFACE;
    descriptor.bInterfaceNumber = interface_number;
    descriptor.bAlternateSetting = alternate_setting;
    descriptor.bNumEndpoints = endpoint_count;
    descriptor.bInterfaceClass = USB_CLASS_VENDOR_SPEC;
    return descriptor;
}

usb_endpoint_descriptor_no_audio make_endpoint(std::uint8_t address, std::uint8_t attributes,
                                               std::uint16_t max_packet) {
    usb_endpoint_descriptor_no_audio descriptor {};
    descriptor.bLength = sizeof(descriptor);
    descriptor.bDescriptorType = USB_DT_ENDPOINT;
    descriptor.bEndpointAddress = address;
    descriptor.bmAttributes = attributes;
    descriptor.wMaxPacketSize = htole16(max_packet);
    descriptor.bInterval = 0;
    return descriptor;
}

void fill_pattern(std::vector<std::uint8_t> &buffer, std::uint8_t seed) {
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<std::uint8_t>(seed + i);
    }
}

/**
 * @brief 检查一段数据是否满足逐字节 +1 的连续性
 *
 * 主机侧按同样规则填充，设备侧不依赖分块边界，任意一段连续数据都能校验。
 */
bool check_continuity(const std::uint8_t *data, std::size_t length) {
    for (std::size_t i = 1; i < length; ++i) {
        if (data[i] != static_cast<std::uint8_t>(data[i - 1] + 1)) {
            return false;
        }
    }
    return true;
}

}  // namespace

/**
 * @brief bulk 传输设备（多接口、多 alt setting）
 *
 * 描述符布局（每个 speed 都一样）：
 * - 接口 0 alt 0：bulk IN 0x81 / OUT 0x02
 * - 接口 0 alt 1：bulk IN 0x83 / OUT 0x04
 * - 接口 1 alt 0：无端点
 * - 接口 1 alt 1：bulk IN 0x85 / OUT 0x06
 *
 * 这样每个 alt setting 的端点集合互不重叠，客户端在 alt 之间切换后
 * 用当前 alt 的端点收发，能验证 SET_INTERFACE 的完整流程。
 */
class BulkDevice : public FfsDevice {
public:
    using FfsDevice::FfsDevice;

    ~BulkDevice() override {
        stop_endpoints();
    }

protected:
    FfsDescriptors make_descriptors() const override {
        FfsDescriptors descriptors;

        auto fill = [](DescriptorList &list, std::uint16_t bulk_packet) {
            list.add(make_interface(0, 0, 2));
            list.add(make_endpoint(0x81, USB_ENDPOINT_XFER_BULK, bulk_packet));
            list.add(make_endpoint(0x02, USB_ENDPOINT_XFER_BULK, bulk_packet));
            list.add(make_interface(0, 1, 2));
            list.add(make_endpoint(0x83, USB_ENDPOINT_XFER_BULK, bulk_packet));
            list.add(make_endpoint(0x04, USB_ENDPOINT_XFER_BULK, bulk_packet));
            list.add(make_interface(1, 0, 0));
            list.add(make_interface(1, 1, 2));
            list.add(make_endpoint(0x85, USB_ENDPOINT_XFER_BULK, bulk_packet));
            list.add(make_endpoint(0x06, USB_ENDPOINT_XFER_BULK, bulk_packet));
        };
        fill(descriptors.full_speed, 64);
        fill(descriptors.high_speed, 512);
        return descriptors;
    }

    void on_enable() override {
        // 每次 SET_INTERFACE 都会重新触发 ENABLE。已经打开的端点必须跳过，
        // 重复给 std::thread 赋值会 terminate；上次没打开成功的端点这次重试
        running = true;
        static constexpr std::array<const char *, 6> names = {"ep1", "ep2", "ep3",
                                                              "ep4", "ep5", "ep6"};
        if (!defaults_applied) {
            // pair2（ep3/ep4）固定用小包，专门制造短包
            endpoints[2].reply_length = 100;
            defaults_applied = true;
        }
        for (std::size_t i = 0; i < endpoints.size(); ++i) {
            EndpointState &endpoint = endpoints[i];
            if (endpoint.fd >= 0) {
                continue;
            }
            endpoint.fd = open_endpoint(names[i]);
            if (endpoint.fd < 0) {
                continue;
            }
            // UDC 自动分配端点可能改写地址，必须以 ioctl 查到的为准
            usb_endpoint_descriptor descriptor {};
            if (!endpoint_descriptor(endpoint.fd, descriptor)) {
                ::close(endpoint.fd);
                endpoint.fd = -1;
                continue;
            }
            endpoint.address = descriptor.bEndpointAddress;
            endpoint.in = (descriptor.bEndpointAddress & USB_DIR_IN) != 0;
            endpoint.max_packet =
                    static_cast<std::uint16_t>(le16toh(descriptor.wMaxPacketSize) & 0x7ff);
            log("端点 %s 实际地址 0x%02x (%s), maxpacket=%u", names[i], endpoint.address,
                endpoint.in ? "IN" : "OUT", endpoint.max_packet);
            endpoint.worker = std::thread([this, &endpoint]() {
                if (endpoint.in) {
                    in_worker(endpoint);
                }
                else {
                    out_worker(endpoint);
                }
            });
        }
    }

    void on_disable() override {
        stop_endpoints();
    }

    void handle_setup(const usb_ctrlrequest &setup) override {
        const std::uint8_t type = setup.bRequestType & USB_TYPE_MASK;
        const std::uint8_t recipient = setup.bRequestType & USB_RECIP_MASK;

        if (type == USB_TYPE_STANDARD && recipient == USB_RECIP_ENDPOINT) {
            handle_endpoint_standard(setup);
            return;
        }

        if (type != USB_TYPE_VENDOR) {
            stall_ep0();
            return;
        }

        const auto request = static_cast<proto::Request>(setup.bRequest);
        const bool in = setup_is_in();
        const std::uint16_t value = setup_value(setup);
        const std::uint16_t index = setup_index(setup);

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

            case proto::Request::set_in_enabled: {
                EndpointState *endpoint = find_endpoint(index, true);
                if (in || endpoint == nullptr) {
                    stall_ep0();
                    return;
                }
                endpoint->enabled = value != 0;
                complete_ep0_no_data();
                break;
            }

            case proto::Request::set_reply_len: {
                EndpointState *endpoint = find_endpoint(index, true);
                if (in || endpoint == nullptr) {
                    stall_ep0();
                    return;
                }
                endpoint->reply_length = value;
                complete_ep0_no_data();
                break;
            }

            case proto::Request::get_out_stats: {
                EndpointState *endpoint = find_endpoint(index, false);
                if (!in || endpoint == nullptr) {
                    stall_ep0();
                    return;
                }
                proto::OutStats stats {};
                stats.chunks = endpoint->chunks.load();
                stats.bytes = endpoint->bytes.load();
                stats.errors = endpoint->errors.load();
                send_ep0(&stats, sizeof(stats));
                break;
            }

            default:
                stall_ep0();
                break;
        }
    }

private:
    struct EndpointState {
        std::uint8_t address = 0;
        bool in = false;
        int fd = -1;
        std::uint16_t max_packet = 64;
        std::thread worker;

        std::atomic<bool> enabled {true};
        std::atomic<std::uint32_t> reply_length {4096};
        std::atomic<std::uint32_t> chunks {0};
        std::atomic<std::uint64_t> bytes {0};
        std::atomic<std::uint32_t> errors {0};
        std::atomic<std::uint32_t> next_byte {0};
        std::atomic<bool> halt_requested {false};
        // 只有 OUT worker 线程读写，不需要原子
        std::uint8_t expected_next_byte = 0;
    };

    /**
     * @brief 按主机发来的端点地址找端点
     * @param address wIndex 里的端点地址
     * @param want_in true 找 IN 端点，false 找 OUT 端点
     */
    EndpointState *find_endpoint(std::uint16_t address, bool want_in) {
        for (EndpointState &endpoint : endpoints) {
            if (endpoint.address == (address & 0xff) && endpoint.in == want_in) {
                return &endpoint;
            }
        }
        return nullptr;
    }

    /**
     * @brief 停止所有 worker 并关闭端点文件
     *
     * 只在 on_disable（整个 function 被禁用）时调用：内核已经先把端点上的
     * 请求以 ESHUTDOWN 还回来，worker 会从阻塞 IO 里退出，join 不会挂住。
     */
    void stop_endpoints() {
        running = false;
        for (EndpointState &endpoint : endpoints) {
            if (endpoint.worker.joinable()) {
                endpoint.worker.join();
            }
            if (endpoint.fd >= 0) {
                ::close(endpoint.fd);
                endpoint.fd = -1;
            }
        }
    }

    /**
     * @brief 对端点做反方向 IO 触发 halt
     *
     * FunctionFS 没有设置 halt 的 ioctl，内核把反方向 IO 解释成 halt 请求，
     * 成功时返回 -EBADMSG。
     */
    static void apply_halt(EndpointState &endpoint) {
        char dummy = 0;
        ssize_t result;
        if (endpoint.in) {
            result = ::read(endpoint.fd, &dummy, sizeof(dummy));
        }
        else {
            result = ::write(endpoint.fd, &dummy, sizeof(dummy));
        }
        if (result < 0 && errno != EBADMSG) {
            log("halt 端点 0x%02x 失败: %s", endpoint.address, std::strerror(errno));
        }
    }

    void in_worker(EndpointState &endpoint) {
        std::vector<std::uint8_t> buffer;
        while (running.load()) {
            if (endpoint.halt_requested.exchange(false)) {
                apply_halt(endpoint);
                continue;
            }
            if (!endpoint.enabled.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            const std::uint32_t length = endpoint.reply_length.load();
            const std::uint32_t base = endpoint.next_byte.load();
            buffer.resize(length);
            fill_pattern(buffer, static_cast<std::uint8_t>(base & 0xff));

            const ssize_t written = ::write(endpoint.fd, buffer.data(), buffer.size());
            if (written < 0) {
                if (errno == ESHUTDOWN) {
                    // 多接口/alt 切换会临时禁用端点，等下一次 ENABLE 后继续
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                if (errno == EBADMSG || errno == EPIPE) {
                    // 端点被 halt，等主机清除后继续
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                if (errno == EIDRM || errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                log("IN 端点 0x%02x 写失败: %s", endpoint.address, std::strerror(errno));
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            endpoint.next_byte.fetch_add(static_cast<std::uint32_t>(written));
            endpoint.chunks.fetch_add(1);
            endpoint.bytes.fetch_add(static_cast<std::uint64_t>(written));
        }
    }

    void out_worker(EndpointState &endpoint) {
        // gadget 请求长度必须不超过一包，否则 dummy_hcd 会把多个 host URB
        // 攒进同一个请求，read 迟迟不返回，统计和上层流控都会滞后
        const std::size_t read_size = endpoint.max_packet > 0 ? endpoint.max_packet : 512;
        std::vector<std::uint8_t> buffer(read_size);
        while (running.load()) {
            if (endpoint.halt_requested.exchange(false)) {
                apply_halt(endpoint);
                continue;
            }

            const ssize_t received = ::read(endpoint.fd, buffer.data(), buffer.size());
            if (received < 0) {
                if (errno == ESHUTDOWN) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                if (errno == EBADMSG || errno == EPIPE) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                if (errno == EIDRM || errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                log("OUT 端点 0x%02x 读失败: %s", endpoint.address, std::strerror(errno));
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            endpoint.chunks.fetch_add(1);
            endpoint.bytes.fetch_add(static_cast<std::uint64_t>(received));
            if (received > 0) {
                // 主机侧整条 OUT 数据流是逐字节 +1 的，跨 read（一包一次）也要接得上
                const auto *bytes_ptr = buffer.data();
                const std::uint8_t expected = endpoint.expected_next_byte;
                const bool ok = bytes_ptr[0] == expected &&
                                check_continuity(bytes_ptr, static_cast<std::size_t>(received));
                endpoint.expected_next_byte =
                        static_cast<std::uint8_t>(bytes_ptr[received - 1] + 1);
                if (!ok) {
                    endpoint.errors.fetch_add(1);
                    log("OUT 端点 0x%02x 数据流不连续: %zd 字节, 首字节 0x%02x 应为 0x%02x",
                        endpoint.address, received, bytes_ptr[0], static_cast<unsigned>(expected));
                }
            }
        }
    }

    void handle_endpoint_standard(const usb_ctrlrequest &setup) {
        const std::uint16_t value = setup_value(setup);
        if (setup.bRequest != USB_REQ_SET_FEATURE && setup.bRequest != USB_REQ_CLEAR_FEATURE) {
            stall_ep0();
            return;
        }
        if (value != USB_ENDPOINT_HALT) {
            stall_ep0();
            return;
        }

        // 端点 recipient 的请求走到这里时，wIndex 已被内核重写成内部端点序号（1 开始）
        const std::uint16_t index = setup_index(setup);
        if (index == 0 || index > endpoints.size()) {
            stall_ep0();
            return;
        }
        EndpointState &endpoint = endpoints[index - 1];

        if (setup.bRequest == USB_REQ_SET_FEATURE) {
            endpoint.halt_requested = true;
        }
        else if (endpoint.fd >= 0) {
            ioctl(endpoint.fd, FUNCTIONFS_CLEAR_HALT);
        }
        complete_ep0_no_data();
    }

    std::array<EndpointState, 6> endpoints {};
    std::atomic<bool> running {false};
    bool defaults_applied = false;
};

}  // namespace usbipdcpp::e2e

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "用法: %s <functionfs 挂载目录>\n", argv[0]);
        return 2;
    }
    usbipdcpp::e2e::BulkDevice device(argv[1]);
    // run() 只在 FunctionFS 出错/EOF 时返回，此时 worker 可能还阻塞在端点 IO，
    // 内核不会再把请求还回来（ep0 还开着，走不到 ffs_data_reset 的清理），
    // 正常析构会 join 挂死；测试进程直接退出，fd 和线程交给内核回收
    std::_Exit(device.run());
}
