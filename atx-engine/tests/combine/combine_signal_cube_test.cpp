#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/combine/signal_cube.hpp"
#include "atx/engine/combine/signal_store.hpp"

namespace atx_test_combine_signal_cube {
namespace fs = std::filesystem;
using namespace atx::engine::combine;
using atx::f64;
using atx::usize;
constexpr usize kT = 24, kK = 3, kN = 16;
constexpr f64 kNan = std::numeric_limits<f64>::quiet_NaN();

class CombineSignalCube : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> serial{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("atx_signal_cube_" + std::to_string(tick) + "_" + std::to_string(serial++));
        ASSERT_TRUE(fs::create_directory(root));
    }
    void TearDown() override { std::error_code ec; fs::remove_all(root, ec); }
    SignalCubeConfig config(CubeStatPrecision precision = CubeStatPrecision::ExactF64V1) const {
        SignalCubeConfig c;
        c.dates = kT; c.alphas = kK; c.instruments = kN; c.chunk_dates = 5;
        c.normalization = CubeNormalize::AlreadyNormalizedV1;
        c.stat_precision = precision; c.horizons = {1, 2, 3, 4}; c.maturity_end = kT - 2;
        c.source_sha256 = std::string(64, '1');
        c.transform_identity = "fixture-signal-store-zscore-clip-v2";
        c.return_identity = "fixture-close-delay1-return-v1";
        c.membership_identity = "all-names-v1";
        for (usize a = 0; a < kK; ++a) c.alpha_identities.push_back("alpha-" + std::to_string(a));
        return c;
    }
    SignalStore store() const {
        auto s = SignalStore::create(kT, kN);
        EXPECT_TRUE(s);
        for (usize a = 0; a < kK; ++a) {
            std::vector<f64> values(kT * kN);
            for (usize t = 0; t < kT; ++t) for (usize i = 0; i < kN; ++i)
                values[t * kN + i] = std::sin(static_cast<f64>((a + 1) * (i + 2) + t) * 0.37);
            values[(a + 1) * kN + a] = kNan;
            EXPECT_TRUE(s->add_signal(values));
        }
        return std::move(*s);
    }
    std::array<std::vector<f64>, 4> labels() const {
        std::array<std::vector<f64>, 4> out;
        std::vector<f64> close(kT * kN);
        for (usize t = 0; t < kT; ++t) for (usize i = 0; i < kN; ++i)
            close[t * kN + i] = 50.0 * std::exp(0.001 * static_cast<f64>(t * i) + 0.01 * std::sin(static_cast<f64>(t + i)));
        for (usize h = 0; h < 4; ++h) {
            out[h].assign(kT * kN, kNan);
            for (usize t = 0; t + h + 2 < kT; ++t) for (usize i = 0; i < kN; ++i)
                out[h][t * kN + i] = close[(t + h + 2) * kN + i] / close[(t + 1) * kN + i] - 1.0;
        }
        out[0][2 * kN + 4] = kNan;
        return out;
    }
    static auto spans(const std::array<std::vector<f64>, 4>& values) {
        std::array<std::span<const f64>, 4> out{};
        for (usize h = 0; h < 4; ++h) out[h] = values[h];
        return out;
    }
    static void same(f64 a, f64 b) {
        if (std::isnan(a)) EXPECT_TRUE(std::isnan(b));
        else EXPECT_EQ(std::bit_cast<atx::u64>(a), std::bit_cast<atx::u64>(b));
    }
    fs::path root;
};

TEST_F(CombineSignalCube, ExactStatisticsMatchLegacyBeforeQuantization) {
    auto source = store(); const auto returns = labels(); const auto cfg = config();
    auto hash = write_signal_cube(source, root / "cube", cfg, spans(returns));
    ASSERT_TRUE(hash) << hash.error().message();
    auto cube = SignalCube::open(root / "cube", *hash);
    ASSERT_TRUE(cube) << cube.error().message();
    EXPECT_EQ(cube->manifest_sha256(), *hash);
    EXPECT_EQ(cube->config().alpha_identities, cfg.alpha_identities);
    std::vector<f64> decoded(kN);
    for (usize t = 0; t < kT; ++t) for (usize a = 0; a < kK; ++a) {
        ASSERT_TRUE(cube->read_signal_row(t, a, decoded));
        const auto row = source.signal_row(a, t);
        for (usize i = 0; i < kN; ++i) {
            if (!std::isfinite(row[i])) EXPECT_TRUE(std::isnan(decoded[i]));
            else EXPECT_LE(std::abs(decoded[i] - row[i]), cfg.quantization_step * 0.5 + 1e-14);
        }
        auto stats = cube->read_stats(t, a);
        ASSERT_TRUE(stats);
        for (const auto v : stats->neutral) EXPECT_TRUE(std::isnan(v));
        if (t == 0) EXPECT_TRUE(std::isnan(stats->lag_one));
        else same(cross_section_corr(row, source.signal_row(a, t-1)), stats->lag_one);
    }
    for (usize h = 0; h < 4; ++h) {
        ASSERT_TRUE(source.set_forward_returns(returns[h]));
        const auto expected = ic_matrix(source, {0, kT});
        auto actual = cube->read_stat_matrix(0, kT, CubeStat::PearsonIc, h);
        ASSERT_TRUE(actual);
        for (usize t = 0; t < kT; ++t) for (usize a = 0; a < kK; ++a) {
            const auto value = (*actual)[t * kK + a];
            if (t + 1 + cfg.horizons[h] >= cfg.maturity_end) EXPECT_TRUE(std::isnan(value));
            else same(expected[t * kK + a], value);
        }
    }
    EXPECT_FALSE(cube->read_signal_row(kT, 0, decoded));
    EXPECT_FALSE(cube->read_stat_matrix(1, kT + 1, CubeStat::PearsonIc, 0));
    EXPECT_FALSE(SignalCube::open(root / "cube", std::string(64, 'f')));
}

TEST_F(CombineSignalCube, Float32StatisticsAreExplicitAndRoundedOnce) {
    auto source = store(); const auto returns = labels();
    ASSERT_TRUE(write_signal_cube(source, root / "exact", config(), spans(returns)));
    ASSERT_TRUE(write_signal_cube(source, root / "float", config(CubeStatPrecision::Float32V1), spans(returns)));
    auto exact = SignalCube::open(root / "exact"), reduced = SignalCube::open(root / "float");
    ASSERT_TRUE(exact); ASSERT_TRUE(reduced);
    EXPECT_EQ(reduced->config().stat_precision, CubeStatPrecision::Float32V1);
    auto a = exact->read_stat_matrix(0, kT, CubeStat::PearsonIc);
    auto b = reduced->read_stat_matrix(0, kT, CubeStat::PearsonIc);
    ASSERT_TRUE(a); ASSERT_TRUE(b);
    for (usize i = 0; i < a->size(); ++i) same(static_cast<f64>(static_cast<float>((*a)[i])), (*b)[i]);
    EXPECT_EQ(fs::file_size(root / "exact" / "stats-0.bin"), 2U * fs::file_size(root / "float" / "stats-0.bin"));
}

TEST_F(CombineSignalCube, IncompleteWritesNeverPublishAndExistingOutputsAreProtected) {
    auto cfg = config(); cfg.dates = 2; cfg.alphas = 1; cfg.alpha_identities.resize(1); cfg.maturity_end = 2;
    auto writer = SignalCubeWriter::create(root / "partial", cfg);
    ASSERT_TRUE(writer);
    const std::vector<f64> row(kN, 0.5);
    EXPECT_FALSE(writer->append_row(1, 0, row));
    ASSERT_TRUE(writer->append_row(0, 0, row));
    EXPECT_FALSE(writer->finish());
    EXPECT_FALSE(fs::exists(root / "partial" / "manifest.json"));
    EXPECT_FALSE(SignalCube::open(root / "partial"));
    EXPECT_FALSE(SignalCubeWriter::create(root / "partial", cfg));
    ASSERT_TRUE(writer->append_row(1, 0, row));
    ASSERT_TRUE(writer->finish());
    EXPECT_FALSE(writer->finish());
    EXPECT_TRUE(SignalCube::open(root / "partial"));
}

TEST_F(CombineSignalCube, RejectsTamperingTruncationAndChunkPathSubstitution) {
    auto source = store(); const auto returns = labels();
    for (const auto* name : {"tamper", "truncate", "path"})
        ASSERT_TRUE(write_signal_cube(source, root / name, config(), spans(returns)));
    {
        std::fstream file(root / "tamper" / "signals-0.bin", std::ios::binary | std::ios::in | std::ios::out);
        char changed = 0; file.read(&changed, 1); changed ^= '\x01';
        file.seekp(0); file.write(&changed, 1);
    }
    EXPECT_FALSE(SignalCube::open(root / "tamper"));
    const auto file = root / "truncate" / "stats-0.bin";
    fs::resize_file(file, fs::file_size(file) - 1);
    EXPECT_FALSE(SignalCube::open(root / "truncate"));
    {
        const auto path = root / "path" / "manifest.json";
        nlohmann::json j; { std::ifstream f(path); f >> j; }
        j["chunks"][0]["signals"] = "../tamper/signals-0.bin";
        std::ofstream f(path); f << j.dump();
    }
    EXPECT_FALSE(SignalCube::open(root / "path"));
}

TEST_F(CombineSignalCube, MembershipMaturityAndUnavailableValuesAreNotZeroFilled) {
    auto cfg = config(); cfg.dates = 8; cfg.alphas = 1; cfg.alpha_identities.resize(1);
    cfg.maturity_end = 5; cfg.return_treatment = CubeReturnTreatment::RawV1;
    auto writer = SignalCubeWriter::create(root / "masked", cfg);
    ASSERT_TRUE(writer);
    std::vector<f64> signal(kN), label(kN);
    std::vector<atx::u8> member(kN, 1); member.back() = 0;
    for (usize i = 0; i < kN; ++i) signal[i] = label[i] = static_cast<f64>(i / 2) * 0.1; // tied ranks
    signal.back() = 1e100; label.back() = -1e100; // masked before normalization/statistics
    std::array<std::span<const f64>, 4> labels{label, {}, {}, {}};
    for (usize t = 0; t < cfg.dates; ++t) ASSERT_TRUE(writer->append_row(t, 0, signal, labels, member));
    ASSERT_TRUE(writer->finish());
    auto cube = SignalCube::open(root / "masked"); ASSERT_TRUE(cube);
    auto first = cube->read_stats(0, 0); ASSERT_TRUE(first);
    EXPECT_DOUBLE_EQ(first->pearson[0], 1.0); EXPECT_DOUBLE_EQ(first->rank[0], 1.0);
    EXPECT_DOUBLE_EQ(first->coverage, 1.0);
    EXPECT_TRUE(std::isnan(first->pearson[1])); EXPECT_TRUE(std::isnan(first->lag_one));
    auto mature = cube->read_stats(3, 0); ASSERT_TRUE(mature);
    EXPECT_TRUE(std::isnan(mature->pearson[0])); // endpoint3+delay1+h1 == exclusive5
    std::vector<f64> decoded(kN); ASSERT_TRUE(cube->read_signal_row(0, 0, decoded));
    EXPECT_TRUE(std::isnan(decoded.back()));
}

TEST_F(CombineSignalCube, GeometryBudgetsAndQuantizerCannotSilentlyOverflow) {
    auto cfg = config();
    auto sizing = preflight_signal_cube(cfg); ASSERT_TRUE(sizing);
    EXPECT_EQ(sizing->signal_bytes, kT * kK * kN * 2U);
    EXPECT_EQ(sizing->stat_bytes, kT * kK * 14U * 8U);
    cfg.max_working_bytes = 1; EXPECT_FALSE(preflight_signal_cube(cfg));
    cfg = config(); cfg.dates = std::numeric_limits<usize>::max(); EXPECT_FALSE(preflight_signal_cube(cfg));
    cfg = config(); cfg.horizons[1] = cfg.horizons[0]; EXPECT_FALSE(preflight_signal_cube(cfg));
    cfg = config(); cfg.alpha_identities[1] = cfg.alpha_identities[0]; EXPECT_FALSE(preflight_signal_cube(cfg));
    cfg = config(); cfg.quantization_step = 0.1; EXPECT_FALSE(preflight_signal_cube(cfg));
    cfg = config();
    auto writer = SignalCubeWriter::create(root / "overflow", cfg); ASSERT_TRUE(writer);
    const std::vector<f64> unrepresentable(kN, 100.0);
    EXPECT_FALSE(writer->append_row(0, 0, unrepresentable));
    EXPECT_FALSE(fs::exists(root / "overflow" / "manifest.json"));
}

} // namespace atx_test_combine_signal_cube
