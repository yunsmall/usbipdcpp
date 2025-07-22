#include "libusb_handler/LibusbDeviceHandler.h"


#include "Session.h"
#include "protocol.h"
#include "SetupPacket.h"
#include "constant.h"
#include "endpoint.h"

usbipdcpp::LibusbDeviceHandler::LibusbDeviceHandler(UsbDevice &handle_device, libusb_device_handle *native_handle):
    DeviceHandlerBase(handle_device), native_handle(native_handle) {
}

usbipdcpp::LibusbDeviceHandler::~LibusbDeviceHandler() {
}

void usbipdcpp::LibusbDeviceHandler::handle_control_urb(Session &session,
                                                        std::uint32_t seqnum,
                                                        const UsbEndpoint &ep,
                                                        std::uint32_t transfer_flags,
                                                        std::uint32_t transfer_buffer_length,
                                                        const SetupPacket &setup_packet, const data_type &req,
                                                        std::error_code &ec) {
    //auto tweak_ret = -1;
    auto tweak_ret = tweak_special_requests(setup_packet);
    //尝试执行特殊操作再对usb进行控制
    if (tweak_ret < 0) {
        SPDLOG_DEBUG("控制传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out?"Out":"In", ep.address);

        auto transfer = libusb_alloc_transfer(0);
        unsigned char *buffer = new unsigned char[LIBUSB_CONTROL_SETUP_SIZE + transfer_buffer_length]{0};
        if (setup_packet.is_out()) {
            memcpy(buffer + LIBUSB_CONTROL_SETUP_SIZE, req.data(), req.size());
        }
        libusb_fill_control_setup(buffer, setup_packet.request_type, setup_packet.request, setup_packet.value,
                                  setup_packet.index, setup_packet.length);

        libusb_callback_args *callback_args = new libusb_callback_args{
                .handler = *this,
                .session = session,
                .seqnum = seqnum,
                .is_out = setup_packet.is_out()
        };

        libusb_fill_control_transfer(transfer, native_handle, buffer, LibusbDeviceHandler::transfer_callback,
                                     callback_args,
                                     timeout_milliseconds);
        //将usbio transder flag转换成libusb的flags
        transfer->flags = get_libusb_transfer_flags(transfer_flags);

        //小修改传输flag
        if (!setup_packet.is_out()) {
            transfer->flags &= LIBUSB_TRANSFER_SHORT_NOT_OK;
        }

        {
            std::lock_guard lock(transferring_data_mutex);
            transferring_data[seqnum] = transfer;
        }

        auto ret = libusb_submit_transfer(transfer);
        if (ret < 0) {
            SPDLOG_ERROR("传输给设备失败");
            libusb_free_transfer(transfer);
        }
        return;
    }
    else {
        SPDLOG_INFO("拦截了控制包：{}", seqnum);
        //SPDLOG_INFO("拦截了包: {}", setup_packet.to_string());
        //返回空包
        session.submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                seqnum,
                static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                0,
                0,
                {},
                {}
                ));
        return;
    }
}

void usbipdcpp::LibusbDeviceHandler::handle_bulk_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                                          UsbInterface &interface,
                                                          std::uint32_t transfer_flags,
                                                          std::uint32_t transfer_buffer_length,
                                                          const data_type &out_data, std::error_code &ec) {
    bool is_out = !ep.is_in();

    SPDLOG_DEBUG("块传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out?"Out":"In", ep.address);
    auto transfer = libusb_alloc_transfer(0);
    auto *buffer = new unsigned char[transfer_buffer_length]{0};
    if (is_out) {
        memcpy(buffer, out_data.data(), out_data.size());
    }
    auto *callback_args = new libusb_callback_args{
            .handler = *this,
            .session = session,
            .seqnum = seqnum,
            .is_out = is_out
    };
    libusb_fill_bulk_transfer(transfer, native_handle, ep.address, buffer, transfer_buffer_length,
                              LibusbDeviceHandler::transfer_callback,
                              callback_args,
                              timeout_milliseconds);
    transfer->flags = get_libusb_transfer_flags(transfer_flags);
    if (is_out) {
        transfer->flags &= LIBUSB_TRANSFER_ADD_ZERO_PACKET;
    }

    {
        std::lock_guard lock(transferring_data_mutex);
        transferring_data[seqnum] = transfer;
    }

    auto err = libusb_submit_transfer(transfer);
    if (err < 0) {
        SPDLOG_ERROR("块传输失败，{}", libusb_strerror(err));
        libusb_free_transfer(transfer);
    }
}

void usbipdcpp::LibusbDeviceHandler::handle_interrupt_transfer(Session &session, std::uint32_t seqnum,
                                                               const UsbEndpoint &ep,
                                                               UsbInterface &interface,
                                                               std::uint32_t transfer_flags,
                                                               std::uint32_t transfer_buffer_length,
                                                               const data_type &out_data, std::error_code &ec) {
    bool is_out = !ep.is_in();

    SPDLOG_DEBUG("中断传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out?"Out":"In", ep.address);
    auto transfer = libusb_alloc_transfer(0);
    auto *buffer = new unsigned char[transfer_buffer_length];
    if (is_out) {
        memcpy(buffer, out_data.data(), out_data.size());
    }
    auto *callback_args = new libusb_callback_args{
            .handler = *this,
            .session = session,
            .seqnum = seqnum,
            .is_out = is_out
    };
    libusb_fill_interrupt_transfer(transfer, native_handle, ep.address, buffer, transfer_buffer_length,
                                   LibusbDeviceHandler::transfer_callback,
                                   callback_args,
                                   timeout_milliseconds);
    transfer->flags = get_libusb_transfer_flags(transfer_flags);

    {
        std::lock_guard lock(transferring_data_mutex);
        transferring_data[seqnum] = transfer;
    }

    auto err = libusb_submit_transfer(transfer);
    if (err < 0) {
        SPDLOG_ERROR("中断传输失败，{}", libusb_strerror(err));
        libusb_free_transfer(transfer);
    }
}

void usbipdcpp::LibusbDeviceHandler::handle_unlink_seqnum(std::uint32_t seqnum) {
    int err = 0;
    {
        std::shared_lock lock(transferring_data_mutex);
        if (transferring_data.contains(seqnum)) {
            err = libusb_cancel_transfer(transferring_data.at(seqnum));
        }
    }
    if (err) {
        SPDLOG_ERROR("libusb_cancel_transfer failed: {}", libusb_strerror(err));
    }
}

void usbipdcpp::LibusbDeviceHandler::stop_transfer() {
}

void usbipdcpp::LibusbDeviceHandler::handle_isochronous_transfer(Session &session,
                                                                 std::uint32_t seqnum,
                                                                 const UsbEndpoint &ep,
                                                                 UsbInterface &interface,
                                                                 std::uint32_t transfer_flags,
                                                                 std::uint32_t transfer_buffer_length,
                                                                 const data_type &req,
                                                                 const std::vector<UsbIpIsoPacketDescriptor> &
                                                                 iso_packet_descriptors, std::error_code &ec) {

    bool is_out = !ep.is_in();
    SPDLOG_DEBUG("同步传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out?"Out":"In", ep.address);

    auto transfer = libusb_alloc_transfer(0);
    auto *buffer = new unsigned char[transfer_buffer_length];
    if (is_out) {
        memcpy(buffer, req.data(), req.size());
    }
    auto *callback_args = new libusb_callback_args{
            .handler = *this,
            .session = session,
            .seqnum = seqnum,
            .is_out = is_out
    };

    libusb_fill_iso_transfer(
            transfer, native_handle, ep.address, buffer, transfer_buffer_length, iso_packet_descriptors.size(),
            LibusbDeviceHandler::transfer_callback, callback_args, timeout_milliseconds);

    for (int i = 0; i < iso_packet_descriptors.size(); i++) {
        auto &libusb_iso_desc_i = transfer->iso_packet_desc[i];
        /* ignore iso->offset; */
        libusb_iso_desc_i.status = error2trxstat(iso_packet_descriptors[i].status);
        libusb_iso_desc_i.actual_length = iso_packet_descriptors[i].actual_length;
        libusb_iso_desc_i.length = iso_packet_descriptors[i].length;
    }

    transfer->flags = get_libusb_transfer_flags(transfer_flags);

    {
        std::lock_guard lock(transferring_data_mutex);
        transferring_data[seqnum] = transfer;
    }

    auto err = libusb_submit_transfer(transfer);
    if (err < 0) {
        SPDLOG_ERROR("同步传输失败，{}", libusb_strerror(err));
        libusb_free_transfer(transfer);
    }


}

void usbipdcpp::LibusbDeviceHandler::transfer_callback(libusb_transfer *trx) {
    auto &callback_arg = *static_cast<libusb_callback_args *>(
        trx->
        user_data);
    // std::error_code ec;
    switch (trx->status) {
        case LIBUSB_TRANSFER_COMPLETED:
            /* OK */
            break;
        case LIBUSB_TRANSFER_ERROR:
            if (!(trx->flags & LIBUSB_TRANSFER_SHORT_NOT_OK)) {
                dev_err(libusb_get_device(trx->dev_handle),
                        "error on endpoint {}", trx->endpoint);
            }
            else {
                //Tweaking status to complete as we received data, but all
                trx->status = LIBUSB_TRANSFER_COMPLETED;
            }
            break;
        case LIBUSB_TRANSFER_CANCELLED:
            dev_info(libusb_get_device(trx->dev_handle),
                     "unlinked by a call to usb_unlink_urb()");
            break;
        case LIBUSB_TRANSFER_STALL:
            dev_err(libusb_get_device(trx->dev_handle),
                    "endpoint {} is stalled", trx->endpoint);
            break;
        case LIBUSB_TRANSFER_NO_DEVICE:
            dev_info(libusb_get_device(trx->dev_handle), "device removed?");
            break;
        default:
            dev_warn(libusb_get_device(trx->dev_handle),
                     "urb completion with unknown status {}",
                     (int)trx->status);
            break;
    }
    SPDLOG_DEBUG("libusb传输了{}个字节", trx->actual_length);

    std::vector<UsbIpIsoPacketDescriptor> iso_packet_descriptors{};
    auto unlink_found = callback_arg.session.get_unlink_seqnum(callback_arg.seqnum);
    if (!std::get<0>(unlink_found)) {
        //发送ret_submit
        data_type received_data;
        if (!callback_arg.is_out) {
            if (trx->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
                received_data = {
                        trx->buffer + LIBUSB_CONTROL_SETUP_SIZE,
                        trx->buffer + LIBUSB_CONTROL_SETUP_SIZE + trx->actual_length
                };
                assert(received_data.size()==trx->actual_length);
            }
            else if (trx->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
                iso_packet_descriptors.resize(trx->num_iso_packets);
                size_t iso_actual_length = 0;
                for (int i = 0; i < trx->num_iso_packets; i++) {
                    auto &iso_packet = trx->iso_packet_desc[i];
                    iso_actual_length += iso_packet.actual_length;
                }
                received_data.resize(iso_actual_length, 0);
                size_t received_data_offset = 0;
                size_t trx_buffer_offset = 0;
                for (int i = 0; i < trx->num_iso_packets; i++) {
                    auto &iso_packet = trx->iso_packet_desc[i];
                    std::memcpy(received_data.data() + received_data_offset, trx->buffer + trx_buffer_offset,
                                iso_packet.actual_length);
                    iso_packet_descriptors[i].offset = received_data_offset;
                    iso_packet_descriptors[i].length = iso_packet.actual_length;
                    iso_packet_descriptors[i].actual_length = iso_packet.actual_length;
                    iso_packet_descriptors[i].status = trxstat2error(iso_packet.status);

                    received_data_offset += iso_packet.actual_length;
                    trx_buffer_offset += iso_packet.length;
                }
            }
            else {
                received_data = {
                        trx->buffer,
                        trx->buffer + trx->actual_length
                };
                assert(received_data.size()==trx->actual_length);
            }
        }


        callback_arg.session.submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                        callback_arg.seqnum,
                        trxstat2error(trx->status),
                        0,
                        trx->num_iso_packets,
                        received_data,
                        iso_packet_descriptors
                        )
                );
    }
    else {

        auto unlink_seqnum = std::get<1>(unlink_found);
        //先把这个包删了，再去发送
        {
            std::lock_guard lock(callback_arg.handler.transferring_data_mutex);
            callback_arg.handler.transferring_data.erase(unlink_seqnum);
        }

        //发送ret_unlink
        callback_arg.session.submit_ret_unlink_and_then_remove_seqnum_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                        unlink_seqnum,
                        trxstat2error(trx->status)
                        ),
                callback_arg.seqnum
                );
    }

    //清理数据
    delete trx->buffer;
    delete &callback_arg;
    libusb_free_transfer(trx);
}
