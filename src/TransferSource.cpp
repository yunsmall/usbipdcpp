#include "usbipdcpp/TransferSource.h"

#include <algorithm>
#include <array>

#include "usbipdcpp/utils/SmallVector.h"

namespace usbipdcpp {

void TransferSource::drain(std::error_code &ec) {
    std::error_code skip_ec;
    if (remaining() != 0) {
        skip_remaining(skip_ec);
        if (skip_ec && !ec) {
            ec = skip_ec;
        }
    }
    auto iso_left = iso_packets_remaining();
    if (iso_left > 0 && !ec) {
        // 描述符数量由协议层限制（<=1024），常见情况用内联存储避免小分配
        SmallVector<UsbIpIsoPacketDescriptor, 16> scratch(static_cast<std::size_t>(iso_left));
        read_iso_descriptors(std::span(scratch.data(), scratch.size()), ec);
    }
}

SocketTransferSource::SocketTransferSource(asio::ip::tcp::socket &sock, std::size_t payload_length,
                                           int num_iso_packets) :
    sock_(sock), payload_remaining_(payload_length), num_iso_packets_(num_iso_packets) {
}

std::size_t SocketTransferSource::read(std::span<std::byte> dst, std::error_code &ec) {
    auto n = std::min(dst.size(), payload_remaining_);
    if (n == 0) {
        return 0;
    }
    asio::read(sock_, asio::buffer(dst.data(), n), ec);
    if (ec) {
        return 0;
    }
    payload_remaining_ -= n;
    return n;
}

std::size_t SocketTransferSource::read_some(std::span<std::byte> dst, std::error_code &ec) {
    auto n = std::min(dst.size(), payload_remaining_);
    if (n == 0) {
        return 0;
    }
    auto got = sock_.read_some(asio::buffer(dst.data(), n), ec);
    if (ec) {
        return 0;
    }
    payload_remaining_ -= got;
    return got;
}

void SocketTransferSource::read_iso_descriptors(std::span<UsbIpIsoPacketDescriptor> out, std::error_code &ec) {
    auto count = std::min(out.size(), static_cast<std::size_t>(num_iso_packets_ - iso_read_));
    for (std::size_t i = 0; i < count; ++i) {
        out[i].from_socket(sock_, ec);
        if (ec) {
            return;
        }
        ++iso_read_;
    }
}

void SocketTransferSource::skip_remaining(std::error_code &ec) {
    std::array<std::byte, 4096> scratch{};
    while (payload_remaining_ > 0) {
        auto n = std::min(scratch.size(), payload_remaining_);
        asio::read(sock_, asio::buffer(scratch.data(), n), ec);
        if (ec) {
            return;
        }
        payload_remaining_ -= n;
    }
}

void SocketTransferSource::mark_consumed(std::size_t n) noexcept {
    payload_remaining_ -= std::min(n, payload_remaining_);
}

} // namespace usbipdcpp
