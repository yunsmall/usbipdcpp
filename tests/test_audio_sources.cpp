// 音频源测试：FourierSource 谐波叠加、频率分量、相位、采样率切换
//（AudioFileSource 已随实现搬入 examples/mock_audio，其测试在 examples/mock_audio/test_audio_file_source.cpp）

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

#include "usbipdcpp/virtual_device/audio_sources/FourierSource.h"
#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"

using namespace usbipdcpp;

// ==================== FourierSource ====================

namespace {

/// Goertzel 算法检测单频分量的功率（相对值，用于存在性判断）
double goertzel_power(const std::int16_t *samples, std::size_t n, double freq, std::uint32_t rate) {
    double w = 2.0 * std::numbers::pi * freq / rate;
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

TEST(FourierSource, NormalizeOverLimitByDefault) {
    // 两谐波叠加峰值 > 1.0（sin θ + sin 2θ 峰值约 1.76）：默认 normalize 模式
    // 整体除以峰值——不削波（无连续满幅平顶），峰值被拉到满幅
    FourierSource src({{440, 1.0}, {880, 1.0}}, {8000}, 1);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);
    auto n = chunk.size / 2;

    int max_abs = 0, flat_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        auto v = std::abs(p[i]);
        max_abs = std::max(max_abs, v);
        if (v == 32767) ++flat_count;
    }
    EXPECT_GT(max_abs, 32760); // 峰值被拉到满幅（浮点舍入 ±7 LSB）
    EXPECT_LT(flat_count, 3); // 无削波平顶（削波时一个周期内有多个连续满幅采样）
}

TEST(FourierSource, ClampModeClips) {
    // 削波模式（normalize=false）：超限采样被削成 ±32767，出现大量连续满幅平顶
    FourierSource src({{440, 1.0}, {880, 1.0}}, {8000}, 1, false);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);
    auto n = chunk.size / 2;

    int max_abs = 0, flat_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        auto v = std::abs(p[i]);
        max_abs = std::max(max_abs, v);
        if (v == 32767) ++flat_count;
    }
    EXPECT_EQ(max_abs, 32767);
    EXPECT_GT(flat_count, 5); // 削波平顶显著多于归一化模式
}

TEST(FourierSource, NoScalingBelowLimit) {
    // 峰值不超满幅时不缩放：50% 幅度输出峰值约 16383（保持音量语义）
    FourierSource src({{440, 0.5}}, {8000}, 1);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);
    auto n = chunk.size / 2;

    int max_abs = 0;
    for (std::size_t i = 0; i < n; ++i)
        max_abs = std::max(max_abs, std::abs(p[i]));
    EXPECT_GT(max_abs, 16000);
    EXPECT_LT(max_abs, 16500);
}

TEST(FourierSource, NegativeAmplitudeInvertsPhase) {
    // 负幅度 = 反相：+A 与 -A 的输出互为相反数（±1 LSB）
    FourierSource pos({{440, 0.5}}, {8000}, 1);
    FourierSource neg({{440, -0.5}}, {8000}, 1);
    AudioChunk a, b;
    ASSERT_TRUE(pos.get_chunk(a));
    ASSERT_TRUE(neg.get_chunk(b));

    auto *pa = reinterpret_cast<const std::int16_t *>(a.data);
    auto *pb = reinterpret_cast<const std::int16_t *>(b.data);
    int diff_count = 0;
    for (std::size_t i = 0; i < a.size / 2; ++i) {
        if (std::abs(pa[i] + pb[i]) > 1)
            ++diff_count;
    }
    EXPECT_EQ(diff_count, 0);
}

TEST(FourierSource, SetNormalizeRegenerates) {
    // 创建后切换防溢出方案应立即重新生成：先削波（出现连续满幅平顶），
    // 切到归一化后平顶消失且峰值仍被拉到满幅
    FourierSource src({{440, 1.0}, {880, 1.0}}, {8000}, 1, false);
    AudioChunk chunk;
    ASSERT_TRUE(src.get_chunk(chunk));
    auto *p = reinterpret_cast<const std::int16_t *>(chunk.data);
    auto n = chunk.size / 2;

    int flat_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::abs(p[i]) == 32767) ++flat_count;
    }
    EXPECT_GT(flat_count, 5); // 削波模式有大量平顶

    src.set_normalize(true);
    ASSERT_TRUE(src.get_chunk(chunk)); // buffer 已重生成，重新取指针
    p = reinterpret_cast<const std::int16_t *>(chunk.data);
    int max_abs = 0;
    flat_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        auto v = std::abs(p[i]);
        max_abs = std::max(max_abs, v);
        if (v == 32767) ++flat_count;
    }
    EXPECT_GT(max_abs, 32760); // 峰值拉到满幅
    EXPECT_LT(flat_count, 3); // 平顶消失（不削波）
}
