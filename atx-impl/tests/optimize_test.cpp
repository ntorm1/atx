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

// Build a research panel with EXPLICITLY HETEROGENEOUS per-instrument return
// variances, for use in the MVO-discrimination sub-test only.
//
// Design (see optimizer.hpp §λ>0 regime):
// The MVO smooth-target t ∝ P V^{-1} P α.  For a diagonal V, V^{-1} scales
// name i by 1/dvar[i].  When all dvar[i] are equal the scale factors out of
// the re-centering and t ∝ demean(α) — identical to the position-mode
// direction.  We break this degeneracy by giving every instrument a UNIQUE
// dvar that is STRICTLY ABOVE the 1e-4 risk-model floor and spans a 4:1
// range: dvar[i] = kDvarBase * (1 + i * kDvarStep).
//
// Alternating price construction: price[t][i] = 100*(1 + vol_i*(-1)^t) gives
// daily return ≈ ±2*vol_i → population variance ≈ 4*vol_i^2 = dvar[i].
// vol_i = sqrt(dvar[i]/4).  Each instrument has a different vol so different
// dvar — V^{-1} then preferentially weights the LOW-variance instruments (the
// ones with larger 1/dvar), producing a directionally different book from
// shape_book (which ignores variance).
//
// We use kNameCap = 1.0 (effectively uncapped) in the discrimination run so
// the optimizer's project() never calls cap_clip_renorm.  Without clip-renorm
// the projected/proximal loop's demean_live + gross_normalize guarantee
// |Σw| < 1e-12, keeping the MVO book constraint-valid.
//
// The construction is entirely deterministic (no RNG).
static atx::core::Result<std::string>
make_research_panel_hetvar(const fs::path& out, atx::usize M, atx::usize D)
{
    // dvar[i] = kDvarBase * (1 + i * kDvarStep), ranging from kDvarBase to
    // kDvarBase*(1 + (M-1)*kDvarStep).  With kDvarBase=5e-4, kDvarStep=0.2:
    //   i=0:  dvar=5e-4   (5× floor)
    //   i=19: dvar=5e-4*(1+3.8)=2.4e-3  (~24× floor)
    // The 5:1 ratio across names is large enough to shift MVO weights by >>
    // 1e-6 after V^{-1} + re-centre + gross-normalise.
    static constexpr atx::f64 kDvarBase = 5e-4;
    static constexpr atx::f64 kDvarStep = 0.2;
    std::vector<atx::f64> close_data;
    close_data.reserve(D * M);
    for (atx::usize t = 0; t < D; ++t) {
        for (atx::usize i = 0; i < M; ++i) {
            const atx::f64 dvar_i = kDvarBase * (1.0 + static_cast<atx::f64>(i) * kDvarStep);
            const atx::f64 vol_i  = std::sqrt(dvar_i / 4.0);  // 4*vol^2 = dvar
            // Alternating sign each day: price is always positive (vol_i < 0.5).
            const atx::f64 sign = (t % 2 == 0) ? 1.0 : -1.0;
            close_data.push_back(100.0 * (1.0 + vol_i * sign));
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

    // -----------------------------------------------------------------------
    // Discrimination assertion: MVO at risk_aversion=1.0 on a
    // heterogeneous-variance research panel MUST differ from the
    // position-mode book on the SAME combo by > 1e-6 on at least one
    // (period, name) pair.
    //
    // Why a separate research panel:
    //   The shared fixture (gentle exponential trend) produces all-floor
    //   dvar[i] = 1e-4.  With a uniform diagonal V, V^{-1}α ∝ α and the MVO
    //   smooth target reduces to demean(α) — identical to position-mode.  We
    //   must use a research panel where instruments have CLEARLY DIFFERENT
    //   dvar so V^{-1} tilts the book direction away from the pure-alpha
    //   direction (optimizer.hpp §λ>0 regime).
    //
    // Why name_cap = gross (uncapped):
    //   Without cap_clip_renorm the optimizer's project() reduces to
    //   demean_live + gross_normalize, which guarantees |Σw| < 1e-12.
    //   A cap < gross with heterogeneous V can break dollar-neutral (the
    //   cap clips asymmetrically and project() does not re-center after
    //   clip), which would invalidate ProducesValidBooks on the shared panel.
    //   The discrimination assertion does not need the cap — the V^{-1} tilt
    //   is observable from direction change alone.
    //
    // Why this proves position_mode bypasses the optimizer:
    //   If position_mode bypasses the MVO path, it runs shape_book which is
    //   V-agnostic.  The MVO path at λ=1 uses V^{-1} — with heterogeneous
    //   dvar the directions differ by >> 1e-6.  If position_mode did NOT
    //   bypass MVO, both runs would use the same code path and produce the
    //   same book, making the assertion vacuous.
    // -----------------------------------------------------------------------
    const fs::path hetvar_research = tmp_dir_ / "research_hetvar.bin";
    {
        auto hr = make_research_panel_hetvar(hetvar_research, kM, kD);
        ASSERT_TRUE(hr.has_value()) << hr.error().message();
    }
    const fs::path pm_disc_path  = tmp_dir_ / "books_pm_disc.bin";
    const fs::path mvo_disc_path = tmp_dir_ / "books_mvo_disc.bin";

    // Position-mode run (signal-as-position; uncapped so all 20 names carry
    // fractional weight proportional to demean(alpha)).
    {
        atx::impl::RunConfig pm_cfg;
        pm_cfg.panel         = hetvar_research.string();
        pm_cfg.combo         = combo_path_;
        pm_cfg.books_out     = pm_disc_path.string();
        pm_cfg.position_mode = true;
        pm_cfg.rebalance     = "weekly";
        pm_cfg.gross         = 1.0;
        pm_cfg.name_cap      = 1.0;  // uncapped: max |w| = 9.5/100 = 0.095 < 1.0
        auto pm_r = atx::impl::run_optimize(pm_cfg);
        ASSERT_TRUE(pm_r.has_value()) << pm_r.error().message();
    }
    // MVO run (risk_aversion=1.0; V^{-1} tilts away from high-variance names).
    {
        atx::impl::RunConfig mvo_cfg;
        mvo_cfg.panel         = hetvar_research.string();
        mvo_cfg.combo         = combo_path_;
        mvo_cfg.books_out     = mvo_disc_path.string();
        mvo_cfg.position_mode = false;
        mvo_cfg.risk_aversion = 1.0;
        mvo_cfg.set_flags.emplace("risk-aversion");
        mvo_cfg.rebalance     = "weekly";
        mvo_cfg.gross         = 1.0;
        mvo_cfg.name_cap      = 1.0;  // uncapped: guarantees exact dollar-neutral
        auto mvo_r = atx::impl::run_optimize(mvo_cfg);
        ASSERT_TRUE(mvo_r.has_value()) << mvo_r.error().message();
    }

    auto pm_disc_r = atx::impl::read_panel(pm_disc_path.string());
    ASSERT_TRUE(pm_disc_r.has_value()) << pm_disc_r.error().message();
    const auto& pm_disc_books = *pm_disc_r;

    auto mvo_disc_r = atx::impl::read_panel(mvo_disc_path.string());
    ASSERT_TRUE(mvo_disc_r.has_value()) << mvo_disc_r.error().message();
    const auto& mvo_disc_books = *mvo_disc_r;

    auto pm_wfid_r = pm_disc_books.field_id("weight");
    ASSERT_TRUE(pm_wfid_r.has_value()) << pm_wfid_r.error().message();
    const auto pm_wfid = *pm_wfid_r;

    auto mvo_wfid_r = mvo_disc_books.field_id("weight");
    ASSERT_TRUE(mvo_wfid_r.has_value()) << mvo_wfid_r.error().message();
    const auto mvo_wfid = *mvo_wfid_r;

    ASSERT_EQ(pm_disc_books.dates(),       mvo_disc_books.dates())
        << "PM-disc and MVO-disc period counts differ";
    ASSERT_EQ(pm_disc_books.instruments(), mvo_disc_books.instruments())
        << "PM-disc and MVO-disc instrument counts differ";

    // At least one (period, name) pair must differ by > 1e-6.
    bool found_divergence = false;
    for (atx::usize s = 0; s < pm_disc_books.dates() && !found_divergence; ++s) {
        const auto pm_ws  = pm_disc_books.field_cross_section(pm_wfid, s);
        const auto mvo_ws = mvo_disc_books.field_cross_section(mvo_wfid, s);
        ASSERT_EQ(pm_ws.size(), kM);
        ASSERT_EQ(mvo_ws.size(), kM);
        for (atx::usize i = 0; i < kM; ++i) {
            if (std::fabs(mvo_ws[i] - pm_ws[i]) > 1e-6) {
                found_divergence = true;
                break;
            }
        }
    }
    EXPECT_TRUE(found_divergence)
        << "MVO(risk_aversion=1.0) book equals position-mode book within 1e-6 "
           "on the heterogeneous-variance panel — V^{-1} risk tilt not observable; "
           "position-mode branch may not be bypassing the optimizer.";
}
