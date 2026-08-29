// WavFileSink 单元测试：WAV 头写入与回填、格式协商、reset 重建文件

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "usbipdcpp/virtual_device/audio_sinks/WavFileSink.h"

using namespace usbipdcpp;

namespace {

/// 临时目录内的独立测试文件（进程隔离，测试结束删除）
std::filesystem::path make_test_path(const char *name) {
    // file_clock 的 rep 在部分 libc++（termux/mac）下对 to_string 重载集产生歧义，
    // 显式转 long long 再格式化
    auto stamp = static_cast<long long>(std::filesystem::file_time_type::clock::now().time_since_epoch().count());
    auto path = std::filesystem::temp_directory_path() /
                (std::string(name) + "_" + std::to_string(stamp) + ".wav");
    std::filesystem::remove(path);
    return path;
}

std::vector<std::uint8_t> read_all(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::uint32_t read_u32le(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint16_t read_u16le(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

} // namespace

TEST(WavFileSink, WriteThenFinalizeProducesValidWav) {
    auto path = make_test_path("wav_write");
    {
        WavFileSink sink(path);
        ASSERT_TRUE(sink.set_format(1, 16, 48000));

        // 写入 1ms 数据（48000Hz 单声道 16 位 = 96 字节）
        std::vector<std::uint8_t> pcm(96, 0xAA);
        sink.write_pcm(pcm.data(), pcm.size());
        sink.finalize();
    }
    auto bytes = read_all(path);
    std::filesystem::remove(path);

    // 44 字节头 + 96 字节数据
    ASSERT_EQ(bytes.size(), 44u + 96u);
    // RIFF 头
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.data()), 4), "RIFF");
    EXPECT_EQ(read_u32le(bytes, 4), 36u + 96u); // chunk size = 36 + data
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.data() + 8), 4), "WAVE");
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.data() + 12), 4), "fmt ");
    EXPECT_EQ(read_u32le(bytes, 16), 16u); // fmt chunk size
    EXPECT_EQ(read_u16le(bytes, 20), 1u); // audio format: PCM
    EXPECT_EQ(read_u16le(bytes, 22), 1u); // channels
    EXPECT_EQ(read_u32le(bytes, 24), 48000u); // sample rate
    EXPECT_EQ(read_u32le(bytes, 28), 48000u * 2); // byte rate
    EXPECT_EQ(read_u16le(bytes, 32), 2u); // block align
    EXPECT_EQ(read_u16le(bytes, 34), 16u); // bits per sample
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.data() + 36), 4), "data");
    EXPECT_EQ(read_u32le(bytes, 40), 96u); // data size
    // 数据内容
    for (std::size_t i = 44; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], 0xAA);
    }
}

TEST(WavFileSink, ResetClosesAndReopens) {
    auto path = make_test_path("wav_reset");
    {
        WavFileSink sink(path);
        ASSERT_TRUE(sink.set_format(1, 16, 48000));
        std::vector<std::uint8_t> pcm(48, 0x01);
        sink.write_pcm(pcm.data(), pcm.size());
        sink.reset(); // 第一段流结束：回填头并关闭

        // 第二段流：重新打开文件（覆盖），数据按新内容写入
        std::vector<std::uint8_t> pcm2(96, 0x02);
        sink.write_pcm(pcm2.data(), pcm2.size());
        sink.finalize();
    }
    auto bytes = read_all(path);
    std::filesystem::remove(path);

    // 最终文件只含第二段数据（覆盖写入）
    ASSERT_EQ(bytes.size(), 44u + 96u);
    EXPECT_EQ(read_u32le(bytes, 40), 96u);
    for (std::size_t i = 44; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], 0x02);
    }
}

TEST(WavFileSink, SetFormatRejectsUnsupported) {
    auto path = make_test_path("wav_format");
    WavFileSink sink(path);

    EXPECT_FALSE(sink.set_format(1, 8, 48000)); // 只支持 16 位
    EXPECT_FALSE(sink.set_format(3, 16, 48000)); // 只支持 1/2 声道
    EXPECT_TRUE(sink.set_format(2, 16, 44100));
    EXPECT_EQ(sink.current_format().channels, 2);
    EXPECT_EQ(sink.current_format().sample_rate, 44100u);
    std::filesystem::remove(path);
}

TEST(WavFileSink, DestructorFinalizes) {
    auto path = make_test_path("wav_dtor");
    {
        WavFileSink sink(path);
        ASSERT_TRUE(sink.set_format(1, 16, 8000));
        std::vector<std::uint8_t> pcm(16, 0x00);
        sink.write_pcm(pcm.data(), pcm.size());
        // 不显式 finalize，析构自动回填
    }
    auto bytes = read_all(path);
    std::filesystem::remove(path);

    ASSERT_EQ(bytes.size(), 44u + 16u);
    EXPECT_EQ(read_u32le(bytes, 4), 36u + 16u);
    EXPECT_EQ(read_u32le(bytes, 40), 16u);
}
