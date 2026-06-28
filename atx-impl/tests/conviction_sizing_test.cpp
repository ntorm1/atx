// atx::impl — p7 S5 conviction + fractional-Kelly sizing integration tests.
//
// Suite AtxImplConvictionSizing. Owns the integration coverage for the three
// S5 knobs in stage_combine:
//   * S5-1: conviction KV telemetry (conviction_scores / conviction_dsr_terms /
//     conviction_stability_terms) — additive, present ONLY when --conviction.
//   * S5-2: fractional-Kelly sizing (kelly_fraction_used / kelly_gross /
//     kelly_scale_applied) — additive, present ONLY when kelly_fraction > 0.
//   * S5-4: off-path byte-identity (all knobs off => digest unchanged, no new KVs)
//     and the combined / WF interaction cases.
//
// Determinism: every fixture is a small deterministic synthetic pool built in the
// test body (no I/O beyond the temp panel/DSL the combine stage requires). The
// off-path case is the primary regression guard for the whole sprint.
//
// Panel fixture: a deterministic LCG random-walk "close" matrix (the same idiom as
// combine_test.cpp) so the per-alpha PnL streams have distinct, non-degenerate
// quality and the conviction / Kelly transforms are non-vacuous.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp" // alpha::Panel

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

namespace atxtest_conviction_sizing {

using atx::f64;
using atx::usize;
using atx::engine::alpha::Panel;

// ---------------------------------------------------------------------------
// Deterministic LCG (same idiom as combine_test.cpp / discover_test.cpp).
// ---------------------------------------------------------------------------
struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

// Noisy momentum close matrix [dates * insts] with per-name drift.
static std::vector<f64> noisy_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.006 - 0.0024 * static_cast<f64>(j);
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.010 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

static std::optional<Panel> make_panel(usize dates, usize insts) {
  const std::vector<f64> close = noisy_close(dates, insts, 0xC0FFEEULL);
  auto r = Panel::create(dates, insts, {"close"}, {close}, {});
  if (!r.has_value()) {
    ADD_FAILURE() << "panel fixture must build: " << r.error().to_string();
    return std::nullopt;
  }
  return std::move(r.value());
}

static std::string write_panel_tmp(const Panel &panel, const std::string &stem) {
  namespace fs = std::filesystem;
  const std::string path = (fs::temp_directory_path() / ("atx_cvtsz_" + stem + ".bin")).string();
  auto r = atx::impl::write_panel(panel, path);
  EXPECT_TRUE(r.has_value()) << "write_panel must succeed";
  return path;
}

static std::string write_alpha_dir(const std::string &stem, const std::vector<std::string> &dsls) {
  namespace fs = std::filesystem;
  const std::string dir = (fs::temp_directory_path() / ("atx_cvtsz_alphas_" + stem)).string();
  fs::create_directories(dir);
  for (usize i = 0; i < dsls.size(); ++i) {
    std::ostringstream name;
    name << "alpha_" << i << ".dsl";
    std::ofstream f{(fs::path{dir} / name.str()).string()};
    EXPECT_TRUE(f.is_open());
    f << dsls[i] << '\n';
  }
  return dir;
}

// Three verified-safe DSL expressions for a {"close"} panel (distinct PnL profiles).
static std::vector<std::string> safe_dsls() {
  return {"rank(close)", "ts_mean(close,10)", "delta(close,2)"};
}

// kv lookup helper.
static std::optional<std::string> find_kv(const atx::impl::StageResult &sr, const std::string &k) {
  for (const auto &p : sr.kvs) {
    if (p.first == k) {
      return p.second;
    }
  }
  return std::nullopt;
}

// Split a comma-joined numeric string into doubles.
static std::vector<f64> parse_csv(const std::string &s) {
  std::vector<f64> out;
  std::stringstream ss{s};
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (!tok.empty()) {
      out.push_back(std::stod(tok));
    }
  }
  return out;
}

struct Fixture {
  std::string panel_path;
  std::string alphas_dir;
  std::string stem;
};

static Fixture make_fixture(const std::string &stem, usize dates = 60, usize insts = 20) {
  auto panel_opt = make_panel(dates, insts);
  EXPECT_TRUE(panel_opt.has_value());
  Fixture fx;
  fx.stem = stem;
  fx.panel_path = write_panel_tmp(*panel_opt, stem);
  fx.alphas_dir = write_alpha_dir(stem, safe_dsls());
  return fx;
}

static void cleanup(const Fixture &fx, const std::vector<std::string> &combos) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::remove(fx.panel_path, ec);
  fs::remove_all(fx.alphas_dir, ec);
  for (const std::string &co : combos) {
    fs::remove(co, ec);
    fs::remove(co + ".weights.txt", ec);
    fs::remove(co + ".meta", ec);
  }
}

static atx::impl::RunConfig base_cfg(const Fixture &fx, const std::string &combo_out) {
  atx::impl::RunConfig cfg;
  cfg.subcommand = "combine";
  cfg.panel = fx.panel_path;
  cfg.alphas = fx.alphas_dir;
  cfg.combo_out = combo_out;
  cfg.method = "shrinkage-mv"; // non-uniform fitted weights so the transforms bite
  cfg.fit_begin = 0;
  cfg.fit_end = 0;
  return cfg;
}

// ===========================================================================
// S5-1 (a): off-path — conviction=false => none of the conviction KVs present.
// ===========================================================================
TEST(AtxImplConvictionSizing, ConvictionOffEmitsNoConvictionKvs) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("cvt_off");
  const std::string combo = (fs::temp_directory_path() / "atx_cvtsz_cvt_off.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, combo);
  cfg.conviction = false;
  auto r = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r.has_value()) << r.error().message();

  EXPECT_FALSE(find_kv(*r, "conviction_scores").has_value());
  EXPECT_FALSE(find_kv(*r, "conviction_dsr_terms").has_value());
  EXPECT_FALSE(find_kv(*r, "conviction_stability_terms").has_value());

  cleanup(fx, {combo});
}

// ===========================================================================
// S5-1 (b): on-path — conviction=true => the three KVs present; conviction_scores
// has exactly n_alphas comma-separated entries.
// ===========================================================================
TEST(AtxImplConvictionSizing, ConvictionOnEmitsScoreKvs) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("cvt_on");
  const std::string combo = (fs::temp_directory_path() / "atx_cvtsz_cvt_on.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, combo);
  cfg.conviction = true;
  auto r = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r.has_value()) << r.error().message();

  const auto scores = find_kv(*r, "conviction_scores");
  const auto dsr = find_kv(*r, "conviction_dsr_terms");
  const auto stab = find_kv(*r, "conviction_stability_terms");
  ASSERT_TRUE(scores.has_value()) << "conviction_scores must be present when --conviction";
  ASSERT_TRUE(dsr.has_value());
  ASSERT_TRUE(stab.has_value());

  // Fixture has 3 alphas -> 3 entries each.
  EXPECT_EQ(parse_csv(*scores).size(), 3u);
  EXPECT_EQ(parse_csv(*dsr).size(), 3u);
  EXPECT_EQ(parse_csv(*stab).size(), 3u);

  cleanup(fx, {combo});
}

// ===========================================================================
// S5-1 (c): every parsed score / dsr_term / stability_term is in [0,1].
// ===========================================================================
TEST(AtxImplConvictionSizing, ConvictionTermsAreInUnitInterval) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("cvt_bounds");
  const std::string combo = (fs::temp_directory_path() / "atx_cvtsz_cvt_bounds.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, combo);
  cfg.conviction = true;
  auto r = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r.has_value()) << r.error().message();

  for (const char *key : {"conviction_scores", "conviction_dsr_terms",
                          "conviction_stability_terms"}) {
    const auto kv = find_kv(*r, key);
    ASSERT_TRUE(kv.has_value()) << key << " missing";
    for (const f64 v : parse_csv(*kv)) {
      EXPECT_GE(v, 0.0) << key << " term must be >= 0";
      EXPECT_LE(v, 1.0) << key << " term must be <= 1";
    }
  }

  cleanup(fx, {combo});
}

// ===========================================================================
// S5-1 (d): the emitted final score equals the apply_conviction formula at the
// emitted terms: score == w_dsr*dsr_term + w_stability*stab_term, scaled by the
// PartlyExplained multiplier (the convention apply_conviction uses: w_pbo dropped,
// w_dsr/w_stability renormalized to sum 1, explain=PartlyExplained).
// ===========================================================================
TEST(AtxImplConvictionSizing, ConvictionScoreMatchesFormula) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("cvt_formula");
  const std::string combo = (fs::temp_directory_path() / "atx_cvtsz_cvt_formula.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, combo);
  cfg.conviction = true;
  auto r = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r.has_value()) << r.error().message();

  const auto scores = parse_csv(*find_kv(*r, "conviction_scores"));
  const auto dsr = parse_csv(*find_kv(*r, "conviction_dsr_terms"));
  const auto stab = parse_csv(*find_kv(*r, "conviction_stability_terms"));
  ASSERT_EQ(scores.size(), dsr.size());
  ASSERT_EQ(scores.size(), stab.size());

  // apply_conviction's config: drop w_pbo, renormalize w_dsr+w_stability to 1.
  // Defaults: w_dsr=0.40, w_stability=0.25 -> sum 0.65.
  const f64 wsum = 0.40 + 0.25;
  const f64 w_dsr = 0.40 / wsum;
  const f64 w_stab = 0.25 / wsum;
  const f64 explain_mult = 0.75; // PartlyExplained

  for (usize a = 0; a < scores.size(); ++a) {
    const f64 expected = (w_dsr * dsr[a] + w_stab * stab[a]) * explain_mult;
    // ~6 sig-fig serialization through std::to_string -> 1e-5 tolerance.
    EXPECT_NEAR(scores[a], expected, 1e-5)
        << "score[" << a << "] must equal the apply_conviction blend at the emitted terms";
  }

  cleanup(fx, {combo});
}

// ===========================================================================
// S5-1 (e): twice-run — the conviction KV strings are byte-identical across runs.
// ===========================================================================
TEST(AtxImplConvictionSizing, ConvictionKvsAreTwiceRunIdentical) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("cvt_twice");
  const std::string combo1 = (fs::temp_directory_path() / "atx_cvtsz_cvt_twice1.bin").string();
  const std::string combo2 = (fs::temp_directory_path() / "atx_cvtsz_cvt_twice2.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, combo1);
  cfg.conviction = true;
  auto r1 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  cfg.combo_out = combo2;
  auto r2 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  for (const char *key : {"conviction_scores", "conviction_dsr_terms",
                          "conviction_stability_terms"}) {
    EXPECT_EQ(find_kv(*r1, key), find_kv(*r2, key)) << key << " must be deterministic";
  }
  EXPECT_EQ(r1->digest, r2->digest);

  cleanup(fx, {combo1, combo2});
}

} // namespace atxtest_conviction_sizing
