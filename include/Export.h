#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef USBIPDCPP_EXPORTS
        #define USBIPDCPP_API __declspec(dllexport)
    #else
        #define USBIPDCPP_API
    #endif
#else
    #define USBIPDCPP_API __attribute__((visibility("default")))
#endif
