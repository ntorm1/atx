// Platform P9 B2 (P11, G-P7, K-P9-5): the eval verb tied to T1's statistics tie fixture
// (fixtures/eval_tie). The fixture's 500 x 12 series become 12 session_ns,net_return CSVs and its
// 12 construction lines a ledger (each line given the ledger schema, ruling B2-3); the verb runs
// once with the fixture's parameters and every committed value is compared under its tie rule:
// integers, strings and flags exact, floats within 1e-12 relative (the P12 tolerance).

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/types.hpp"
#include "atx/engine/eval/stats_ext.hpp"
#include "atx/engine/research/eval/eval_cli.hpp"
#include "atx/engine/research/eval/eval_report.hpp"
#include "atx/engine/research/eval/house_moments.hpp"
#include "atx/engine/research/ledger/chain.hpp"

namespace {

namespace fs = std::filesystem;
namespace ev = atx::engine::research::eval;
using atx::f64;
using atx::i64;
using atx::usize;
using Json = nlohmann::json;

constexpr usize kT = 500;
constexpr usize kK = 12;
constexpr i64 kDayNs = 86'400'000'000'000LL;
constexpr i64 kFirstSessionNs = 1'577'923'200'000'000'000LL; // 2020-01-02Z

std::string read_bytes(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void write_bytes(const fs::path &p, const std::string &text) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << text;
}

const Json &expected() {
  static const Json doc = Json::parse(read_bytes(fs::path(ATX_EVAL_TIE_FIXTURE) / "expected.json"));
  return doc;
}

std::string name(usize k) { return (k < 10U ? "trial_0" : "trial_") + std::to_string(k); }

std::string repr(f64 v) {
  std::array<char, 64> buf{};
  const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  return std::string(buf.data(), r.ptr);
}

fs::path scratch(std::string_view what) {
  const fs::path dir = fs::temp_directory_path() / ("atx_research_eval_" + std::string(what));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

// The fixture as verb inputs: one CSV per trial and the ledger JSONL.
struct Inputs {
  fs::path dir;
  std::vector<fs::path> csv;
  fs::path ledger;
};

const Inputs &inputs() {
  static const Inputs in = [] {
    Inputs out;
    out.dir = scratch("tie");
    const std::string bytes = read_bytes(fs::path(ATX_EVAL_TIE_FIXTURE) / "series.f64");
    EXPECT_EQ(bytes.size(), kT * kK * sizeof(f64));
    std::vector<f64> x(kT * kK);
    std::memcpy(x.data(), bytes.data(), std::min(bytes.size(), x.size() * sizeof(f64)));
    for (usize k = 0; k < kK; ++k) {
      std::string text = "session_ns,net_return\n";
      for (usize t = 0; t < kT; ++t) {
        text += std::to_string(kFirstSessionNs + static_cast<i64>(t) * kDayNs) + "," +
                repr(x[t * kK + k]) + "\n";
      }
      out.csv.push_back(out.dir / (name(k) + ".csv"));
      write_bytes(out.csv.back(), text);
    }
    const Json records =
        Json::parse(read_bytes(fs::path(ATX_EVAL_TIE_FIXTURE) / "ledger_records.json"))["records"];
    std::string jsonl;
    for (Json rec : records) {
      rec["schema"] = std::string(atx::engine::research::ledger::kLedgerSchema); // ruling B2-3
      jsonl += rec.dump() + "\n";
    }
    out.ledger = out.dir / "fixture-ledger.jsonl"; // never the ledger of record (review B2 S10)
    write_bytes(out.ledger, jsonl);
    return out;
  }();
  return in;
}

ev::EvalRequest tie_request() {
  const Inputs &in = inputs();
  const Json &par = expected()["parameters"];
  ev::EvalRequest rq;
  for (usize k = 0; k < kK; ++k) {
    rq.daily.push_back(ev::LabeledPath{name(k), in.csv[k].string()});
  }
  rq.paired_of.push_back(ev::LabeledPath{name(0), in.csv[1].string()});
  rq.protocol = "v8";
  rq.paired_config.draws = par["paired"]["draws"].get<usize>();
  rq.paired_config.block = par["paired"]["block"].get<usize>();
  rq.paired_config.seed = par["paired"]["seed"].get<atx::u64>();
  rq.paired_config.one_sided = par["paired"]["one_sided"].get<bool>();
  rq.dsr_n = par["dsr_n"].get<usize>();
  rq.psr_benchmarks = par["psr_sr_stars_annual"].get<std::vector<f64>>();
  rq.psr_alpha = par["alpha"].get<f64>();
  rq.onc = true;
  rq.onc_init = par["onc_n_init"].get<usize>();
  rq.onc_seed = par["onc_seed"].get<atx::u64>();
  rq.pbo = true;
  rq.pbo_blocks = par["pbo_blocks"].get<usize>();
  rq.ledger = in.ledger.string();
  rq.window_id = expected()["inputs"]["ledger"]["window_id"].get<std::string>();
  rq.trials.push_back(ev::LabeledPath{name(0), "0000000000000001"});
  return rq;
}

const Json &tie_doc() {
  static const Json doc = [] {
    const auto r = ev::evaluate(tie_request());
    EXPECT_TRUE(r.has_value()) << r.error().to_string();
    return r.has_value() ? *r : Json::object();
  }();
  return doc;
}

// The tie rule of a value: every key of `want` is in `got` with an equal value (floats within
// 1e-12 relative, Python's math.isclose(rel_tol=1e-12, abs_tol=1e-300)); extra engine keys are
// allowed.
void tie(const Json &want, const Json &got, const std::string &path,
         std::vector<std::string> &bad) {
  if (want.is_object()) {
    if (!got.is_object()) {
      bad.push_back(path + ": not an object");
      return;
    }
    for (const auto &[k, v] : want.items()) {
      if (!got.contains(k)) {
        bad.push_back(path + "." + k + ": missing");
        continue;
      }
      tie(v, got[k], path + "." + k, bad);
    }
    return;
  }
  if (want.is_array()) {
    if (!got.is_array() || got.size() != want.size()) {
      bad.push_back(path + ": array length");
      return;
    }
    for (usize i = 0; i < want.size(); ++i) {
      tie(want[i], got[i], path + "[" + std::to_string(i) + "]", bad);
    }
    return;
  }
  if (want.is_number_float()) {
    if (!got.is_number()) {
      bad.push_back(path + ": " + got.dump() + " != " + want.dump());
      return;
    }
    const f64 w = want.get<f64>();
    const f64 g = got.get<f64>();
    if (!(std::fabs(g - w) <= std::fmax(1e-12 * std::fmax(std::fabs(w), std::fabs(g)), 1e-300))) {
      std::ostringstream s;
      s.precision(17);
      s << path << ": " << g << " != " << w;
      bad.push_back(s.str());
    }
    return;
  }
  if (want.is_number_integer()) {
    if (!got.is_number_integer() || got.get<i64>() != want.get<i64>()) {
      bad.push_back(path + ": " + got.dump() + " != " + want.dump());
    }
    return;
  }
  if (want != got) {
    bad.push_back(path + ": " + got.dump() + " != " + want.dump());
  }
}

void expect_tied(const Json &want, const Json &got, const std::string &what) {
  std::vector<std::string> bad;
  tie(want, got, what, bad);
  EXPECT_TRUE(bad.empty()) << bad.size() << " difference(s), first: " << bad.front();
}

const Json &values() { return expected()["values"]; }
const Json &book() { return tie_doc()["cells"][0]; }

TEST(EvalVerb, TieMomentsAndPsr) {
  ASSERT_EQ(tie_doc()["cells"].size(), kK);
  expect_tied(values()["moments"], book()["net_moments"], "moments");
  expect_tied(values()["net_moments"], book()["net_moments"], "net_moments");
  expect_tied(values()["psr"], book()["psr"], "psr");
}

TEST(EvalVerb, TieDsrCrossLoAndHouse) {
  for (usize k = 0; k < kK; ++k) {
    expect_tied(values()["dsr_cross"][k], tie_doc()["cells"][k]["dsr"]["cross"],
                "dsr_cross[" + std::to_string(k) + "]");
  }
  expect_tied(values()["dsr_lo"], book()["dsr"]["lo"], "dsr_lo");
  // dsr_total.ledger_rows: "v8" is house_v1 (N = construction count, V = window variance).
  expect_tied(values()["dsr_house_v1"], book()["dsr"]["tot"], "dsr_house_v1");
  const Json &house = book()["dsr"]["house_v1"];
  const Json &v8 = values()["dsr_house_v1"]["v8"];
  expect_tied(v8["n"], house["n"], "house_v1.n");
  expect_tied(v8["dsr"], house["dsr"], "house_v1.dsr");
  expect_tied(v8["sr0_daily"], house["sr0_daily"], "house_v1.sr0_daily");
  expect_tied(values()["dsr_house_v1"]["variance_sr"], house["variance_sr"], "house_v1.V");
}

TEST(EvalVerb, TieOncAndEffectiveNDsr) {
  expect_tied(values()["onc"], tie_doc()["onc"], "onc");
  expect_tied(values()["dsr_onc"], book()["dsr"]["onc"], "dsr_onc");
}

TEST(EvalVerb, TiePbo) {
  expect_tied(values()["pbo"], tie_doc()["pbo"], "pbo");
  const Json &pbo = tie_doc()["pbo"];
  EXPECT_EQ(pbo["logits"].size(), values()["pbo"]["splits"].get<usize>());
  EXPECT_EQ(pbo["engine"]["ambiguous_comparisons"].get<atx::u64>(), 0U);
}

TEST(EvalVerb, TiePaired) {
  expect_tied(values()["paired"], book()["paired"], "paired");
}

TEST(EvalVerb, TieNormPpf) {
  const Json &q = values()["norm_ppf"];
  namespace st = atx::engine::eval;
  expect_tied(q["z_one_minus_alpha"], Json(st::norm_ppf(1.0 - 0.05)), "z");
  for (const char *label : {"dsr", "house", "onc"}) {
    const f64 n = q[std::string(label) + "_n"].get<f64>();
    expect_tied(q[std::string(label) + "_one_minus_1_over_n"], Json(st::norm_ppf(1.0 - 1.0 / n)),
                label);
    expect_tied(q[std::string(label) + "_one_minus_1_over_ne"],
                Json(st::norm_ppf(1.0 - 1.0 / (n * std::exp(1.0)))), label);
  }
}

// K-P9-5's document: atx-research-eval ... --json gives {psr, dsr: {house_v1, ...}, min_trl,
// pbo, onc, paired: {dsr, se, p_one, p_two, memmel_se}, conditional: {...}}.
TEST(EvalVerb, JsonSchema) {
  const Json &d = tie_doc();
  EXPECT_EQ(d["schema"], "atx.research-eval/v1");
  for (const char *key : {"config", "ledger", "cells", "psr", "dsr", "min_trl", "pbo", "onc",
                          "paired", "conditional", "hand"}) {
    EXPECT_TRUE(d.contains(key)) << key;
  }
  for (const char *key : {"house_v1", "tot", "cross", "lo", "onc"}) {
    EXPECT_TRUE(d["dsr"].contains(key)) << key;
  }
  EXPECT_TRUE(d["dsr"]["house_v1"]["dsr"].is_number());
  for (const char *key : {"dsr", "se", "p_one", "p_two", "memmel_se", "reference"}) {
    EXPECT_TRUE(d["paired"].contains(key)) << key;
  }
  EXPECT_EQ(d["paired"]["se"], d["paired"]["lw"]["se"]);
  EXPECT_EQ(d["paired"]["p_two"], d["paired"]["lw"]["p_value"]);
  EXPECT_EQ(d["paired"]["p_one"], d["paired"]["lw"]["p_one_sided"]);
  ASSERT_TRUE(d["min_trl"].is_array());
  EXPECT_EQ(d["min_trl"].size(), 2U);
  EXPECT_EQ(d["psr"], d["cells"][0]["psr"]);
  const Json &l = d["ledger"];
  EXPECT_EQ(l["chain_rule"], "atx-ledger-chain-fold/v1");
  EXPECT_EQ(l["chain_head"].get<std::string>().size(), 64U);
  EXPECT_EQ(l["lines"].get<usize>(), kK);
  const Json &c = d["conditional"];
  ASSERT_TRUE(c["rows"].is_array());
  ASSERT_EQ(c["rows"].size(), 1U); // the one paired cell
  EXPECT_EQ(c["rows"][0]["name"], name(0));
  EXPECT_EQ(c["rows"][0]["raw"], d["paired"]["dsr"]);
  EXPECT_TRUE(c["cumulative"].contains("adjusted_sum"));
  EXPECT_EQ(d["cells"][0]["conditional"], c["rows"][0]);
  EXPECT_TRUE(d["cells"][1]["paired"].is_null());
}

TEST(EvalVerb, HandRowIsThePrefixState) {
  ev::EvalRequest rq = tie_request();
  rq.onc = false;
  rq.pbo = false;
  rq.hands.push_back(ev::HandBook{"0000000000000003", inputs().csv[2].string()});
  const auto doc = ev::evaluate(rq);
  ASSERT_TRUE(doc.has_value()) << doc.error().to_string();
  const Json &h = (*doc)["hand"][0];
  EXPECT_EQ(h["prefix_lines"].get<usize>(), 3U);
  EXPECT_EQ(h["tot"]["n"].get<i64>(), 3);
  EXPECT_EQ(h["parts"]["n_tot"].get<i64>(), 3);
  EXPECT_TRUE(h["in_ledger"].get<bool>());
  rq.hands = {ev::HandBook{"ffffffffffffffff", inputs().csv[2].string()}};
  const auto refused = ev::evaluate(rq);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().message().find("has no construction line"), std::string::npos);
}

// P9 ruling PRE-15: DSR_hand at N = N_tot - M (dsr.tot.tot_minus_m) on the Python ledger rules'
// fixture (fixtures/research_ledger: trials by the defect rule, a campaign registry M > 0, V from
// its window's cells). The whole-ledger rows leave out exactly M and keep V; the value is the
// house DSR at that N; a --hand book keeps the prefix rule (no tot_minus_m row); the text labels
// both.
const Json &ledger_fixture() {
  static const Json doc =
      Json::parse(read_bytes(fs::path(ATX_RESEARCH_LEDGER_FIXTURE) / "expected.json"));
  return doc;
}

TEST(EvalVerb, HandRowAtNTotMinusMLeavesOutTheCampaigns) {
  const Json &fx = ledger_fixture();
  const Json &parts = fx["values"]["trial_breakdown"];
  const i64 trials = parts["ledger_trials"].get<i64>();
  const i64 m = parts["campaign_registry"].get<i64>();
  ASSERT_GT(m, 0); // the fixture holds a mining campaign: N_tot - M is not N_tot
  const std::string ledgered = fx["values"]["construction_ids"][0].get<std::string>();
  const fs::path ledger =
      fs::path(ATX_RESEARCH_LEDGER_FIXTURE) / fx["ledger"]["file"].get<std::string>();
  ev::EvalRequest rq;
  rq.daily.push_back(ev::LabeledPath{"book", inputs().csv[0].string()});
  rq.daily.push_back(ev::LabeledPath{"old", inputs().csv[1].string()});
  rq.trials.push_back(ev::LabeledPath{"old", ledgered});
  rq.ledger = ledger.string();
  rq.window_id = fx["ledger"]["window_id"].get<std::string>();
  rq.hands.push_back(ev::HandBook{ledgered, inputs().csv[1].string()});
  const auto doc = ev::evaluate(rq);
  ASSERT_TRUE(doc.has_value()) << doc.error().to_string();
  for (usize k = 0; k < 2U; ++k) {
    const Json &c = (*doc)["cells"][k];
    const Json &t = c["dsr"]["tot"];
    const i64 own = k == 0U ? 1 : 0; // "book" is not ledgered (+ 1), "old" is
    EXPECT_EQ(t["in_ledger"].get<bool>(), k == 1U);
    EXPECT_EQ(t["tot"]["n"].get<i64>(), parts["n_tot"].get<i64>() + own);
    const i64 n = t["tot_minus_m"]["n"].get<i64>();
    EXPECT_EQ(n, trials + own);
    EXPECT_EQ(n, t["tot"]["n"].get<i64>() - m);
    // the house DSR at (N_tot - M, the ledger's V), bit for bit
    const Json &mj = c["net_moments"];
    const ev::HouseMoments hm{mj["sessions"].get<usize>(), mj["sr_daily"].get<f64>(),
                              mj["skew"].get<f64>(), mj["kurtosis"].get<f64>()};
    ASSERT_TRUE(t["variance_sr"].is_number());
    const ev::DsrRow want = ev::dsr_at(hm, static_cast<usize>(n), t["variance_sr"].get<f64>());
    ASSERT_TRUE(want.dsr && want.sr0_daily && want.sr0_annual);
    const Json &row = t["tot_minus_m"];
    EXPECT_EQ(row["dsr"].get<f64>(), *want.dsr);
    EXPECT_EQ(row["sr0_daily"].get<f64>(), *want.sr0_daily);
    EXPECT_EQ(row["sr0_annual"].get<f64>(), *want.sr0_annual);
    EXPECT_GT(row["dsr"].get<f64>(), t["tot"]["dsr"].get<f64>()); // fewer trials deflate less
  }
  const Json &h = (*doc)["hand"][0];
  EXPECT_TRUE(h.contains("tot") && h.contains("v8"));
  EXPECT_FALSE(h.contains("tot_minus_m"));
  const std::string out = ev::eval_text(*doc);
  EXPECT_NE(out.find(" | hand (N_tot-M=" + std::to_string(trials + 1) + ") "), std::string::npos)
      << out;
  EXPECT_NE(out.find("== DSR_hand (prefix rule) " + ledgered + ": prefix "), std::string::npos)
      << out;
}

TEST(EvalVerb, CliWritesTheDocumentAndRefuses) {
  const Inputs &in = inputs();
  const fs::path out_path = in.dir / "eval.json";
  const std::string book_arg = "book=" + in.csv[0].string();
  const std::string ref = in.csv[1].string();
  const std::string out_str = out_path.string();
  const std::vector<std::string_view> args = {"atx-research-eval", "--daily", book_arg,
                                              "--paired", ref, "--protocol", "v8",
                                              "--draws", "199", "--json", "--out", out_str};
  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(ev::research_eval_main(args, out, err), 0) << err.str();
  const Json doc = Json::parse(out.str());
  EXPECT_EQ(doc, Json::parse(read_bytes(out_path)));
  EXPECT_EQ(doc["config"]["draws"].get<usize>(), 199U);
  EXPECT_EQ(doc["config"]["seed"].get<atx::u64>(), 20260929U);
  EXPECT_EQ(doc["cells"][0]["label"], "book");

  std::ostringstream o2;
  std::ostringstream e2;
  const std::vector<std::string_view> bogus = {"atx-research-eval", "--bogus", "x"};
  EXPECT_EQ(ev::research_eval_main(bogus, o2, e2), 2);

  const fs::path sealed = in.dir / "sealed.csv";
  write_bytes(sealed, "session_ns,net_return\n1704067200000000000,0.01\n");
  const std::string sealed_arg = sealed.string();
  const std::vector<std::string_view> refuse = {"atx-research-eval", "--daily", sealed_arg};
  std::ostringstream o3;
  std::ostringstream e3;
  EXPECT_EQ(ev::research_eval_main(refuse, o3, e3), 1);
  EXPECT_FALSE(e3.str().empty());
}

TEST(EvalVerb, ProtocolDefaultsFollowNavSumm) {
  const std::vector<std::string_view> v7 = {"--daily", "x.csv"};
  const auto a = ev::parse_eval_args(v7);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->request.paired_config.draws, 2000U);
  EXPECT_EQ(a->request.paired_config.seed, 20260927U);
  EXPECT_EQ(a->request.daily[0].label, "x.csv");
  const std::vector<std::string_view> v8 = {"--daily", "b=x.csv", "--protocol", "v8", "--seed",
                                            "7", "--psr-benchmarks", "0,0.5,,1"};
  const auto b = ev::parse_eval_args(v8);
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->request.paired_config.draws, 4999U);
  EXPECT_EQ(b->request.paired_config.seed, 7U);
  EXPECT_EQ(b->request.psr_benchmarks, (std::vector<f64>{0.0, 0.5, 1.0}));
  const std::vector<std::string_view> twice = {"--daily", "x", "--seed", "1", "--seed", "2"};
  EXPECT_FALSE(ev::parse_eval_args(twice).has_value());
}

} // namespace
