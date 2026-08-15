// 音频源测试：
// - AudioFileSource：WAV 加载、采样率切换（miniaudio 重采样）、循环/非循环、软失败（仅 miniaudio 构建）
// - FourierSource：谐波叠加、频率分量、相位、采样率切换（无条件）

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "usbipdcpp/virtual_device/audio_sources/FourierSource.h"
#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"
#ifdef USBIPDCPP_USE_MINIAUDIO
#include "usbipdcpp/virtual_device/audio_sources/AudioFileSource.h"
#endif

using namespace usbipdcpp;

#ifdef USBIPDCPP_USE_MINIAUDIO

namespace {

/// 生成 16 位 PCM WAV 测试文件（440Hz 正弦），返回文件路径
std::string make_test_wav(const std::string &tag, std::uint32_t sample_rate, std::uint16_t channels, double seconds) {
    auto total_frames = static_cast<std::size_t>(sample_rate * seconds);
    std::vector<std::int16_t> samples(total_frames * channels);
    for (std::size_t i = 0; i < total_frames; ++i) {
        auto v = static_cast<std::int16_t>(16384.0 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / sample_rate));
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
        // 采样率必须为 8kHz 整数倍：校验失败软失败
        AudioFileSource src(path, std::vector<std::uint32_t>{44100});

        AudioChunk chunk;
        EXPECT_FALSE(src.get_chunk(chunk));
        EXPECT_TRUE(src.set_format(1, 16, 48000));
        EXPECT_FALSE(src.set_format(1, 16, 44100));
    }
    std::filesystem::remove(path);
}

#endif // USBIPDCPP_USE_MINIAUDIO

// ==================== FourierSource（不依赖 miniaudio） ====================

namespace {

/// Goertzel 算法检测单频分量的功率（相对值，用于存在性判断）
double goertzel_power(const std::int16_t *samples, std::size_t n, double freq, std::uint32_t rate) {
    double w = 2.0 * 3.14159265358979323846 * freq / rate;
    double coeff = 2.0 * std::cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        s0 = samples[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

} // namespace

TEST(FourierSource, SingleHarmonicMatchesSine) {
    // 单谐波 440Hz 满幅（相位 0）与 SineWaveSource 输出近似一致（±1 LSB）
    FourierSource fourier({{440, 1.0}}, {8000}, 1);
    SineWaveSource sine(440, {8000}, 1, 1.0);

    AudioChunk a, b;
    ASSERT_TRUE(fourier.get_chunk(a));
    ASSERT_TRUE(sine.get_chunk(b));
    ASSERT_EQ(a.size, b.size);

    auto *pa = reinterpret_cast<const std::int16_t *>(a.data);
    auto *pb = reinterpret_cast<const std::int16_t *>(b.data);
    int diff_count = 0;
    for (std::size_t i = 0; i < a.size / 2; ++i) {
        if (std::abs(pa[i] - pb[i]) > 1)
            ++diff_count;
    }
    EXPECT_EQ(diff_count, 0);
}

TEST(FourierSource, HarmonicsDetected) {
    // 440:50 + 880:25 → 两个频率分量功率应显著大于附近不存在频率
    FourierSource src({{440, 0.5}, {880, 0.25}}, {8000}, 1);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);
    auto n = chunk.size / 2;

    auto p440 = goertzel_power(p, n, 440, 8000);
    auto p880 = goertzel_power(p, n, 880, 8000);
    auto p500 = goertzel_power(p, n, 500, 8000); // 不存在的分量作参照
    EXPECT_GT(p440, p500 * 100);
    EXPECT_GT(p880, p500 * 100);
}

TEST(FourierSource, PhaseShift) {
    // 440Hz 相位 π/2：y(t) = sin(2πft + π/2) = cos(2πft)
    // 样本 0 处 cos(0)=1 → 满幅正峰值；样本 8 处 cos(2π·0.44)≈-0.93 → 接近负满幅
    FourierSource src({{440, 1.0, 1.5707963267948966}}, {8000}, 1);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);

    EXPECT_GT(p[0], 32000);
    EXPECT_LT(p[8], -30000);
    // 对照：相位 0 时样本 0 处 sin(0)=0
    FourierSource no_phase({{440, 1.0, 0.0}}, {8000}, 1);
    AudioChunk chunk2;
    ASSERT_TRUE(no_phase.get_chunk(chunk2));
    auto *p2 = reinterpret_cast<const std::int16_t *>(chunk2.data);
    EXPECT_NEAR(p2[0], 0, 400);
}

TEST(FourierSource, ResampleToDifferentRate) {
    FourierSource src({{440, 0.5}}, {8000, 16000}, 1);
    ASSERT_TRUE(src.set_format(1, 16, 16000));
    EXPECT_EQ(src.current_format().sample_rate, 16000);

    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    EXPECT_EQ(chunk.size, 16000 * 2); // 1 秒 16kHz 单声道 16 位
}

TEST(FourierSource, InvalidHarmonicsFiltered) {
    // NaN/Inf 幅度与相位会被剔除（否则 sin(NaN) 转整数是未定义行为），剩余合法项正常发声
    double nan = std::numeric_limits<double>::quiet_NaN();
    double inf = std::numeric_limits<double>::infinity();
    FourierSource src({{440, 0.5}, {0, 1.0}, {880, nan}, {1000, 0.5, inf}, {1100, -inf}}, {8000}, 1);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);

    // 全部样本必须是有限整数（非法项未污染输出）
    for (std::size_t i = 0; i < chunk.size / 2; ++i) {
        EXPECT_GE(p[i], -32767);
        EXPECT_LE(p[i], 32767);
    }
    // 440Hz 分量仍在（幅度 50%）
    auto p440 = goertzel_power(p, chunk.size / 2, 440, 8000);
    EXPECT_GT(p440, 1.0);
}
