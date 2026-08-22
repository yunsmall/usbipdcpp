# AudioFileSource（--audio 选项）依赖 miniaudio（单头文件库，可选）。
# miniaudio 查找两种方式：先直接找 miniaudio.h，找不到走 pkgconf（Termux 等
# 只有 pkg-config 的环境）；两种都找不到才算真找不到，此时打印 DISABLED 并跳过
# AudioFileSource（sine/fourier 音源不受影响），同 mock_uvc_ffmpeg 的做法。
# stb_vorbis.c 的布局差异（vcpkg include 根 / Termux include/stb/ 子目录）
# 由 AudioFileSource.cpp 里的 __has_include 自适应，无需在此检测。
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)
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

file(GLOB_RECURSE src *.cpp *.c)
# 测试文件单独编译进 test target，不参与主程序
list(FILTER src EXCLUDE REGEX "test_.*\\.cpp")
if (MINIAUDIO_INCLUDE_DIRS)
    message(STATUS "miniaudio: ENABLED (include: ${MINIAUDIO_INCLUDE_DIRS})")
else ()
    list(FILTER src EXCLUDE REGEX "AudioFileSource")
    message(STATUS "miniaudio: DISABLED (--audio option unavailable)")
endif ()
add_executable(mock_audio ${src})
target_link_libraries(mock_audio usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_audio PRIVATE .)
if (MINIAUDIO_INCLUDE_DIRS)
    target_include_directories(mock_audio PRIVATE ${MINIAUDIO_INCLUDE_DIRS})
    target_compile_definitions(mock_audio PRIVATE USBIPDCPP_USE_MINIAUDIO)
endif ()

# AudioFileSource 单元测试（实现随示例走：构建 tests 且 miniaudio 可用时添加）
if (USBIPDCPP_BUILD_TESTS AND MINIAUDIO_INCLUDE_DIRS)
    find_package(GTest CONFIG QUIET)
    if (GTest_FOUND)
        include(GoogleTest)
        enable_testing()
        add_executable(test_mock_audio_file test_audio_file_source.cpp AudioFileSource.cpp)
        target_link_libraries(test_mock_audio_file PRIVATE usbipdcpp_virtual_device GTest::gtest GTest::gtest_main)
        target_include_directories(test_mock_audio_file PRIVATE . ${MINIAUDIO_INCLUDE_DIRS})
        target_compile_definitions(test_mock_audio_file PRIVATE USBIPDCPP_USE_MINIAUDIO)
        # PRE_TEST：测试发现移到构建期（同 tests/CMakeLists.txt 的做法，避免并行构建竞态）
        gtest_discover_tests(test_mock_audio_file DISCOVERY_MODE PRE_TEST)
    endif ()
endif ()
