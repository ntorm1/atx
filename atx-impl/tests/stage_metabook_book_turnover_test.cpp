// stage_metabook_book_turnover_test.cpp — p9 Sprint 5 (S5-1): the fund-book,
// per-day, cross-sleeve-netted turnover RATE — measured unconditionally on
// run_metabook (from result.report.turnover_net, the S2-3 fund-netted series),
// then gated opt-in via --book-turnover-gate.
//
// Suite: StageMetabookBookTurnover
//
// This exercises the EXACT new gate/measure code in run_metabook. It drives the
// deterministic SingleSleeve stage path (no --library-dir) with combo panels that
// control the fund book's period-to-period movement. The cross-sleeve NETTING of
// result.report.turnover_net itself (net < gross under crossing) is proven
// separately and thoroughly in metabook_netting_test.cpp (ReducesTurnoverOnOffsetting
// Sleeves, MultiSleeveByCorrClusterReachesTheStageEndToEnd); book_turnover_per_day is
// a sleeve-count-agnostic reduction over whatever turnover_net the frozen MetaBook
// driver produces, so SingleSleeve fully exercises the S5-1 wiring under test.
//
//   (a) BookTurnoverGate_OffPathByteIdentical — gate 0.0 vs a loose non-binding gate
//       vs twice-run: identical fund-book digest + raw bytes (measure-only path).
//   (b) BookTurnoverGate_RedGreen — a CONSTANT combo (stable fund book -> low
//       turnover) vs a SIGN-FLIPPING combo (inverting fund book -> high turnover):
//       the measured rate orders them (low < high), and a gate set between them
//       admits the low fixture and rejects the high one (fail-closed).
//   (c) BookTurnoverGate_TwiceRun — identical rate kv + identical accept/reject twice.
//   (d) N/A: pure order-fixed reduction over the frozen MetaBook driver's already-
//       deterministic turnover_net (no parallel path in run_metabook).

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stage_metabook.hpp"

#include "atx/engine/alpha/panel.hpp"

namespace atxtest_stage_metabook_book_turnover {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
using atx::f64;
using atx::usize;

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

class StageMetabookBookTurnover : public ::testing::Test {
protected:
  fs::path tmp_dir_;
  void SetUp() override {
    tmp_dir_ = fs::temp_directory_path() / "atx_s5_1_metabook_book_turnover";
    fs::create_directories(tmp_dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_dir_, ec);
  }
  atx::impl::RunConfig base_cfg(const std::string& research, const std::string& combo,
                                const std::string& books_out) {
    atx::impl::RunConfig cfg;
    cfg.panel = research;
    cfg.combo = combo;
    cfg.gross = 1.0;
    cfg.name_cap = 1.0;
    cfg.rebalance = "weekly";
    cfg.books_out = books_out;
    return cfg;
  }
};

TEST_F(StageMetabookBookTurnover, BookTurnoverGate_OffPathByteIdentical) {
  constexpr usize M = 6, D = 40;
  const auto research = *make_trend_research(tmp_dir_ / "research.bin", M, D);
  const auto combo = *make_constant_combo(tmp_dir_ / "combo.bin", M, D);
  const atx::impl::MetaBookStageConfig scfg; // SingleSleeve, no library

  auto cfg0 = base_cfg(research, combo, (tmp_dir_ / "books_gate0.bin").string());
  auto r0 = atx::impl::run_metabook(cfg0, scfg);
  ASSERT_TRUE(r0.has_value()) << r0.error().message();

  auto cfgl = base_cfg(research, combo, (tmp_dir_ / "books_loose.bin").string());
  cfgl.book_turnover_gate = 100.0;
  cfgl.set_flags.emplace("book-turnover-gate");
  auto rl = atx::impl::run_metabook(cfgl, scfg);
  ASSERT_TRUE(rl.has_value()) << rl.error().message();

  EXPECT_EQ(r0->digest, rl->digest)
      << "book_turnover_gate must not perturb the fund-book digest (measure-only path)";
  EXPECT_EQ(read_bytes((tmp_dir_ / "books_gate0.bin").string()),
            read_bytes((tmp_dir_ / "books_loose.bin").string()))
      << "fund books.bin not byte-identical between gate-off and loose-gate runs";
  EXPECT_NE(find_kv(*r0, "book_turnover_per_day"), "<missing:book_turnover_per_day>")
      << "book_turnover_per_day must be surfaced even when the gate is off";
}

TEST_F(StageMetabookBookTurnover, BookTurnoverGate_RedGreen) {
  constexpr usize M = 6, D = 60;
  constexpr usize step = 5; // weekly
  const auto research = *make_trend_research(tmp_dir_ / "research.bin", M, D);
  const auto lo_combo = *make_constant_combo(tmp_dir_ / "combo_lo.bin", M, D);
  const auto hi_combo = *make_flipping_combo(tmp_dir_ / "combo_hi.bin", M, D, step);
  const atx::impl::MetaBookStageConfig scfg;

  auto cfg_lo = base_cfg(research, lo_combo, (tmp_dir_ / "books_lo.bin").string());
  auto rlo = atx::impl::run_metabook(cfg_lo, scfg);
  ASSERT_TRUE(rlo.has_value()) << rlo.error().message();
  const f64 rate_lo = std::stod(find_kv(*rlo, "book_turnover_per_day"));

  auto cfg_hi = base_cfg(research, hi_combo, (tmp_dir_ / "books_hi.bin").string());
  auto rhi = atx::impl::run_metabook(cfg_hi, scfg);
  ASSERT_TRUE(rhi.has_value()) << rhi.error().message();
  const f64 rate_hi = std::stod(find_kv(*rhi, "book_turnover_per_day"));

  ASSERT_GT(rate_hi, rate_lo * 1.5)
      << "flipping fund book must have a strictly higher per-day turnover than the constant "
         "book: rate_lo=" << rate_lo << " rate_hi=" << rate_hi;
  ASSERT_GT(rate_lo, 0.0) << "vacuous: constant fund book produced zero turnover";

  const f64 gate = 0.5 * (rate_lo + rate_hi);

  auto cfg_lo_g = base_cfg(research, lo_combo, (tmp_dir_ / "books_lo_g.bin").string());
  cfg_lo_g.book_turnover_gate = gate;
  cfg_lo_g.set_flags.emplace("book-turnover-gate");
  auto rlo_g = atx::impl::run_metabook(cfg_lo_g, scfg);
  ASSERT_TRUE(rlo_g.has_value())
      << "low-turnover fund book must PASS a gate above its rate: " << rlo_g.error().message();

  auto cfg_hi_g = base_cfg(research, hi_combo, (tmp_dir_ / "books_hi_g.bin").string());
  cfg_hi_g.book_turnover_gate = gate;
  cfg_hi_g.set_flags.emplace("book-turnover-gate");
  auto rhi_g = atx::impl::run_metabook(cfg_hi_g, scfg);
  EXPECT_FALSE(rhi_g.has_value())
      << "high-turnover fund book must be REJECTED by a gate below its rate (" << rate_hi << ")";
}

TEST_F(StageMetabookBookTurnover, BookTurnoverGate_TwiceRun) {
  constexpr usize M = 6, D = 60;
  const auto research = *make_trend_research(tmp_dir_ / "research.bin", M, D);
  const auto combo = *make_constant_combo(tmp_dir_ / "combo.bin", M, D);
  const atx::impl::MetaBookStageConfig scfg;

  auto cfg = base_cfg(research, combo, (tmp_dir_ / "books_a.bin").string());
  cfg.book_turnover_gate = 0.05;
  cfg.set_flags.emplace("book-turnover-gate");
  auto r1 = atx::impl::run_metabook(cfg, scfg);

  cfg.books_out = (tmp_dir_ / "books_b.bin").string();
  auto r2 = atx::impl::run_metabook(cfg, scfg);

  ASSERT_EQ(r1.has_value(), r2.has_value()) << "accept/reject outcome differs across runs";
  if (r1.has_value()) {
    EXPECT_EQ(find_kv(*r1, "book_turnover_per_day"), find_kv(*r2, "book_turnover_per_day"));
    EXPECT_EQ(r1->digest, r2->digest);
  }
}

} // namespace atxtest_stage_metabook_book_turnover
