// 连接/停止/竞态类测试：单次运行很难暴露问题，需要大量重复（如
// ctest --repeat until-fail:2000）才能压出偶发的竞态与挂起。与纯数据结构的
// 测试（test_network.cpp）分开存放，方便只对这类测试做高频重复：
//   ctest --repeat until-fail:2000 --timeout 120 -R "TestNetworkConnection"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "test_utils.h"

#include "usbipdcpp/Server.h"
#include "usbipdcpp/network.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

TEST(TestNetworkConnection,ServerCanRestartAfterStop) {
    // stop() 必须关闭 acceptor 释放端口，否则再次 start() 时 bind 同一端口失败。
    // 端口 0 由系统分配：第一次 start 后 ep 被更新为实际端口，第二次 start
    // 重绑同一端口，验证端口确实释放（并行压测下端口 0 无探测竞争）
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;

    for (int round = 0; round < 2; round++) {
        ASSERT_FALSE(server.start(ep));
        // 端口 0 启动，实际监听端点（含系统分配的端口）用 endpoint() 查询
        auto actual_ep = server.endpoint();

        // acceptor 在 start() 内同步 bind，返回时监听已就绪，轮询仅为防御性
        // 等待。超时给 4 秒：覆盖率插桩等慢速构建下服务器启动可能超过 1 秒
        asio::ip::tcp::socket probe_sock(io);
        bool connected = false;
        for (int i = 0; i < 200; i++) {
            std::error_code ec;
            probe_sock.connect(actual_ep, ec);
            if (!ec) {
                connected = true;
                break;
            }
            probe_sock.close();
            probe_sock = asio::ip::tcp::socket(io);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(connected);
        // 主动断开，让服务器侧的 session 快速退出
        probe_sock.close();

        server.stop();
    }
}

TEST(TestNetworkConnection,ServerCanStopWithoutConnection) {
    // start() 后没有任何客户端连接就 stop()：挂在 accept 上的阻塞 accept 被
    // 唤醒连接打断，网络线程要能干净退出，且可以再次 start()
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;

    for (int round = 0; round < 3; round++) {
        ASSERT_FALSE(server.start(ep));
        // 不做任何连接，直接停止
        server.stop();
    }
}

// ---------------------------------------------------------------------------
// start/stop 循环（合法用法）下的刁钻场景
// ---------------------------------------------------------------------------

TEST(TestNetworkConnection,ManyStartStopCycles) {
    // 无客户端情况下 100 轮 start/stop：acceptor 每轮都要干净释放端口，
    // 网络线程每轮都要干净退出，不能累积残留
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    for (int round = 0; round < 100; round++) {
        ASSERT_FALSE(server.start(ep));
        server.stop();
    }
}

TEST(TestNetworkConnection,ServerCanRestartAfterAcceptError) {
#ifdef _WIN32
    // Windows 上无法通过占满 fd 制造 accept 错误（对应 WSAEMFILE 不可控），跳过
    GTEST_SKIP() << "Windows 上无法通过占满 fd 制造 accept 错误";
#else
    // 回归两个 bug：fd 耗尽时 accept_loop 走错误分支退出后——
    // - 错误分支必须 close acceptor 释放监听端口（否则端口占用到 stop()）
    // - stop() 必须能正常收尾（网络线程已死，join 立即返回、close 幂等），
    //   且错误分支已 close 的 acceptor 能被下次 start() 重新 open/bind/listen
    //   （acceptor 是长命成员，错误退出时已关，重新初始化无残留）
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));
    // 端口 0 由系统分配，start 后查询实际端口供子进程 connect
    const auto port = server.endpoint().port();

    // RAII 包装占用的 fd：ASSERT 失败会直接 return 退出测试函数，vector<int>
    // 析构不会 close fd，若中途失败未释放，后续所有测试都将在 fd 紧张的
    // 状态下运行（socket 创建失败、accept 报 EMFILE），连环失败难以排查。
    // move 必须把源置空：vector 扩容走 move，若两边都持 fd 会在析构时
    // double close，可能关掉后续测试新开 socket 复用的 fd 号
    struct ScopedFd {
        int fd = -1;
        ScopedFd() = default;
        explicit ScopedFd(int f) : fd(f) {
        }
        ScopedFd(const ScopedFd &) = delete;
        ScopedFd &operator=(const ScopedFd &) = delete;
        ScopedFd(ScopedFd &&other) noexcept : fd(other.fd) {
            other.fd = -1;
        }
        ScopedFd &operator=(ScopedFd &&other) noexcept {
            if (this != &other) {
                if (fd >= 0) {
                    ::close(fd);
                }
                fd = other.fd;
                other.fd = -1;
            }
            return *this;
        }
        ~ScopedFd() {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    };

    // 占满进程 fd：占满后服务器的 accept4 将报 EMFILE。本进程不能再发起
    // connect（WSL 实测 connect 也要分配 fd，占满时直接 EMFILE），因此用 fork
    // 的子进程发起连接：子进程关闭继承的 fd 后限额恢复，connect 不受影响
    std::vector<ScopedFd> occupied_fds;
    for (int i = 0; i < 65536; i++) {
        int fd = ::open("/dev/null", O_RDONLY);
        if (fd < 0) {
            break;
        }
        occupied_fds.push_back(ScopedFd{fd});
    }
    ASSERT_FALSE(occupied_fds.empty());

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // 子进程：关闭从父进程继承的所有 fd（stdin/out/err 之外），恢复自己的
        // fd 限额，否则 connect 同样 EMFILE。closefrom 在 macOS 默认 feature
        // 宏下不声明、close_range 是 Linux 专属，统一用可移植的循环。
        // 只做原始 socket 操作，不碰 asio 和 gtest（fork 后这些库的锁状态
        // 不安全），结果通过退出码回传
        long max_fd = sysconf(_SC_OPEN_MAX);
        for (long fd = 3; fd < max_fd; ++fd) {
            ::close(static_cast<int>(fd));
        }
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            _exit(1);
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int ret = ::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        _exit(ret == 0 ? 0 : 1);
    }
    int status = 0;
    ASSERT_GE(waitpid(pid, &status, 0), 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "子进程 connect 失败";

    // 保持 fd 占满一段确定时间再释放：子进程 connect 返回后、本进程释放 fd
    // 之前，服务器必须已经执行 accept4（fd 仍满 → EMFILE）。若先释放 fd，
    // 服务器 accept4 可能成功（不 EMFILE），端口一直监听导致下面的轮询超时
    // （高频重复实测复现）
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 正常路径提前释放（异常路径由 ScopedFd 析构兜底），保证后续测试不受影响
    occupied_fds.clear();

    // 轮询等待服务器释放监听端口：错误分支 close acceptor 后端口立即可绑定，
    // 可绑定 = 网络线程已退出（同时验证错误分支确实 close 了端口）
    bool port_released = false;
    for (int i = 0; i < 200; i++) {
        asio::ip::tcp::acceptor probe(io);
        probe.open(asio::ip::tcp::v4());
        probe.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        std::error_code probe_ec;
        probe.bind(ep, probe_ec);
        if (!probe_ec) {
            port_released = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(port_released);

    // stop→start 后必须恢复正常：此时网络线程已因错误退出，stop() 的 join
    // 立即返回、close 幂等；acceptor 已被错误分支关闭，start() 重新初始化
    server.stop();
    ASSERT_FALSE(server.start(ep));

    // 新客户端连接 + devlist 请求得到正常回复 = accept 循环活着
    asio::ip::tcp::socket client2(io);
    ASSERT_TRUE(connect_with_retry(client2, server.endpoint()));
    usbipdcpp::error_code send_ec;
    asio::write(client2, asio::buffer(UsbIpCommand::OpReqDevlist{}.to_bytes()), send_ec);
    ASSERT_FALSE(send_ec);
    ASSERT_EQ(usbipdcpp::read_u16(client2), USBIP_VERSION);
    ASSERT_EQ(usbipdcpp::read_u16(client2), OP_REP_DEVLIST);
    client2.close();

    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
#endif
}

TEST(TestNetworkConnection,StopWithSilentClient) {
    // 客户端连接后不发任何数据静默挂着（session 挂在 parse_op 的阻塞读上），
    // 此时 stop()：immediately_stop 要打断挂起的读，session 干净退出。
    // 与导入设备后的传输态打断（ServerCanStopWithImportedDevice）互补
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));

    server.stop(); // 客户端还静默挂着，直接 stop

    client.close();
}

TEST(TestNetworkConnection,StopWithMultipleClients) {
    // 多个客户端同时挂着（多个 session 同时被打断），stop() 要并发打断并
    // join 所有 session 线程，任何一个都不能卡住
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));

    std::vector<asio::ip::tcp::socket> clients;
    for (int i = 0; i < 10; i++) {
        clients.emplace_back(io);
        ASSERT_TRUE(connect_with_retry(clients.back(), server.endpoint()));
    }

    server.stop(); // 10 个 session 同时被打断

    for (auto &client: clients) {
        client.close();
    }
}

TEST(TestNetworkConnection,DisconnectRightAfterDevlistRequest) {
    // 客户端发出 devlist 请求后不等回复立即断开：服务器可能正在收集设备
    // 列表或写回复时发现连接已断，session 要干净退出，不崩
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));

    for (int i = 0; i < 5; i++) {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        usbipdcpp::error_code send_ec;
        asio::write(client, asio::buffer(UsbIpCommand::OpReqDevlist{}.to_bytes()), send_ec);
        ASSERT_FALSE(send_ec);
        if (i % 2 == 0) {
            rst_disconnect(client); // 不读回复直接 RST
        }
        else {
            client.close(); // 不读回复直接 FIN
        }
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkConnection,StopDuringClientConnectRace) {
    // stop() 与客户端并发连接竞争：客户端线程反复尝试连接（server 停止期间
    // 连接会失败），主线程反复 start/stop。stop 时可能恰有连接刚被 accept、
    // session 刚创建，任何一轮都不能崩或卡死
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    std::atomic_bool stop_flag = false;
    // 实际端口（端口 0 启动，由系统分配）在 start 返回后用 endpoint() 查询，
    // 经原子变量传给客户端线程；直接让客户端线程读 ep/endpoint() 会与
    // start 期间的回写并发（TSan 实测命中）
    std::atomic<std::uint16_t> target_port = 0;

    std::thread client_thread([&]() {
        while (!stop_flag) {
            auto port = target_port.load();
            if (port != 0) {
                asio::ip::tcp::socket client(io);
                std::error_code ec;
                client.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port), ec);
                if (!ec) {
                    client.close(); // 连上就断，模拟短暂的连接
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    for (int round = 0; round < 50; round++) {
        ASSERT_FALSE(server.start(ep));
        target_port.store(server.endpoint().port());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        server.stop();
        target_port.store(0);
    }

    stop_flag = true;
    client_thread.join();
}

TEST(TestNetworkConnection,ManyQuickConnections) {
    // 客户端以各种方式快速连接并断开：优雅断开（FIN）、RST 重置、发垃圾
    // 数据后断开。服务器要全部处理干净（session 各自退出），之后 stop() 正常
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));

    for (int i = 0; i < 50; i++) {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        switch (i % 3) {
            case 0:
                client.close(); // 优雅断开（FIN）
                break;
            case 1:
                rst_disconnect(client); // 异常掉线（RST）
                break;
            case 2: {
                // 发一段垃圾数据再断开：服务器要么解析失败，要么读中断
                const std::array<std::uint8_t, 8> garbage = {0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01};
                asio::write(client, asio::buffer(garbage));
                client.close();
                break;
            }
        }
    }

    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}
