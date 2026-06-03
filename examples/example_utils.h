#pragma once

#include <cstdint>
#include <cxxopts.hpp>
#include <iostream>
#include <string>

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
