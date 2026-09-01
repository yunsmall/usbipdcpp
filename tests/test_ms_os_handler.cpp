// Microsoft OS 1.0 描述符测试：0xEE 签名串（"MSFT100"）与 Compatible ID 特征
// 描述符。Windows 枚举设备时先用 0xEE 字符串探测 MS OS 支持，再以
// bMS_VendorCode 请求 Compatible ID 加载内置驱动（如 RNDIS → rndismp）

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/MSOSSimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/MsOsConstants.h"
#include "usbipdcpp/virtual_device/RndisMsOsDeviceHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// 把 protected 的协议处理方法提到 public，测试直接投递控制请求
class TestableMsOsHandler : public MSOSSimpleVirtualDeviceHandler {
public:
    using MSOSSimpleVirtualDeviceHandler::MSOSSimpleVirtualDeviceHandler;
    using MSOSSimpleVirtualDeviceHandler::get_special_string_descriptor;
    using MSOSSimpleVirtualDeviceHandler::handle_non_standard_request_type_control_urb;
};

// 带设备级 MS OS 支持的设备（对齐 examples/mock_rndis 的绑定方式）
struct MsOsFixture {
    // 传输分配器：必须声明在 stub 之前（成员逆序析构，op 后死）
    GenericTransferOperator op;
    StringPool pool;
    CaptureResponder stub;
    std::shared_ptr<UsbDevice> device;
    TestableMsOsHandler *dev = nullptr;
    usbipdcpp::error_code ec;

    MsOsFixture() {
        std::vector<UsbInterface> interfaces = {
                {.interface_class = 0x02,
                 .interface_subclass = 0x02,
                 .interface_protocol = 0xFF,
                 .endpoints = {}}};
        device = UsbDevice::make("1-1", 0x1234, 0x56E2, std::move(interfaces), 1, 1,
                                 static_cast<std::uint8_t>(ClassCode::CDC));
        dev = device->with_handler<TestableMsOsHandler>(pool).get();
        dev->setup_interface_handlers();
        // 绑定响应器：接口 handler 没挂不碍事（on_new_connection 跳过无
        // handler 的接口），失败场景不在本测试范围，错误留存在 ec 成员
        dev->on_new_connection(stub, ec);
    }

    // 找 seqnum 对应的 RET_SUBMIT（找不到返回 nullptr）
    const UsbIpResponse::UsbIpRetSubmit *find_submit(std::uint32_t seqnum) const {
        for (auto &s : stub.submits) {
            if (s.header.seqnum == seqnum) {
                return &s;
            }
        }
        return nullptr;
    }
};

// 投递一个 IN 方向设备级控制请求（MS OS Compatible ID 请求的形状）
void deliver_ms_os_request(MsOsFixture &fx, std::uint32_t seqnum, const SetupPacket &setup) {
    auto submit = make_cmd_submit(fx.op, seqnum, 0x80, UsbIpDirection::In, setup.length, setup);
    fx.dev->handle_non_standard_request_type_control_urb(
            seqnum, UsbEndpoint::get_ep0_in(UsbSpeed::Full), 0, setup.length, setup, std::move(submit.transfer),
            fx.ec);
    ASSERT_FALSE(fx.ec);
}

// 标准 MS OS Compatible ID 请求：Vendor|Device|IN(0xC0) + bRequest=0x01 +
// wValue=0x0000 + wIndex=0x0004 + wLength=40
SetupPacket make_compat_id_request(std::uint8_t request = MS_OS_DEFAULT_VENDOR_CODE, std::uint16_t value = 0,
                                   std::uint16_t index = MS_OS_COMPAT_ID_WINDEX, std::uint16_t length = 40) {
    return {.request_type = 0xC0, .request = request, .value = value, .index = index, .length = length};
}

} // namespace

// ========== 0xEE 签名串（GET_DESCRIPTOR String） ==========

TEST(TestMsOsHandler, SpecialStringNotConfiguredFallsThrough) {
    // 未配置 Compatible ID = 不提供 MS OS 描述符：0xEE 走默认（nullopt）
    MsOsFixture fx;
    EXPECT_EQ(fx.dev->get_special_string_descriptor(MS_OS_STRING_INDEX), std::nullopt);
    EXPECT_EQ(fx.dev->get_special_string_descriptor(1), std::nullopt);
}

TEST(TestMsOsHandler, SpecialStringReturnsMsftSignature) {
    // 配置后 0xEE 返回 18 字节签名串：bLength=18、type=0x03 STRING、
    // "MSFT100" UTF-16LE、bMS_VendorCode、bPad=0（对齐内核 composite.c
    // usb_os_string 的布局）
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");

    auto desc = fx.dev->get_special_string_descriptor(MS_OS_STRING_INDEX);
    ASSERT_TRUE(desc.has_value());
    ASSERT_EQ(desc->size(), sizeof(MsOsStringDesc));
    EXPECT_EQ((*desc)[0], 18u);
    EXPECT_EQ((*desc)[1], static_cast<std::uint8_t>(DescriptorType::String));
    const data_type expected_sig = {
            0x4D, 0x00, 0x53, 0x00, 0x46, 0x00, 0x54, 0x00, 0x31, 0x00, 0x30, 0x00, 0x30, 0x00,
    };
    EXPECT_TRUE(std::equal(expected_sig.begin(), expected_sig.end(), desc->begin() + 2));
    EXPECT_EQ((*desc)[16], MS_OS_DEFAULT_VENDOR_CODE);
    EXPECT_EQ((*desc)[17], 0u);
    // 其他索引不受影响
    EXPECT_EQ(fx.dev->get_special_string_descriptor(1), std::nullopt);
}

TEST(TestMsOsHandler, SpecialStringUsesCustomVendorCode) {
    // set_ms_os_vendor_code 后签名串里的 bMS_VendorCode 跟着变
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    fx.dev->set_ms_os_vendor_code(0x21);

    auto desc = fx.dev->get_special_string_descriptor(MS_OS_STRING_INDEX);
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ((*desc)[16], 0x21u);
}

// ========== Compatible ID 特征描述符（Vendor 请求） ==========

TEST(TestMsOsHandler, CompatIdRequestReturnsDescriptor) {
    // 完整请求 → 40 字节描述符（16 头 + 24 功能节），字段对齐内核
    // composite.c fill_ext_compat 的布局
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");

    deliver_ms_os_request(fx, 1, make_compat_id_request());
    const auto *ret = fx.find_submit(1);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, 0u);
    ASSERT_EQ(ret->actual_length, 40u);

    auto data = ret_submit_data(*ret);
    ASSERT_EQ(data.size(), 40u);
    // 头：dwLength=40、bcdVersion=0x0100、wIndex=0x0004、bCount=1、Reserved[7]=0
    EXPECT_EQ(read_le32_at(data, 0), 40u);
    EXPECT_EQ(read_le16_at(data, 4), 0x0100u);
    EXPECT_EQ(read_le16_at(data, 6), 0x0004u);
    EXPECT_EQ(data[8], 1u);
    EXPECT_TRUE(std::all_of(data.begin() + 9, data.begin() + 16, [](std::uint8_t b) { return b == 0; }));
    // 功能节：bFirstInterface=0、Reserved1=0x01（对齐内核固定值）、
    // CompatibleID="RNDIS"+3 个 0、SubCompatibleID 全 0、Reserved2 全 0
    EXPECT_EQ(data[16], 0u);
    EXPECT_EQ(data[17], 0x01u);
    const std::string compat(reinterpret_cast<const char *>(data.data() + 18), 8);
    EXPECT_EQ(compat, std::string("RNDIS\0\0\0", 8));
    EXPECT_TRUE(std::all_of(data.begin() + 26, data.begin() + 40, [](std::uint8_t b) { return b == 0; }));
}

TEST(TestMsOsHandler, CompatIdNotConfiguredStalls) {
    // 未配置 Compatible ID：整个 MS OS 支持关闭，请求走父类 EPIPE
    MsOsFixture fx;
    deliver_ms_os_request(fx, 1, make_compat_id_request());
    const auto *ret = fx.find_submit(1);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(ret->actual_length, 0u);
}

TEST(TestMsOsHandler, CompatIdWrongIndexStalls) {
    // wIndex ≠ 0x0004（如 Extended Properties 的 0x0005）：非本类职责，EPIPE
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    deliver_ms_os_request(fx, 1, make_compat_id_request(MS_OS_DEFAULT_VENDOR_CODE, 0, 0x0005));
    EXPECT_EQ(fx.find_submit(1)->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(TestMsOsHandler, CompatIdWrongValueStalls) {
    // wValue 低字节非 0（1 = Extended Properties 请求）：不支持，EPIPE
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    deliver_ms_os_request(fx, 1, make_compat_id_request(MS_OS_DEFAULT_VENDOR_CODE, 1));
    EXPECT_EQ(fx.find_submit(1)->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(TestMsOsHandler, CompatIdOutDirectionStalls) {
    // OUT 方向（0x40 而非 0xC0）：Compatible ID 只可能是 IN 请求，EPIPE
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    SetupPacket setup = make_compat_id_request();
    setup.request_type = 0x40; // Vendor | Device | OUT
    deliver_ms_os_request(fx, 1, setup);
    EXPECT_EQ(fx.find_submit(1)->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(TestMsOsHandler, CompatIdWrongRequestStalls) {
    // bRequest ≠ bMS_VendorCode（如 0x00 未定义请求）：EPIPE
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    deliver_ms_os_request(fx, 1, make_compat_id_request(0x00));
    EXPECT_EQ(fx.find_submit(1)->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(TestMsOsHandler, CompatIdUsesCustomVendorCode) {
    // 自定义 bMS_VendorCode：请求号匹配时正常响应，旧请求号 EPIPE
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    fx.dev->set_ms_os_vendor_code(0x21);

    deliver_ms_os_request(fx, 1, make_compat_id_request(0x21));
    EXPECT_EQ(fx.find_submit(1)->status, 0u);

    deliver_ms_os_request(fx, 2, make_compat_id_request(MS_OS_DEFAULT_VENDOR_CODE));
    EXPECT_EQ(fx.find_submit(2)->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(TestMsOsHandler, CompatIdTruncatedByBufferLength) {
    // 主机 wLength 小于 40（按缓冲截断，对齐 get_string_descriptor 的截断行为）
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    deliver_ms_os_request(fx, 1, make_compat_id_request(MS_OS_DEFAULT_VENDOR_CODE, 0, MS_OS_COMPAT_ID_WINDEX, 20));
    const auto *ret = fx.find_submit(1);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, 0u);
    ASSERT_EQ(ret->actual_length, 20u);
    EXPECT_EQ(ret_submit_data(*ret).size(), 20u);
}

TEST(TestMsOsHandler, CompatIdIncludesSubCompatibleId) {
    // SubCompatibleID：Windows 驱动匹配可能依赖它（rndismp 按
    // MS_SUBCOMP_5162001 匹配，见 rndiscmp.inf），空值导致匹配失败
    MsOsFixture fx;
    fx.dev->set_ms_os_compatible_id("RNDIS");
    fx.dev->set_ms_os_sub_compatible_id("5162001");
    deliver_ms_os_request(fx, 1, make_compat_id_request());
    const auto *ret = fx.find_submit(1);
    ASSERT_NE(ret, nullptr);
    ASSERT_EQ(ret->actual_length, 40u);
    auto data = ret_submit_data(*ret);
    ASSERT_EQ(data.size(), 40u);
    const std::string sub(reinterpret_cast<const char *>(data.data() + 26), 8);
    EXPECT_EQ(sub, std::string("5162001\0", 8));
}

// 编译期断言：RndisMsOsDeviceHandler 的配置 setter 已被 using 隐藏为 private
// （RNDIS 预设是功能定义的一部分，不允许外部改动）
template <typename T, typename = void>
struct CanCallSetCompatibleId : std::false_type {};
template <typename T>
struct CanCallSetCompatibleId<T, std::void_t<decltype(std::declval<T &>().set_ms_os_compatible_id(""))>>
        : std::true_type {};

TEST(TestMsOsHandler, RndisDeviceHandlerPresets) {
    // RndisMsOsDeviceHandler（final）预设 RNDIS + 5162001（功能定义的一部分），
    // RNDIS 设备装配一行搞定；完整 Compatible ID 响应字节由
    // CompatIdIncludesSubCompatibleId（基类手配同值）覆盖
    MsOsFixture fx;
    RndisMsOsDeviceHandler rndis{*fx.device, fx.pool};
    EXPECT_EQ(rndis.get_ms_os_compatible_id(), "RNDIS");
    EXPECT_EQ(rndis.get_ms_os_sub_compatible_id(), "5162001");
    EXPECT_FALSE(CanCallSetCompatibleId<RndisMsOsDeviceHandler>::value);
}
