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

// read combo.bin bytes for a byte-identity check.
static std::vector<char> read_bytes(const std::string &path) {
  std::ifstream f{path, std::ios::binary};
  return std::vector<char>{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// ===========================================================================
// S5-2 (a): off-path byte-identity — kelly_fraction=0.0 (default) => digest
// unchanged from the no-Kelly baseline; the three kelly_* KVs are absent.
// ===========================================================================
TEST(AtxImplConvictionSizing, KellyOffIsByteIdentical) {
  namespace fs = std::filesystem;

  // (default field check) kelly_fraction / kelly_max_gross defaults.
  atx::impl::RunConfig def;
  EXPECT_EQ(def.kelly_fraction, 0.0);
  EXPECT_EQ(def.kelly_max_gross, 1.0);

  const Fixture fx = make_fixture("kelly_off");
  const std::string combo_base = (fs::temp_directory_path() / "atx_cvtsz_kelly_base.bin").string();
  const std::string combo_zero = (fs::temp_directory_path() / "atx_cvtsz_kelly_zero.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, combo_base);
  auto r_base = atx::impl::run_combine(cfg); // kelly_fraction stays 0.0 (default)
  ASSERT_TRUE(r_base.has_value()) << r_base.error().message();

  cfg.kelly_fraction = 0.0; // explicit off
  cfg.combo_out = combo_zero;
  auto r_zero = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r_zero.has_value()) << r_zero.error().message();

  EXPECT_EQ(r_base->digest, r_zero->digest) << "kelly_fraction 0 must be byte-identical to default";
  EXPECT_NE(r_base->digest, atx::u64{0});
  EXPECT_EQ(read_bytes(combo_base), read_bytes(combo_zero));

  EXPECT_FALSE(find_kv(*r_base, "kelly_fraction_used").has_value());
  EXPECT_FALSE(find_kv(*r_base, "kelly_gross").has_value());
  EXPECT_FALSE(find_kv(*r_base, "kelly_scale_applied").has_value());

  cleanup(fx, {combo_base, combo_zero});
}

// ===========================================================================
// S5-2 (b): on-path weights change — kelly_fraction=0.25 produces a DIFFERENT
// combined book than the no-Kelly run, and the kelly_* KVs are present.
// ===========================================================================
TEST(AtxImplConvictionSizing, KellyOnChangesWeightsAndEmitsKvs) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("kelly_on");
  const std::string combo_off = (fs::temp_directory_path() / "atx_cvtsz_kelly_on_off.bin").string();
  const std::string combo_on = (fs::temp_directory_path() / "atx_cvtsz_kelly_on_on.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, combo_off);
  auto r_off = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r_off.has_value()) << r_off.error().message();

  cfg.kelly_fraction = 0.25;
  cfg.combo_out = combo_on;
  auto r_on = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r_on.has_value()) << r_on.error().message();

  EXPECT_NE(r_on->digest, r_off->digest) << "kelly_fraction>0 must change the combined book";

  const auto frac = find_kv(*r_on, "kelly_fraction_used");
  const auto gross = find_kv(*r_on, "kelly_gross");
  const auto scale = find_kv(*r_on, "kelly_scale_applied");
  ASSERT_TRUE(frac.has_value());
  ASSERT_TRUE(gross.has_value());
  ASSERT_TRUE(scale.has_value());
  EXPECT_NEAR(std::stod(*frac), 0.25, 1e-9);

  cleanup(fx, {combo_off, combo_on});
}

// ===========================================================================
// S5-2 (c): gross is bounded — with kelly_max_gross=1.0 (default), kelly_gross <= 1
// always; and kelly_scale_applied < 1.0 exactly when the unclamped gross exceeded 1.
// ===========================================================================
TEST(AtxImplConvictionSizing, KellyGrossIsBounded) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("kelly_gross");
  const std::string combo = (fs::temp_directory_path() / "atx_cvtsz_kelly_gross.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, combo);
  cfg.kelly_fraction = 0.25;
  cfg.kelly_max_gross = 1.0;
  auto r = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r.has_value()) << r.error().message();

  const f64 gross = std::stod(*find_kv(*r, "kelly_gross"));
  const f64 scale = std::stod(*find_kv(*r, "kelly_scale_applied"));
  EXPECT_LE(gross, 1.0 + 1e-9) << "kelly_gross must respect max_gross=1.0";
  EXPECT_GT(gross, 0.0);
  // scale < 1 iff the clamp bound; scale == 1 iff slack. Either way scale in (0,1].
  EXPECT_GT(scale, 0.0);
  EXPECT_LE(scale, 1.0 + 1e-12);

  cleanup(fx, {combo});
}

// ===========================================================================
// S5-2 (d): kelly × conviction interaction — with both --conviction and
// --kelly-fraction, the Kelly weights reflect the per-alpha conviction scaling;
// the combined book differs from the kelly-only run (conviction modulates mu's
// per-name scale). Pins that the two knobs compose.
// ===========================================================================
TEST(AtxImplConvictionSizing, KellyWithConvictionComposes) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("kelly_cvt");
  const std::string combo_k = (fs::temp_directory_path() / "atx_cvtsz_kelly_only.bin").string();
  const std::string combo_kc = (fs::temp_directory_path() / "atx_cvtsz_kelly_cvt.bin").string();

  // Kelly only (no conviction).
  atx::impl::RunConfig cfg = base_cfg(fx, combo_k);
  cfg.kelly_fraction = 0.25;
  cfg.conviction = false;
  auto r_k = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r_k.has_value()) << r_k.error().message();

  // Kelly + conviction.
  cfg.conviction = true;
  cfg.combo_out = combo_kc;
  auto r_kc = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r_kc.has_value()) << r_kc.error().message();

  // Both KV families present in the combined run.
  EXPECT_TRUE(find_kv(*r_kc, "kelly_gross").has_value());
  EXPECT_TRUE(find_kv(*r_kc, "conviction_scores").has_value());
  // Conviction modulates the per-alpha conviction vector Kelly consumes, so the
  // combined book differs from the kelly-only book (the fixture alphas have
  // distinct conviction scores < 1).
  EXPECT_NE(r_k->digest, r_kc->digest)
      << "conviction must modulate the Kelly-sized book vs kelly-only";

  cleanup(fx, {combo_k, combo_kc});
}

// ===========================================================================
// S5-2 (e): twice-run — same inputs => byte-identical weights/digest and KVs.
// ===========================================================================
TEST(AtxImplConvictionSizing, KellyRunIsTwiceRunIdentical) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("kelly_twice");
  const std::string combo1 = (fs::temp_directory_path() / "atx_cvtsz_kelly_twice1.bin").string();
  const std::string combo2 = (fs::temp_directory_path() / "atx_cvtsz_kelly_twice2.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, combo1);
  cfg.kelly_fraction = 0.25;
  auto r1 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  cfg.combo_out = combo2;
  auto r2 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  EXPECT_EQ(r1->digest, r2->digest);
  EXPECT_EQ(read_bytes(combo1), read_bytes(combo2));
  for (const char *key : {"kelly_fraction_used", "kelly_gross", "kelly_scale_applied"}) {
    EXPECT_EQ(find_kv(*r1, key), find_kv(*r2, key)) << key << " must be deterministic";
  }

  cleanup(fx, {combo1, combo2});
}

// ===========================================================================
// S5-4 (a): off-path byte-identity (the MANDATORY class-a regression guard for the
// whole sprint). All knobs off (conviction=false, kelly_fraction=0, walk_forward=0):
// two runs => identical digests AND none of conviction_scores / kelly_gross /
// walk_forward_oos_sharpe present.
// ===========================================================================
TEST(AtxImplConvictionSizing, AllKnobsOffByteIdentical) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("s54_off");
  const std::string combo1 = (fs::temp_directory_path() / "atx_cvtsz_s54_off1.bin").string();
  const std::string combo2 = (fs::temp_directory_path() / "atx_cvtsz_s54_off2.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, combo1);
  cfg.conviction = false;
  cfg.kelly_fraction = 0.0;
  cfg.walk_forward = 0;
  auto r1 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  cfg.combo_out = combo2;
  auto r2 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  EXPECT_EQ(r1->digest, r2->digest);
  EXPECT_NE(r1->digest, atx::u64{0});
  EXPECT_EQ(read_bytes(combo1), read_bytes(combo2));

  EXPECT_FALSE(find_kv(*r1, "conviction_scores").has_value());
  EXPECT_FALSE(find_kv(*r1, "kelly_gross").has_value());
  EXPECT_FALSE(find_kv(*r1, "walk_forward_oos_sharpe").has_value());

  cleanup(fx, {combo1, combo2});
}

// ===========================================================================
// S5-4 (b): conviction-only — conviction_scores present, kelly_gross absent; the
// digest differs from the all-off baseline; twice-run identical.
// ===========================================================================
TEST(AtxImplConvictionSizing, ConvictionOnlyComposes) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("s54_cvt");
  const std::string base = (fs::temp_directory_path() / "atx_cvtsz_s54_cvt_base.bin").string();
  const std::string c1 = (fs::temp_directory_path() / "atx_cvtsz_s54_cvt1.bin").string();
  const std::string c2 = (fs::temp_directory_path() / "atx_cvtsz_s54_cvt2.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, base);
  auto r_base = atx::impl::run_combine(cfg); // all off
  ASSERT_TRUE(r_base.has_value()) << r_base.error().message();

  cfg.conviction = true;
  cfg.combo_out = c1;
  auto r1 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  cfg.combo_out = c2;
  auto r2 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  EXPECT_TRUE(find_kv(*r1, "conviction_scores").has_value());
  EXPECT_FALSE(find_kv(*r1, "kelly_gross").has_value());
  EXPECT_NE(r1->digest, r_base->digest) << "conviction must change the book vs all-off";
  EXPECT_EQ(r1->digest, r2->digest) << "conviction-only twice-run identical";

  cleanup(fx, {base, c1, c2});
}

// ===========================================================================
// S5-4 (c): kelly-only (no prior conviction) — kelly_gross present, conviction_scores
// absent; digest differs from baseline; twice-run identical.
// ===========================================================================
TEST(AtxImplConvictionSizing, KellyOnlyComposes) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("s54_kelly");
  const std::string base = (fs::temp_directory_path() / "atx_cvtsz_s54_kelly_base.bin").string();
  const std::string c1 = (fs::temp_directory_path() / "atx_cvtsz_s54_kelly1.bin").string();
  const std::string c2 = (fs::temp_directory_path() / "atx_cvtsz_s54_kelly2.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, base);
  auto r_base = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r_base.has_value()) << r_base.error().message();

  cfg.conviction = false;
  cfg.kelly_fraction = 0.25;
  cfg.combo_out = c1;
  auto r1 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  cfg.combo_out = c2;
  auto r2 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  EXPECT_TRUE(find_kv(*r1, "kelly_gross").has_value());
  EXPECT_FALSE(find_kv(*r1, "conviction_scores").has_value());
  EXPECT_NE(r1->digest, r_base->digest) << "kelly must change the book vs all-off";
  EXPECT_EQ(r1->digest, r2->digest) << "kelly-only twice-run identical";

  cleanup(fx, {base, c1, c2});
}

// ===========================================================================
// S5-4 (d): combined — conviction=true, kelly_fraction=0.25 => both KV families
// present; kelly_gross <= 1.0 (default max_gross); twice-run identical.
// ===========================================================================
TEST(AtxImplConvictionSizing, CombinedKnobsComposeBounded) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("s54_both");
  const std::string c1 = (fs::temp_directory_path() / "atx_cvtsz_s54_both1.bin").string();
  const std::string c2 = (fs::temp_directory_path() / "atx_cvtsz_s54_both2.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, c1);
  cfg.conviction = true;
  cfg.kelly_fraction = 0.25;
  auto r1 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  cfg.combo_out = c2;
  auto r2 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  EXPECT_TRUE(find_kv(*r1, "conviction_scores").has_value());
  ASSERT_TRUE(find_kv(*r1, "kelly_gross").has_value());
  EXPECT_LE(std::stod(*find_kv(*r1, "kelly_gross")), 1.0 + 1e-9);
  EXPECT_EQ(r1->digest, r2->digest) << "combined twice-run identical";

  cleanup(fx, {c1, c2});
}

// ===========================================================================
// S5-4 (e): WF + conviction — conviction=true, walk_forward=2 =>
// walk_forward_oos_sharpe KV present and finite; twice-run identical. Validates the
// T7 NEW-1 path end-to-end in atx-impl.
// ===========================================================================
TEST(AtxImplConvictionSizing, WalkForwardWithConvictionEmitsFiniteSharpe) {
  namespace fs = std::filesystem;
  const Fixture fx = make_fixture("s54_wf");
  const std::string c1 = (fs::temp_directory_path() / "atx_cvtsz_s54_wf1.bin").string();
  const std::string c2 = (fs::temp_directory_path() / "atx_cvtsz_s54_wf2.bin").string();

  atx::impl::RunConfig cfg = base_cfg(fx, c1);
  cfg.conviction = true;
  cfg.walk_forward = 2;
  auto r1 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  cfg.combo_out = c2;
  auto r2 = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  const auto wf = find_kv(*r1, "walk_forward_oos_sharpe");
  ASSERT_TRUE(wf.has_value()) << "walk_forward_oos_sharpe must be present with --walk-forward";
  const auto folds = parse_csv(*wf);
  EXPECT_EQ(folds.size(), 2u) << "2 folds -> 2 per-fold Sharpes";
  for (const f64 s : folds) {
    EXPECT_TRUE(std::isfinite(s)) << "each fold OOS Sharpe must be finite";
  }
  const auto mean = find_kv(*r1, "walk_forward_oos_sharpe_mean");
  ASSERT_TRUE(mean.has_value());
  EXPECT_TRUE(std::isfinite(std::stod(*mean)));
  EXPECT_EQ(r1->digest, r2->digest) << "WF+conviction twice-run identical";

  cleanup(fx, {c1, c2});
}

} // namespace atxtest_conviction_sizing
