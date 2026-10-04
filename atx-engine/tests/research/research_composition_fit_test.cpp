// P9 lane D2: atx-engine-research-composition -- the composition rules relocated from atx-impl and
// the fit verb on a K-P9-4 factor series (fit.hpp, composition_cli.hpp, ew_theme_std.hpp).
//
// ResearchComposition.FitEqualsPythonFixture is the acceptance contract: the committed fixture
// (fixtures/research_composition/fit_v1.json) holds synthetic inputs and the values today's Python
// fit side computes on them (atx-impl/tools/test_composition_fit_fixture.py runs composition_rules,
// composition_ic_shrink, composition_theme_erc, composition_theme_tsmom and composition_resid
// unchanged); every "exact" case is reproduced bit for bit, the one non-exact case (BLAS order of
// the covariance's inner products) within 1e-12 x max(1, |x|).
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/combine/group_shrink.hpp"
#include "atx/engine/research/composition/composition_cli.hpp"
#include "atx/engine/research/composition/ew_theme_std.hpp"
#include "atx/engine/research/composition/fit.hpp"
#include "atx/engine/research/composition/theme_erc.hpp"
#include "atx/engine/research/composition/weights_schema.hpp"

namespace {

namespace fs = std::filesystem;
namespace rc = atx::engine::research::composition;
namespace cb = atx::engine::combine;
using Json = nlohmann::json;
using atx::f64;
using atx::i64;
using atx::u64;
using atx::u8;
using atx::usize;

std::string read_bytes(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_bytes(const fs::path &path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

const Json &fixture() {
  static const Json doc =
      Json::parse(read_bytes(fs::path(ATX_RESEARCH_COMPOSITION_FIXTURE) / "fit_v1.json"));
  return doc;
}

// A fresh directory under the process-isolated temp path (atx-test-scratch).
fs::path scratch(std::string_view name) {
  const fs::path dir =
      fs::temp_directory_path() / ("atx_research_composition_" + std::string(name));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

// The fixture's inputs, owned.
struct Inputs {
  std::vector<std::string> ids;
  usize decisions{};
  std::vector<f64> factors; // decisions x candidates
  std::vector<i64> sessions;
  std::vector<u8> train;
  [[nodiscard]] rc::FactorSeriesView view() const {
    return rc::FactorSeriesView{ids.size(), decisions, factors, sessions, train};
  }
};

Inputs inputs() {
  const Json &in = fixture().at("inputs");
  Inputs out;
  out.ids = in.at("ids").get<std::vector<std::string>>();
  out.decisions = in.at("decisions").get<usize>();
  for (const Json &row : in.at("factors")) {
    for (const Json &v : row) {
      out.factors.push_back(v.is_null() ? std::numeric_limits<f64>::quiet_NaN() : v.get<f64>());
    }
  }
  out.sessions = in.at("sessions").get<std::vector<i64>>();
  for (const Json &v : in.at("train")) {
    out.train.push_back(static_cast<u8>(v.get<int>()));
  }
  return out;
}

// One fitted value against the Python's: bit for bit, or within 1e-12 x max(1, |x|).
void expect_value(const Json &expected, f64 actual, bool exact, const std::string &where) {
  const f64 want = expected.get<f64>();
  if (exact) {
    EXPECT_EQ(std::bit_cast<u64>(want), std::bit_cast<u64>(actual))
        << where << ": python " << want << " c++ " << actual;
  } else {
    EXPECT_LE(std::abs(want - actual), 1e-12 * std::max(1.0, std::abs(want)))
        << where << ": python " << want << " c++ " << actual;
  }
}

void expect_values(const Json &expected, const std::vector<f64> &actual, bool exact,
                   const std::string &where) {
  ASSERT_EQ(expected.size(), actual.size()) << where;
  for (usize i = 0; i < actual.size(); ++i) {
    expect_value(expected.at(i), actual[i], exact, where + "[" + std::to_string(i) + "]");
  }
}

// composition_rules.member_cap's passes: {capped: [id, ...], excess_by_theme: {theme: x}}.
void expect_passes(const Json &expected, const std::vector<cb::GroupCapPass> &actual,
                   const std::vector<std::string> &members, const std::vector<std::string> &themes,
                   bool exact, const std::string &where) {
  ASSERT_EQ(expected.size(), actual.size()) << where;
  for (usize p = 0; p < actual.size(); ++p) {
    const std::string at = where + " pass " + std::to_string(p);
    std::vector<std::string> capped;
    for (const usize k : actual[p].capped) capped.push_back(members[k]);
    EXPECT_EQ(expected.at(p).at("capped").get<std::vector<std::string>>(), capped) << at;
    const Json &excess = expected.at(p).at("excess_by_theme");
    usize spilled = 0;
    for (usize g = 0; g < themes.size(); ++g) {
      if (!(actual[p].excess[g] > 0.0)) continue;
      ++spilled;
      ASSERT_TRUE(excess.contains(themes[g])) << at << " " << themes[g];
      expect_value(excess.at(themes[g]), actual[p].excess[g], exact, at + " " + themes[g]);
    }
    EXPECT_EQ(excess.size(), spilled) << at;
  }
}

std::vector<std::string> member_ids(const rc::FitSpec &spec) {
  std::vector<std::string> out;
  for (const auto &m : spec.members) out.push_back(m.id);
  return out;
}

std::string f64_bytes(const std::vector<f64> &values) {
  std::string out(values.size() * sizeof(f64), '\0');
  if (!values.empty()) std::memcpy(out.data(), values.data(), out.size());
  return out;
}

// The fixture's factor series as a K-P9-4 directory (atx.factor-series/v1).
fs::path write_factor_dir(const fs::path &dir, const Inputs &in) {
  const fs::path factors = dir / "factors";
  fs::create_directories(factors);
  const std::string factor = f64_bytes(in.factors);
  const std::string tau = f64_bytes(std::vector<f64>(in.ids.size(), 0.25));
  write_bytes(factors / "factor.f64", factor);
  write_bytes(factors / "factor_h21.f64", factor);
  write_bytes(factors / "tau.f64", tau);
  Json candidates = Json::array();
  for (const auto &id : in.ids) candidates.push_back(Json{{"id", id}});
  const auto sha = [](const std::string &bytes) { return atx::core::sha256_hex(bytes).value(); };
  const Json manifest{{"schema", "atx.factor-series/v1"},
                      {"status", "complete"},
                      {"role_manifest_sha256", std::string(64, 'e')},
                      {"decisions", in.decisions},
                      {"decision_sessions_ns", in.sessions},
                      {"candidates", std::move(candidates)},
                      {"files", Json{{"factor.f64", Json{{"sha256", sha(factor)}}},
                                     {"factor_h21.f64", Json{{"sha256", sha(factor)}}},
                                     {"tau.f64", Json{{"sha256", sha(tau)}}}}}};
  write_bytes(factors / "manifest.json", manifest.dump(2) + "\n");
  return factors;
}

int run_main(const std::vector<std::string> &argv, std::ostream &out, std::ostream &err) {
  std::vector<std::string_view> args{"atx-research-composition"};
  for (const auto &a : argv) args.emplace_back(a);
  return rc::research_composition_main(args, out, err);
}

} // namespace

// numpy's pairwise summation, ported: np.add.reduce on vectors of 5..1001 values and mean(axis=1)
// on 256-value rows, each bit for bit (the pytest shows the sequential sum differs above 128).
TEST(ResearchComposition, NumpyPairwiseSumIsNumpys) {
  const Json &p = fixture().at("pairwise");
  for (const Json &c : p.at("sums")) {
    const auto x = c.at("x").get<std::vector<f64>>();
    expect_value(c.at("sum"), rc::numpy_pairwise_sum(x), true, "sum n=" + std::to_string(x.size()));
  }
  const auto &rows = p.at("rows");
  for (usize r = 0; r < rows.size(); ++r) {
    const auto x = rows.at(r).get<std::vector<f64>>();
    expect_value(p.at("mean_axis1").at(r),
                 rc::numpy_pairwise_sum(x) / static_cast<f64>(x.size()), true,
                 "mean row " + std::to_string(r));
  }
}

// F-15: on two themes the equal-risk-contribution shares are inverse volatility whatever the
// correlation; C = [[4, 1.2], [1.2, 1]] gives b = (1/3, 2/3).
TEST(ResearchComposition, ErcClosedFormTwoThemes) {
  const std::vector<f64> covariance{4.0, 1.2, 1.2, 1.0};
  const std::vector<f64> share{0.5, 0.5, 0.5, 0.5};
  const std::vector<usize> theme{0, 0, 1, 1};
  const auto fit = rc::theme_erc_weights(share, theme, 2, covariance, true);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_NEAR(fit->theme_share[0], 1.0 / 3.0, 1e-12);
  EXPECT_NEAR(fit->theme_share[1], 2.0 / 3.0, 1e-12);
  EXPECT_NEAR(fit->contribution[0], fit->contribution[1], 1e-12);
  EXPECT_LE(fit->dispersion, rc::theme_erc_dispersion);
  ASSERT_EQ(fit->uncapped.size(), 4U);
  for (usize k = 0; k < 4; ++k) {
    EXPECT_EQ(fit->uncapped[k], share[k] * fit->theme_share[theme[k]]) << k;
  }
  // The cap 1/(2T) = 1/4 binds theme 1 (1/3 each) and its excess fills theme 0 to the cap.
  EXPECT_EQ(fit->cap, 0.25);
  ASSERT_EQ(fit->passes.size(), fit->cap_passes);
  ASSERT_GE(fit->passes.size(), 1U);
  EXPECT_EQ(fit->passes[0].capped, (std::vector<usize>{2, 3}));
  f64 total = 0.0;
  for (const f64 w : fit->weights) total += w;
  EXPECT_NEAR(total, 1.0, 1e-15);
  // Untraced, the same weights and no record.
  const auto plain = rc::theme_erc_weights(share, theme, 2, covariance);
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain->weights, fit->weights);
  EXPECT_TRUE(plain->passes.empty());
  EXPECT_TRUE(plain->uncapped.empty());
}

// theme-erc-v1 step 2 by hand: two one-member themes over four TRAIN decisions (a fifth outside
// TRAIN is ignored): centred (-1.5, -.5, .5, 1.5) and (-3, -1, 1, 3), divisor n - 1 = 3.
TEST(ResearchComposition, SleeveCovarianceClosedForm) {
  const std::vector<std::vector<f64>> series{{1.0, 2.0, 3.0, 4.0, 100.0},
                                             {2.0, 4.0, 6.0, 8.0, -100.0}};
  const std::vector<u8> train{1, 1, 1, 1, 0};
  const std::vector<f64> share{1.0, 1.0};
  const std::vector<usize> theme{0, 1};
  const auto cov = rc::theme_sleeve_covariance(series, train, share, theme, 2);
  ASSERT_TRUE(cov) << cov.error().to_string();
  EXPECT_EQ(cov->decisions, 4U);
  EXPECT_EQ(cov->covariance[0], 5.0 / 3.0);
  EXPECT_EQ(cov->covariance[1], 10.0 / 3.0);
  EXPECT_EQ(std::bit_cast<u64>(cov->covariance[1]), std::bit_cast<u64>(cov->covariance[2]));
  EXPECT_EQ(cov->covariance[3], 20.0 / 3.0);
  const std::vector<u8> one{1, 0, 0, 0, 0};
  EXPECT_FALSE(rc::theme_sleeve_covariance(series, one, share, theme, 2));
}

// The trace of cap_across_groups records each pass and never changes the arithmetic.
TEST(ResearchComposition, CapTraceRecordsEveryPass) {
  const std::vector<usize> group{0, 1, 1, 1, 2, 2};
  std::vector<f64> plain{0.5, 0.1, 0.1, 0.1, 0.1, 0.1};
  std::vector<f64> traced = plain;
  std::vector<cb::GroupCapPass> trace;
  const auto a = cb::cap_across_groups(plain, group, 3, 0.25, 1e-12);
  const auto b = cb::cap_across_groups(traced, group, 3, 0.25, 1e-12, &trace);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_EQ(*a, *b);
  EXPECT_EQ(plain, traced);
  ASSERT_EQ(trace.size(), *b);
  ASSERT_EQ(trace.size(), 1U);
  EXPECT_EQ(trace[0].capped, (std::vector<usize>{0}));
  EXPECT_EQ(trace[0].excess, (std::vector<f64>{0.25, 0.0, 0.0}));
}

// Rule 5 of ew-theme-std-v1: table rows in order, each member's source tier re-graded; a source
// tier outside the row is refused.
TEST(ResearchComposition, TierRegradesAreRuleFive) {
  const std::vector<std::string> ids{"ear", "sue", "x"};
  std::vector<std::string> tiers{"B+", "C+", "A"};
  const auto applied = rc::apply_tier_regrades(ids, tiers);
  ASSERT_TRUE(applied) << applied.error().to_string();
  EXPECT_EQ(tiers, (std::vector<std::string>{"C+", "C+", "A"}));
  ASSERT_EQ(applied->size(), rc::tier_regrades_v8.size());
  const std::vector<std::pair<std::string, std::string>> want{
      {"res_mom_12_1", "not-a-member"}, {"ear", "applied"}, {"sue", "declared-unchanged"},
      {"ins_opp", "not-a-member"}};
  for (usize r = 0; r < want.size(); ++r) {
    EXPECT_EQ((*applied)[r].id, want[r].first);
    EXPECT_EQ((*applied)[r].status, want[r].second);
    EXPECT_EQ((*applied)[r].member, want[r].second != "not-a-member");
  }
  EXPECT_EQ((*applied)[1].source_tier, "B+");
  std::vector<std::string> already{"C+"};
  const std::vector<std::string> ear{"ear"};
  const auto at_target = rc::apply_tier_regrades(ear, already);
  ASSERT_TRUE(at_target);
  EXPECT_EQ((*at_target)[1].status, "already-at-target");
  std::vector<std::string> wrong{"A"};
  const auto refused = rc::apply_tier_regrades(ear, wrong);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().message().find("tier re-grade of ear"), std::string::npos);
}

TEST(ResearchComposition, ThemeResidOrderIsTheRegisteredOrder) {
  std::vector<std::string> registered;
  for (const auto t : rc::theme_resid_frozen_prefix) registered.emplace_back(t);
  registered.emplace_back("filing_events");
  const std::vector<std::string> weighted{"filing_events", "low_risk", "value", "low_risk"};
  const auto order = rc::theme_resid_order(registered, weighted);
  ASSERT_TRUE(order) << order.error().to_string();
  EXPECT_EQ(*order, (std::vector<std::string>{"value", "low_risk", "filing_events"}));
  const std::vector<std::string> outside{"value", "merger_arbitrage"};
  EXPECT_FALSE(rc::theme_resid_order(registered, outside));
  EXPECT_FALSE(rc::theme_resid_order(registered, std::vector<std::string>{}));
  std::vector<std::string> unfrozen = registered;
  std::swap(unfrozen[0], unfrozen[1]);
  EXPECT_FALSE(rc::theme_resid_order(unfrozen, weighted));
}

// The acceptance contract: every case of the committed fixture, every fitted value.
TEST(ResearchComposition, FitEqualsPythonFixture) {
  const Inputs in = inputs();
  const rc::FactorSeriesView view = in.view();
  ASSERT_GE(fixture().at("cases").size(), 6U);
  for (const Json &c : fixture().at("cases")) {
    const std::string name = c.at("name").get<std::string>();
    SCOPED_TRACE(name);
    const bool exact = c.at("exact").get<bool>();
    const auto spec = rc::parse_fit_spec(c.at("spec").dump());
    ASSERT_TRUE(spec) << spec.error().to_string();
    const auto fit = rc::fit_composition(view, in.ids, *spec);
    ASSERT_TRUE(fit) << fit.error().to_string();
    const Json &e = c.at("expected");
    const auto ids = member_ids(*spec);
    const Json &parent = e.at("parent");
    expect_values(parent.at("weights"), fit->parent_weights, exact, "parent weights");
    expect_values(parent.at("weights_before_cap"), fit->parent_uncapped, exact, "parent uncapped");
    expect_value(parent.at("cap"), fit->parent_cap, true, "parent cap");
    expect_passes(parent.at("cap_iterations"), fit->parent_passes, ids, fit->themes, exact,
                  "parent");
    if (parent.contains("member_tiers")) {
      EXPECT_EQ(parent.at("member_tiers").get<std::vector<std::string>>(), fit->tiers);
      expect_values(parent.at("member_scores"), fit->scores, true, "scores");
      const Json &regrades = parent.at("tier_regrades");
      ASSERT_EQ(regrades.size(), fit->regrades.size());
      for (usize r = 0; r < regrades.size(); ++r) {
        const Json &row = regrades.at(r);
        EXPECT_EQ(row.at("id").get<std::string>(), fit->regrades[r].id);
        EXPECT_EQ(row.at("from").get<std::string>(), fit->regrades[r].from);
        EXPECT_EQ(row.at("to").get<std::string>(), fit->regrades[r].to);
        EXPECT_EQ(row.at("status").get<std::string>(), fit->regrades[r].status);
        EXPECT_EQ(row.contains("source_tier"), fit->regrades[r].member);
        if (fit->regrades[r].member) {
          EXPECT_EQ(row.at("source_tier").get<std::string>(), fit->regrades[r].source_tier);
        }
      }
    } else {
      expect_values(parent.at("ic"), fit->ic, exact, "ic");
      expect_values(parent.at("shrunk_ic"), fit->shrunk, exact, "shrunk");
      expect_values(parent.at("within_theme_shares"), fit->share, exact, "share");
      for (usize t = 0; t < fit->themes.size(); ++t) {
        expect_value(parent.at("theme_mean_ic").at(fit->themes[t]), fit->theme_mean[t], exact,
                     "theme mean " + fit->themes[t]);
      }
      std::vector<std::string> equal;
      for (usize t = 0; t < fit->themes.size(); ++t) {
        if (fit->equal_theme[t] != 0U) equal.push_back(fit->themes[t]);
      }
      EXPECT_EQ(parent.at("equal_share_themes").get<std::vector<std::string>>(), equal);
    }
    EXPECT_EQ(e.contains("theme_erc"), fit->erc);
    if (fit->erc) {
      const Json &erc = e.at("theme_erc");
      expect_values(erc.at("within_theme_shares"), fit->erc_share, exact, "erc share");
      EXPECT_EQ(erc.at("theme_order").get<std::vector<std::string>>(), fit->themes);
      EXPECT_EQ(erc.at("decisions").get<usize>(), fit->train_decisions);
      const usize t = fit->themes.size();
      for (usize i = 0; i < t; ++i) {
        for (usize j = 0; j < t; ++j) {
          expect_value(erc.at("covariance").at(i).at(j), fit->covariance[i * t + j], exact,
                       "cov " + std::to_string(i) + "," + std::to_string(j));
        }
      }
      expect_values(erc.at("theme_shares"), fit->erc_fit.theme_share, exact, "erc theme shares");
      expect_values(erc.at("risk_contributions"), fit->erc_fit.contribution, exact,
                    "erc contributions");
      expect_value(erc.at("dispersion"), fit->erc_fit.dispersion, exact, "erc dispersion");
      expect_value(erc.at("cap"), fit->erc_fit.cap, true, "erc cap");
      expect_values(erc.at("weights_before_cap"), fit->erc_fit.uncapped, exact, "erc uncapped");
      expect_values(erc.at("weights"), fit->erc_fit.weights, exact, "erc weights");
      expect_passes(erc.at("cap_iterations"), fit->erc_fit.passes, ids, fit->themes, exact, "erc");
    }
    EXPECT_EQ(e.contains("theme_tsmom"), fit->tsmom);
    if (fit->tsmom) {
      const Json &ts = e.at("theme_tsmom");
      std::vector<std::string> order;
      for (const usize t : fit->schedule.themes) order.push_back(fit->themes[t]);
      EXPECT_EQ(ts.at("theme_order").get<std::vector<std::string>>(), order);
      expect_values(ts.at("parent_masses"), fit->schedule.parent_mass, exact,
                    "tsmom parent masses");
      const Json &blocks = ts.at("blocks");
      ASSERT_EQ(blocks.size(), fit->schedule.blocks.size());
      for (usize b = 0; b < blocks.size(); ++b) {
        const auto &block = fit->schedule.blocks[b];
        const std::string at = "block " + std::to_string(b);
        EXPECT_EQ(blocks.at(b).at("decision_index").get<usize>(), block.decision_index) << at;
        EXPECT_EQ(blocks.at(b).at("from_session").get<i64>(), block.from_session) << at;
        expect_values(blocks.at(b).at("trailing"), block.trailing, exact, at + " trailing");
        expect_values(blocks.at(b).at("masses"), block.masses, exact, at + " masses");
        EXPECT_EQ(blocks.at(b).at("themes_off").get<usize>(), block.off) << at;
      }
      EXPECT_EQ(ts.at("theme_blocks_off").get<usize>(), fit->schedule.off);
      EXPECT_EQ(ts.at("theme_on_count").get<std::vector<usize>>(), fit->schedule.on_count);
    }
    EXPECT_EQ(e.contains("theme_resid"), fit->resid);
    if (fit->resid) {
      EXPECT_EQ(e.at("theme_resid").at("order").get<std::vector<std::string>>(), fit->resid_order);
    }
    expect_values(e.at("weights"), fit->weights, exact, "final weights");
  }
}

TEST(ResearchComposition, FitRefusesMalformedSpecs) {
  const Inputs in = inputs();
  const rc::FactorSeriesView view = in.view();
  const Json base = fixture().at("cases").at(0).at("spec");
  const auto refused = [&](Json spec, std::string_view needle) {
    const auto parsed = rc::parse_fit_spec(spec.dump());
    if (!parsed) {
      EXPECT_NE(parsed.error().message().find(needle), std::string::npos)
          << parsed.error().message();
      return;
    }
    const auto fit = rc::fit_composition(view, in.ids, *parsed);
    ASSERT_FALSE(fit) << needle;
    EXPECT_NE(fit.error().message().find(needle), std::string::npos) << fit.error().message();
  };
  Json unknown = base;
  unknown["composition"] = "ew-theme-v1";
  refused(unknown, "composition must be");
  Json missing = base;
  missing["members"][0]["id"] = "not-a-candidate";
  refused(missing, "member not in the factor series");
  Json twice = base;
  twice["members"][1]["id"] = twice["members"][0]["id"];
  refused(twice, "member listed twice");
  Json negative = base;
  negative["members"][0]["sign"] = -1;
  refused(negative, "must be +1");
  Json gain = base;
  gain["members"][0]["gain"] = 0.5;
  refused(gain, "aim gain");
  Json resid = base;
  resid["theme_resid"] = true;
  resid["theme_erc"] = true;
  refused(resid, "theme-resid-v1 is refused");
  Json unscored = base;
  unscored["tier_scores"].erase("A");
  refused(unscored, "tier without a declared score");
  Json schema = base;
  schema["schema"] = "atx.composition-fit-spec/v0";
  refused(schema, "atx.composition-fit-spec/v1");
}

// The executable on a K-P9-4 directory: composition_fit.json is the fit of the spec (every
// decision of this series lies in TRAIN), publication is exclusive, refusals publish nothing.
TEST(ResearchComposition, CliPublishesTheFit) {
  const fs::path dir = scratch("cli");
  const Inputs in = inputs();
  const fs::path factors = write_factor_dir(dir, in);
  const Json &c = fixture().at("cases").at(1);
  write_bytes(dir / "spec.json", c.at("spec").dump());
  const fs::path out = dir / "fit";
  std::ostringstream o;
  std::ostringstream e;
  ASSERT_EQ(run_main({"fit", "--factors", factors.string(), "--spec", (dir / "spec.json").string(),
                      "--output", out.string()},
                     o, e),
            0)
      << e.str();
  const Json doc = Json::parse(read_bytes(out / std::string(rc::kFitFile)));
  EXPECT_EQ(doc.at("schema").get<std::string>(), std::string(rc::kFitSchema));
  EXPECT_EQ(doc.at("status").get<std::string>(), "complete");
  EXPECT_EQ(doc.at("composition"), c.at("spec").at("composition"));
  EXPECT_EQ(doc.at("inputs").at("role_manifest_sha256").get<std::string>(), std::string(64, 'e'));
  EXPECT_EQ(doc.at("train_decisions").get<usize>(), in.decisions); // every session is in TRAIN
  Inputs all = in;
  all.train.assign(all.decisions, 1U);
  const auto spec = rc::parse_fit_spec(c.at("spec").dump());
  ASSERT_TRUE(spec);
  const auto fit = rc::fit_composition(all.view(), all.ids, *spec);
  ASSERT_TRUE(fit);
  expect_values(doc.at("weights"), fit->weights, true, "cli weights");
  expect_values(doc.at("theme_erc").at("theme_shares"), fit->erc_fit.theme_share, true, "cli erc");
  EXPECT_EQ(doc.at("theme_tsmom").at("blocks").size(), fit->schedule.blocks.size());
  // Exclusive output; usage errors exit 2; a malformed spec exits 1 and publishes nothing.
  EXPECT_EQ(run_main({"fit", "--factors", factors.string(), "--spec", (dir / "spec.json").string(),
                      "--output", out.string()},
                     o, e),
            1);
  EXPECT_EQ(run_main({"fit", "--factors", factors.string()}, o, e), 2);
  EXPECT_EQ(run_main({"screen"}, o, e), 2);
  write_bytes(dir / "bad.json", "{\"schema\":\"atx.composition-fit-spec/v1\"}");
  const fs::path refused = dir / "refused";
  EXPECT_EQ(run_main({"fit", "--factors", factors.string(), "--spec", (dir / "bad.json").string(),
                      "--output", refused.string()},
                     o, e),
            1);
  EXPECT_FALSE(fs::exists(refused));
}

// P19: v1 and v2 are today's weights files; v3 is K-P9-8's (lane D3).
TEST(ResearchComposition, WeightsSchemaIds) {
  EXPECT_EQ(rc::weights_schema_v1, "atx.dsl-composition-weights/v1");
  EXPECT_EQ(rc::weights_schema_v2, "atx.dsl-composition-weights/v2");
  EXPECT_EQ(rc::weights_schema_v3, "atx.dsl-composition-weights/v3");
}
