// stage_combine_participation_test.cpp — p8 S3-SEAM: the capacity
// participation UNIT-bug fix S4-1 handed off to Sprint 3.
//
// S4-1 (atx-engine/plans/p8/sprint-4-progress.md) fixed the identical bug at
// its two OWNED sites (risk/capacity.hpp, factory/fitness.cpp): a share COUNT
// was divided by a DOLLAR-ADV (Sigma close*volume), leaving the quotient off by
// a factor of `price`. S4-1's ledger recorded the SAME bug, unfixed, at TWO
// S3-owned sites in stage_combine.cpp (confirmed present at kickoff):
//   * alpha_capacity_aum:        part_per_aum = abs_w / (price * adv)
//   * alpha_max_participation:   part = (target_aum * abs_w / price) / adv
// Correct participation is notional/dollar-ADV (unitless): abs_w is already a
// FRACTION of book (not a share count), so `abs_w / adv` (no `price` at all)
// is participation-per-unit-AUM; multiplying by target_aum gives the
// dollar-participation ratio directly -- `price` never belongs in either
// formula. This is a CONTRACT-B correctness fix (Sprint-4-progress.md's
// determinism-contract taxonomy): the corrected numbers differ from today's,
// but they feed ONLY the capacity kvs telemetry (stage_combine.cpp:737-768),
// never combo.bin / the hashed panel digest -- so the combine DIGEST is
// byte-identical before/after; only the capacity kvs VALUES change (see
// CapacityKvsKeysAreEmittedButNotFoldedIntoDigest below).
//
// RED -> GREEN proof (S4-1's own by-construction method): PRICE INVARIANCE.
// Two panels differ ONLY by a uniform 8x share-price rescale (close *= 8,
// volume /= 8 -- an EXACT power-of-two rescale, chosen so the rescale
// introduces no floating-point rounding of its own): every per-step RETURN
// (close[t]/close[t-1]) is scale-invariant, so the alpha's positions/PnL/
// realized edge are identical between the two panels; dollar-ADV
// (close*volume, an 8x*1/8x product) is likewise invariant. Only the raw
// share PRICE differs (exactly 8x). A price-DEPENDENT (buggy) participation
// formula must therefore report DIFFERENT capacity/participation telemetry
// between the two panels; a price-INVARIANT (fixed) one must report the SAME
// telemetry, to within ordinary floating-point tolerance.
//
// Suite: StageCombineParticipation

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

namespace atxtest_stage_combine_participation {

using atx::f64;
using atx::usize;
using atx::engine::alpha::Panel;

// ---------------------------------------------------------------------------
// Local self-contained fixture helpers (mirrors the atx-impl/tests convention
// -- each test TU owns its own small copies rather than sharing test-only
// infrastructure across translation units).
// ---------------------------------------------------------------------------

static std::optional<Panel> make_panel(usize dates, usize insts,
                                       const std::vector<std::string>& field_names,
                                       const std::vector<std::vector<f64>>& columns) {
    auto r = Panel::create(dates, insts, field_names, columns, {});
    if (!r.has_value()) {
        ADD_FAILURE() << "panel fixture must build: " << r.error().to_string();
        return std::nullopt;
    }
    return std::move(r.value());
}

static std::string write_panel_tmp(const Panel& panel, const std::string& stem) {
    namespace fs = std::filesystem;
    const std::string path =
        (fs::temp_directory_path() / ("atx_impl_scp_" + stem + ".bin")).string();
    auto r = atx::impl::write_panel(panel, path);
    EXPECT_TRUE(r.has_value()) << "write_panel must succeed";
    return path;
}

static std::string write_alpha_dir(const std::string& stem, const std::vector<std::string>& dsls) {
    namespace fs = std::filesystem;
    const std::string dir = (fs::temp_directory_path() / ("atx_impl_scp_alphas_" + stem)).string();
    fs::create_directories(dir);
    for (usize i = 0; i < dsls.size(); ++i) {
        const std::string name = "alpha_" + std::to_string(i) + ".dsl";
        std::ofstream f{(fs::path{dir} / name).string()};
        EXPECT_TRUE(f.is_open());
        f << dsls[i] << '\n';
    }
    return dir;
}

static std::string find_kv(const atx::impl::StageResult& sr, const std::string& k) {
    for (const auto& p : sr.kvs) {
        if (p.first == k) return p.second;
    }
    return "";
}

// ---------------------------------------------------------------------------
// make_reversal_panel(price_scale) -- the S6-1 reversal fixture (proven to
// give a POSITIVE realized edge for "delta(close,2)", i.e. it actually
// reaches the part_per_aum/part arithmetic instead of short-circuiting to a
// 0.0/+inf sentinel), parameterized by a uniform price rescale. `price_scale`
// multiplies every close cell; volume is divided by the SAME factor so
// dollar-ADV (close*volume) is invariant to the rescale -- isolating price as
// the ONE thing that differs between two calls. 8.0 is an exact power of two
// (no rounding from the rescale itself), so the two panels' returns/PnL/ADV
// are floating-point IDENTICAL and only the raw share price differs 8x.
// ---------------------------------------------------------------------------
struct Lcg {
    std::uint64_t s;
    [[nodiscard]] f64 next() noexcept {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t hi = s >> 11U;
        return 2.0 * (static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U)) - 1.0;
    }
};

static std::optional<Panel> make_reversal_panel(f64 price_scale) {
    constexpr usize D = 100U;
    constexpr usize N = 4U;
    constexpr usize kEnd = 80U;
    const std::array<f64, N> drift_up  = {+0.020, +0.010, -0.010, -0.020};
    const std::array<f64, N> drift_rev = {-0.025, -0.015, +0.005, +0.015};

    std::vector<f64> close(D * N);
    std::array<f64, N> px{};
    px.fill(100.0);
    Lcg rng{0xCAFEBABEULL};
    constexpr f64 noise = 0.002;
    for (usize t = 0; t < D; ++t) {
        const bool rev = (t >= kEnd);
        for (usize j = 0; j < N; ++j) {
            const f64 dr = rev ? drift_rev[j] : drift_up[j];
            px[j] *= (1.0 + dr + noise * rng.next());
            close[t * N + j] = px[j] * price_scale;
        }
    }
    std::vector<f64> volume(D * N, 1.0e7 / price_scale);
    return make_panel(D, N, {"close", "volume"}, {close, volume});
}

// Drive run_combine on a reversal panel at `price_scale` with capacity
// telemetry on; returns the StageResult (asserts success).
static atx::impl::StageResult run_capacity_probe(f64 price_scale, const std::string& tag) {
    namespace fs = std::filesystem;
    auto panel_opt = make_reversal_panel(price_scale);
    EXPECT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, tag);
    const std::string alphas_dir = write_alpha_dir(tag, {"delta(close,2)"});
    const std::string combo_out = (fs::temp_directory_path() / ("atx_impl_scp_" + tag + ".bin")).string();

    atx::impl::RunConfig cfg;
    cfg.subcommand     = "combine";
    cfg.panel          = panel_path;
    cfg.alphas         = alphas_dir;
    cfg.combo_out      = combo_out;
    cfg.method         = "equal";
    cfg.fit_begin      = 0;
    cfg.fit_end        = 0;
    cfg.capacity_floor = 1.0;   // any positive capacity AUM passes the floor
    cfg.target_aum     = 1.0e8;
    cfg.corr_penalty   = 0.0;

    auto r = atx::impl::run_combine(cfg);
    EXPECT_TRUE(r.has_value()) << "run_combine must succeed: "
                               << (r.has_value() ? "" : r.error().message());

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    fs::remove(combo_out, ec);
    fs::remove(combo_out + ".weights.txt", ec);
    fs::remove(combo_out + ".meta", ec);
    return r.value_or(atx::impl::StageResult{});
}

// ===========================================================================
//  RED -> GREEN: alpha_capacity_aum's part_per_aum must be price-invariant.
// ===========================================================================
TEST(StageCombineParticipation, CapacityAumIsPriceInvariantForEqualNotional) {
    const atx::impl::StageResult r1 = run_capacity_probe(1.0, "cap_scale1");
    const atx::impl::StageResult r2 = run_capacity_probe(8.0, "cap_scale8");

    const std::string cap1_csv = find_kv(r1, "capacity_alpha_aum");
    const std::string cap2_csv = find_kv(r2, "capacity_alpha_aum");
    ASSERT_FALSE(cap1_csv.empty()) << "capacity_alpha_aum must be emitted (scale=1)";
    ASSERT_FALSE(cap2_csv.empty()) << "capacity_alpha_aum must be emitted (scale=8)";

    const f64 cap1 = std::stod(cap1_csv);
    const f64 cap2 = std::stod(cap2_csv);
    ASSERT_GT(cap1, 0.0) << "fixture must reach the part_per_aum arithmetic (positive edge), "
                         << "not the gross_edge_bps<=0 short-circuit -- got cap1=" << cap1;
    ASSERT_GT(cap2, 0.0) << "fixture must reach the part_per_aum arithmetic, got cap2=" << cap2;

    // Correct participation (abs_w/adv, unitless) never reads price at all, so
    // an 8x uniform price rescale (with dollar-ADV held fixed by construction)
    // must leave the capacity AUM UNCHANGED -- tight relative tolerance (this
    // is a floating-point-noise-only gap, not a "close enough" approximation).
    EXPECT_NEAR(cap1, cap2, std::abs(cap1) * 1e-6)
        << "S3-SEAM: capacity_alpha_aum must be price-invariant for equal notional/ADV "
        << "(part_per_aum = abs_w/adv, no price term) -- got scale1=" << cap1
        << " scale8=" << cap2 << " (buggy abs_w/(price*adv) would differ by ~8x)";
}

// ===========================================================================
//  RED -> GREEN: alpha_max_participation's `part` must be price-invariant.
// ===========================================================================
TEST(StageCombineParticipation, MaxParticipationIsPriceInvariantForEqualNotional) {
    const atx::impl::StageResult r1 = run_capacity_probe(1.0, "part_scale1");
    const atx::impl::StageResult r2 = run_capacity_probe(8.0, "part_scale8");

    const std::string p1_str = find_kv(r1, "capacity_max_participation");
    const std::string p2_str = find_kv(r2, "capacity_max_participation");
    ASSERT_FALSE(p1_str.empty()) << "capacity_max_participation must be emitted (scale=1)";
    ASSERT_FALSE(p2_str.empty()) << "capacity_max_participation must be emitted (scale=8)";

    const f64 part1 = std::stod(p1_str);
    const f64 part2 = std::stod(p2_str);
    ASSERT_GT(part1, 0.0) << "fixture must produce a nonzero max participation";
    ASSERT_GT(part2, 0.0) << "fixture must produce a nonzero max participation";

    EXPECT_NEAR(part1, part2, std::abs(part1) * 1e-6)
        << "S3-SEAM: capacity_max_participation must be price-invariant "
        << "(part = target_aum*abs_w/adv, no price term) -- got scale1=" << part1
        << " scale8=" << part2 << " (buggy target_aum*abs_w/(price*adv) would differ by ~8x)";
}

// ===========================================================================
//  The combine DIGEST is untouched by this fix (participation feeds capacity
//  kvs telemetry only, never combo.bin) -- the off-path/on-path digest is
//  driven purely by the DSL/panel/method, so a capacity-floor run at ANY
//  price scale must still produce a well-formed, non-empty combined book.
//  (The dedicated S4-1-style "no golden re-baseline needed" claim is recorded
//  in the ledger; this test is the structural half -- the fix must not have
//  broken the stage's basic contract.)
// ===========================================================================
TEST(StageCombineParticipation, CapacityKvsKeysAreEmittedButNotFoldedIntoDigest) {
    const atx::impl::StageResult r_cap_on  = run_capacity_probe(1.0, "digest_capon");

    namespace fs = std::filesystem;
    auto panel_opt = make_reversal_panel(1.0);
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "digest_capoff");
    const std::string alphas_dir = write_alpha_dir("digest_capoff", {"delta(close,2)"});
    const std::string combo_out = (fs::temp_directory_path() / "atx_impl_scp_digest_capoff.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.combo_out  = combo_out;
    cfg.method     = "equal";
    // capacity_floor left at its 0.0 default -> capacity telemetry OFF.
    auto r_cap_off = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_cap_off.has_value()) << r_cap_off.error().message();

    EXPECT_EQ(r_cap_on.digest, r_cap_off->digest)
        << "capacity telemetry (on vs off) must not perturb the combine digest -- "
        << "participation feeds kvs only, never combo.bin";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    fs::remove(combo_out, ec);
    fs::remove(combo_out + ".weights.txt", ec);
    fs::remove(combo_out + ".meta", ec);
}

} // namespace atxtest_stage_combine_participation
