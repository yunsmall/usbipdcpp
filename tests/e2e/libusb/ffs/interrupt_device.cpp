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

constexpr std::uint32_t counter_magic = 0x5a5a5a5a;

/**
 * @brief 检查一段数据是否满足逐字节 +1 的连续性
 *
 * 主机侧整条 OUT 数据流按同样规则填充，设备侧按读到的每一段校验。
 */
bool check_continuity(const std::uint8_t *data, std::size_t length) {
    for (std::size_t i = 1; i < length; ++i) {
        if (data[i] != static_cast<std::uint8_t>(data[i - 1] + 1)) {
            return false;
        }
    }
    return true;
}

usb_interface_descriptor make_interface(std::uint8_t endpoint_count) {
    usb_interface_descriptor descriptor {};
    descriptor.bLength = sizeof(descriptor);
    descriptor.bDescriptorType = USB_DT_INTERFACE;
    descriptor.bInterfaceNumber = 0;
    descriptor.bAlternateSetting = 0;
    descriptor.bNumEndpoints = endpoint_count;
    descriptor.bInterfaceClass = USB_CLASS_VENDOR_SPEC;
    return descriptor;
}

usb_endpoint_descriptor_no_audio make_endpoint(std::uint8_t address, std::uint16_t max_packet) {
    usb_endpoint_descriptor_no_audio descriptor {};
    descriptor.bLength = sizeof(descriptor);
    descriptor.bDescriptorType = USB_DT_ENDPOINT;
    descriptor.bEndpointAddress = address;
    descriptor.bmAttributes = USB_ENDPOINT_XFER_INT;
    descriptor.wMaxPacketSize = htole16(max_packet);
    descriptor.bInterval = 1;
    return descriptor;
}

}  // namespace

/**
 * @brief interrupt 传输设备
 *
 * IN 端点持续输出 8 字节一组 {计数器, 计数器异或固定值}，OUT 端点接收
 * 并统计。用来覆盖 interrupt 的收发、并发、取消、halt 等行为。
 */
class InterruptDevice : public FfsDevice {
public:
    using FfsDevice::FfsDevice;

    ~InterruptDevice() override {
        stop_endpoints();
    }

protected:
    FfsDescriptors make_descriptors() const override {
        FfsDescriptors descriptors;

        auto fill = [](DescriptorList &list) {
            list.add(make_interface(2));
            list.add(make_endpoint(0x81, 64));
            list.add(make_endpoint(0x02, 64));
        };
        fill(descriptors.full_speed);
        fill(descriptors.high_speed);
        return descriptors;
    }

    void on_enable() override {
        // 每次 SET_INTERFACE 都会重新触发 ENABLE。已经打开的端点必须跳过，
        // 重复给 std::thread 赋值会 terminate；上次没打开成功的端点这次重试
        running = true;
        static constexpr std::array<const char *, 2> names = {"ep1", "ep2"};
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
        std::atomic<std::uint32_t> reply_length {8};
        std::atomic<std::uint32_t> chunks {0};
        std::atomic<std::uint64_t> bytes {0};
        std::atomic<std::uint32_t> errors {0};
        std::atomic<std::uint32_t> next_counter {0};
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
            // 计数器只在写成功后推进：端点被禁用时本次写会以 ESHUTDOWN 返回，
            // 若提前取号，重试后的数据流里就会出现空洞
            const std::uint32_t base = endpoint.next_counter.load();
            buffer.resize(length);
            for (std::size_t offset = 0; offset + 8 <= buffer.size(); offset += 8) {
                const std::uint32_t counter = base + static_cast<std::uint32_t>(offset / 8);
                const std::uint32_t check = counter ^ counter_magic;
                std::memcpy(buffer.data() + offset, &counter, sizeof(counter));
                std::memcpy(buffer.data() + offset + 4, &check, sizeof(check));
            }
            // 长度不是 8 的整数倍时，填充尾巴，客户端只校验完整的组
            for (std::size_t offset = buffer.size() - buffer.size() % 8; offset < buffer.size();
                 ++offset) {
                buffer[offset] = 0;
            }

            const ssize_t written = ::write(endpoint.fd, buffer.data(), buffer.size());
            if (written < 0) {
                if (errno == ESHUTDOWN) {
                    // alt 切换会临时禁用端点，等下一次 ENABLE 后继续
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                if (errno == EIDRM || errno == EINTR || errno == EAGAIN || errno == EBADMSG ||
                    errno == EPIPE) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                log("IN 端点 0x%02x 写失败: %s", endpoint.address, std::strerror(errno));
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            endpoint.chunks.fetch_add(1);
            endpoint.bytes.fetch_add(static_cast<std::uint64_t>(written));
            endpoint.next_counter.fetch_add(static_cast<std::uint32_t>(written / 8));
        }
    }

    void out_worker(EndpointState &endpoint) {
        // 请求长度取一包，避免 dummy_hcd 把多个 host URB 攒进同一个请求
        const std::size_t read_size = endpoint.max_packet > 0 ? endpoint.max_packet : 64;
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
                if (errno == EIDRM || errno == EINTR || errno == EAGAIN || errno == EBADMSG ||
                    errno == EPIPE) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                log("OUT 端点 0x%02x 读失败: %s", endpoint.address, std::strerror(errno));
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            endpoint.chunks.fetch_add(1);
            endpoint.bytes.fetch_add(static_cast<std::uint64_t>(received));
            if (received > 0) {
                // 主机侧 OUT 数据流逐字节 +1，跨 read 也要接得上
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

        // 端点 recipient 请求的 wIndex 已被内核重写成内部端点序号（1 开始）
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

    std::array<EndpointState, 2> endpoints {};
    std::atomic<bool> running {false};
};

}  // namespace usbipdcpp::e2e

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "用法: %s <functionfs 挂载目录>\n", argv[0]);
        return 2;
    }
    usbipdcpp::e2e::InterruptDevice device(argv[1]);
    // run() 只在 FunctionFS 出错/EOF 时返回，此时 worker 可能还阻塞在端点 IO，
    // 内核不会再把请求还回来（ep0 还开着，走不到 ffs_data_reset 的清理），
    // 正常析构会 join 挂死；测试进程直接退出，fd 和线程交给内核回收
    std::_Exit(device.run());
}
