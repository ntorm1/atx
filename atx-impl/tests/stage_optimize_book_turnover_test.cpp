// stage_optimize_book_turnover_test.cpp — p9 Sprint 5 (S5-1): the book-level,
// per-day, cross-sleeve-netted turnover RATE — measured unconditionally on
// run_optimize, then gated opt-in via --book-turnover-gate.
//
// Suite: StageOptimizeBookTurnover
//
// Four determinism/behavior classes (S5-1 Accept):
//   (a) BookTurnoverGate_OffPathByteIdentical — cfg.book_turnover_gate == 0.0
//       (default) produces a books.bin digest + raw bytes IDENTICAL to a run with
//       a LOOSE (non-binding) gate and to itself run twice: the always-on measure
//       kv changes sr.kvs only, never the book bytes (the digest is set before the
//       kv is appended).
//   (b) BookTurnoverGate_RedGreen — two fixtures deliberately at the extremes: a
//       CONSTANT combo (book barely moves rebalance-to-rebalance -> low turnover)
//       and a SIGN-FLIPPING combo (book inverts every rebalance -> near-maximal
//       turnover). The measured book_turnover_per_day must ORDER them correctly
//       (low < high — the metric's sign/direction pin). With the gate set BETWEEN
//       the two measured rates, the low fixture returns Ok (kv < gate) and the
//       high fixture returns Err (fail-closed).
//   (c) BookTurnoverGate_TwiceRun — same cfg+panel twice -> identical
//       book_turnover_per_day kv string AND identical accept/reject outcome.
//   (d) N/A: book_turnover_per_day is a pure, order-fixed reduction over an
//       already-proven-deterministic MultiPeriodResult.turnover (no parallel_for
//       touches run_optimize). Stated here rather than silently omitted.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"

namespace atxtest_stage_optimize_book_turnover {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
using atx::f64;
using atx::usize;

// A gently-trending research panel (close only; all in-universe).
static atx::core::Result<std::string> make_trend_research(const fs::path& out, usize M, usize D) {
  std::vector<f64> close;
  close.reserve(D * M);
  for (usize t = 0; t < D; ++t) {
    for (usize i = 0; i < M; ++i) {
      const f64 drift = 0.0002 * (1.0 + static_cast<f64>(i) * 0.1);
      close.push_back(100.0 * std::exp(drift * static_cast<f64>(t)));
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"close"}, {close}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

// LOW-turnover combo: a fixed long-first-half / short-second-half alpha, CONSTANT
// across every date -> the optimized book is (near) constant period-to-period.
static atx::core::Result<std::string> make_constant_combo(const fs::path& out, usize M, usize D) {
  std::vector<f64> a;
  a.reserve(D * M);
  const usize half = M / 2;
  for (usize t = 0; t < D; ++t) {
    (void)t;
    for (usize i = 0; i < M; ++i) a.push_back(i < half ? 1.0 : -1.0);
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"alpha"}, {a}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

// HIGH-turnover combo: the SAME long/short pattern but its sign FLIPS every weekly
// (step=5) rebalance period, so the book inverts rebalance-to-rebalance.
static atx::core::Result<std::string> make_flipping_combo(const fs::path& out, usize M, usize D,
                                                          usize step) {
  std::vector<f64> a;
  a.reserve(D * M);
  const usize half = M / 2;
  for (usize t = 0; t < D; ++t) {
    const f64 sgn = (((t / step) % 2U) == 0U) ? 1.0 : -1.0;
    for (usize i = 0; i < M; ++i) a.push_back(sgn * (i < half ? 1.0 : -1.0));
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"alpha"}, {a}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

static std::string find_kv(const atx::impl::StageResult& sr, const std::string& key) {
  for (const auto& [k, v] : sr.kvs) if (k == key) return v;
  return "<missing:" + key + ">";
}

static std::vector<char> read_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

class StageOptimizeBookTurnover : public ::testing::Test {
protected:
  fs::path tmp_dir_;
  void SetUp() override {
    tmp_dir_ = fs::temp_directory_path() / "atx_s5_1_optimize_book_turnover";
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
    return cfg;
  }
};

// (a) off-path byte-identity: default gate (0.0) vs a loose non-binding gate, and
// twice-run, all produce the identical books digest + raw bytes.
TEST_F(StageOptimizeBookTurnover, BookTurnoverGate_OffPathByteIdentical) {
  constexpr usize M = 12, D = 60;
  const auto research = *make_trend_research(tmp_dir_ / "research.bin", M, D);
  const auto combo = *make_constant_combo(tmp_dir_ / "combo.bin", M, D);

  auto cfg = base_cfg(research, combo);
  cfg.books_out = (tmp_dir_ / "books_gate0.bin").string();
  auto r0 = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(r0.has_value()) << r0.error().message();

  // A loose gate that cannot bind (100/day is far above any realized rate): the
  // book bytes must be IDENTICAL to the gate-off run — the gate/measure path never
  // perturbs book construction.
  cfg.book_turnover_gate = 100.0;
  cfg.set_flags.emplace("book-turnover-gate");
  cfg.books_out = (tmp_dir_ / "books_loose.bin").string();
  auto rl = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(rl.has_value()) << rl.error().message();

  EXPECT_EQ(r0->digest, rl->digest)
      << "book_turnover_gate must not perturb the books digest (measure-only path)";
  EXPECT_EQ(read_bytes((tmp_dir_ / "books_gate0.bin").string()),
            read_bytes((tmp_dir_ / "books_loose.bin").string()))
      << "books.bin not byte-identical between gate-off and loose-gate runs";

  // The rate is surfaced unconditionally, even at the default (gate off).
  EXPECT_NE(find_kv(*r0, "book_turnover_per_day"), "<missing:book_turnover_per_day>")
      << "book_turnover_per_day must be surfaced as a kv even when the gate is off";
}

// (b) RED->GREEN: the metric orders the two extreme books, and the gate bites.
TEST_F(StageOptimizeBookTurnover, BookTurnoverGate_RedGreen) {
  constexpr usize M = 12, D = 80;
  constexpr usize step = 5; // weekly
  const auto research = *make_trend_research(tmp_dir_ / "research.bin", M, D);
  const auto lo_combo = *make_constant_combo(tmp_dir_ / "combo_lo.bin", M, D);
  const auto hi_combo = *make_flipping_combo(tmp_dir_ / "combo_hi.bin", M, D, step);

  // Measure both rates with the gate OFF (0.0).
  auto cfg_lo = base_cfg(research, lo_combo);
  cfg_lo.books_out = (tmp_dir_ / "books_lo.bin").string();
  auto rlo = atx::impl::run_optimize(cfg_lo);
  ASSERT_TRUE(rlo.has_value()) << rlo.error().message();
  const f64 rate_lo = std::stod(find_kv(*rlo, "book_turnover_per_day"));

  auto cfg_hi = base_cfg(research, hi_combo);
  cfg_hi.books_out = (tmp_dir_ / "books_hi.bin").string();
  auto rhi = atx::impl::run_optimize(cfg_hi);
  ASSERT_TRUE(rhi.has_value()) << rhi.error().message();
  const f64 rate_hi = std::stod(find_kv(*rhi, "book_turnover_per_day"));

  // The metric must ORDER the two extremes correctly (sign/direction pin).
  ASSERT_GT(rate_hi, rate_lo * 1.5)
      << "flipping book must have a strictly (much) higher per-day turnover than the "
         "constant book: rate_lo=" << rate_lo << " rate_hi=" << rate_hi;
  ASSERT_GT(rate_lo, 0.0) << "vacuous: constant book produced zero turnover";

  // Gate set strictly between the two measured rates.
  const f64 gate = 0.5 * (rate_lo + rate_hi);

  auto cfg_lo_g = base_cfg(research, lo_combo);
  cfg_lo_g.books_out = (tmp_dir_ / "books_lo_g.bin").string();
  cfg_lo_g.book_turnover_gate = gate;
  cfg_lo_g.set_flags.emplace("book-turnover-gate");
  auto rlo_g = atx::impl::run_optimize(cfg_lo_g);
  ASSERT_TRUE(rlo_g.has_value())
      << "low-turnover book must PASS a gate above its rate: " << rlo_g.error().message();
  EXPECT_LT(std::stod(find_kv(*rlo_g, "book_turnover_per_day")), gate);

  auto cfg_hi_g = base_cfg(research, hi_combo);
  cfg_hi_g.books_out = (tmp_dir_ / "books_hi_g.bin").string();
  cfg_hi_g.book_turnover_gate = gate;
  cfg_hi_g.set_flags.emplace("book-turnover-gate");
  auto rhi_g = atx::impl::run_optimize(cfg_hi_g);
  EXPECT_FALSE(rhi_g.has_value())
      << "high-turnover book must be REJECTED by a gate below its rate (" << rate_hi << ")";
}

// (c) twice-run: identical rate kv string + identical accept/reject outcome.
TEST_F(StageOptimizeBookTurnover, BookTurnoverGate_TwiceRun) {
  constexpr usize M = 12, D = 80;
  const auto research = *make_trend_research(tmp_dir_ / "research.bin", M, D);
  const auto combo = *make_constant_combo(tmp_dir_ / "combo.bin", M, D);

  auto cfg = base_cfg(research, combo);
  cfg.book_turnover_gate = 0.10;
  cfg.set_flags.emplace("book-turnover-gate");

  cfg.books_out = (tmp_dir_ / "books_a.bin").string();
  auto r1 = atx::impl::run_optimize(cfg);
  cfg.books_out = (tmp_dir_ / "books_b.bin").string();
  auto r2 = atx::impl::run_optimize(cfg);

  ASSERT_EQ(r1.has_value(), r2.has_value()) << "accept/reject outcome differs across runs";
  if (r1.has_value()) {
    EXPECT_EQ(find_kv(*r1, "book_turnover_per_day"), find_kv(*r2, "book_turnover_per_day"));
    EXPECT_EQ(r1->digest, r2->digest);
  }
}

} // namespace atxtest_stage_optimize_book_turnover
