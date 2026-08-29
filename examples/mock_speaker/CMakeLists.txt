# PlaybackSink（本机播放）依赖 miniaudio（单头文件库，可选）。
# 查找方式同 mock_audio：先直接找 miniaudio.h（含 Homebrew 的 include/miniaudio/
# 子目录布局），找不到走 pkgconf（Termux 等只有 pkg-config 的环境）；两种都找不到
# 时 mock_speaker 仍正常编译（--output 的 WavFileSink 与丢弃模式不受影响），
# 只是不提供本机播放选项
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)
if (NOT MINIAUDIO_INCLUDE_DIR)
    find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h PATH_SUFFIXES miniaudio)
endif ()
if (MINIAUDIO_INCLUDE_DIR)
    set(MINIAUDIO_INCLUDE_DIRS ${MINIAUDIO_INCLUDE_DIR})
else ()
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(MINIAUDIO_PKGCONF QUIET miniaudio)
        if (MINIAUDIO_PKGCONF_FOUND)
            set(MINIAUDIO_INCLUDE_DIRS ${MINIAUDIO_PKGCONF_INCLUDE_DIRS})
        endif ()
    endif ()
endif ()

file(GLOB_RECURSE src *.cpp)
# 测试文件单独编译进 test target，不参与主程序
list(FILTER src EXCLUDE REGEX "test_.*\\.cpp")
if (MINIAUDIO_INCLUDE_DIRS)
    message(STATUS "miniaudio: ENABLED (include: ${MINIAUDIO_INCLUDE_DIRS})")
else ()
    list(FILTER src EXCLUDE REGEX "PlaybackSink")
    message(STATUS "miniaudio: DISABLED (no local playback)")
endif ()
add_executable(mock_speaker ${src})
target_link_libraries(mock_speaker usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_speaker PRIVATE .)
if (MINIAUDIO_INCLUDE_DIRS)
    target_include_directories(mock_speaker PRIVATE ${MINIAUDIO_INCLUDE_DIRS})
    target_compile_definitions(mock_speaker PRIVATE USBIPDCPP_USE_MINIAUDIO)
endif ()
