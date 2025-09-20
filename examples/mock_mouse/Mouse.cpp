#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "DeviceHandler/SimpleVirtualDeviceHandler.h"
#include "InterfaceHandler/HidVirtualInterfaceHandler.h"
#include "Server.h"
#include "Session.h"

using namespace usbipdcpp;

class MockMouseInterfaceHandler : public HidVirtualInterfaceHandler {
public:
    MockMouseInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool);

    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                   std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                   const data_type &out_data,
                                   std::error_code &ec) override;
    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;

    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override;

    std::uint8_t request_get_interface(std::uint32_t *p_status) override;

    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;

    std::uint16_t request_get_status(std::uint32_t *p_status) override;

    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override;

    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;

    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override;

    std::uint16_t get_report_descriptor_size() override;

    data_type get_report_descriptor() override;


    void handle_non_hid_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                 std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                                 const SetupPacket &setup_packet,
                                                 const data_type &out_data, std::error_code &ec) override;
    data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                 std::uint32_t *p_status) override;
    void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length, const data_type &data,
                            std::uint32_t *p_status) override;
    data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                               std::uint32_t *p_status) override;
    void request_set_idle(std::uint8_t speed, std::uint32_t *p_status) override;


    data_type report_descriptor{
            // HID报告描述符 - 5按钮鼠标带滚轮
            0x05, 0x01, // Usage Page (Generic Desktop)
            0x09, 0x02, // Usage (Mouse)
            0xA1, 0x01, // Collection (Application)
            0x09, 0x01, //   Usage (Pointer)
            0xA1, 0x00, //   Collection (Physical)

            // 按钮区域 (5个按键 + 3位填充)
            0x05, 0x09, //   Usage Page (Button)
            0x19, 0x01, //   Usage Minimum (Button 1)
            0x29, 0x05, //   Usage Maximum (Button 5)
            0x15, 0x00, //   Logical Minimum (0)
            0x25, 0x01, //   Logical Maximum (1)
            0x95, 0x05, //   Report Count (5)  // 5个按钮
            0x75, 0x01, //   Report Size (1)   // 每个按钮1位
            0x81, 0x02, //   Input (Data,Var,Abs)

            0x95, 0x01, //   Report Count (1)  // 填充3位
            0x75, 0x03, //   Report Size (3)
            0x81, 0x03, //   Input (Const,Var,Abs) // 常量填充

            // 光标移动区域 (X/Y轴)
            0x05, 0x01, //   Usage Page (Generic Desktop)
            0x09, 0x30, //   Usage (X)
            0x09, 0x31, //   Usage (Y)
            0x15, 0x81, //   Logical Minimum (-127)
            0x25, 0x7F, //   Logical Maximum (127)
            0x75, 0x08, //   Report Size (8)   // 8位分辨率
            0x95, 0x02, //   Report Count (2)  // X和Y两个轴
            0x81, 0x06, //   Input (Data,Var,Rel) // 相对坐标

            // 滚轮区域
            0x09, 0x38, //   Usage (Wheel)
            0x15, 0x81, //   Logical Minimum (-127)
            0x25, 0x7F, //   Logical Maximum (127)
            0x75, 0x08, //   Report Size (8)
            0x95, 0x01, //   Report Count (1)
            0x81, 0x06, //   Input (Data,Var,Rel) // 相对滚动量

            0xC0, //   End Collection (Physical)
            0xC0, // End Collection (Application)

    };
    /*
报告描述符说明：
按钮部分:

5个独立按钮 (左键、右键、中键、侧键1、侧键2)
每个按钮占用1位 (0=释放, 1=按下)
用3位常量填充，使字节对齐
光标移动:

X/Y轴相对移动量
8位有符号整数 (-127到+127)
相对坐标模式 (REL)
滚轮:

垂直滚动量
8位有符号整数 (-127到+127)
中键按下时作为按钮，滚动时作为滚轮
报告格式：
[字节0] | 按钮状态 (bit0-4) + 填充 (bit5-7)
[字节1] | X轴移动量 (相对值)
[字节2] | Y轴移动量 (相对值)
[字节3] | 滚轮移动量 (相对值)
*/

    struct State {
        bool left_pressed = false;
        bool right_pressed = false;
        bool middle_pressed = false;
        bool side_pressed = false;
        bool extra_pressed = false;

        std::int8_t wheel_vertical = 0;

        std::int8_t move_horizontal = 0;
        std::int8_t move_vertical = 0;

        bool operator==(const State &) const = default;
    };

    std::atomic_bool should_immediately_stop = false;

    State last_state;
    State current_state;
    std::condition_variable state_cv;
    std::mutex state_mutex;


    std::deque<std::uint32_t> int_req_queue;
    std::shared_mutex int_req_queue_mutex;

    std::thread send_thread;
    std::atomic<std::int16_t> idle_speed = 1;
};

void MockMouseInterfaceHandler::on_new_connection(Session &current_session, error_code &ec) {
    session = &current_session;
    should_immediately_stop = false;
    last_state = State{};
    current_state = State{};
    int_req_queue.clear();
    idle_speed = 1;

    send_thread = std::thread([this]() {
        while (!should_immediately_stop) {
            //等待状态变化通知
            std::unique_lock lock(state_mutex);
            state_cv.wait(lock, [this]() {
                return current_state != last_state || should_immediately_stop;
            });
            if (should_immediately_stop)
                break;
            //清空所有发来的中断传输
            while (true) {
                std::uint32_t seqnum{};
                bool has_seqnum = false;
                {
                    std::lock_guard lock(int_req_queue_mutex);
                    auto at_begin = int_req_queue.begin();
                    if (at_begin != int_req_queue.end()) {
                        seqnum = *at_begin;
                        int_req_queue.pop_front();
                        has_seqnum = true;
                    }
                }

                //如果有值，则发送
                if (has_seqnum) {
                    data_type ret(4, 0);
                    {
                        if (current_state.left_pressed) {
                            ret[0] |= 0b00000001;
                        }
                        if (current_state.right_pressed) {
                            ret[0] |= 0b00000010;
                        }
                        if (current_state.middle_pressed) {
                            ret[0] |= 0b00000100;
                        }
                        if (current_state.side_pressed) {
                            ret[0] |= 0b00001000;
                        }
                        if (current_state.extra_pressed) {
                            ret[0] |= 0b00010000;
                        }
                        ret[1] = current_state.move_horizontal;
                        ret[2] = current_state.move_vertical;
                        ret[3] = current_state.wheel_vertical;
                    }

                    if (!should_immediately_stop) {
                        session->submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, ret)
                                );
                    }
                }
                //没值了就继续等待
                else {
                    break;
                }
            }
            last_state = current_state;
        }
    });
}

void MockMouseInterfaceHandler::on_disconnection(error_code &ec) {
    session = nullptr;
    should_immediately_stop = true;
    state_cv.notify_all();
    send_thread.join();
}

MockMouseInterfaceHandler::MockMouseInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool):
    HidVirtualInterfaceHandler(handle_interface, string_pool) {
}

void MockMouseInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                          std::uint32_t transfer_flags,
                                                          std::uint32_t transfer_buffer_length,
                                                          const data_type &out_data,
                                                          std::error_code &ec) {
    if (ep.is_in()) {
        //往队列里添加东西
        std::lock_guard lock(int_req_queue_mutex);
        int_req_queue.emplace_back(seqnum);
    }
    else {
        {
            std::lock_guard lock(int_req_queue_mutex);
            int_req_queue.clear();
        }
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum)
                );
    }

}

void MockMouseInterfaceHandler::request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_clear_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void MockMouseInterfaceHandler::request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                               std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_endpoint_clear_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

std::uint8_t MockMouseInterfaceHandler::request_get_interface(std::uint32_t *p_status) {
    return 0;
}

void MockMouseInterfaceHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting != 0) {
        SPDLOG_WARN("unhandled request_set_interface");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

std::uint16_t MockMouseInterfaceHandler::request_get_status(std::uint32_t *p_status) {
    return 0;
}

std::uint16_t MockMouseInterfaceHandler::request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) {
    return 0;
}

void MockMouseInterfaceHandler::request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_set_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void MockMouseInterfaceHandler::request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                             std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_endpoint_set_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

std::uint16_t MockMouseInterfaceHandler::get_report_descriptor_size() {
    return report_descriptor.size();
}

data_type MockMouseInterfaceHandler::get_report_descriptor() {
    return report_descriptor;

}

void MockMouseInterfaceHandler::handle_non_hid_request_type_control_urb(std::uint32_t seqnum,
                                                                        const UsbEndpoint &ep,
                                                                        std::uint32_t transfer_flags,
                                                                        std::uint32_t transfer_buffer_length,
                                                                        const SetupPacket &setup_packet,
                                                                        const data_type &out_data,
                                                                        std::error_code &ec) {
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_no_iso(seqnum, {}));
}

data_type MockMouseInterfaceHandler::request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                                        std::uint32_t *p_status) {
    auto report_type = static_cast<HIDReportType>(type);
    if (report_type == HIDReportType::Input) {
        std::unique_lock lock(state_mutex);
        data_type result;
        switch (report_id) {
            case 0: {
                vector_append_to_net(result, (std::uint8_t) current_state.left_pressed);
                break;
            }
            case 1: {
                vector_append_to_net(result, (std::uint8_t) current_state.right_pressed);
                break;
            }
            case 2: {
                vector_append_to_net(result, (std::uint8_t) current_state.middle_pressed);
                break;
            }
            case 3: {
                vector_append_to_net(result, (std::uint8_t) current_state.side_pressed);
                break;
            }
            case 4: {
                vector_append_to_net(result, (std::uint8_t) current_state.extra_pressed);
                break;
            }
            case 5: {
                vector_append_to_net(result, (std::uint8_t) current_state.wheel_vertical);
                break;
            }
            case 6: {
                vector_append_to_net(result, (std::uint8_t) current_state.wheel_vertical);
                break;
            }
            case 7: {
                vector_append_to_net(result, (std::uint8_t) current_state.move_horizontal);
                break;
            }
            case 8: {
                vector_append_to_net(result, (std::uint8_t) current_state.move_vertical);
                break;
            }
            default: {
                SPDLOG_WARN("unhandled request_get_report");
                *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
            }
        }
        return result;
    }
    SPDLOG_WARN("unhandled request_get_report");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    return {};
}

void MockMouseInterfaceHandler::request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                                   const data_type &data, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_set_report");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

data_type MockMouseInterfaceHandler::request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                                      std::uint32_t *p_status) {
    data_type result;
    vector_append_to_net(result, (std::uint16_t) idle_speed);
    return result;
}

void MockMouseInterfaceHandler::request_set_idle(std::uint8_t speed, std::uint32_t *p_status) {
    idle_speed = speed;
}

int main() {
    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(
                        ClassCode::HID),
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x03,
                                    // 8 bytes
                                    .max_packet_size = 8,
                                    // Interrupt
                                    .interval = 10
                            }
                    }
            }
    };
    interfaces[0].with_handler<MockMouseInterfaceHandler>(string_pool);


    auto mock_mouse = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_mouse",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Low),
            .vendor_id = 0x1234,
            .product_id = 0x5678,
            .device_bcd = 0xabcd,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_default_ep0_in(),
            .ep0_out = UsbEndpoint::get_default_ep0_out(),
    });
    mock_mouse->with_handler<SimpleVirtualDeviceHandler>(string_pool);

    MockMouseInterfaceHandler &mouse_interface_handler = *std::dynamic_pointer_cast<MockMouseInterfaceHandler>(
            mock_mouse->interfaces[0].handler);


    Server server;
    server.add_device(std::move(mock_mouse));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 54324};

    server.start(endpoint);

    std::chrono::seconds run_time{10};
    SPDLOG_INFO("Start turning over left button");
    for (int i = 0; i < std::chrono::duration_cast<std::chrono::seconds>(run_time).count(); i++) {
        {
            std::unique_lock lock(mouse_interface_handler.state_mutex);
            mouse_interface_handler.current_state.left_pressed = !mouse_interface_handler.current_state.left_pressed;
            mouse_interface_handler.state_cv.notify_one();
        }
        SPDLOG_INFO("Turn over left button");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // system("pause");

    server.stop();

    return 0;
}
