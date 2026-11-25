#include "usbipdcpp/LibusbHandler/LibusbTransferOperator.h"

#include <cstdlib>
#include <span>

#include <asio.hpp>
#include <libusb.h>
#include <spdlog/spdlog.h>

#include "usbipdcpp/LibusbHandler/LibusbDeviceHandler.h"
#include "usbipdcpp/TransferSource.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/utils/SmallVector.h"

using namespace usbipdcpp;

namespace usbipdcpp::detail {

libusb_transfer *LibusbTransferLM::create() {
    return libusb_alloc_transfer(0);
}

void LibusbTransferLM::destroy(libusb_transfer *p) {
    libusb_free_transfer(p);
}

void LibusbTransferReset::reset(libusb_transfer &t) {
    t.actual_length = 0;
    t.status = LIBUSB_TRANSFER_COMPLETED;
    // num_iso_packets 只被 libusb_fill_iso_transfer 设置，池只回收非等时
    // 传输（free_transfer_handle 按 num_iso_packets==0 判回池），复用对象
    // 本应恒为 0；显式清零把该不变式局部化，防止未来池策略变化（如等时
    // 也入池）时残留值被非等时路径误读
    t.num_iso_packets = 0;
}

} // namespace usbipdcpp::detail

void *LibusbTransferOperator::alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                                    const UsbIpHeaderBasic &header, const SetupPacket &setup_packet) {
    libusb_transfer *trx;
    if (num_iso_packets == 0) {
        trx = transfer_pool_.alloc();
        if (!trx) [[unlikely]] {
            trx = libusb_alloc_transfer(0);
            if (!trx) [[unlikely]] {
                return nullptr;
            }
        }
    }
    else {
        trx = libusb_alloc_transfer(num_iso_packets);
        if (!trx) [[unlikely]] {
            return nullptr;
        }
        // libusb_alloc_transfer 不会设置公开的 num_iso_packets 字段（文档 io.c L443 明确说明），
        // 必须用户自行赋值，否则 recv_transfer_data 中描述符读取循环读到垃圾值导致协议错位。
        trx->num_iso_packets = num_iso_packets;
    }

    std::size_t write_offset = (header.ep == 0) ? LIBUSB_CONTROL_SETUP_SIZE : 0;
    std::size_t actual_buffer_length = buffer_length + write_offset;

    // 零长度非控制传输是合法的（例如 bulk OUT length=0），malloc(0) 允许返回
    // nullptr；glibc 会返回可释放指针，但部分嵌入式 libc 返回 nullptr，会把
    // 合法请求当成内存不足断连。只放大分配大小，trx->length 仍记录实际长度
    trx->buffer = static_cast<unsigned char *>(
            malloc(actual_buffer_length > 0 ? actual_buffer_length : 1));
    if (!trx->buffer) [[unlikely]] {
        if (num_iso_packets == 0) {
            if (!transfer_pool_.free(trx))
                libusb_free_transfer(trx);
        }
        else {
            libusb_free_transfer(trx);
        }
        return nullptr;
    }
    trx->length = static_cast<int>(actual_buffer_length);
    return trx;
}

void LibusbTransferOperator::free_transfer_handle(void *handle) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    free(trx->buffer);
    if (trx->num_iso_packets == 0) {
        if (!transfer_pool_.free(trx))
            libusb_free_transfer(trx);
    }
    else {
        libusb_free_transfer(trx);
    }
}

libusb_transfer *LibusbTransferOperator::alloc_transfer() {
    auto *trx = transfer_pool_.alloc();
    if (!trx) [[unlikely]] {
        trx = libusb_alloc_transfer(0);
    }
    if (trx) {
        // buffer 由借用方（分块槽位）在 fill 时设置；清空防止复用上一个
        // 用户留下的悬垂指针（普通路径的 buffer 归本类所有）
        trx->buffer = nullptr;
    }
    return trx;
}

void LibusbTransferOperator::free_transfer(libusb_transfer *trx) {
    if (!trx) {
        return;
    }
    // buffer 属于借用方的分块槽位，这里只归还 transfer 对象；置空防止以后
    // 被误用 free_transfer_handle 时 free 掉槽位内存
    trx->buffer = nullptr;
    if (!transfer_pool_.free(trx)) {
        libusb_free_transfer(trx);
    }
}

std::size_t LibusbTransferOperator::get_actual_length(void *handle) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    return trx->actual_length;
}

bool LibusbTransferOperator::transfer_is_in(void *handle) {
    auto *trx = static_cast<libusb_transfer *>(handle);

    if (trx->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        // libusb 控制传输的端点地址为 0，方向由 setup 包的 bmRequestType 决定。
        const auto *setup = libusb_control_transfer_get_setup(trx);
        return (setup->bmRequestType & LIBUSB_ENDPOINT_IN) != 0;
    }

    return (trx->endpoint & LIBUSB_ENDPOINT_IN) != 0;
}

UsbIpIsoPacketDescriptor LibusbTransferOperator::get_iso_descriptor(void *handle, int index) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    auto &iso = trx->iso_packet_desc[index];
    // libusb 的 iso 包在 buffer 中连续存放，offset = 前面所有包的 length 累加
    unsigned offset = 0;
    for (int i = 0; i < index; i++) {
        offset += trx->iso_packet_desc[i].length;
    }
    return UsbIpIsoPacketDescriptor{
            .offset = offset,
            .length = iso.length,
            .actual_length = iso.actual_length,
            .status = static_cast<std::uint32_t>(LibusbDeviceHandler::trxstat2error(iso.status)),
    };
}

void LibusbTransferOperator::set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    auto &iso = trx->iso_packet_desc[index];
    iso.status = LibusbDeviceHandler::error2trxstat(desc.status);
    iso.actual_length = desc.actual_length;
    iso.length = desc.length;
}

void LibusbTransferOperator::send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                std::error_code &ec) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    if (trx->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS && trx->num_iso_packets > 0) {
        SmallVector<asio::const_buffer, 130> buffers;
        SmallVector<decltype(UsbIpIsoPacketDescriptor{}.to_bytes()), 130> desc_bytes;
        // offset: buffer 中的包槽位偏移（pkt.length 步长），同时用于数据读取和描述符 offset 字段。
        //   槽位大小由客户端 CMD_SUBMIT 的描述符 length 决定，必须按 pkt.length 步进而非 actual_length，
        //   否则 vhci 会把包 N 的数据错误地写入包 N-1 的槽位中。
        // length 已由协议层按方向算好（transfer_is_in 查询，OUT 恒传 0，
        // 见 RetSubmit/CmdSubmit 的 to_socket），这里只按 length > 0 决定
        // 是否发数据，不再自行判断方向
        bool need_to_send_buffer = (length > 0);
        std::uint32_t offset = 0;
        for (int i = 0; i < trx->num_iso_packets; i++) {
            auto &pkt = trx->iso_packet_desc[i];
            if (need_to_send_buffer)
                buffers.push_back(asio::buffer(trx->buffer + offset, pkt.actual_length));
            UsbIpIsoPacketDescriptor desc{
                    .offset = offset,
                    .length = pkt.length,
                    .actual_length = pkt.actual_length,
                    .status = static_cast<std::uint32_t>(LibusbDeviceHandler::trxstat2error(pkt.status)),
            };
            desc_bytes.push_back(desc.to_bytes());
            offset += pkt.length;
        }
        for (auto &bytes: desc_bytes) {
            buffers.push_back(asio::buffer(bytes));
        }
        asio::write(sock, buffers, ec);
    }
    else if (length > 0) {
        auto *buf = trx->buffer + (trx->type == LIBUSB_TRANSFER_TYPE_CONTROL ? LIBUSB_CONTROL_SETUP_SIZE : 0);
        asio::write(sock, asio::buffer(buf, length), ec);
    }
}

void LibusbTransferOperator::recv_transfer_data(void *handle, TransferSource &source, std::error_code &ec) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    if (source.remaining() > 0) {
        // 控制传输 buffer 前 8 字节留给 setup 包，由后续 receive_urb 填入；
        // 此处从偏移 8 开始读取数据阶段内容。
        // trx->type 此时尚未设置，不能用来判断传输类型；改用 buffer 长度判断：
        // alloc 时控制传输多加了 LIBUSB_CONTROL_SETUP_SIZE，
        // 因此 trx->length > payload 长度说明 buffer 包含 setup 前缀
        bool is_control = (static_cast<std::size_t>(trx->length) > source.remaining());
        auto *buf = trx->buffer + (is_control ? LIBUSB_CONTROL_SETUP_SIZE : 0);
        source.read({reinterpret_cast<std::byte *>(buf), source.remaining()}, ec);
        if (ec) {
            return;
        }
    }

    // 校验并读取 ISO 描述符：length/actual_length 是客户端可控字段，必须验证
    // 才能写入 libusb transfer——libusb 按 length 从缓冲区读写数据，length
    // 总和超过缓冲区大小（trx->length，ISO 传输不含 setup 前缀）会越界读写
    // （堆溢出），actual_length 超过 length 则包数据溢出。不合法拒绝整个
    // 命令（调用方 ec 非空时断开连接）
    if (trx->num_iso_packets <= 0) {
        return;
    }
    SmallVector<UsbIpIsoPacketDescriptor, 16> descriptors;
    descriptors.resize(static_cast<std::size_t>(trx->num_iso_packets));
    source.read_iso_descriptors(std::span(descriptors), ec);
    if (ec) {
        return;
    }
    std::uint64_t total_length = 0;
    for (int i = 0; i < trx->num_iso_packets; i++) {
        const auto &iso_desc = descriptors[static_cast<std::size_t>(i)];
        if (iso_desc.actual_length > iso_desc.length ||
            total_length + iso_desc.length > static_cast<std::uint64_t>(trx->length)) [[unlikely]] {
            SPDLOG_ERROR("ISO 描述符非法：包 {} length={} actual_length={}（剩余缓冲 {}）",
                         i, iso_desc.length, iso_desc.actual_length,
                         static_cast<std::uint64_t>(trx->length) - total_length);
            ec = std::make_error_code(std::errc::invalid_argument);
            return;
        }
        total_length += iso_desc.length;
        // 客户端描述符里的 offset 被丢弃：libusb 的 iso_packet_desc 没有 offset
        // 字段（buffer 布局隐式连续），发送端 offset 由 send_transfer_data 按
        // pkt.length 累加自行计算，恶意 offset 无法影响服务器的数据定位
        set_iso_descriptor(handle, i, iso_desc);
    }
}
