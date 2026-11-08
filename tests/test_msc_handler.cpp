#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/MscBulkOnlyHandler.h"
#include "usbipdcpp/virtual_device/storage_backends/MemoryBackend.h"
#include "usbipdcpp/virtual_device/storage_backends/RawImageBackend.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageIoTransfer.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageTransferOperator.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// MSC 设备 + handler 指针（handler 由 device 持有，生命周期同 device）
struct MscFixture {
    std::shared_ptr<UsbDevice> device;
    MscBulkOnlyHandler *msc = nullptr;
    UsbEndpoint ep_in;
    UsbEndpoint ep_out;
};

// 构造 MSC 设备（对齐 examples/mock_msc）：Mass Storage + SCSI + BOT，
// bulk IN 0x81 + bulk OUT 0x02，High speed
MscFixture make_msc_device(StringPool &string_pool, std::unique_ptr<StorageBackend> backend,
                           bool read_only = false, MscConfig config = {}) {
    std::vector<UsbInterface> interfaces = {MscBulkOnlyHandler::make_interface(0x81, 0x02)};
    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/mock_msc",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::High),
            .vendor_id = 0x1234,
            .product_id = 0x5691,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::High),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::High),
    });
    auto msc = device->interfaces[0].with_handler<MscBulkOnlyHandler>(string_pool, std::move(backend),
                                                                      std::move(config), read_only);
    device->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();
    auto &intf = device->interfaces[0];
    return {device, msc.get(), intf.endpoints[0][0], intf.endpoints[0][1]};
}

// 构造 CBW 字节流（BOT 协议，小端字段，结构体直接 memcpy）
data_type make_cbw(std::uint32_t tag, std::uint8_t flags, const std::array<std::uint8_t, 16> &cdb,
                   std::uint32_t transfer_len) {
    CBW cbw{};
    cbw.dCBWSignature = CBW_SIGNATURE;
    cbw.dCBWTag = tag;
    cbw.dCBWDataTransferLength = transfer_len;
    cbw.bmCBWFlags = flags;
    cbw.bCBWLUN = 0;
    cbw.bCBWCBLength = 16;
    std::memcpy(cbw.CBWCB, cdb.data(), 16);
    data_type bytes(sizeof(CBW));
    std::memcpy(bytes.data(), &cbw, sizeof(CBW));
    return bytes;
}

// READ10 / WRITE10 CDB（LBA 与块数大端）
std::array<std::uint8_t, 16> read10_cdb(std::uint32_t lba, std::uint16_t blocks) {
    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = 0x28; // READ(10)
    cdb[2] = static_cast<std::uint8_t>((lba >> 24) & 0xFF);
    cdb[3] = static_cast<std::uint8_t>((lba >> 16) & 0xFF);
    cdb[4] = static_cast<std::uint8_t>((lba >> 8) & 0xFF);
    cdb[5] = static_cast<std::uint8_t>(lba & 0xFF);
    cdb[7] = static_cast<std::uint8_t>((blocks >> 8) & 0xFF);
    cdb[8] = static_cast<std::uint8_t>(blocks & 0xFF);
    return cdb;
}

std::array<std::uint8_t, 16> write10_cdb(std::uint32_t lba, std::uint16_t blocks) {
    auto cdb = read10_cdb(lba, blocks);
    cdb[0] = 0x2A; // WRITE(10)
    return cdb;
}

// 模拟网络层收下 OUT 数据（对齐 StorageTransferOperator::recv_transfer_data 的
// fallback 路径：alloc 时 prepare_out_buffer 设 external_buf，收完回调
// on_out_data_received）。CBW（Idle）时 external_buf 为空走 fallback_data
void deliver_out_data(MscBulkOnlyHandler &msc, std::uint32_t seqnum, const data_type &payload) {
    // 用基类指针调虚函数分配：避免对 StorageTransferOperator 做多态下行
    // static_cast（Linux 链接时引用其 RTTI，部分构建配置下解析失败）
    auto *op = msc.get_transfer_operator();
    UsbIpHeaderBasic header{};
    header.direction = UsbIpDirection::Out;
    auto *trx = StorageIoTransfer::from_handle(op->alloc_transfer_handle(payload.size(), 0, header, {}));
    if (trx->external_buf) {
        std::memcpy(trx->external_buf, payload.data(), payload.size());
    }
    else {
        trx->fallback_data = payload;
    }
    msc.on_out_data_received(trx, payload.size());
}

// 主机的 IN 请求：transfer 必须由接口的 op 分配（handle_bulk_transfer 强转
// StorageIoTransfer 写 external_buf），不能走 make_cmd_submit 的 GenericTransfer
void deliver_in_request(MscBulkOnlyHandler &msc, std::uint32_t seqnum, std::uint32_t length, const UsbEndpoint &ep_in,
                        usbipdcpp::error_code &ec) {
    auto *op = msc.get_transfer_operator();
    UsbIpHeaderBasic header{};
    header.direction = UsbIpDirection::In;
    auto *trx = StorageIoTransfer::from_handle(op->alloc_transfer_handle(length, 0, header, {}));
    TransferHandle handle(trx, op);
    msc.handle_bulk_transfer(seqnum, ep_in, 0, length, std::move(handle), ec);
}

// 从 RET_SUBMIT 读 IN 数据（external_buf 指向 staging 或 mmap）
data_type data_from_submit(const UsbIpResponse::UsbIpRetSubmit &ret) {
    auto *trx = StorageIoTransfer::from_handle(ret.transfer.get());
    const auto *p = static_cast<const std::uint8_t *>(trx->external_buf);
    return data_type(p, p + ret.actual_length);
}

// 从 RET_SUBMIT 读 CSW（Status 阶段应答，数据在 fallback_data）
CSW csw_from_submit(const UsbIpResponse::UsbIpRetSubmit &ret) {
    auto *trx = StorageIoTransfer::from_handle(ret.transfer.get());
    CSW csw{};
    std::memcpy(&csw, trx->fallback_data.data(), sizeof(CSW));
    return csw;
}

/// 可注入短读/短写的后端：get_direct_buffer 恒为 nullptr，强制走
/// staging 拷贝路径（短读写只在这条路径上能观察到）
class FailingBackend : public StorageBackend {
public:
    explicit FailingBackend(std::unique_ptr<MemoryBackend> inner) : inner_(std::move(inner)) {
    }

    std::size_t read(std::uint64_t lba, std::uint16_t count, void *buffer) override {
        if (short_read)
            return 0;
        return inner_->read(lba, count, buffer);
    }

    std::size_t write(std::uint64_t lba, std::uint16_t count, const void *data) override {
        if (short_write)
            return 0;
        return inner_->write(lba, count, data);
    }

    std::uint64_t block_count() const override {
        return inner_->block_count();
    }

    std::uint32_t block_size() const override {
        return inner_->block_size();
    }

    bool short_read = false;
    bool short_write = false;

private:
    std::unique_ptr<MemoryBackend> inner_;
};

} // namespace

TEST(TestMscHandler, Read10ServesBlocksThenCsw) {
    // READ10：CBW → DataIn（mmap 零拷贝路径，external_buf 直指后端缓冲）→
    // CSW（status 0、residue 0）。对齐 BOT 协议状态机
    StringPool string_pool;
    auto backend = std::make_unique<MemoryBackend>(8);
    data_type pattern(1024);
    for (std::size_t i = 0; i < pattern.size(); i++) {
        pattern[i] = static_cast<std::uint8_t>(i);
    }
    std::memcpy(backend->get_direct_buffer(2), pattern.data(), pattern.size()); // 预填 LBA 2-3
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // CBW：READ10 LBA=2 读 2 块（1024 字节），IN 方向
    deliver_out_data(*fixture.msc, 1, make_cbw(0x1234, 0x80, read10_cdb(2, 2), 1024));

    // 主机 IN 请求拉数据
    deliver_in_request(*fixture.msc, 2, 1024, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 2u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, 1024u);
    EXPECT_EQ(data_from_submit(stub.submits[0]), pattern);

    // 主机 IN 请求拉 CSW
    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[1].header.seqnum, 3u);
    EXPECT_EQ(stub.submits[1].status, 0u);
    EXPECT_EQ(stub.submits[1].actual_length, 13u);
    auto csw = csw_from_submit(stub.submits[1]);
    EXPECT_EQ(csw.dCSWSignature, CSW_SIGNATURE);
    EXPECT_EQ(csw.dCSWTag, 0x1234u);
    EXPECT_EQ(csw.dCSWDataResidue, 0u);
    EXPECT_EQ(csw.bCSWStatus, 0u); // passed

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, Write10WritesBackendThenCsw) {
    // WRITE10：CBW → DataOut（数据入 staging/mmap）→ CSW。写盘后后端内容变化
    StringPool string_pool;
    auto backend = std::make_unique<MemoryBackend>(8);
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // CBW：WRITE10 LBA=1 写 1 块（512 字节），OUT 方向
    deliver_out_data(*fixture.msc, 1, make_cbw(0xABCD, 0x00, write10_cdb(1, 1), 512));

    // DataOut：512 字节数据
    data_type payload(512);
    for (std::size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<std::uint8_t>(i * 3 + 7);
    }
    deliver_out_data(*fixture.msc, 2, payload);

    // 主机 IN 请求拉 CSW
    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 3u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.dCSWSignature, CSW_SIGNATURE);
    EXPECT_EQ(csw.dCSWTag, 0xABCDu);
    EXPECT_EQ(csw.dCSWDataResidue, 0u);
    EXPECT_EQ(csw.bCSWStatus, 0u);

    // 后端 LBA 1 内容已更新
    auto *direct = fixture.msc->get_backend()->get_direct_buffer(1);
    EXPECT_EQ(std::memcmp(direct, payload.data(), payload.size()), 0);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, InvalidCbwSignatureReportsFailedCsw) {
    // CBW 签名非法：不进任何命令分支，直接 CSW status=1（command failed），
    // residue = 应传未传字节数（= dCBWDataTransferLength）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto bad_cbw = make_cbw(0x7777, 0x80, read10_cdb(0, 1), 512);
    bad_cbw[0] = 0xDE;
    bad_cbw[1] = 0xAD;
    bad_cbw[2] = 0xBE;
    bad_cbw[3] = 0xEF;
    deliver_out_data(*fixture.msc, 1, bad_cbw);

    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.dCSWTag, 0x7777u);
    EXPECT_EQ(csw.dCSWDataResidue, 512u);
    EXPECT_EQ(csw.bCSWStatus, 1u); // failed

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, TestUnitReadyCompletesWithoutDataStage) {
    // TEST UNIT READY：无数据阶段，直接 Status 出 CSW（passed）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::TestUnitReady;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x1111, 0x00, cdb, 0));

    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.dCSWTag, 0x1111u);
    EXPECT_EQ(csw.dCSWDataResidue, 0u);
    EXPECT_EQ(csw.bCSWStatus, 0u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, InRequestBeforeCbwStalls) {
    // 未收 CBW（Idle 状态）时的 IN 请求：回 EPIPE（状态机不认这个阶段）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_in_request(*fixture.msc, 1, 512, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(stub.submits[0].actual_length, 0u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, OutRequestAcknowledgedWithoutData) {
    // 端点 OUT 请求（非 CBW 路径）：只 ack 不消费（BOT 的 OUT 数据处理由
    // prepare_out_buffer/on_out_data_received 走网络收数据回调，不走这里）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    fixture.msc->handle_bulk_transfer(1, fixture.ep_out, 0, 31, TransferHandle{}, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, 31u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, Write10OnReadOnlyDeviceFailsCsw) {
    // 只读设备收 WRITE10：CBW 解析即标记命令失败，CSW status=1（对齐内核
    // fsg 的只读介质拒绝），数据阶段不接收
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8), /*read_only=*/true);

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_out_data(*fixture.msc, 1, make_cbw(0x2222, 0x00, write10_cdb(1, 1), 512));

    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.dCSWTag, 0x2222u);
    EXPECT_EQ(csw.dCSWDataResidue, 512u); // 数据阶段未传输
    EXPECT_EQ(csw.bCSWStatus, 1u);        // failed

    // 后端内容未被修改
    data_type backend_data(512, 0);
    auto *direct = fixture.msc->get_backend()->get_direct_buffer(1);
    EXPECT_EQ(std::memcmp(direct, backend_data.data(), 512), 0);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, RequestSenseReturnsSenseData) {
    // REQUEST SENSE：固定格式 sense 数据（0x70 + 附加长度 10）经 DataIn 返回，
    // 对齐内核 fsg 的响应结构
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::RequestSense;
    cdb[7] = 18; // 分配长度
    deliver_out_data(*fixture.msc, 1, make_cbw(0x3333, 0x80, cdb, 18));

    deliver_in_request(*fixture.msc, 2, 18, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 2u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, 18u);
    auto sense = data_from_submit(stub.submits[0]);
    ASSERT_EQ(sense.size(), 18u);
    EXPECT_EQ(sense[0], 0x70);         // 固定格式、有效
    EXPECT_EQ(sense[7], 10);           // 附加长度
    EXPECT_EQ(sense[12], 0);           // ASC
    EXPECT_EQ(sense[13], 0);           // ASCQ

    // CSW
    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 2u);
    auto csw = csw_from_submit(stub.submits[1]);
    EXPECT_EQ(csw.dCSWTag, 0x3333u);
    EXPECT_EQ(csw.bCSWStatus, 0u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, RequestSenseReportsLastCommandFailure) {
    // 不支持的 SCSI 命令 → CSW failed，随后 REQUEST SENSE 必须返回
    // ILLEGAL REQUEST / INVALID COMMAND；sense 取走后再次查询回 NO SENSE
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = 0x7F; // 未实现的命令
    deliver_out_data(*fixture.msc, 1, make_cbw(0x3334, 0x00, cdb, 0));
    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(csw_from_submit(stub.submits[0]).bCSWStatus, 1u); // failed

    auto request_sense = [&](std::uint32_t seq, std::uint32_t tag) {
        std::array<std::uint8_t, 16> sense_cdb{};
        sense_cdb[0] = ScsiCmd::RequestSense;
        sense_cdb[7] = 18;
        deliver_out_data(*fixture.msc, seq, make_cbw(tag, 0x80, sense_cdb, 18));
        auto before = stub.submits.size();
        deliver_in_request(*fixture.msc, seq + 1, 18, fixture.ep_in, ec);
        EXPECT_EQ(stub.submits.size(), before + 1);
        return data_from_submit(stub.submits.back());
    };

    auto sense = request_sense(3, 0x3335);
    ASSERT_EQ(sense.size(), 18u);
    EXPECT_EQ(sense[0], 0x70);
    EXPECT_EQ(sense[2], static_cast<std::uint8_t>(ScsiSenseKey::IllegalRequest));
    EXPECT_EQ(sense[12], static_cast<std::uint8_t>(ScsiAsc::InvalidCommandOperationCode));

    // sense 已被取走：第二次查询应回到 NO SENSE
    deliver_in_request(*fixture.msc, 5, 13, fixture.ep_in, ec); // 第一条 REQUEST SENSE 的 CSW
    ASSERT_FALSE(ec);
    auto sense2 = request_sense(6, 0x3337);
    ASSERT_EQ(sense2.size(), 18u);
    EXPECT_EQ(sense2[2], static_cast<std::uint8_t>(ScsiSenseKey::NoSense));
    EXPECT_EQ(sense2[12], static_cast<std::uint8_t>(ScsiAsc::NoAdditionalSense));

    fixture.msc->on_disconnection(ec);
}

// ========== MSC 边缘情况 ==========

TEST(TestMscHandler, InquiryStandardReturnsIdentifiers) {
    // 标准 INQUIRY：固定字段（设备类型/可移动介质/SPC-4）+ 厂商/产品/版本
    // （来自 MscConfig，on_setup_interface_handlers 补全默认值）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8), false,
                                   MscConfig{.vendor = "TestVend", .product = "TestProduct1234",
                                             .revision = "1.2", .serial = "SN001"});

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::Inquiry;
    cdb[4] = 36;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x4444, 0x80, cdb, 36));

    deliver_in_request(*fixture.msc, 2, 36, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    ASSERT_EQ(stub.submits[0].actual_length, 36u);
    auto data = data_from_submit(stub.submits[0]);
    EXPECT_EQ(data[0], 0x00); // 直接访问块设备
    EXPECT_EQ(data[1], 0x80); // 可移动介质
    EXPECT_EQ(data[2], 0x07); // SPC-5
    EXPECT_EQ(data[3], 0x02); // HiSup=0（平坦寻址）、响应格式 2
    EXPECT_EQ(data[4], 31);   // 附加长度
    EXPECT_EQ(std::string(data.begin() + 8, data.begin() + 16), "TestVend"); // 8 字符正好
    EXPECT_EQ(std::string(data.begin() + 16, data.begin() + 32), "TestProduct1234 "); // 15 字符补 1 空格
    EXPECT_EQ(std::string(data.begin() + 32, data.begin() + 36), "1.2 ");

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, InquiryVpdSupportedPagesListsBlockPages) {
    // VPD 0x00：必须列出 0x00/0x80/0xB0/0xB1/0xB2（SPC-4 Supported VPD Pages）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::Inquiry;
    cdb[1] = 0x01; // EVPD=1
    cdb[2] = 0x00;
    cdb[4] = 9; // 分配长度
    deliver_out_data(*fixture.msc, 1, make_cbw(0x4445, 0x80, cdb, 9));

    deliver_in_request(*fixture.msc, 2, 9, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto data = data_from_submit(stub.submits[0]);
    ASSERT_EQ(data.size(), 9u);
    EXPECT_EQ(data[1], 0x00);
    EXPECT_EQ(data[3], 5); // 页数
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 4, data.end())),
              (std::vector<std::uint8_t>{0x00, 0x80, 0xB0, 0xB1, 0xB2}));

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, InquiryVpdUnitSerialNumberHasTwoByteLength) {
    // Unit Serial Number（SPC-4 §7.8.1x）：PAGE LENGTH 在 byte 2-3 大端，
    // 序列号从 byte 4 开始。内核 scsi_get_vpd_page 用 get_unaligned_be16(&buf[2])
    // 取长度，写成 1 字节会让长度字段把首字符算进去
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8), false,
                                   MscConfig{.serial = "SN001"});

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::Inquiry;
    cdb[1] = 0x01; // EVPD=1
    cdb[2] = 0x80;
    cdb[4] = 64;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x4448, 0x80, cdb, 64));

    deliver_in_request(*fixture.msc, 2, 64, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto data = data_from_submit(stub.submits[0]);
    ASSERT_EQ(data.size(), 4u + 5u);
    EXPECT_EQ(data[1], 0x80);
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 2, data.begin() + 4)),
              (std::vector<std::uint8_t>{0x00, 0x05}));
    EXPECT_EQ(std::string(data.begin() + 4, data.end()), "SN001");

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, InquiryVpdBlockLimitsIsPageB0) {
    // Block Limits 是 0xB0（SBC-4 §5.4.5 Table 450），不是 0xB1。
    // 主机 sd 层按固定偏移解析：byte 6-7 粒度、8-11 最大传输、20-23 最大 UNMAP、
    // 24-27 UNMAP 描述符数、28-31 最优 UNMAP 粒度、32-35 UGAVALID、36-43 WRITE SAME
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::Inquiry;
    cdb[1] = 0x01; // EVPD=1
    cdb[2] = 0xB0;
    cdb[3] = 0;
    cdb[4] = 64; // 分配长度
    deliver_out_data(*fixture.msc, 1, make_cbw(0x4446, 0x80, cdb, 64));

    deliver_in_request(*fixture.msc, 2, 64, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto data = data_from_submit(stub.submits[0]);
    ASSERT_EQ(data.size(), 64u);
    EXPECT_EQ(sizeof(VpdBlockLimits), 64u);

    EXPECT_EQ(data[0], 0x00); // 直接访问块设备
    EXPECT_EQ(data[1], 0xB0); // Block Limits
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 2, data.begin() + 4)),
              (std::vector<std::uint8_t>{0x00, 0x3C})); // 页长度 60
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 20, data.begin() + 24)),
              (std::vector<std::uint8_t>{0x00, 0x01, 0x00, 0x00})); // UNMAP LBA 数 65536
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 24, data.begin() + 28)),
              (std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x40})); // 描述符数 64
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 28, data.begin() + 32)),
              (std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x08})); // 最优粒度 8 块
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 32, data.begin() + 36)),
              (std::vector<std::uint8_t>{0x80, 0x00, 0x00, 0x08})); // UGAVALID + 对齐 8 块
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 36, data.begin() + 44)),
              (std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0xFF, 0xFF})); // WRITE SAME 65535
    // 未声明的能力保持 0：COMPARE AND WRITE 不支持、无原子写特性
    EXPECT_EQ(data[5], 0x00);
    for (std::size_t i = 44; i < 64; ++i) {
        EXPECT_EQ(data[i], 0x00) << "byte " << i;
    }

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, InquiryVpdBlockDeviceCharacteristicsIsPageB1) {
    // Block Device Characteristics 是 0xB1（SBC-4 §5.4.3 Table 439）：
    // byte 4-5 介质旋转速率，0001h = 非旋转介质（Table 440）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::Inquiry;
    cdb[1] = 0x01; // EVPD=1
    cdb[2] = 0xB1;
    cdb[4] = 64;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x4447, 0x80, cdb, 64));

    deliver_in_request(*fixture.msc, 2, 64, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto data = data_from_submit(stub.submits[0]);
    ASSERT_EQ(data.size(), 64u);
    EXPECT_EQ(sizeof(VpdBlockDeviceCharacteristics), 64u);

    EXPECT_EQ(data[0], 0x00);
    EXPECT_EQ(data[1], 0xB1);
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 2, data.begin() + 4)),
              (std::vector<std::uint8_t>{0x00, 0x3C}));
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 4, data.begin() + 6)),
              (std::vector<std::uint8_t>{static_cast<std::uint8_t>(MSC_MEDIUM_ROTATION_NON_ROTATING >> 8),
                                         static_cast<std::uint8_t>(MSC_MEDIUM_ROTATION_NON_ROTATING)}));
    // product type / form factor / zoned 等特性均不报告
    for (std::size_t i = 6; i < 64; ++i) {
        EXPECT_EQ(data[i], 0x00) << "byte " << i;
    }

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, InquiryVpdLogicalBlockProvisioningIsPageB2) {
    // Logical Block Provisioning（SBC-4 §5.4.13 Table 467）：byte 5 声明
    // LBPU/LBPWS/LBPWS10/LBPRZ，byte 6 bit2-0 = 2h（thin provisioned）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::Inquiry;
    cdb[1] = 0x01; // EVPD=1
    cdb[2] = 0xB2;
    cdb[4] = 8;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x4449, 0x80, cdb, 8));

    deliver_in_request(*fixture.msc, 2, 8, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto data = data_from_submit(stub.submits[0]);
    ASSERT_EQ(data.size(), 8u);
    EXPECT_EQ(data[1], 0xB2);
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 2, data.begin() + 4)),
              (std::vector<std::uint8_t>{0x00, 0x04}));
    EXPECT_EQ(data[4], 0x00); // 不支持置备阈值
    EXPECT_EQ(data[5], LBP_FLAG_LBPU | LBP_FLAG_LBPWS | LBP_FLAG_LBPWS10 | LBP_FLAG_LBPRZ_ZERO);
    EXPECT_EQ(data[6], PROVISIONING_TYPE_THIN);
    EXPECT_EQ(data[7], 0x00); // 不支持置备百分比

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, ReadCapacity16ReportsLbpmeAndLbprz) {
    // SBC-4 §3.23.2 Table 122 byte 14：bit7 LBPME、bit6 LBPRZ。
    // 主机看不到 LBPME 就不会读 0xB2/0xB0，discard 永远不会启用
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::ReadCapacity16;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x5559, 0x80, cdb, 32));

    deliver_in_request(*fixture.msc, 2, 32, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto data = data_from_submit(stub.submits[0]);
    ASSERT_EQ(data.size(), 32u);
    EXPECT_EQ(data[14], static_cast<std::uint8_t>(RC16_LBPME | RC16_LBPRZ));
    EXPECT_EQ(data[15], 0x00); // 最低对齐 LBA = 0
    // byte 12/13：RC BASIS、PROT_EN、块大小指数都应为 0（无保护信息、512:512）
    EXPECT_EQ(data[12], 0x00);
    EXPECT_EQ(data[13], 0x00);
    for (std::size_t i = 16; i < 32; ++i) {
        EXPECT_EQ(data[i], 0x00) << "byte " << i;
    }

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, ReadFormatCapacitiesUsesShiftedFormatType) {
    // READ FORMAT CAPACITIES：byte 3 = 容量列表长度，byte 8 的 bit7-2
    // 才是 Format Type（2 = 已格式化介质，编码为 0x08）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::ReadFormatCapacities;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x555A, 0x80, cdb, 12));

    deliver_in_request(*fixture.msc, 2, 12, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto data = data_from_submit(stub.submits[0]);
    ASSERT_EQ(data.size(), 12u);
    EXPECT_EQ(data[0], 0x00);
    EXPECT_EQ(data[1], 0x00);
    EXPECT_EQ(data[2], 0x00);
    EXPECT_EQ(data[3], 0x08); // 容量列表长度
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 4, data.begin() + 8)),
              (std::vector<std::uint8_t>{0, 0, 0, 8})); // 8 块
    EXPECT_EQ(data[8], static_cast<std::uint8_t>(ScsiFormatType::FormattedMedia) << 2);
    EXPECT_EQ((std::vector<std::uint8_t>(data.begin() + 9, data.begin() + 12)),
              (std::vector<std::uint8_t>{0, 2, 0})); // 块大小 512

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, ReadCapacity10ReportsCapacity) {
    // READ CAPACITY(10)：最后 LBA 与块大小（大端）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::ReadCapacity10;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x5555, 0x80, cdb, 8));

    deliver_in_request(*fixture.msc, 2, 8, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    ASSERT_EQ(stub.submits[0].actual_length, 8u);
    auto data = data_from_submit(stub.submits[0]);
    EXPECT_EQ(data, (data_type{0, 0, 0, 7, 0, 0, 2, 0})); // last_lba=7, block_size=512

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, ReadCapacityWithoutBackendReportsOneBlock) {
    // backend 为空是设备配置错误：READ CAPACITY 不能解引用空指针，
    // 按 1 个 512 字节块报告（块数 0 时同样不能回绕成 0xFFFFFFFF 个块）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, nullptr);

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::ReadCapacity10;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x5556, 0x80, cdb, 8));

    deliver_in_request(*fixture.msc, 2, 8, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    ASSERT_EQ(stub.submits[0].actual_length, 8u);
    EXPECT_EQ(data_from_submit(stub.submits[0]), (data_type{0, 0, 0, 0, 0, 0, 2, 0}));

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, ReadCapacity16WithoutBackendReportsOneBlock) {
    // READ CAPACITY(16) 同样不能解引用空 backend（12 字节：8 字节 LBA + 4 字节块大小）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, nullptr);

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::ReadCapacity16;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x5557, 0x80, cdb, 12));

    deliver_in_request(*fixture.msc, 2, 12, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    ASSERT_EQ(stub.submits[0].actual_length, 12u);
    EXPECT_EQ(data_from_submit(stub.submits[0]),
              (data_type{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0}));

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, ReadCapacityZeroBlockBackendDoesNotWrapLastLba) {
    // 空镜像 block_count()==0：last_lba 必须是 0，不能是 block_count()-1 回绕出的
    // 0xFFFFFFFF（主机会据此建出 2TB 卷）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(0));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::ReadCapacity10;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x5558, 0x80, cdb, 8));

    deliver_in_request(*fixture.msc, 2, 8, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    ASSERT_EQ(stub.submits[0].actual_length, 8u);
    EXPECT_EQ(data_from_submit(stub.submits[0]), (data_type{0, 0, 0, 0, 0, 0, 2, 0}));

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, Read10ShortReadFailsCsw) {
    // 后端短读（镜像截断/I/O 错误）：命令失败，CSW status=1，
    // residue = 应传未传字节数（数据阶段一个字节都没发）
    StringPool string_pool;
    auto backend = std::make_unique<FailingBackend>(std::make_unique<MemoryBackend>(8));
    backend->short_read = true;
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_out_data(*fixture.msc, 1, make_cbw(0x5560, 0x80, read10_cdb(0, 2), 1024));

    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    ASSERT_EQ(stub.submits[0].actual_length, 13u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.bCSWStatus, 1u); // failed
    EXPECT_EQ(csw.dCSWDataResidue, 1024u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, Write10ShortWriteFailsCsw) {
    // 后端短写（磁盘满/I/O 错误）：数据阶段已完成，但必须回 CHECK CONDITION，
    // 不能让主机误以为数据已落盘
    StringPool string_pool;
    auto backend = std::make_unique<FailingBackend>(std::make_unique<MemoryBackend>(8));
    backend->short_write = true;
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_out_data(*fixture.msc, 1, make_cbw(0x5561, 0x00, write10_cdb(0, 1), 512));
    deliver_out_data(*fixture.msc, 2, data_type(512, 0x5A));

    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    ASSERT_EQ(stub.submits[0].actual_length, 13u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.bCSWStatus, 1u); // failed
    EXPECT_EQ(csw.dCSWDataResidue, 512u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, Read10BeyondCapacityFails) {
    // READ10 越界（LBA+count > 容量）：命令失败，CSW status=1
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_out_data(*fixture.msc, 1, make_cbw(0x6666, 0x80, read10_cdb(7, 2), 1024));

    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.dCSWTag, 0x6666u);
    EXPECT_EQ(csw.dCSWDataResidue, 1024u);
    EXPECT_EQ(csw.bCSWStatus, 1u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, Read10AcrossMultipleInRequests) {
    // 大数据读分多个 IN 请求：staging_offset_ 续传，各段拼接后等于后端内容，
    // 全部发完才转 Status（此场景走 mmap 零拷贝，external_buf 指向后端缓冲）
    StringPool string_pool;
    auto backend = std::make_unique<MemoryBackend>(8);
    data_type pattern(2048);
    for (std::size_t i = 0; i < pattern.size(); i++) {
        pattern[i] = static_cast<std::uint8_t>(i * 7);
    }
    std::memcpy(backend->get_direct_buffer(0), pattern.data(), pattern.size());
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_out_data(*fixture.msc, 1, make_cbw(0x7777, 0x80, read10_cdb(0, 4), 2048));

    deliver_in_request(*fixture.msc, 2, 1024, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    deliver_in_request(*fixture.msc, 3, 1024, fixture.ep_in, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[0].actual_length, 1024u);
    EXPECT_EQ(stub.submits[1].actual_length, 1024u);
    auto first = data_from_submit(stub.submits[0]);
    auto second = data_from_submit(stub.submits[1]);
    first.insert(first.end(), second.begin(), second.end());
    EXPECT_EQ(first, pattern);

    // 数据全部发完 → CSW
    deliver_in_request(*fixture.msc, 4, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 3u);
    EXPECT_EQ(csw_from_submit(stub.submits[2]).bCSWStatus, 0u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, Write10AcrossMultipleOutRequests) {
    // 大数据写分多个 OUT 请求：数据入 mmap（零拷贝路径），累积到声明长度
    // 才写盘转 Status。后端内容与拼接数据一致
    StringPool string_pool;
    auto backend = std::make_unique<MemoryBackend>(8);
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_out_data(*fixture.msc, 1, make_cbw(0x8888, 0x00, write10_cdb(0, 2), 1024));

    data_type first_half(512, 0x11);
    data_type second_half(512, 0x22);
    deliver_out_data(*fixture.msc, 2, first_half);
    deliver_out_data(*fixture.msc, 3, second_half);

    deliver_in_request(*fixture.msc, 4, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(csw_from_submit(stub.submits[0]).bCSWStatus, 0u);

    auto *direct = fixture.msc->get_backend()->get_direct_buffer(0);
    EXPECT_EQ(std::memcmp(direct, first_half.data(), 512), 0);
    EXPECT_EQ(std::memcmp(static_cast<char *>(direct) + 512, second_half.data(), 512), 0);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, UnmapPunchesHoles) {
    // UNMAP：DataOut 收描述符列表（8 字节头 + 每 16 字节一个块描述符），
    // 逐段 punch_hole 清零后端
    StringPool string_pool;
    auto backend = std::make_unique<MemoryBackend>(8);
    data_type pattern(512, 0xFF);
    for (std::uint64_t lba = 0; lba < 8; lba++) {
        std::memcpy(backend->get_direct_buffer(lba), pattern.data(), 512);
    }
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::Unmap;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x9999, 0x00, cdb, 40));

    // 40 字节：8 字节头 + 2 个描述符（LBA=2 cnt=1、LBA=5 cnt=2）。
    // 大端字段：LBA 低字节在偏移末尾（offset+7），cnt 低字节在 offset+11
    data_type unmap_data(40, 0);
    unmap_data[15] = 2;                 // 描述符 1：LBA=2（be64 低字节在 [15]）
    unmap_data[19] = 1;                 // 描述符 1：cnt=1（be32 低字节在 [19]）
    unmap_data[31] = 5;                 // 描述符 2：LBA=5
    unmap_data[35] = 2;                 // 描述符 2：cnt=2
    deliver_out_data(*fixture.msc, 2, unmap_data);

    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(csw_from_submit(stub.submits[0]).bCSWStatus, 0u);

    data_type zero(512, 0);
    EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(2), zero.data(), 512), 0);
    EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(5), zero.data(), 512), 0);
    EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(6), zero.data(), 512), 0);
    EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(7), pattern.data(), 512), 0); // 未动

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, UnmapOutOfRangeFailsWithoutPunching) {
    // UNMAP 描述符越界（LBA + 块数超出介质）：SBC-4 要求回 CHECK CONDITION，
    // 不能把越界范围交给 punch_hole（后端直接写内存，越界即写坏）
    StringPool string_pool;
    auto backend = std::make_unique<MemoryBackend>(8);
    data_type pattern(512, 0xFF);
    for (std::uint64_t lba = 0; lba < 8; lba++) {
        std::memcpy(backend->get_direct_buffer(lba), pattern.data(), 512);
    }
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::Unmap;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x9998, 0x00, cdb, 40));

    // 描述符 1 合法（LBA=2 cnt=1），描述符 2 越界（LBA=8 cnt=1，共 8 块）
    data_type unmap_data(40, 0);
    unmap_data[15] = 2;
    unmap_data[19] = 1;
    unmap_data[31] = 8;
    unmap_data[35] = 1;
    deliver_out_data(*fixture.msc, 2, unmap_data);

    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(csw_from_submit(stub.submits[0]).bCSWStatus, 1u); // CHECK CONDITION

    // 先整体校验再执行：前面合法的描述符也不能被部分执行
    EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(2), pattern.data(), 512), 0);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, WriteSameFillsRange) {
    // WRITE SAME(16)：DataOut 收 1 块填充数据，逐块写入整个范围（LBA=1 起 3 块）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = 0x93; // WRITE SAME(16)
    cdb[9] = 1;    // LBA=1（be64，低字节在 [9]）
    cdb[13] = 3;   // cnt=3（be32，低字节在 [13]）
    deliver_out_data(*fixture.msc, 1, make_cbw(0xAAAA, 0x00, cdb, 512));

    data_type fill(512, 0x5A);
    deliver_out_data(*fixture.msc, 2, fill);

    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(csw_from_submit(stub.submits[0]).bCSWStatus, 0u);

    for (std::uint64_t lba = 1; lba <= 3; lba++) {
        EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(lba), fill.data(), 512), 0);
    }
    data_type zero(512, 0);
    EXPECT_EQ(std::memcmp(fixture.msc->get_backend()->get_direct_buffer(0), zero.data(), 512), 0); // 未动

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, UnsupportedCommandFails) {
    // 未知 SCSI 命令：命令失败，CSW status=1（不产生数据阶段）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = 0x7F;
    deliver_out_data(*fixture.msc, 1, make_cbw(0xBBBB, 0x80, cdb, 1024));

    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.dCSWTag, 0xBBBBu);
    EXPECT_EQ(csw.bCSWStatus, 1u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, SetInterfaceRejectsNonZeroAlt) {
    // USB 2.0 §9.4.10：MSC 接口只有 alt 0，不存在的 alt 回 Request Error
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    std::uint32_t status = 0;
    fixture.msc->request_set_interface(1, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    status = 1;
    fixture.msc->request_set_interface(0, &status);
    EXPECT_EQ(status, 0u);
}

TEST(TestMscHandler, ReadWriteLbaPlusCountOverflowRejected) {
    // lba=0xFFFFFFFF 时 uint32_t 的 lba+count 会回绕成小值，越界 CDB 曾能
    // 绕过容量检查直达后端缓冲区。修复后按 64 位比较回 CHECK CONDITION
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(1024));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // READ10：lba=0xFFFFFFFF + 1 块，回绕后为 0
    deliver_out_data(*fixture.msc, 1, make_cbw(0x6667, 0x80, read10_cdb(0xFFFFFFFFu, 1), 512));
    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.bCSWStatus, 1u); // failed
    EXPECT_EQ(csw.dCSWDataResidue, 512u);

    // WRITE10：同样的回绕路径
    deliver_out_data(*fixture.msc, 3, make_cbw(0x6668, 0x00, write10_cdb(0xFFFFFFFFu, 1), 512));
    deliver_in_request(*fixture.msc, 4, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 2u);
    csw = csw_from_submit(stub.submits[1]);
    EXPECT_EQ(csw.bCSWStatus, 1u);
    EXPECT_EQ(csw.dCSWDataResidue, 512u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, DataResidueDoesNotLeakToNextCommand) {
    // WRITE SAME 的 dCBWDataTransferLength 大于 1 块时 CSW 残留非零；下一条
    // 无数据阶段的命令（TEST UNIT READY）必须从 0 残留开始，不能把上一条
    // 的残留带进 CSW（BOT：dCSWDataResidue = 应传未传字节数）
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, std::make_unique<MemoryBackend>(8));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // WRITE SAME(16) LBA=1 共 1 块，但主机声明传输 1024 字节 → 残留 512
    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = 0x93;
    cdb[9] = 1;  // LBA=1
    cdb[13] = 1; // cnt=1
    deliver_out_data(*fixture.msc, 1, make_cbw(0xAAAA, 0x00, cdb, 1024));
    deliver_out_data(*fixture.msc, 2, data_type(512, 0x5A));
    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.bCSWStatus, 0u);
    EXPECT_EQ(csw.dCSWDataResidue, 512u); // 多出的 512 字节未消费

    // TEST UNIT READY：无数据阶段，残留必须是 0
    std::array<std::uint8_t, 16> tur{};
    tur[0] = ScsiCmd::TestUnitReady;
    deliver_out_data(*fixture.msc, 4, make_cbw(0xAAAB, 0x00, tur, 0));
    deliver_in_request(*fixture.msc, 5, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 2u);
    csw = csw_from_submit(stub.submits[1]);
    EXPECT_EQ(csw.bCSWStatus, 0u);
    EXPECT_EQ(csw.dCSWDataResidue, 0u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, ReadStagingDataSurvivesDisconnection) {
    // staging 路径的 READ 应答由 sender 线程异步发送。RET_SUBMIT 入队后断连
    // （on_disconnection 清空 staging_data_）不能让应答数据变成悬垂指针
    StringPool string_pool;
    data_type pattern(1024);
    for (std::size_t i = 0; i < pattern.size(); i++) {
        pattern[i] = static_cast<std::uint8_t>(i * 7 + 1);
    }
    // FailingBackend 不提供 direct buffer，强制走 staging 拷贝路径
    auto backend = std::make_unique<FailingBackend>(std::make_unique<MemoryBackend>(8));
    ASSERT_EQ(backend->write(0, 2, pattern.data()), 1024u);
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    deliver_out_data(*fixture.msc, 1, make_cbw(0x6A01, 0x80, read10_cdb(0, 2), 1024));
    deliver_in_request(*fixture.msc, 2, 1024, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);

    // 应答还没发出去就断连
    fixture.msc->on_disconnection(ec);

    // 应答缓冲必须仍持有拷贝的数据（不是 staging 内部指针）
    EXPECT_EQ(data_from_submit(stub.submits[0]), pattern);
}

TEST(TestMscHandler, Write10DataStageLongerThanCbwIsRejected) {
    // 主机声明的数据阶段比 CBW 的 dCBWDataTransferLength 还长：不能让 socket
    // 直读进 mmap（会写出后端缓冲范围），必须丢弃并按 CHECK CONDITION 收尾
    StringPool string_pool;
    auto backend = std::make_unique<MemoryBackend>(8);
    data_type original(512, 0x11);
    ASSERT_EQ(backend->write(7, 1, original.data()), 512u);
    auto fixture = make_msc_device(string_pool, std::move(backend));

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // WRITE10 LBA=7（最后一块）写 1 块，CBW 声明 512 字节
    deliver_out_data(*fixture.msc, 1, make_cbw(0x6A02, 0x00, write10_cdb(7, 1), 512));

    // 主机却发来 1024 字节的 OUT 传输
    auto *op = fixture.msc->get_transfer_operator();
    UsbIpHeaderBasic header{};
    header.direction = UsbIpDirection::Out;
    auto *trx = StorageIoTransfer::from_handle(op->alloc_transfer_handle(1024, 0, header, {}));
    TransferHandle handle(trx, op);
    EXPECT_EQ(trx->external_buf, nullptr); // 不直指 mmap
    EXPECT_FALSE(trx->direct_io);
    trx->fallback_data.assign(1024, 0x22); // 模拟读入的超长数据
    fixture.msc->on_out_data_received(trx, 1024);

    deliver_in_request(*fixture.msc, 2, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.bCSWStatus, 1u); // failed
    EXPECT_EQ(csw.dCSWDataResidue, 512u);

    // 目标块内容不能被超长数据覆盖
    data_type after(512, 0);
    ASSERT_EQ(fixture.msc->get_backend()->read(7, 1, after.data()), 512u);
    EXPECT_EQ(after, original);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, UnmapWithoutBackendFailsCsw) {
    // 没装配后端（配置错误）的设备收到 UNMAP：不能解引用空后端，回 CHECK CONDITION
    StringPool string_pool;
    auto fixture = make_msc_device(string_pool, nullptr);
    ASSERT_EQ(fixture.msc->get_backend(), nullptr);

    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.msc->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::array<std::uint8_t, 16> cdb{};
    cdb[0] = ScsiCmd::Unmap;
    deliver_out_data(*fixture.msc, 1, make_cbw(0x6A03, 0x00, cdb, 40));
    deliver_out_data(*fixture.msc, 2, data_type(40, 0));

    deliver_in_request(*fixture.msc, 3, 13, fixture.ep_in, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    auto csw = csw_from_submit(stub.submits[0]);
    EXPECT_EQ(csw.bCSWStatus, 1u);

    fixture.msc->on_disconnection(ec);
}

TEST(TestMscHandler, RawImageBackendInvalidPathIsSafe) {
    // 镜像打不开（目录不存在）时后端对象仍然构造成功但不可用：I/O 必须安全
    // 返回 0，不能拿空映射指针做 memcpy
    RawImageBackend backend("usbipdcpp_no_such_dir_6f1c/img.bin", 8);
    EXPECT_FALSE(backend.is_valid());
    data_type buf(512, 0xAA);
    EXPECT_EQ(backend.read(0, 1, buf.data()), 0u);
    EXPECT_EQ(backend.write(0, 1, buf.data()), 0u);
    backend.punch_hole(0, 1); // 不能崩
    EXPECT_EQ(backend.get_direct_buffer(0), nullptr);
}
