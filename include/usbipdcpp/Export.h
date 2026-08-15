#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
    // 如果是编译的静态库
    #ifdef USBIPDCPP_STATIC
        #define USBIPDCPP_API
    // 没定义则是编译的动态库
    // 根据是否定义了 USBIPDCPP_EXPORTS 判断是在编译库还是在使用库
    #else
        #ifdef USBIPDCPP_EXPORTS
            #define USBIPDCPP_API __declspec(dllexport)
        #else
            #define USBIPDCPP_API __declspec(dllimport)
        #endif
    #endif
#else
    #define USBIPDCPP_API __attribute__((visibility("default")))
#endif
