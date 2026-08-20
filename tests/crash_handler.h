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

#elif defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <cstdio>
#include <cstdlib>

// 用 _Unwind_Backtrace 收集返回地址（Itanium C++ ABI，GCC/Clang 编译器运行时
// 提供，glibc/bionic/musl 全有且无 API 门控），再经 dladdr 解析为模块+文件内
// 偏移（glibc 2.34+/bionic/macOS 均在 libc 内，无 API 门控），可直接喂 addr2line
#include <dlfcn.h>
#include <unwind.h>

namespace {

/// _Unwind_Backtrace 回调：把返回地址收集进 buffer
struct unwind_state {
    void **buffer;
    int count;
    int max;
};

_Unwind_Reason_Code unwind_callback(struct _Unwind_Context *ctx, void *arg) {
    auto *state = static_cast<unwind_state *>(arg);
    if (state->count < state->max) {
        state->buffer[state->count++] = reinterpret_cast<void *>(_Unwind_GetIP(ctx));
    }
    return _URC_NO_REASON;
}

/// 打印一帧：优先 dladdr 解析（模块+符号名+偏移），失败退化为裸地址
void print_frame(int index, void *addr) {
    Dl_info info;
    if (dladdr(addr, &info) != 0 && info.dli_fname != nullptr) {
        if (info.dli_sname != nullptr) {
            std::fprintf(stderr, "  [%d] %s(%s+0x%tx) [%p]\n", index, info.dli_fname, info.dli_sname,
                         static_cast<char *>(addr) - static_cast<char *>(info.dli_saddr), addr);
        }
        else {
            std::fprintf(stderr, "  [%d] %s(+0x%tx) [%p]\n", index, info.dli_fname,
                         static_cast<char *>(addr) - static_cast<char *>(info.dli_fbase), addr);
        }
    }
    else {
        std::fprintf(stderr, "  [%d] %p\n", index, addr);
    }
}

} // namespace

inline void crash_signal_handler(int sig) {
    std::fprintf(stderr, "\n=== CRASH: signal %d ===\n", sig);
    void *stack[64];
    unwind_state state{stack, 0, 64};
    _Unwind_Backtrace(unwind_callback, &state);
    for (int i = 0; i < state.count; ++i) {
        print_frame(i, stack[i]);
    }
    std::fflush(stderr);
    _exit(128 + sig);
}

inline int install_crash_handler() {
    std::signal(SIGSEGV, crash_signal_handler);
    std::signal(SIGABRT, crash_signal_handler);
    std::signal(SIGBUS, crash_signal_handler);
    return 0;
}

#else
// 其他平台（无 _WIN32 / Unix 宏）：退化为不注册，崩溃由系统默认处理
inline int install_crash_handler() {
    return 0;
}
#endif

// 静态初始化阶段注册（C++17 inline 变量，每个进程执行一次）
inline int g_crash_handler_installed = install_crash_handler();
