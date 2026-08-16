#pragma once

// 进程启动时注册崩溃/异常处理器，崩溃时打印当前线程调用栈，便于 CI 定位
// 偶发的崩溃现场（Windows 上 gtest 默认崩溃栈为空，Linux Release 无符号时
// 只有地址）。栈以地址形式打印，配 addr2line（Linux）/ pdb（Windows）离线
// 符号化。挂起（不崩溃）的现场不在这里覆盖，由 ctest --timeout 定位测试名。
//
// 用法：测试 .cpp 通过 test_utils.h 间接包含本文件；每个测试是独立进程
// （gtest_discover_tests），静态初始化阶段（main 之前）注册一次。

#if defined(_WIN32)
#include <windows.h>
#include <cstdio>

inline LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    std::fprintf(stderr, "\n=== CRASH: exception code 0x%08lx ===\n",
                 ep->ExceptionRecord->ExceptionCode);
    void *stack[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stack, nullptr);
    for (USHORT i = 0; i < frames; ++i) {
        std::fprintf(stderr, "  [%u] %p\n", i, stack[i]);
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER; // 交给系统默认处理
}

inline int install_crash_handler() {
    SetUnhandledExceptionFilter(crash_filter);
    return 0;
}

#else
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <unistd.h>

inline void crash_signal_handler(int sig) {
    std::fprintf(stderr, "\n=== CRASH: signal %d ===\n", sig);
    void *stack[64];
    int frames = backtrace(stack, 64);
    backtrace_symbols_fd(stack, frames, STDERR_FILENO);
    std::fflush(stderr);
    _exit(128 + sig);
}

inline int install_crash_handler() {
    std::signal(SIGSEGV, crash_signal_handler);
    std::signal(SIGABRT, crash_signal_handler);
    std::signal(SIGBUS, crash_signal_handler);
    return 0;
}
#endif

// 静态初始化阶段注册（C++17 inline 变量，每个进程执行一次）
inline int g_crash_handler_installed = install_crash_handler();
