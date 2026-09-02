# AudioFileSource（--audio 选项）依赖 miniaudio（单头文件库，可选），检测逻辑
# 照抄 mock_audio（AudioFileSource 源码在 mock_audio 目录，跨目录引用）：
# miniaudio 查找两种方式：先直接找 miniaudio.h，找不到走 pkgconf（Termux 等
# 只有 pkg-config 的环境）；两种都找不到才算真找不到，此时打印 DISABLED 并跳过
# AudioFileSource（sine/fourier 音源不受影响），同 mock_uvc_ffmpeg 的做法。
# stb_vorbis.c 的布局差异（vcpkg include 根 / Termux include/stb/ 子目录）
# 由 AudioFileSource.cpp 里的 __has_include 自适应，无需在此检测。
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)
if (NOT MINIAUDIO_INCLUDE_DIR)
    # Homebrew 0.11.25+ 把头文件装在 include/miniaudio/ 子目录，其 miniaudio.pc 的
    # Cflags 也指向 include 根（pkgconf 分支同样找不到），故必须直接找子目录。
    # PATH_SUFFIXES 使 find_path 返回 <prefix>/include/miniaudio，正是 -I 需要的目录
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

# AudioVideoFileSource（--video 音画同步源）依赖 FFmpeg（含 libswresample 音频
# 重采样），检测照 mock_uvc_ffmpeg；不可用时 --video 选项不注册
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(ffmpeg IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)
endif ()

file(GLOB_RECURSE src *.cpp *.c)
# 测试文件单独编译进 test target，不参与主程序（同 mock_audio 的做法）
list(FILTER src EXCLUDE REGEX "test_.*\\.cpp")
# AudioVideoFileSource 只在 FFmpeg 可用时编入（非 FFmpeg 构建无 libav* 头）
list(FILTER src EXCLUDE REGEX "AudioVideoFileSource")
if (MINIAUDIO_INCLUDE_DIRS)
    message(STATUS "miniaudio: ENABLED (include: ${MINIAUDIO_INCLUDE_DIRS})")
    list(APPEND src ../mock_audio/AudioFileSource.cpp)
else ()
    message(STATUS "miniaudio: DISABLED (--audio option unavailable)")
endif ()
if (PkgConfig_FOUND AND ffmpeg_FOUND)
    list(APPEND src AudioVideoFileSource.cpp)
    message(STATUS "FFmpeg: ENABLED (building A/V-synchronized --video source)")
else ()
    message(STATUS "FFmpeg: DISABLED (--video option unavailable)")
endif ()
add_executable(mock_webcam ${src})
target_link_libraries(mock_webcam PRIVATE usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_webcam PRIVATE . ../mock_audio)
if (MINIAUDIO_INCLUDE_DIRS)
    target_include_directories(mock_webcam PRIVATE ${MINIAUDIO_INCLUDE_DIRS})
    target_compile_definitions(mock_webcam PRIVATE USBIPDCPP_USE_MINIAUDIO)
endif ()
if (PkgConfig_FOUND AND ffmpeg_FOUND)
    target_link_libraries(mock_webcam PRIVATE PkgConfig::ffmpeg)
    target_compile_definitions(mock_webcam PRIVATE USBIPDCPP_USE_FFMPEG)
endif ()

# mock_webcam 装配层单元测试（实现随示例走：构建 tests 且 GTest 可用时添加，
# 同 mock_audio；只测装配布局，依赖的 SineWaveSource/ColorBarSource 无第三方依赖，
# 不要求 FFmpeg/miniaudio）
if (USBIPDCPP_BUILD_TESTS)
    find_package(GTest CONFIG QUIET)
    if (GTest_FOUND)
        include(GoogleTest)
        enable_testing()
        add_executable(test_mock_webcam test_webcam.cpp)
        target_link_libraries(test_mock_webcam PRIVATE usbipdcpp_virtual_device GTest::gtest GTest::gtest_main)
        target_include_directories(test_mock_webcam PRIVATE .)
        # PRE_TEST：测试发现移到构建期（同 tests/CMakeLists.txt 的做法，避免并行构建竞态）
        gtest_discover_tests(test_mock_webcam DISCOVERY_MODE PRE_TEST)
    endif ()
endif ()
