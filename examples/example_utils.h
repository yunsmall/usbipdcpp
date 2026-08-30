#pragma once

#include <csignal>
#include <cstdint>
#include <cxxopts.hpp>
#include <iostream>
#include <semaphore>
#include <string>

#if defined(_WIN32)
// 控制台事件处理需要 windows.h；WIN32_LEAN_AND_MEAN 排除 winsock.h（否则与
// asio 的 winsock2.h 冲突），NOMINMAX 防止 min/max 宏破坏 std::min/std::max
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#if defined(_WIN32)
// 控制台事件处理需要 windows.h；NOMINMAX 防止 min/max 宏破坏 std::min/std::max
#define NOMINMAX
#include <windows.h>
#endif

/// 创建带通用参数的 cxxopts::Options（--port, --busid, --help）。
inline cxxopts::Options make_example_options(const std::string &name, const std::string &desc) {
    cxxopts::Options opts(name, desc);
    opts.add_options()
        ("p,port", "TCP port", cxxopts::value<std::uint16_t>()->default_value("53240"))
        ("b,busid", "Bus ID", cxxopts::value<std::string>()->default_value("1-1"))
        ("h,help", "Print help");
    return opts;
}

/// 标准解析 + 错误/帮助打印，成功返回 ParseResult。
inline cxxopts::ParseResult parse_example_args(cxxopts::Options &opts, int argc, char **argv) {
    try {
        auto result = opts.parse(argc, argv);
        if (result.count("help")) {
            std::cout << opts.help() << std::endl;
            std::exit(0);
        }
        return result;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        std::cout << opts.help() << std::endl;
        std::exit(1);
    }
}

namespace {

// 退出信号量：handler 置位，wait_for_exit 阻塞等待。
// 头文件工具函数用匿名命名空间：每个 TU 一份（只在本 TU 使用，无跨 TU 共享）。
// POSIX 信号 handler 中调用 release()：libstdc++/libc++ 实现为原子 + futex，
// 无锁无分配，实践中 async-signal 安全（sem_t 的 sem_post 是标准保证的
// 等价物）；Windows 控制台 handler 在普通线程上下文执行，无此约束
std::binary_semaphore g_wait_exit_sem(0);

#if defined(_WIN32)
/// Windows 控制台事件处理器（Ctrl+C / Ctrl+Break / 关窗口 / 注销 / 关机）。
/// 返回 TRUE 表示已处理：进程不会按默认动作终止，主线程由信号量唤醒后清理退出
BOOL WINAPI console_ctrl_handler(DWORD) {
    g_wait_exit_sem.release();
    return TRUE;
}
#else
/// POSIX 信号处理器（SIGINT / SIGTERM）
void exit_signal_handler(int) {
    g_wait_exit_sem.release();
}
#endif

} // namespace

/// 阻塞等待退出信号，返回后调用方清理退出（server.stop 等）。
/// POSIX：Ctrl+C（SIGINT）/ kill -TERM（SIGTERM）——后台部署直接
/// `./mock_xxx &`，退出 `kill -TERM <pid>`，服务器走正常清理路径；
/// Windows：控制台事件（Ctrl+C / Ctrl+Break / 关窗口等），同样优雅清理。
/// 用 handler + 信号量而非 sigwait：sigwait 要求调用线程先阻塞信号，而服务器
/// 连接线程未阻塞，进程信号会被它们按默认动作（直接终止进程）吃掉，清理
/// 代码跑不到；handler 对全进程生效则无此问题
inline void wait_for_exit() {
#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    g_wait_exit_sem.acquire();
#else
    struct sigaction sa {};
    sa.sa_handler = exit_signal_handler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    g_wait_exit_sem.acquire();
#endif
}
