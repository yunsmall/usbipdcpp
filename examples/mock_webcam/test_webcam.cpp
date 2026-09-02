// mock_webcam（UVC 摄像头 + UAC 麦克风复合设备）装配层测试：两个功能 helper
// 顺序装配到同一 UsbDevice 的布局正确性（接口号、IAD 归属、端点唯一性、handler
// 类型）。各 handler 的行为已由 test_uvc_handler / test_uac_speaker_stream 覆盖

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <vector>

#include "usbipdcpp/Device.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/UvcVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"
#include "usbipdcpp/virtual_device/video_sources/ColorBarSource.h"

using namespace usbipdcpp;

namespace {

// 与 mock_webcam_main 相同的装配流程：UVC(VC/VS) 接口 0-1 + UAC 麦克风(AC/AS) 接口 2-3
struct WebcamTestEnv {
    StringPool pool;
    std::shared_ptr<UsbDevice> device;

    WebcamTestEnv() {
        // 端点布局照抄 mock_webcam_main：UVC VS 用 0x81、UAC AS 用 0x83
        // （0x81 已被 UVC VS 占用，端点号设备内唯一，对齐内核 g_webcam 的
        // uvc ep1 + uac ep2/3 分配）
        std::vector<UsbInterface> interfaces = {
                UsbInterface{.interface_class = 0x0E, .interface_subclass = 0x01, .interface_protocol = 0x01,
                             .endpoints = {{UsbEndpoint{.address = 0x87, .attributes = 0x03,
                                                        .max_packet_size = 16, .interval = 8}}}},
                UsbInterface{.interface_class = 0x0E, .interface_subclass = 0x02, .interface_protocol = 0x01,
                             .endpoints = {{}, {UsbEndpoint{.address = 0x81, .attributes = 0x01,
                                                            .max_packet_size = 512, .interval = 1}}}},
                UsbInterface{.interface_class = 0x01, .interface_subclass = 0x01, .interface_protocol = 0x00,
                             .endpoints = {{UsbEndpoint{.address = 0x82, .attributes = 0x03,
                                                        .max_packet_size = 2, .interval = 4}}}},
                UsbInterface{.interface_class = 0x01, .interface_subclass = 0x02, .interface_protocol = 0x00,
                             .endpoints = {{}, {UsbEndpoint{.address = 0x83, .attributes = 0x01,
                                                            .max_packet_size = 192, .interval = 4}}}},
        };

        device = std::make_shared<UsbDevice>(UsbDevice{
                .path = "/usbipdcpp/mock_webcam",
                .busid = "1-1",
                .bus_num = 1,
                .dev_num = 1,
                .speed = static_cast<std::uint32_t>(UsbSpeed::High),
                .vendor_id = 0x1234,
                .product_id = 0x5685,
                .device_bcd = 0x0100,
                .device_class = 0xEF, // Miscellaneous (IAD)：复合设备必须
                .device_subclass = 0x02,
                .device_protocol = 0x01,
                .configuration_value = 1,
                .num_configurations = 1,
                .interfaces = interfaces,
                .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::High),
                .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::High),
        });
        device->assign_interface_numbers();
        // 装配顺序同 mock_webcam_main：先 UVC 后 UAC（UAC 打开设备级调度器，
        // UVC 等时 URB 不走调度器，互不影响，见 UvcDeviceHelper::setup 注释）。
        // 构造函数里不能用 ASSERT（非 void 返回），装配失败用 EXPECT 报出
        auto ec_uvc = UvcDeviceHelper::setup(device, 0, pool, std::make_unique<ColorBarSource>(320, 240, 15));
        EXPECT_EQ(ec_uvc, std::error_code());
        auto ec_uac = UacDeviceHelper::setup_microphone(device, 2, pool,
                                                        std::make_unique<SineWaveSource>(440, std::vector<std::uint32_t>{48000}, 1, 0.5));
        EXPECT_EQ(ec_uac, std::error_code());
    }
};

} // namespace

TEST(WebcamDevice, InterfacesNumberedInOrder) {
    WebcamTestEnv env;
    for (std::size_t i = 0; i < env.device->interfaces.size(); i++) {
        EXPECT_EQ(env.device->interfaces[i].interface_number, i);
    }
}

TEST(WebcamDevice, FunctionGroupIadsDeclared) {
    WebcamTestEnv env;
    // IAD 由各功能组首个接口声明：UVC 组 = VC（接口 0，UvcDeviceHelper 挂），
    // UAC 组 = AC（接口 2，UacDeviceHelper 挂）。复合设备（0xEF）里 usbccgp
    // 只按 IAD 归并接口组，UAC 组没有 IAD 时 AC/AS 被拆开，usbaudio.sys
    // 启动失败（Windows 实测 CM_PROB_FAILED_START），故 helper 必须对称声明
    ASSERT_TRUE(env.device->interfaces[0].interface_association_descriptor.has_value());
    auto uvc_iad = *env.device->interfaces[0].interface_association_descriptor;
    EXPECT_EQ(uvc_iad.bInterfaceCount, 2);
    EXPECT_EQ(uvc_iad.bFunctionClass, 0x0E); // CC_VIDEO
    EXPECT_EQ(uvc_iad.bFunctionSubClass, 0x03); // SC_VIDEO_INTERFACE_COLLECTION

    ASSERT_TRUE(env.device->interfaces[2].interface_association_descriptor.has_value());
    auto uac_iad = *env.device->interfaces[2].interface_association_descriptor;
    EXPECT_EQ(uac_iad.bInterfaceCount, 2);
    EXPECT_EQ(uac_iad.bFunctionClass, 0x01); // CC_AUDIO
    EXPECT_EQ(uac_iad.bFunctionSubClass, 0); // 音频无 collection 子类概念
    EXPECT_EQ(uac_iad.iFunction,
              env.device->interfaces[2].handler->get_string_interface_value());
    // 组内非首接口不声明 IAD
    EXPECT_FALSE(env.device->interfaces[1].interface_association_descriptor.has_value());
    EXPECT_FALSE(env.device->interfaces[3].interface_association_descriptor.has_value());
}

TEST(WebcamDevice, EndpointAddressesUnique) {
    WebcamTestEnv env;
    // 端点号设备内唯一：UAC AS 用 0x83 避开 UVC VS 的 0x81（组合设备的
    // 经典错误——单设备各自正常，复合后端点冲突）
    std::set<std::uint8_t> seen;
    for (auto &intf: env.device->interfaces) {
        for (auto &alt_eps: intf.endpoints) {
            for (auto &ep: alt_eps) {
                EXPECT_TRUE(seen.insert(ep.address).second) << "重复端点地址 0x" << std::hex
                                                            << static_cast<int>(ep.address);
            }
        }
    }
}

TEST(WebcamDevice, HandlerTypesCorrect) {
    WebcamTestEnv env;
    EXPECT_NE(std::dynamic_pointer_cast<UvcVideoControlHandler>(env.device->interfaces[0].handler), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<UvcVideoStreamingHandler>(env.device->interfaces[1].handler), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<UacAudioControlHandler>(env.device->interfaces[2].handler), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<UacAudioStreamingSourceHandler>(env.device->interfaces[3].handler), nullptr);
}

TEST(WebcamDevice, DeviceClassIsIadProtocol) {
    WebcamTestEnv env;
    // bDeviceClass=0xEF/0x02/0x01（IAD 协议）：Windows usbccgp 按 IAD 拆分
    // 功能组，UVC/UAC 各自绑定类驱动（同 mock_uvc）
    EXPECT_EQ(env.device->device_class, 0xEF);
    EXPECT_EQ(env.device->device_subclass, 0x02);
    EXPECT_EQ(env.device->device_protocol, 0x01);
}
