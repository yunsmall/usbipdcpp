#include "usbipdcpp/virtual_device/network_backends/TunBackend.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if.h>
#include <linux/if_tun.h>

namespace usbipdcpp {

namespace {
// 帧缓冲上限：标准 MTU 1500 + 以太网头，留余量
constexpr std::size_t TUN_MAX_FRAME = 2048;
} // namespace

TunBackend::TunBackend(std::string if_name, std::array<std::uint8_t, 4> ip,
                       std::array<std::uint8_t, 4> netmask) :
    if_name_(std::move(if_name)) {
    // 打开 tun 设备：TUNSETIFF 把 fd 绑到一个新建 tun 接口，fd 关闭时接口销毁
    tun_fd_ = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (tun_fd_ < 0) {
        throw std::runtime_error("打开 /dev/net/tun 失败（需要 root？内核无 CONFIG_TUN？）：" +
                                 std::string(std::strerror(errno)));
    }
    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, if_name_.c_str(), IFNAMSIZ - 1);
    // 必须用 IFF_TAP（二层设备）而非 IFF_TUN：虚拟网卡数据面是完整以太网帧
    // （cdc_ether 主机发的 ARP 请求等），IFF_TUN 是三层点对点设备（NOARP），
    // 写入的 ARP 帧会被内核按 IP 版本检查直接丢弃（实测跨机 ping 不通）
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI; // 以太网帧（不含 4 字节 PI 头）
    if (::ioctl(tun_fd_, TUNSETIFF, &ifr) < 0) {
        int err = errno;
        ::close(tun_fd_);
        tun_fd_ = -1;
        throw std::runtime_error("TUNSETIFF 失败：" + std::string(std::strerror(err)));
    }
    if_name_ = ifr.ifr_name; // 含 %d 时内核回填实际接口名

    // 配 IP/掩码并置 UP：走 AF_INET 套接字 ioctl（与 ip 命令同机制）
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        int err = errno;
        ::close(tun_fd_);
        tun_fd_ = -1;
        throw std::runtime_error("创建配置套接字失败：" + std::string(std::strerror(err)));
    }
    auto set_addr = [&](unsigned long req, const std::uint8_t *addr4) {
        struct ifreq r {};
        std::strncpy(r.ifr_name, if_name_.c_str(), IFNAMSIZ - 1);
        auto *sin = reinterpret_cast<sockaddr_in *>(&r.ifr_addr);
        sin->sin_family = AF_INET;
        std::memcpy(&sin->sin_addr, addr4, 4);
        return ::ioctl(sock, req, &r);
    };
    if (set_addr(SIOCSIFADDR, ip.data()) < 0 || set_addr(SIOCSIFNETMASK, netmask.data()) < 0) {
        int err = errno;
        ::close(sock);
        ::close(tun_fd_);
        tun_fd_ = -1;
        throw std::runtime_error("配置 tun 接口地址失败：" + std::string(std::strerror(err)));
    }
    std::memset(&ifr, 0, sizeof(ifr));
    std::strncpy(ifr.ifr_name, if_name_.c_str(), IFNAMSIZ - 1);
    if (::ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        int err = errno;
        ::close(sock);
        ::close(tun_fd_);
        tun_fd_ = -1;
        throw std::runtime_error("读取接口标志失败：" + std::string(std::strerror(err)));
    }
    ifr.ifr_flags |= IFF_UP;
    if (::ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
        int err = errno;
        ::close(sock);
        ::close(tun_fd_);
        tun_fd_ = -1;
        throw std::runtime_error("启用接口失败：" + std::string(std::strerror(err)));
    }
    ::close(sock);

    read_thread_ = std::thread(&TunBackend::read_loop, this);
}

TunBackend::~TunBackend() {
    stop_ = true;
    // 先 join 再 close：close() 不会打断另一线程正阻塞的 read（fd 引用仍在
    // 内核文件对象上），直接 close 会让 join 永久等待（进程无法退出）；poll
    // 循环在 stop_ 置位后一个超时周期内返回，join 有界
    if (read_thread_.joinable()) {
        read_thread_.join();
    }
    if (tun_fd_ >= 0) {
        ::close(tun_fd_); // 释放 fd，同时销毁 tap 接口
        tun_fd_ = -1;
    }
}

void TunBackend::send_frame(const std::uint8_t *data, std::size_t size) {
    count_rx(size);
    if (tun_fd_ < 0 || size > TUN_MAX_FRAME) {
        return; // 已关闭或超长帧直接丢弃（网络数据可丢，TCP 重传兜底）
    }
    // tap 设备单帧消费，write 一般整帧写入；部分写入/失败视为丢帧，不重试
    ::write(tun_fd_, data, size);
}

void TunBackend::read_loop() {
    std::uint8_t buf[TUN_MAX_FRAME];
    struct pollfd pfd {};
    pfd.fd = tun_fd_;
    pfd.events = POLLIN;
    while (!stop_) {
        // 带超时的 poll：既等数据又定期检查 stop_（析构 join 需要及时返回）
        int r = ::poll(&pfd, 1, 100);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; // fd 失效（接口销毁等）
        }
        if (r == 0) {
            continue; // 超时，回查 stop_
        }
        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }
        ssize_t n = ::read(tun_fd_, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        send_to_host(buf, static_cast<std::size_t>(n));
    }
}

} // namespace usbipdcpp
