// AudioFileSource 单元测试（实现随示例走，miniaudio 可用且构建 tests 时编译）：
// WAV 加载、采样率切换（miniaudio 重采样）、循环/非循环、软失败
// 由 examples/mock_audio/CMakeLists.txt 在 USBIPDCPP_BUILD_TESTS 时添加

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <fstream>
#include <string>
#include <vector>

#include "AudioFileSource.h"

using namespace usbipdcpp;

namespace {

/// 生成 16 位 PCM WAV 测试文件（440Hz 正弦），返回文件路径
std::string make_test_wav(const std::string &tag, std::uint32_t sample_rate, std::uint16_t channels, double seconds) {
    auto total_frames = static_cast<std::size_t>(sample_rate * seconds);
    std::vector<std::int16_t> samples(total_frames * channels);
    for (std::size_t i = 0; i < total_frames; ++i) {
        auto v = static_cast<std::int16_t>(16384.0 * std::sin(2.0 * std::numbers::pi * 440.0 * i / sample_rate));
        for (std::uint16_t ch = 0; ch < channels; ++ch) {
            samples[i * channels + ch] = v;
        }
    }

    auto path = (std::filesystem::temp_directory_path() / ("usbipdcpp_test_" + tag + ".wav")).string();

    std::ofstream f(path, std::ios::binary);
    auto put_u16 = [&f](std::uint16_t v) {
        f.put(static_cast<char>(v & 0xFF));
        f.put(static_cast<char>((v >> 8) & 0xFF));
    };
    auto put_u32 = [&f](std::uint32_t v) {
        f.put(static_cast<char>(v & 0xFF));
        f.put(static_cast<char>((v >> 8) & 0xFF));
        f.put(static_cast<char>((v >> 16) & 0xFF));
        f.put(static_cast<char>((v >> 24) & 0xFF));
    };

    auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
    f.write("RIFF", 4);
    put_u32(36 + data_bytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put_u32(16); // fmt chunk 大小
    put_u16(1); // PCM
    put_u16(channels);
    put_u32(sample_rate);
    put_u32(sample_rate * channels * 2); // byte rate
    put_u16(channels * 2); // block align
    put_u16(16); // bits per sample
    f.write("data", 4);
    put_u32(data_bytes);
    f.write(reinterpret_cast<const char *>(samples.data()), static_cast<std::streamsize>(data_bytes));
    f.close();
    return path;
}

} // namespace

TEST(AudioFileSource, LoadWavBasic) {
    auto path = make_test_wav("basic", 8000, 1, 1.0);
    {
        AudioFileSource src(path);

        auto fmt = src.current_format();
        EXPECT_EQ(fmt.sample_rate, 8000);
        EXPECT_EQ(fmt.channels, 1);
        EXPECT_EQ(fmt.bits_per_sample, 16);

        // 每块 1ms：8 帧 × 2 字节 = 16 字节，且非全零
        AudioChunk chunk;
        ASSERT_TRUE(src.get_chunk(chunk));
        EXPECT_EQ(chunk.size, 16);
        bool has_nonzero = false;
        for (std::size_t i = 0; i < chunk.size; ++i) {
            if (chunk.data[i] != 0)
                has_nonzero = true;
        }
        EXPECT_TRUE(has_nonzero);
    } // src 析构释放文件句柄
    std::filesystem::remove(path);
}

TEST(AudioFileSource, ResampleToDifferentRate) {
    auto path = make_test_wav("resample", 8000, 1, 1.0);
    {
        AudioFileSource src(path, std::vector<std::uint32_t>{8000, 16000});

        ASSERT_TRUE(src.set_format(1, 16, 16000));
        EXPECT_EQ(src.current_format().sample_rate, 16000);

        AudioChunk chunk;
        ASSERT_TRUE(src.get_chunk(chunk));
        EXPECT_EQ(chunk.size, 32); // 16 帧 × 2 字节
    }
    std::filesystem::remove(path);
}

TEST(AudioFileSource, StereoWav) {
    auto path = make_test_wav("stereo", 16000, 2, 1.0);
    {
        AudioFileSource src(path);

        EXPECT_EQ(src.current_format().channels, 2);
        EXPECT_EQ(src.current_format().sample_rate, 16000);

        // 声道数与文件不一致时拒绝切换
        EXPECT_FALSE(src.set_format(1, 16, 16000));
        EXPECT_FALSE(src.set_format(2, 8, 16000));
        // 不在列表中的采样率拒绝切换
        EXPECT_FALSE(src.set_format(2, 16, 32000));

        AudioChunk chunk;
        ASSERT_TRUE(src.get_chunk(chunk));
        EXPECT_EQ(chunk.size, 64); // 16 帧 × 2 声道 × 2 字节
    }
    std::filesystem::remove(path);
}

TEST(AudioFileSource, NonLoopStopsAtEnd) {
    // 10ms 的文件，非循环：10 次 get_chunk 后应返回 false
    auto path = make_test_wav("nonloop", 8000, 1, 0.01);
    {
        AudioFileSource src(path, std::vector<std::uint32_t>{}, false);

        int ok = 0;
        AudioChunk chunk;
        while (ok < 100 && src.get_chunk(chunk)) {
            ++ok;
        }
        EXPECT_EQ(ok, 10);
        EXPECT_FALSE(src.get_chunk(chunk));
    }
    std::filesystem::remove(path);
}

TEST(AudioFileSource, LoopWrapsAround) {
    auto path = make_test_wav("loop", 8000, 1, 0.01);
    {
        AudioFileSource src(path, std::vector<std::uint32_t>{}, true);

        // 循环模式：读取远超文件长度（1 秒 = 1000 块）仍持续有数据
        AudioChunk chunk;
        for (int i = 0; i < 1000; ++i) {
            ASSERT_TRUE(src.get_chunk(chunk));
            EXPECT_EQ(chunk.size, 16);
        }
    }
    std::filesystem::remove(path);
}

TEST(AudioFileSource, InvalidFileSoftFail) {
    auto path = (std::filesystem::temp_directory_path() / "usbipdcpp_test_invalid.wav").string();
    {
        std::ofstream f(path, std::ios::binary);
        f << "not a wav file";
    }
    {
        // 软失败（同 FfmpegSource）：不抛异常，返回合法降级格式，无数据输出；
        // 与降级格式一致的切换请求按 no-op 接受，保证驱动能开流
        AudioFileSource src(path);

        auto fmts = src.supported_formats();
        ASSERT_FALSE(fmts.empty());
        EXPECT_EQ(fmts.front().bits_per_sample, 16);
        EXPECT_EQ(src.current_format().sample_rate, 48000);

        EXPECT_TRUE(src.set_format(1, 16, 48000));
        EXPECT_FALSE(src.set_format(1, 16, 16000));
        AudioChunk chunk;
        EXPECT_FALSE(src.get_chunk(chunk));
    }
    std::filesystem::remove(path);
}

TEST(AudioFileSource, InvalidSampleRateListSoftFail) {
    auto path = make_test_wav("badrate", 8000, 1, 1.0);
    {
        // 采样率列表含非法值（0）：校验失败软失败
        AudioFileSource src(path, std::vector<std::uint32_t>{0});

        AudioChunk chunk;
        EXPECT_FALSE(src.get_chunk(chunk));
        EXPECT_TRUE(src.set_format(1, 16, 48000));
        EXPECT_FALSE(src.set_format(1, 16, 44100));
    }
    std::filesystem::remove(path);
}
