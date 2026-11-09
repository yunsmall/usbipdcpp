/**
 * @file iso_device.cpp
 * @brief libusb e2e 的等时传输虚拟设备（项目虚拟设备框架实现）
 *
 * 链路：本进程（USB/IP 服务器）→ vhci 导入 → libusb_server 再导出 →
 * 第二个 vhci 导入 → e2e_libusb_iso_client。设备不经 UDC/gadget，
 * 直接在 USB/IP 协议层响应等时 URB。
 */

#include "iso_pattern.hpp"
#include "protocol.hpp"

#include "usbipdcpp/Server.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/usbipdcpp_core.h"
#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

namespace {

using namespace usbipdcpp;

constexpr std::uint8_t iso_attributes =
        static_cast<std::uint8_t>(EndpointAttributes::Isochronous) |
        static_cast<std::uint8_t>(IsoSyncType::NoSync);

volatile std::sig_atomic_t stop_requested = 0;

void on_signal(int) {
    stop_requested = 1;
}

void print_usage(const char *program) {
    std::fprintf(stderr, "用法: %s [-p 端口] [-b busid]\n", program);
}

/**
 * @brief 等时接口处理器
 *
 * alt 0 为零带宽（无端点），alt 1 是 1024 字节的 IN/OUT 对，alt 2 换成
 * 512 字节、bInterval=2 的另一个 IN 端点。数据面按包序填充/校验
 * iso_pattern.hpp 定义的格式，OUT 统计通过 vendor 控制请求读取。
 */
class IsoInterfaceHandler : public VirtualInterfaceHandler {
public:
    IsoInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
        VirtualInterfaceHandler(handle_interface, string_pool) {
    }

    void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                     std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                     int num_iso_packets, std::error_code &ec) override {
        auto *trx = GenericTransfer::from_handle(transfer.get());
        if (trx->iso_descriptors.empty()) {
            // 等时端点必须带包描述符；num_iso_packets 为 0 的 URB 在真实
            // 主机控制器上会被拒绝，这里按 EPIPE 应答
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
        }

        // 延迟用于验证 UNLINK：IN 方向按 vendor 请求设置的值推迟服务，
        // 客户端在这段时间内取消传输就能稳定命中挂起中的 URB
        {
            std::uint32_t length_sum = 0;
            for (const auto &iso: trx->iso_descriptors) {
                length_sum += iso.length;
            }
            SPDLOG_INFO("iso {} seq={} 端点=0x{:02x} 包数={} 缓冲={} 槽位和={} 首槽 offset={} length={}",
                        ep.is_in() ? "IN" : "OUT", seqnum, ep.address, num_iso_packets, transfer_buffer_length,
                        length_sum, trx->iso_descriptors.front().offset, trx->iso_descriptors.front().length);
        }
        const auto delay = ep.is_in() ? std::chrono::microseconds(in_delay_us.load(std::memory_order_relaxed))
                                      : std::chrono::microseconds(0);
        const bool is_in = ep.is_in();
        const int packet_count = num_iso_packets;

        device_handler->get_transfer_scheduler().submit(
                *responder, ep, EndpointAttributes::Isochronous, delay, seqnum, std::move(transfer),
                [this, is_in, packet_count](TransferResponder &current_responder, const UsbEndpoint &endpoint,
                                            std::uint32_t urb_seqnum, TransferHandle &&handle) {
                    if (is_in) {
                        process_in(current_responder, endpoint, urb_seqnum, packet_count, std::move(handle));
                    }
                    else {
                        process_out(current_responder, endpoint, urb_seqnum, packet_count, std::move(handle));
                    }
                });
    }

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override {
        const auto request = static_cast<e2e::proto::IsoRequest>(setup_packet.request);

        if (setup_packet.request_type == e2e::proto::iso_request_type_in && !setup_packet.is_out()) {
            data_type result;
            switch (request) {
                case e2e::proto::IsoRequest::get_info:
                    vector_append_to_le(result, e2e::proto::version);
                    break;
                case e2e::proto::IsoRequest::get_iso_stats:
                    vector_append_to_le(result, out_packets.load(std::memory_order_relaxed),
                                        out_bytes.load(std::memory_order_relaxed),
                                        out_errors.load(std::memory_order_relaxed),
                                        in_packets.load(std::memory_order_relaxed));
                    break;
                default:
                    responder->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
            }
            if (result.size() > setup_packet.length) {
                result.resize(setup_packet.length);
            }
            auto *trx = GenericTransfer::from_handle(transfer.get());
            trx->data = std::move(result);
            trx->actual_length = trx->data.size();
            trx->data_offset = 0;
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                    static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            return;
        }

        if (setup_packet.request_type == e2e::proto::iso_request_type_out && setup_packet.is_out()) {
            std::uint32_t status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
            switch (request) {
                case e2e::proto::IsoRequest::reset_iso_stats:
                    out_packets.store(0, std::memory_order_relaxed);
                    out_bytes.store(0, std::memory_order_relaxed);
                    out_errors.store(0, std::memory_order_relaxed);
                    in_packets.store(0, std::memory_order_relaxed);
                    next_in_packet.store(0, std::memory_order_relaxed);
                    break;
                case e2e::proto::IsoRequest::set_in_delay:
                    in_delay_us.store(static_cast<std::uint32_t>(setup_packet.value) * 1000u,
                                      std::memory_order_relaxed);
                    break;
                case e2e::proto::IsoRequest::set_fail_mask:
                    fail_mask.store(setup_packet.value, std::memory_order_relaxed);
                    break;
                default:
                    status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                    break;
            }
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(seqnum, status, 0));
            return;
        }

        // 非本设备定义的厂商请求：STALL，客户端用它验证错误路径
        responder->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }

private:
    void process_in(TransferResponder &current_responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                    int packet_count, TransferHandle transfer) {
        auto *trx = GenericTransfer::from_handle(transfer.get());
        std::uint32_t total = 0;
        const std::uint32_t base = next_in_packet.load(std::memory_order_relaxed);
        const std::uint32_t mask = fail_mask.load(std::memory_order_relaxed);
        for (int i = 0; i < packet_count; ++i) {
            auto &iso = trx->iso_descriptors[static_cast<std::size_t>(i)];
            // 内核 usb_submit_urb 把 iso_frame_desc 的 status 初始化为 -EXDEV，
            // 不清零的话主机侧音频等驱动会跳过所有包
            iso.status = 0;
            if (i < 32 && ((mask >> i) & 1u) != 0) {
                // 让指定槽位失败：服务端必须如实把该包状态和 0 长度回给客户端，
                // 同时 header actual_length 仍等于各包 actual_length 之和
                iso.status = static_cast<std::uint32_t>(-EPIPE);
                iso.actual_length = 0;
                continue;
            }
            if (iso.offset > trx->data.size() || iso.length > trx->data.size() - iso.offset) {
                // 主机给出的槽位越界：按空包应答，不能越界写
                iso.actual_length = 0;
                continue;
            }
            e2e::iso::fill_packet(trx->data.data() + iso.offset, iso.length,
                                  base + static_cast<std::uint32_t>(i));
            iso.actual_length = iso.length;
            total += iso.length;
        }
        next_in_packet.store(base + static_cast<std::uint32_t>(packet_count), std::memory_order_relaxed);
        in_packets.fetch_add(static_cast<std::uint32_t>(packet_count), std::memory_order_relaxed);

        SPDLOG_INFO("iso IN seq={} 完成 actual={}", seqnum, total);
        trx->actual_length = total;
        current_responder.submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), total, 0,
                static_cast<std::uint32_t>(packet_count), std::move(transfer)));
        device_handler->get_transfer_scheduler().on_urb_done(ep.address, seqnum);
    }

    void process_out(TransferResponder &current_responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                     int packet_count, TransferHandle transfer) {
        auto *trx = GenericTransfer::from_handle(transfer.get());
        std::uint32_t total = 0;
        std::uint32_t bad = 0;
        const std::uint32_t mask = fail_mask.load(std::memory_order_relaxed);
        for (int i = 0; i < packet_count; ++i) {
            auto &iso = trx->iso_descriptors[static_cast<std::size_t>(i)];
            iso.status = 0;
            if (i < 32 && ((mask >> i) & 1u) != 0) {
                iso.status = static_cast<std::uint32_t>(-EPIPE);
                iso.actual_length = 0;
                ++bad;
                continue;
            }
            if (iso.length > 0) {
                if (iso.offset > trx->data.size() || iso.length > trx->data.size() - iso.offset) {
                    iso.actual_length = 0;
                    ++bad;
                    continue;
                }
                if (!e2e::iso::verify_packet(trx->data.data() + iso.offset, iso.length)) {
                    ++bad;
                }
            }
            iso.actual_length = iso.length;
            total += iso.length;
        }
        out_packets.fetch_add(static_cast<std::uint32_t>(packet_count), std::memory_order_relaxed);
        out_bytes.fetch_add(total, std::memory_order_relaxed);
        out_errors.fetch_add(bad, std::memory_order_relaxed);

        SPDLOG_INFO("iso OUT seq={} 完成 actual={} 校验错误={}", seqnum, total, bad);
        trx->actual_length = total;
        current_responder.submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), total, 0,
                static_cast<std::uint32_t>(packet_count), std::move(transfer)));
        device_handler->get_transfer_scheduler().on_urb_done(ep.address, seqnum);
    }

    std::atomic<std::uint32_t> in_delay_us{0};
    std::atomic<std::uint32_t> out_packets{0};
    std::atomic<std::uint32_t> out_bytes{0};
    std::atomic<std::uint32_t> out_errors{0};
    std::atomic<std::uint32_t> in_packets{0};
    std::atomic<std::uint32_t> next_in_packet{0};
    std::atomic<std::uint32_t> fail_mask{0};
};

}  // namespace

int main(int argc, char **argv) {
    std::uint16_t port = 53264;
    std::string busid = "1-1";
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "-p") == 0 || std::strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        }
        else if ((std::strcmp(argv[i], "-b") == 0 || std::strcmp(argv[i], "--busid") == 0) && i + 1 < argc) {
            busid = argv[++i];
        }
        else {
            print_usage(argv[0]);
            return 2;
        }
    }

    spdlog::set_level(spdlog::level::info);

    StringPool string_pool;

    // 接口 0 三个 alt：零带宽 / 1024 字节 IN+OUT / 512 字节 IN（bInterval=2）
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = 0xFF,
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints =
                            {
                                    {},
                                    {
                                            UsbEndpoint{.address = 0x81,
                                                        .attributes = iso_attributes,
                                                        .max_packet_size = 1024,
                                                        .interval = 1},
                                            UsbEndpoint{.address = 0x02,
                                                        .attributes = iso_attributes,
                                                        .max_packet_size = 1024,
                                                        .interval = 1},
                                    },
                                    {
                                            UsbEndpoint{.address = 0x83,
                                                        .attributes = iso_attributes,
                                                        .max_packet_size = 512,
                                                        .interval = 2},
                                    },
                            },
            },
    };

    auto device = UsbDevice::make(busid, e2e::proto::vid, e2e::proto::pid_iso, std::move(interfaces), 1, 1, 0,
                                  "/usbipdcpp/e2e_vdev_iso", UsbSpeed::High);
    device->assign_interface_numbers();
    device->interfaces[0].with_handler<IsoInterfaceHandler>(string_pool);
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    // 等时响应（含可取消的延迟）交给设备级调度器，与 vudc 的帧服务语义一致
    device_handler->set_use_transfer_scheduler(true);
    device_handler->setup_interface_handlers();

    Server server;
    if (server.add_device(std::move(device)) != AddDeviceStatus::Added) {
        SPDLOG_ERROR("添加设备失败：busid {} 已被占用", busid);
        return 1;
    }

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    SPDLOG_INFO("等时虚拟设备已启动：端口 {}，busid {}", port, busid);
    while (stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    return 0;
}
