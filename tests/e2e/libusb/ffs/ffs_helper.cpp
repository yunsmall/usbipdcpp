#include "ffs_helper.hpp"

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

namespace usbipdcpp::e2e {

namespace {

/**
 * @brief FunctionFS 字符串块的头部（内核 uapi 只给了描述符头，没有这个）
 *
 * 即使设备不需要任何字符串，也必须写一个 str_count=lang_count=0 的
 * 最小块，FunctionFS 才会从“读字符串”状态进入 ACTIVE 并创建端点文件。
 */
struct FunctionfsStringsHead {
    std::uint32_t magic;
    std::uint32_t length;
    std::uint32_t str_count;
    std::uint32_t lang_count;
} __attribute__((packed));

void append_bytes(std::vector<std::uint8_t> &blob, const void *data, std::size_t length) {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    blob.insert(blob.end(), bytes, bytes + length);
}

/**
 * @brief 按 FunctionFS V2 格式拼描述符块
 *
 * length 字段必须等于整次 write 的长度（内核 __ffs_data_got_descs 会校验），
 * 所以先写占位值，拼完再回填。
 */
std::vector<std::uint8_t> build_descriptor_blob(const FfsDescriptors &descriptors) {
    std::vector<std::uint8_t> blob;

    usb_functionfs_descs_head_v2 head {};
    head.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    head.length = 0;
    // ALL_CTRL_RECIP：让 device recipient 的 vendor 请求也能转到用户态，
    // 这样可以用 wIndex 直接携带端点地址。
    head.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC |
                         FUNCTIONFS_ALL_CTRL_RECIP);
    append_bytes(blob, &head, sizeof(head));

    // 所有 speed 的描述符个数必须集中在描述符块前面，不能和各自的
    // 描述符字节交错（内核 __ffs_data_got_descs 先连续读 counts 再连续读描述符）
    const std::uint32_t full_speed_count = htole32(descriptors.full_speed.count);
    const std::uint32_t high_speed_count = htole32(descriptors.high_speed.count);
    append_bytes(blob, &full_speed_count, sizeof(full_speed_count));
    append_bytes(blob, &high_speed_count, sizeof(high_speed_count));

    append_bytes(blob, descriptors.full_speed.bytes.data(), descriptors.full_speed.bytes.size());
    append_bytes(blob, descriptors.high_speed.bytes.data(), descriptors.high_speed.bytes.size());

    const std::uint32_t total_length = htole32(static_cast<std::uint32_t>(blob.size()));
    std::memcpy(blob.data() + offsetof(usb_functionfs_descs_head_v2, length), &total_length,
                sizeof(total_length));
    return blob;
}

}  // namespace

FfsDevice::FfsDevice(std::string mount_dir) : mount_dir(std::move(mount_dir)) {
}

FfsDevice::~FfsDevice() {
    if (ep0_fd >= 0) {
        ::close(ep0_fd);
    }
}

int FfsDevice::run() {
    const std::string ep0_path = endpoint_path("ep0");
    ep0_fd = ::open(ep0_path.c_str(), O_RDWR);
    if (ep0_fd < 0) {
        log("打开 %s 失败: %s", ep0_path.c_str(), std::strerror(errno));
        return 1;
    }

    try {
        write_descriptors();
        write_strings();
    }
    catch (const std::exception &e) {
        log("写 FunctionFS 描述符失败: %s", e.what());
        return 1;
    }

    log("FFS_READY");
    std::fflush(stderr);

    std::array<usb_functionfs_event, 8> events {};
    while (true) {
        const ssize_t length = ::read(ep0_fd, events.data(), sizeof(events));
        if (length < 0) {
            if (errno == EINTR) {
                continue;
            }
            log("读 ep0 事件失败: %s", std::strerror(errno));
            return 1;
        }
        if (length == 0) {
            log("ep0 读到 EOF，退出事件循环");
            return 0;
        }
        const std::size_t count = static_cast<std::size_t>(length) / sizeof(usb_functionfs_event);
        for (std::size_t i = 0; i < count; ++i) {
            handle_event(events[i]);
        }
    }
}

std::string FfsDevice::endpoint_path(const char *name) const {
    return mount_dir + "/" + name;
}

int FfsDevice::open_endpoint(const char *name) const {
    const std::string path = endpoint_path(name);
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        log("打开 %s 失败: %s", path.c_str(), std::strerror(errno));
    }
    return fd;
}

bool FfsDevice::endpoint_descriptor(int fd, usb_endpoint_descriptor &descriptor) {
    descriptor = usb_endpoint_descriptor {};
    const int result = ioctl(fd, FUNCTIONFS_ENDPOINT_DESC, &descriptor);
    if (result < 0) {
        log("取端点描述符失败: %s", std::strerror(errno));
        return false;
    }
    return true;
}

bool FfsDevice::setup_is_in() const {
    return (pending_setup.bRequestType & USB_DIR_IN) != 0;
}

std::uint16_t FfsDevice::setup_length() const {
    return le16toh(pending_setup.wLength);
}

std::uint16_t FfsDevice::setup_value(const usb_ctrlrequest &setup) {
    return le16toh(setup.wValue);
}

std::uint16_t FfsDevice::setup_index(const usb_ctrlrequest &setup) {
    return le16toh(setup.wIndex);
}

void FfsDevice::send_ep0(const void *data, std::size_t length) {
    ssize_t written;
    do {
        written = ::write(ep0_fd, data, length);
    } while (written < 0 && errno == EINTR);
    if (written < 0) {
        if (errno == EIDRM || errno == ESHUTDOWN) {
            // 主机已经取消（超时/断开），属正常控制请求失败
            return;
        }
        log("写 ep0 失败: %s", std::strerror(errno));
        return;
    }
    if (static_cast<std::size_t>(written) != length) {
        log("ep0 短写: %zd/%zu", written, length);
    }
}

ssize_t FfsDevice::recv_ep0(void *data, std::size_t length) {
    ssize_t received;
    do {
        received = ::read(ep0_fd, data, length);
    } while (received < 0 && errno == EINTR);
    if (received < 0 && errno != EIDRM && errno != ESHUTDOWN) {
        log("读 ep0 数据失败: %s", std::strerror(errno));
    }
    return received;
}

void FfsDevice::complete_ep0_no_data() {
    recv_ep0(nullptr, 0);
}

void FfsDevice::stall_ep0() {
    char dummy = 0;
    // 反方向操作 ep0 是 FunctionFS 约定的 stall 方式；成功时返回 -1，
    // IN 方向 errno 为 EL2HLT，属正常结果，不能当错误打日志
    if (setup_is_in()) {
        ssize_t result;
        do {
            result = ::read(ep0_fd, &dummy, sizeof(dummy));
        } while (result < 0 && errno == EINTR);
        if (result < 0 && errno != EL2HLT && errno != EIDRM && errno != ESHUTDOWN) {
            log("stall ep0 失败: %s", std::strerror(errno));
        }
    }
    else {
        const ssize_t written = ::write(ep0_fd, &dummy, sizeof(dummy));
        if (written < 0 && errno != EL2HLT && errno != EIDRM && errno != ESHUTDOWN) {
            log("stall ep0 失败: %s", std::strerror(errno));
        }
    }
}

void FfsDevice::log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    std::fprintf(stderr, "[ffs] ");
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
    std::fflush(stderr);
}

void FfsDevice::write_descriptors() {
    const std::vector<std::uint8_t> blob = build_descriptor_blob(make_descriptors());
    const ssize_t written = ::write(ep0_fd, blob.data(), blob.size());
    if (written < 0) {
        std::string dump;
        char item[8];
        for (const std::uint8_t byte : blob) {
            std::snprintf(item, sizeof(item), "%02x ", byte);
            dump += item;
        }
        log("描述符块（%zu 字节）: %s", blob.size(), dump.c_str());
        throw std::runtime_error(std::string("write: ") + std::strerror(errno));
    }
    if (static_cast<std::size_t>(written) != blob.size()) {
        throw std::runtime_error("写入长度与描述符块长度不一致");
    }
}

void FfsDevice::write_strings() {
    FunctionfsStringsHead head {};
    head.magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
    head.length = htole32(sizeof(head));
    head.str_count = 0;
    head.lang_count = 0;

    const ssize_t written = ::write(ep0_fd, &head, sizeof(head));
    if (written < 0) {
        throw std::runtime_error(std::string("write strings: ") + std::strerror(errno));
    }
    if (static_cast<std::size_t>(written) != sizeof(head)) {
        throw std::runtime_error("写入字符串块长度不一致");
    }
}

void FfsDevice::handle_event(const usb_functionfs_event &event) {
    switch (event.type) {
        case FUNCTIONFS_ENABLE:
            log("端点已启用");
            on_enable();
            break;

        case FUNCTIONFS_DISABLE:
            log("端点已禁用");
            on_disable();
            break;

        case FUNCTIONFS_SETUP:
            pending_setup = event.u.setup;
            try {
                handle_setup(pending_setup);
            }
            catch (const std::exception &e) {
                log("处理控制请求 %02x 失败: %s", pending_setup.bRequest, e.what());
                stall_ep0();
            }
            break;

        case FUNCTIONFS_SUSPEND:
            log("设备挂起");
            break;

        case FUNCTIONFS_RESUME:
            log("设备恢复");
            break;

        case FUNCTIONFS_BIND:
            log("function 已绑定到配置");
            break;

        case FUNCTIONFS_UNBIND:
            log("function 已从配置解绑");
            break;

        default:
            log("未知 ep0 事件 %u", event.type);
            break;
    }
}

}  // namespace usbipdcpp::e2e
