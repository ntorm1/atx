// stage_optimize_participation_cap_test.cpp — p9 Sprint 5 (S5-2): the %ADV
// participation-rate cap threaded INTO the optimizer's QP construction (a real
// risk::ConstraintSet.part + risk::CapacityRef), not just the post-hoc report curve.
//
// Suite: StageOptimizeParticipationCap
//
//   (a) ParticipationCap_OffPathByteIdentical — participation_cap == 0.0 (default)
//       leaves mc.constraints unset -> the fast (non-augmented) PortfolioOptimizer
//       path runs, deterministic + unchanged (twice-run identical), and a BINDING
//       cap produces a DIFFERENT digest (the augmented path is a distinct branch).
//   (b) ParticipationCap_BoundsThinAdvName — a fixture with one THIN-ADV name
//       (tiny volume) carrying a concentrated alpha tilt. WITHOUT the cap the thin
//       name sizes up toward name_cap; WITH a tight --participation-cap the SAME
//       name's realized weight is (i) strictly lower than the uncapped run and (ii)
//       at/under the closed-form bound rho*H*adv*price/nav. The capped book ALSO
//       stays dollar-neutral AND within name_cap (the augmented path's gross/pos
//       discipline is carried forward from the fast path -- the correctness trap).
//   (c) ParticipationCap_TwiceRunByteIdentical — identical augmented-path books
//       digest across two runs.
//   (d) N/A: the per-period augmented solve inherits ConstrainedQpSolver's proven
//       ADMM determinism (fixed iteration count, no thread/time); run_optimize has
//       no parallel entry point. Stated here rather than silently omitted.

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"

namespace atxtest_stage_optimize_participation_cap {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
using atx::f64;
using atx::usize;

constexpr usize kThinName = 0; // instrument 0 is the deliberately thin-ADV name

// Research: gently-varying close (well-defined variance, price stays ~100) + a
// per-name "volume" with ONE thin name (kThinName) and the rest deeply liquid.
static atx::core::Result<std::string> make_thin_adv_research(const fs::path& out, usize M, usize D) {
  std::vector<f64> close(D * M, 100.0);
  std::vector<f64> volume(D * M, 0.0);
  for (usize i = 0; i < M; ++i) {
    const f64 idio_amp = 0.0004 * (1.0 + static_cast<f64>(i % 5));
    f64 level = 100.0;
    for (usize t = 0; t < D; ++t) {
      const f64 ret = idio_amp * std::sin(0.7 * static_cast<f64>(t) + static_cast<f64>(i));
      if (t > 0) level *= (1.0 + ret);
      close[t * M + i] = level;
      volume[t * M + i] = (i == kThinName) ? 1.0e3 : 1.0e8; // thin vs deeply liquid
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"close", "volume"}, {close, volume}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

// Combo: a strong long tilt CONCENTRATED on the thin name, small opposite tilt on
// the rest -> without a participation cap the optimizer loads the thin name toward
// name_cap.
static atx::core::Result<std::string> make_thin_tilt_combo(const fs::path& out, usize M, usize D) {
  std::vector<f64> a;
  a.reserve(D * M);
  for (usize t = 0; t < D; ++t) {
    (void)t;
    for (usize i = 0; i < M; ++i) a.push_back(i == kThinName ? 5.0 : -1.0);
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"alpha"}, {a}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

struct BookLast {
  std::vector<f64> w;
  f64 net = 0.0;      // Σ w  (dollar-neutrality residual)
  f64 max_abs = 0.0;  // max |w_i| (name-cap check)
};

static atx::core::Result<BookLast> last_period_book(const std::string& books_path, usize M) {
  ATX_TRY(auto books, atx::impl::read_panel(books_path));
  ATX_TRY(const auto wfid, books.field_id("weight"));
  const usize last = books.dates() - 1;
  const auto cs = books.field_cross_section(wfid, last);
  BookLast b;
  b.w.assign(cs.begin(), cs.end());
  for (usize i = 0; i < M; ++i) {
    b.net += b.w[i];
    b.max_abs = std::max(b.max_abs, std::fabs(b.w[i]));
  }
  return atx::core::Ok(std::move(b));
}

class StageOptimizeParticipationCap : public ::testing::Test {
protected:
  fs::path tmp_dir_;
  void SetUp() override {
    tmp_dir_ = fs::temp_directory_path() / "atx_s5_2_participation_cap";
    fs::create_directories(tmp_dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_dir_, ec);
  }
  atx::impl::RunConfig base_cfg(const std::string& research, const std::string& combo) {
    atx::impl::RunConfig cfg;
    cfg.panel = research;
    cfg.combo = combo;
    cfg.gross = 1.0;
    cfg.name_cap = 0.5;
    cfg.rebalance = "weekly";
    cfg.risk_aversion = 1.0;
    cfg.set_flags.emplace("risk-aversion");
    cfg.report_aum = 1.0e7; // NAV anchor for the participation bound
    return cfg;
  }
};

// (a) off-path byte-identity + the binding-cap branch differs.
TEST_F(StageOptimizeParticipationCap, ParticipationCap_OffPathByteIdentical) {
  constexpr usize M = 8, D = 60;
  const auto research = *make_thin_adv_research(tmp_dir_ / "research.bin", M, D);
  const auto combo = *make_thin_tilt_combo(tmp_dir_ / "combo.bin", M, D);

  auto cfg = base_cfg(research, combo);
  cfg.books_out = (tmp_dir_ / "books_off_a.bin").string();
  auto a = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(a.has_value()) << a.error().message();
  cfg.books_out = (tmp_dir_ / "books_off_b.bin").string();
  auto b = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(b.has_value()) << b.error().message();
  EXPECT_EQ(a->digest, b->digest) << "cap-off (fast) path must be deterministic";

  // A binding cap must route to the augmented path -> a DIFFERENT book.
  cfg.participation_cap = 0.05;
  cfg.set_flags.emplace("participation-cap");
  cfg.books_out = (tmp_dir_ / "books_capped.bin").string();
  auto c = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(c.has_value()) << c.error().message();
  EXPECT_NE(a->digest, c->digest)
      << "a binding participation cap must change the book (else the cap is inert)";
}

// (b) the cap genuinely BINDS the thin name, and the augmented book keeps its
// dollar-neutral / name-cap discipline (the S5-2 correctness trap).
TEST_F(StageOptimizeParticipationCap, ParticipationCap_BoundsThinAdvName) {
  constexpr usize M = 8, D = 60;
  const auto research = *make_thin_adv_research(tmp_dir_ / "research.bin", M, D);
  const auto combo = *make_thin_tilt_combo(tmp_dir_ / "combo.bin", M, D);

  // Uncapped baseline.
  auto cfg0 = base_cfg(research, combo);
  cfg0.books_out = (tmp_dir_ / "books_uncapped.bin").string();
  auto r0 = atx::impl::run_optimize(cfg0);
  ASSERT_TRUE(r0.has_value()) << r0.error().message();
  auto b0 = last_period_book(cfg0.books_out, M);
  ASSERT_TRUE(b0.has_value());

  // Capped.
  const f64 rho = 0.05;
  auto cfg1 = base_cfg(research, combo);
  cfg1.participation_cap = rho;
  cfg1.set_flags.emplace("participation-cap");
  cfg1.books_out = (tmp_dir_ / "books_capped.bin").string();
  auto r1 = atx::impl::run_optimize(cfg1);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  auto b1 = last_period_book(cfg1.books_out, M);
  ASSERT_TRUE(b1.has_value());

  // Closed-form bound: rho * H(=1) * adv * price / nav. adv = 20-day trailing mean
  // of "volume" (constant 1e3 here); price = close at the last date; nav = report_aum.
  auto research_panel = atx::impl::read_panel(research);
  ASSERT_TRUE(research_panel.has_value());
  const auto cls_fid = research_panel->field_id("close");
  ASSERT_TRUE(cls_fid.has_value());
  const usize last = research_panel->dates() - 1;
  const f64 price0 = research_panel->field_all(*cls_fid)[last * M + kThinName];
  const f64 adv0 = 1.0e3;
  const f64 nav = 1.0e7;
  const f64 bound = rho * 1.0 * adv0 * price0 / nav;

  const f64 w0_uncapped = std::fabs(b0->w[kThinName]);
  const f64 w0_capped = std::fabs(b1->w[kThinName]);

  // The thin name was genuinely loaded without the cap (else the test is vacuous).
  ASSERT_GT(w0_uncapped, bound * 5.0)
      << "vacuous: uncapped thin-name weight (" << w0_uncapped
      << ") is not materially above the participation bound (" << bound << ")";

  // (i) strictly lower with the cap, and (ii) at/under the closed-form bound (to
  // the augmented QP's ADMM feasibility tolerance -- the constrained solve satisfies
  // the box to a finite convergence tolerance, not machine epsilon, so a sub-0.1%
  // overshoot of the closed-form bound is convergence slack, not an unbound cap).
  EXPECT_LT(w0_capped, w0_uncapped)
      << "participation cap did not reduce the thin name's weight: uncapped=" << w0_uncapped
      << " capped=" << w0_capped;
  constexpr f64 kAdmmRelTol = 1e-3;
  EXPECT_LE(w0_capped, bound * (1.0 + kAdmmRelTol))
      << "capped thin-name weight " << w0_capped << " exceeds the %ADV bound " << bound
      << " by more than the ADMM feasibility tolerance";
  // The alpha WANTS more of the thin name than the cap allows, so the cap is the
  // ACTIVE binding constraint: the weight is pinned AT the participation ceiling
  // (not merely below it for some unrelated reason).
  EXPECT_GE(w0_capped, bound * (1.0 - 0.05))
      << "capped thin-name weight " << w0_capped << " is well below the %ADV bound " << bound
      << " -- the cap is not the binding constraint (vacuous)";

  // The augmented book must STILL be dollar-neutral and within name_cap (the trap:
  // attaching .part alone would silently drop gross/name-cap since the augmented
  // path does not read cfg.single).
  EXPECT_NEAR(b1->net, 0.0, 1e-6) << "capped book is not dollar-neutral (Σw=" << b1->net << ")";
  EXPECT_LE(b1->max_abs, cfg1.name_cap + 1e-6)
      << "capped book violates name_cap: max|w|=" << b1->max_abs;
}

// (c) twice-run byte-identity on the augmented (capped) path.
TEST_F(StageOptimizeParticipationCap, ParticipationCap_TwiceRunByteIdentical) {
  constexpr usize M = 8, D = 60;
  const auto research = *make_thin_adv_research(tmp_dir_ / "research.bin", M, D);
  const auto combo = *make_thin_tilt_combo(tmp_dir_ / "combo.bin", M, D);

  auto cfg = base_cfg(research, combo);
  cfg.participation_cap = 0.05;
  cfg.set_flags.emplace("participation-cap");

  cfg.books_out = (tmp_dir_ / "books_a.bin").string();
  auto r1 = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  cfg.books_out = (tmp_dir_ / "books_b.bin").string();
  auto r2 = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();
  EXPECT_EQ(r1->digest, r2->digest);
}

} // namespace atxtest_stage_optimize_participation_cap
