find_package(PkgConfig QUIET)

if (PkgConfig_FOUND)
    pkg_check_modules(ffmpeg IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
endif ()

if (PkgConfig_FOUND AND ffmpeg_FOUND)
    file(GLOB_RECURSE src *.cpp *.c)
    add_executable(mock_uvc_ffmpeg ${src})
    target_link_libraries(mock_uvc_ffmpeg PRIVATE usbipdcpp_virtual_device cxxopts::cxxopts PkgConfig::ffmpeg)
    target_include_directories(mock_uvc_ffmpeg PRIVATE .)
else ()
    message(WARNING "FFmpeg or PkgConfig not found, skipping mock_uvc_ffmpeg")
endif ()
