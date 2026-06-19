#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "book_shape.hpp"
#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;

// Build a small research panel (M instruments, D dates, field "close").
// Prices: 100 * exp(small per-instrument drift * t), ensuring non-NaN,
// non-zero, finite variance.
static atx::core::Result<std::string>
make_research_panel(const fs::path& out, atx::usize M, atx::usize D)
{
    std::vector<atx::f64> close_data;
    close_data.reserve(D * M);
    for (atx::usize t = 0; t < D; ++t) {
        for (atx::usize i = 0; i < M; ++i) {
            // Each instrument has a slightly different drift so variances differ.
            const atx::f64 drift = 0.0002 * (1.0 + static_cast<atx::f64>(i) * 0.1);
            close_data.push_back(100.0 * std::exp(drift * static_cast<atx::f64>(t)));
        }
    }
    std::vector<std::uint8_t> uni(D * M, 1u);
    ATX_TRY(auto panel,
            alpha::Panel::create(D, M, {"close"}, {close_data}, uni));
    ATX_TRY(auto digest,
            atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

// Build a combo panel (M instruments, D dates, field "alpha").
// alpha[d, i] = (i - M/2.0) + 0.01*(d % 5) so the optimizer has a real
// long/short opinion each period.
static atx::core::Result<std::string>
make_combo_panel(const fs::path& out, atx::usize M, atx::usize D)
{
    std::vector<atx::f64> alpha_data;
    alpha_data.reserve(D * M);
    for (atx::usize t = 0; t < D; ++t) {
        const atx::f64 wobble = 0.01 * static_cast<atx::f64>(t % 5);
        for (atx::usize i = 0; i < M; ++i) {
            alpha_data.push_back(
                (static_cast<atx::f64>(i) - static_cast<atx::f64>(M) / 2.0) + wobble);
        }
    }
    std::vector<std::uint8_t> uni(D * M, 1u);
    ATX_TRY(auto panel,
            alpha::Panel::create(D, M, {"alpha"}, {alpha_data}, uni));
    ATX_TRY(auto digest,
            atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

// Parse turnover values from the sidecar .meta.txt file.
// Each schedule line is: "s=<s> period=<p> turnover=<tv> cost_bps=<c>"
static std::vector<double> parse_sidecar_turnover(const std::string& meta_path) {
    std::ifstream f(meta_path);
    if (!f.is_open()) return {};
    std::vector<double> tvs;
    std::string line;
    while (std::getline(f, line)) {
        // Look for lines starting with "s="
        if (line.rfind("s=", 0) != 0) continue;
        // Find "turnover=" token
        const auto pos = line.find("turnover=");
        if (pos == std::string::npos) continue;
        const auto end = line.find(' ', pos);
        const std::string val = (end == std::string::npos)
            ? line.substr(pos + 9)
            : line.substr(pos + 9, end - pos - 9);
        tvs.push_back(std::stod(val));
    }
    return tvs;
}

// ---------------------------------------------------------------------------
// Fixture: synthesize panels once, reuse across tests.
// ---------------------------------------------------------------------------

class AtxImplOptimize : public ::testing::Test {
protected:
    static constexpr atx::usize kM = 20;
    static constexpr atx::usize kD = 80;

    fs::path tmp_dir_;
    std::string research_path_;
    std::string combo_path_;

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / "atx_impl_optimize_test";
        fs::create_directories(tmp_dir_);

        auto rr = make_research_panel(tmp_dir_ / "research.bin", kM, kD);
        ASSERT_TRUE(rr.has_value()) << rr.error().message();
        research_path_ = *rr;

        auto cr = make_combo_panel(tmp_dir_ / "combo.bin", kM, kD);
        ASSERT_TRUE(cr.has_value()) << cr.error().message();
        combo_path_ = *cr;
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }
};

// ---------------------------------------------------------------------------
// Test 1: ProducesValidBooks — pins the constraint contract.
// ---------------------------------------------------------------------------
TEST_F(AtxImplOptimize, ProducesValidBooks) {
    const fs::path books_path = tmp_dir_ / "books1.bin";

    atx::impl::RunConfig cfg;
    cfg.panel     = research_path_;
    cfg.combo     = combo_path_;
    cfg.books_out = books_path.string();
    cfg.gross     = 1.0;
    cfg.name_cap  = 0.10;
    cfg.rebalance = "weekly";
    cfg.risk_aversion = 1.0;

    auto result = atx::impl::run_optimize(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    // Reload and validate shape.
    auto pr = atx::impl::read_panel(books_path.string());
    ASSERT_TRUE(pr.has_value()) << pr.error().message();
    const auto& books = *pr;
    EXPECT_EQ(books.instruments(), kM);
    // Weekly step=5 over D=80 dates: 0,5,10,...,75 => 16 periods.
    const atx::usize expected_S = (kD + 4) / 5;  // ceil(80/5)=16
    EXPECT_EQ(books.dates(), expected_S);

    // Verify constraint contract for each book row.
    ASSERT_TRUE(books.num_fields() >= 1);
    const auto weight_fid = books.field_id("weight");
    ASSERT_TRUE(weight_fid.has_value()) << weight_fid.error().message();

    const double gross = cfg.gross;
    const double name_cap = cfg.name_cap;

    for (atx::usize s = 0; s < books.dates(); ++s) {
        const auto ws = books.field_cross_section(*weight_fid, s);
        ASSERT_EQ(ws.size(), kM);

        double sum_w = 0.0;
        double gross_w = 0.0;
        double max_abs_w = 0.0;
        for (const double w : ws) {
            sum_w  += w;
            gross_w += std::fabs(w);
            if (std::fabs(w) > max_abs_w) max_abs_w = std::fabs(w);
        }
        EXPECT_LT(std::fabs(sum_w), 1e-7)
            << "book s=" << s << " not dollar-neutral: Σw=" << sum_w;
        EXPECT_LE(gross_w, gross + 1e-6)
            << "book s=" << s << " gross violated: " << gross_w;
        EXPECT_LE(max_abs_w, name_cap + 1e-6)
            << "book s=" << s << " name_cap violated: " << max_abs_w;
    }

    // Sidecar must exist.
    EXPECT_TRUE(fs::exists(books_path.string() + ".meta.txt"))
        << "missing sidecar .meta.txt";
}

// ---------------------------------------------------------------------------
// Test 2: DeterministicAcrossRuns — same digest, byte-identical books.bin.
// ---------------------------------------------------------------------------
TEST_F(AtxImplOptimize, DeterministicAcrossRuns) {
    const fs::path books_a = tmp_dir_ / "books_det_a.bin";
    const fs::path books_b = tmp_dir_ / "books_det_b.bin";

    atx::impl::RunConfig cfg;
    cfg.panel     = research_path_;
    cfg.combo     = combo_path_;
    cfg.gross     = 1.0;
    cfg.name_cap  = 0.10;
    cfg.rebalance = "weekly";
    cfg.risk_aversion = 1.0;

    cfg.books_out = books_a.string();
    auto r1 = atx::impl::run_optimize(cfg);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();

    cfg.books_out = books_b.string();
    auto r2 = atx::impl::run_optimize(cfg);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    // Digests must be equal and non-zero.
    EXPECT_NE(r1->digest, 0u) << "digest should be non-zero";
    EXPECT_EQ(r1->digest, r2->digest) << "digests differ across runs";

    // Byte-identical files.
    std::ifstream fa(books_a.string(), std::ios::binary);
    std::ifstream fb(books_b.string(), std::ios::binary);
    ASSERT_TRUE(fa.is_open());
    ASSERT_TRUE(fb.is_open());
    const std::vector<char> data_a((std::istreambuf_iterator<char>(fa)),
                                    std::istreambuf_iterator<char>());
    const std::vector<char> data_b((std::istreambuf_iterator<char>(fb)),
                                    std::istreambuf_iterator<char>());
    EXPECT_EQ(data_a, data_b) << "books.bin not byte-identical across runs";
}

// ---------------------------------------------------------------------------
// Test 3: TurnoverPenaltyReducesTurnover — κ=5 <= κ=0 (+ small tolerance).
// ---------------------------------------------------------------------------
TEST_F(AtxImplOptimize, TurnoverPenaltyReducesTurnover) {
    const fs::path books_k0 = tmp_dir_ / "books_k0.bin";
    const fs::path books_k5 = tmp_dir_ / "books_k5.bin";

    atx::impl::RunConfig base;
    base.panel     = research_path_;
    base.combo     = combo_path_;
    base.gross     = 1.0;
    base.name_cap  = 0.10;
    base.rebalance = "weekly";
    base.risk_aversion = 1.0;

    // Run with κ=0.
    base.books_out         = books_k0.string();
    base.turnover_penalty  = 0.0;
    auto r0 = atx::impl::run_optimize(base);
    ASSERT_TRUE(r0.has_value()) << r0.error().message();

    // Run with κ=5.
    base.books_out         = books_k5.string();
    base.turnover_penalty  = 5.0;
    auto r5 = atx::impl::run_optimize(base);
    ASSERT_TRUE(r5.has_value()) << r5.error().message();

    // Parse turnover from sidecars.
    const auto tv0 = parse_sidecar_turnover(books_k0.string() + ".meta.txt");
    const auto tv5 = parse_sidecar_turnover(books_k5.string() + ".meta.txt");
    ASSERT_FALSE(tv0.empty()) << "could not parse κ=0 sidecar turnover";
    ASSERT_FALSE(tv5.empty()) << "could not parse κ=5 sidecar turnover";

    const double sum0 = std::accumulate(tv0.begin(), tv0.end(), 0.0);
    const double sum5 = std::accumulate(tv5.begin(), tv5.end(), 0.0);

    EXPECT_LE(sum5, sum0 + 1e-9)
        << "expected κ=5 total turnover (" << sum5
        << ") <= κ=0 total turnover (" << sum0 << ")";
}

// ---------------------------------------------------------------------------
// Test 4: MissingArgsFails — Err(InvalidArgument) when inputs are empty.
// ---------------------------------------------------------------------------
TEST_F(AtxImplOptimize, MissingArgsFails) {
    {
        // All empty.
        atx::impl::RunConfig cfg;
        auto r = atx::impl::run_optimize(cfg);
        EXPECT_FALSE(r.has_value());
        if (!r.has_value()) {
            EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
        }
    }
    {
        // Missing combo.
        atx::impl::RunConfig cfg;
        cfg.panel     = research_path_;
        cfg.books_out = (tmp_dir_ / "x.bin").string();
        auto r = atx::impl::run_optimize(cfg);
        EXPECT_FALSE(r.has_value());
        if (!r.has_value()) {
            EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
        }
    }
    {
        // Missing books_out.
        atx::impl::RunConfig cfg;
        cfg.panel = research_path_;
        cfg.combo = combo_path_;
        auto r = atx::impl::run_optimize(cfg);
        EXPECT_FALSE(r.has_value());
        if (!r.has_value()) {
            EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
        }
    }
}

// ---------------------------------------------------------------------------
// Test 5: RejectsUnknownRebalance — invalid --rebalance returns InvalidArgument.
// ---------------------------------------------------------------------------
TEST_F(AtxImplOptimize, RejectsUnknownRebalance) {
    const fs::path books_path = tmp_dir_ / "books_bad_rebalance.bin";

    atx::impl::RunConfig cfg;
    cfg.panel        = research_path_;
    cfg.combo        = combo_path_;
    cfg.books_out    = books_path.string();
    cfg.gross        = 1.0;
    cfg.name_cap     = 0.10;
    cfg.risk_aversion = 1.0;
    cfg.rebalance    = "monthly";  // not a valid value

    auto r = atx::impl::run_optimize(cfg);
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
    }
}

// ---------------------------------------------------------------------------
// Test 6: PositionModeBookEqualsShapedComboCrossSection — with --position-mode,
// each book row equals shape_book applied to the combo "alpha" cross-section
// at the rebalance date, with live mask from research.in_universe.
// ---------------------------------------------------------------------------
TEST_F(AtxImplOptimize, PositionModeBookEqualsShapedComboCrossSection) {
    const fs::path books_path = tmp_dir_ / "books_posmode.bin";

    atx::impl::RunConfig cfg;
    cfg.panel         = research_path_;
    cfg.combo         = combo_path_;
    cfg.books_out     = books_path.string();
    cfg.position_mode = true;
    cfg.rebalance     = "weekly";
    cfg.gross         = 1.0;
    cfg.name_cap      = 0.5;

    auto r = atx::impl::run_optimize(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message();

    auto books_r = atx::impl::read_panel(books_path.string());
    ASSERT_TRUE(books_r.has_value()) << books_r.error().message();
    const auto& books = *books_r;

    auto combo_r = atx::impl::read_panel(combo_path_);
    ASSERT_TRUE(combo_r.has_value()) << combo_r.error().message();
    const auto& combo = *combo_r;

    auto research_r = atx::impl::read_panel(research_path_);
    ASSERT_TRUE(research_r.has_value()) << research_r.error().message();
    const auto& research = *research_r;

    auto wfid_r = books.field_id("weight");
    ASSERT_TRUE(wfid_r.has_value()) << wfid_r.error().message();
    const auto wfid = *wfid_r;

    auto afid_r = combo.field_id("alpha");
    ASSERT_TRUE(afid_r.has_value()) << afid_r.error().message();
    const auto afid = *afid_r;

    const atx::usize N = research.instruments();

    for (atx::usize s = 0; s < books.dates(); ++s) {
        const atx::usize d = 5 * s;  // weekly step=5
        const auto cs = combo.field_cross_section(afid, d);
        std::vector<atx::f64> expect(cs.begin(), cs.end());
        std::vector<std::uint8_t> live(N);
        for (atx::usize i = 0; i < N; ++i) {
            live[i] = research.in_universe(d, i) ? 1 : 0;
        }
        atx::impl::shape_book(expect, std::span<const std::uint8_t>{live}, 1.0, 0.5);

        const auto got = books.field_cross_section(wfid, s);
        ASSERT_EQ(got.size(), N) << "books row size mismatch at s=" << s;
        for (atx::usize i = 0; i < N; ++i) {
            EXPECT_NEAR(got[i], expect[i], 1e-9)
                << "s=" << s << " i=" << i
                << " got=" << got[i] << " expect=" << expect[i];
        }
    }

    // Sidecar must exist.
    EXPECT_TRUE(fs::exists(books_path.string() + ".meta.txt"))
        << "missing sidecar .meta.txt for position_mode books";
}
