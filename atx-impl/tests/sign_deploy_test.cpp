// atx::impl — S6-0 sign-correct deploy tests (suite SignDeployTest, TDD).
//
// Context: the combine->optimize->report pipeline feeds a COMBINED TARGET-WEIGHT
// panel to run_optimize. The default MVO path (position_mode=false, effective
// risk_aversion=1.0) treats that target-weight panel as an expected-return proxy
// and the diagonal-variance solve  t = (1/2λ)·P V⁻¹ P α  re-weights each name by
// 1/dvar and re-centers. When the high-conviction names of a GOOD target-weight
// book happen to be high-variance, the 1/dvar tilt crushes them and the
// re-centering loads the wrong-way names instead, so the DEPLOYED book realizes a
// NEGATIVE Sharpe even though the target-weight blend itself realizes a POSITIVE
// Sharpe (the reported "+alpha -> -Sharpe" inversion). The position-mode path
// (shape_book: dollar-neutralize -> gross-normalize -> name-cap) deploys the
// target weights directly and is sign-preserving.
//
// Metric: REALIZED portfolio Sharpe (portfolio_sharpe in summary.txt), the exact
// quantity the report stage scores and the one the bug report cites. (The naive
// book·alpha alignment is NOT usable here: for the κ=0/λ>0 smooth solve it is
// Σ t·α = Σ α²/dvar > 0 ALWAYS, so it can never witness this bug — the inversion
// is between the deployed book and the REALIZED RETURNS, not the input α.)
//
// Tests:
//   SignCorrectRoutingFixesInvertedDeploy — (b) on-path RED->GREEN. The sign-correct
//                                           route (position_mode -> shape_book)
//                                           deploys a POSITIVE-Sharpe book; the SAME
//                                           panel on the DEFAULT MVO path deploys a
//                                           NEGATIVE-Sharpe (inverted) book. The RED
//                                           step (default-path assertion FAILING at
//                                           sharpe=-3.47) was captured pre-fix; this
//                                           committed test pins both sides so the
//                                           inversion cannot silently return.
//   DefaultPathByteIdentityDigest         — (a) off-path pin: the DEFAULT
//                                           (position_mode=false) deployed-book digest
//                                           is pinned, so any change to default
//                                           behavior fails. (Opt-in fix => default
//                                           path byte-identical.)

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

namespace atx_impl_sign_deploy {

namespace fs    = std::filesystem;
namespace alpha = atx::engine::alpha;

using atx::f64;
using atx::usize;

// ---------------------------------------------------------------------------
// Deterministic LCG (same idiom as the other atx-impl tests) — zero-mean noise in
// [-1, 1] so each instrument's realized return is mu_i + vol_i * noise.
// ---------------------------------------------------------------------------
struct Lcg {
    std::uint64_t s;
    f64 next() noexcept {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t hi = s >> 11U;
        const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
        return 2.0 * u - 1.0;
    }
};

// ---------------------------------------------------------------------------
// Fixture. Per-instrument parameters chosen (offline search against a faithful
// replica of the diagonal-V MVO solve) so that:
//   * the raw target-weight blend (== the position-mode deploy) realizes a clearly
//     POSITIVE Sharpe on the synthetic returns, but
//   * the DEFAULT MVO deploy of the SAME target-weight panel realizes a clearly
//     NEGATIVE Sharpe (the 1/dvar tilt + re-center inverts the book).
// alpha_i  : the combined target weight for name i (dollar-neutral cross-section).
// mu_i     : the per-period expected (drift) return for name i (dollar-neutral).
// vol_i    : the per-period return volatility for name i (drives dvar_i ≈ vol_i²).
// The winners (large +mu) are deliberately HIGH-vol and the alpha is correctly
// LONG them; the MVO 1/dvar tilt drops those winners and overloads low-vol names
// where the alpha is wrong, flipping realized PnL.
// ---------------------------------------------------------------------------
class SignDeployTest : public ::testing::Test {
protected:
    static constexpr usize kM = 8;     // instruments
    static constexpr usize kD = 160;   // dates (weekly step=5 -> 32 rebalances)

    // Offline-solved inverting cross-section (see header). Dollar-neutral by
    // construction (each vector sums to ~0; we re-center in SetUp to be exact).
    static constexpr f64 kAlpha[kM] = {
        0.2049, 1.2303, -1.3263, -0.2371, -0.2495, 1.1715, 0.0876, -0.8815};
    static constexpr f64 kMu[kM] = {
        0.1332, 0.3219, -0.3649, 0.5236, 0.1996, 0.0332, 0.0188, -0.8655};
    static constexpr f64 kVol[kM] = {
        0.0427, 0.0723, 0.1817, 0.0209, 0.0200, 0.0933, 0.0252, 0.0638};

    fs::path    work_dir_;
    std::string research_path_;
    std::string combo_path_;
    std::vector<f64> alpha_xs_;        // re-centered alpha cross-section

    void SetUp() override {
        work_dir_ = fs::temp_directory_path() / "atx_impl_sign_deploy_test";
        std::error_code ec;
        fs::remove_all(work_dir_, ec);
        fs::create_directories(work_dir_, ec);

        // Exactly dollar-neutralize the alpha and the drift cross-sections.
        alpha_xs_.assign(kAlpha, kAlpha + kM);
        std::vector<f64> mu(kMu, kMu + kM);
        const auto demean = [](std::vector<f64>& v) {
            f64 m = 0.0;
            for (f64 x : v) m += x;
            m /= static_cast<f64>(v.size());
            for (f64& x : v) x -= m;
        };
        demean(alpha_xs_);
        demean(mu);

        // Research "close": geometric path with per-period return mu_i + vol_i*eps.
        // eps is deterministic LCG noise (zero-mean), so each name's return
        // variance ≈ vol_i² (monotone control of dvar) and its mean ≈ mu_i (the
        // realized edge). No NaNs, all prices positive.
        std::vector<f64> close(kD * kM, 0.0);
        Lcg rng{0xA1F0C0DEULL};
        std::vector<f64> px(kM, 100.0);
        for (usize t = 0; t < kD; ++t) {
            for (usize i = 0; i < kM; ++i) {
                const f64 r = mu[i] * 0.01 + kVol[i] * rng.next();
                px[i] *= (1.0 + r);
                close[t * kM + i] = px[i];
            }
        }
        std::vector<std::uint8_t> uni_r(kD * kM, 1u);
        auto rp = alpha::Panel::create(kD, kM, {"close"}, {close}, uni_r);
        ASSERT_TRUE(rp.has_value()) << rp.error().to_string();
        research_path_ = (work_dir_ / "research.bin").string();
        auto rw = atx::impl::write_panel(*rp, research_path_);
        ASSERT_TRUE(rw.has_value()) << rw.error().to_string();

        // Combo panel: the SAME (re-centered) alpha cross-section every date — a
        // steady combined TARGET-WEIGHT book. Field MUST be named "alpha".
        std::vector<f64> combo(kD * kM, 0.0);
        for (usize t = 0; t < kD; ++t)
            for (usize i = 0; i < kM; ++i)
                combo[t * kM + i] = alpha_xs_[i];
        std::vector<std::uint8_t> uni_c(kD * kM, 1u);
        auto cp = alpha::Panel::create(kD, kM, {"alpha"}, {combo}, uni_c);
        ASSERT_TRUE(cp.has_value()) << cp.error().to_string();
        combo_path_ = (work_dir_ / "combo.bin").string();
        auto cw = atx::impl::write_panel(*cp, combo_path_);
        ASSERT_TRUE(cw.has_value()) << cw.error().to_string();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(work_dir_, ec);
    }

    // Parse a key=value double from summary.txt.
    static f64 read_summary_double(const fs::path& summary, const std::string& key) {
        std::ifstream f(summary);
        std::string line;
        while (std::getline(f, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (line.substr(0, eq) == key) return std::stod(line.substr(eq + 1));
        }
        return std::numeric_limits<f64>::quiet_NaN();
    }

    struct DeployResult {
        f64       portfolio_sharpe = 0.0;
        atx::u64  books_digest     = 0;
    };

    // Run optimize (deploy) + report and return the REALIZED portfolio Sharpe and
    // the deployed-book digest.
    atx::core::Result<DeployResult> run_deploy(bool position_mode,
                                               const std::string& tag)
    {
        const fs::path books_path  = work_dir_ / (tag + "_books.bin");
        const fs::path report_path = work_dir_ / (tag + "_report");
        std::error_code ec;
        fs::create_directories(report_path, ec);

        atx::impl::RunConfig opt_cfg;
        opt_cfg.panel         = research_path_;
        opt_cfg.combo         = combo_path_;
        opt_cfg.books_out     = books_path.string();
        opt_cfg.gross         = 1.0;
        opt_cfg.name_cap      = 1.0;   // non-binding so the sign is not masked
        opt_cfg.rebalance     = "weekly";
        opt_cfg.position_mode = position_mode;
        // Leave risk_aversion at its struct default and DO NOT set the
        // "risk-aversion" flag, so the MVO path uses its effective default λ=1.0
        // (the inverting path) — exactly the default on-path being tested.

        ATX_TRY(auto opt_sr, atx::impl::run_optimize(opt_cfg));

        atx::impl::RunConfig rep_cfg;
        rep_cfg.panel      = research_path_;
        rep_cfg.books      = books_path.string();
        rep_cfg.report_out = report_path.string();
        rep_cfg.report_aum = 1e9;
        ATX_TRY(auto rep_sr, atx::impl::run_report(rep_cfg));
        (void)rep_sr;

        DeployResult res;
        res.books_digest     = opt_sr.digest;
        res.portfolio_sharpe =
            read_summary_double(report_path / "summary.txt", "portfolio_sharpe");
        return atx::core::Ok(res);
    }
};

constexpr f64 SignDeployTest::kAlpha[];
constexpr f64 SignDeployTest::kMu[];
constexpr f64 SignDeployTest::kVol[];

// ---------------------------------------------------------------------------
// (b) on-path RED->GREEN — the sign-correct routing fixes the inversion.
//
// RED step (captured before the fix; see task-s6-0-report.md): running the SAME
// "deployed Sharpe must be > 0" assertion on the DEFAULT path FAILED with
// portfolio_sharpe = -3.47 — the default MVO deploy inverts the book. The GREEN
// assertion below routes the SAME combined target-weight panel through the
// sign-correct path (RunConfig::position_mode = true -> shape_book) and the
// deployed book realizes a POSITIVE Sharpe (same sign as the profitable blend).
//
// This single test also PINS the inversion so the bug cannot silently return: the
// default path must stay NEGATIVE (the opt-in fix leaves it byte-identical) while
// the sign-correct path must be POSITIVE — and strictly above the default, proving
// the routing genuinely flips the realized sign rather than merely scaling it.
// ---------------------------------------------------------------------------
TEST_F(SignDeployTest, SignCorrectRoutingFixesInvertedDeploy)
{
    // Sign-correct deploy (position-mode / shape_book): the documented fix.
    auto fixed = run_deploy(/*position_mode=*/true, "pos_mode");
    ASSERT_TRUE(fixed.has_value()) << fixed.error().message();
    ASSERT_FALSE(std::isnan(fixed->portfolio_sharpe)) << "fixed sharpe is NaN";

    // Default MVO deploy of the SAME panel: the inverting path (characterization).
    auto deflt = run_deploy(/*position_mode=*/false, "mvo_default");
    ASSERT_TRUE(deflt.has_value()) << deflt.error().message();
    ASSERT_FALSE(std::isnan(deflt->portfolio_sharpe)) << "default sharpe is NaN";

    // GREEN: the sign-correct deploy realizes a POSITIVE Sharpe (same sign as the
    // raw target-weight blend).
    EXPECT_GT(fixed->portfolio_sharpe, 0.0)
        << "sign-correct (position-mode) deploy must realize a POSITIVE Sharpe "
           "(same sign as the target-weight blend); got "
        << fixed->portfolio_sharpe;

    // The inversion is REAL: the DEFAULT MVO deploy of the SAME profitable panel
    // realizes a NEGATIVE Sharpe (the reported "+alpha -> -Sharpe" bug). If this
    // ever flips positive, the synthetic fixture stopped exercising the bug and
    // the RED proof is vacuous — fail loudly so the fixture is revisited.
    EXPECT_LT(deflt->portfolio_sharpe, 0.0)
        << "fixture must still EXERCISE the inversion: default MVO deploy should "
           "realize a NEGATIVE Sharpe, got "
        << deflt->portfolio_sharpe;

    // The fix strictly flips the sign (not a marginal change).
    EXPECT_GT(fixed->portfolio_sharpe, deflt->portfolio_sharpe)
        << "sign-correct deploy (" << fixed->portfolio_sharpe
        << ") must beat the inverted default deploy (" << deflt->portfolio_sharpe
        << ")";
}

// ---------------------------------------------------------------------------
// Off-path byte-identity pin: the DEFAULT (position_mode=false) deployed-book
// digest must equal a pinned value, so any change to the default optimize path is
// caught. The sign-correct fix is opt-in (selected via position_mode) and a
// SEPARATE code path; it MUST NOT touch the default MVO book.
// ---------------------------------------------------------------------------
TEST_F(SignDeployTest, DefaultPathByteIdentityDigest)
{
    auto a = run_deploy(/*position_mode=*/false, "pin_a");
    ASSERT_TRUE(a.has_value()) << a.error().message();
    auto b = run_deploy(/*position_mode=*/false, "pin_b");
    ASSERT_TRUE(b.has_value()) << b.error().message();

    EXPECT_EQ(a->books_digest, b->books_digest)
        << "default MVO deploy must be deterministic across runs";
    EXPECT_NE(a->books_digest, atx::u64{0})
        << "default MVO deploy digest must be non-zero";

    // Pinned digest of the default path (captured pre-fix). A regression that
    // alters the default optimize book changes this digest and fails here.
    static constexpr atx::u64 kPinnedDefaultDigest = 5744281451106956152ULL;
    EXPECT_EQ(a->books_digest, kPinnedDefaultDigest)
        << "default MVO deployed-book digest changed; the sign-correct fix must "
           "be opt-in and leave the default path byte-identical. Update the pin "
           "ONLY if the default path was intentionally changed.";
}

} // namespace atx_impl_sign_deploy
