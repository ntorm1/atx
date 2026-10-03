// research/admission (P9 B1) against the fitter: fixtures/research_admission/screen_v4_v1.json is
// fit_composition_weights.py's own screen_v4 rows and admission_csv bytes (v4-prior-v1 and -v2) on
// synthetic factor rows, written by atx-impl/tools/test_factor_series_admission.py, which pins the
// committed file against the fitter's code. The C++ screen reproduces every decision exactly and
// every float to 1e-12 (Ruling P12); the C++ table writer turns the fitter's rows into the fitter's
// bytes; the executable screens a K-P9-4 directory into the same table.
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/research/admission/admission_cli.hpp"
#include "atx/engine/research/admission/admission_csv.hpp"
#include "atx/engine/research/admission/screen.hpp"

namespace adm = atx::engine::research::admission;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using atx::f64;
using atx::i32;
using atx::i64;
using atx::u8;
using atx::usize;

namespace {

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

std::string read_bytes(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot open " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_bytes(const fs::path &path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

const Json &fixture() {
  static const Json doc =
      Json::parse(read_bytes(fs::path(ATX_RESEARCH_ADMISSION_FIXTURE) / "screen_v4_v1.json"));
  return doc;
}

// A fresh directory under the process-isolated temp path (atx-test-scratch).
fs::path scratch(std::string_view name) {
  const fs::path dir = fs::temp_directory_path() / ("atx_research_admission_" + std::string(name));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

// The fixture's inputs, owned.
struct Inputs {
  usize candidates{};
  usize decisions{};
  std::vector<std::string> ids;
  std::vector<f64> factors; // decisions x candidates
  std::vector<f64> taus;
  std::vector<u8> mask;
  std::vector<usize> tiers;
  std::vector<i32> priors;
  std::vector<adm::CandidateMeta> meta;
  [[nodiscard]] adm::ScreenInput input() const {
    return {candidates, decisions, factors, taus, mask, tiers, priors};
  }
};

Inputs inputs() {
  const Json &doc = fixture();
  Inputs in;
  in.decisions = doc.at("decisions").get<usize>();
  const int exponent = doc.at("scale_exponent").get<int>();
  for (const Json &c : doc.at("candidates")) {
    adm::CandidateMeta m;
    m.id = c.at("id").get<std::string>();
    m.family = c.at("family").get<std::string>();
    m.theme = c.at("theme").get<std::string>();
    m.tier = adm::tier_from_grade(c.at("tier").get<std::string>()).value();
    m.prior_sign = c.at("prior_sign").get<i32>();
    m.runner_sign = c.at("runner_sign").get<i32>();
    m.cache_entry = c.at("cache_entry").get<std::string>();
    m.cache_payload_sha256 = c.at("cache_payload_sha256").get<std::string>();
    in.ids.push_back(m.id);
    in.taus.push_back(c.at("tau").get<f64>());
    in.tiers.push_back(m.tier.rank);
    in.priors.push_back(m.prior_sign);
    in.meta.push_back(std::move(m));
  }
  in.candidates = in.ids.size();
  for (const Json &row : doc.at("factor_units")) {
    for (const Json &units : row) {
      in.factors.push_back(
          units.is_null() ? kNaN : std::ldexp(static_cast<f64>(units.get<i64>()), exponent));
    }
  }
  for (const Json &m : doc.at("train_mask")) {
    in.mask.push_back(static_cast<u8>(m.get<int>()));
  }
  return in;
}

usize index_of(const std::vector<std::string> &ids, const std::string &id) {
  const auto it = std::find(ids.begin(), ids.end(), id);
  if (it == ids.end()) {
    throw std::runtime_error("unknown id " + id);
  }
  return static_cast<usize>(it - ids.begin());
}

std::optional<f64> optional_float(const Json &v) {
  return v.is_null() ? std::nullopt : std::optional<f64>(v.get<f64>());
}

std::optional<usize> optional_ref(const Json &v, const std::vector<std::string> &ids) {
  return v.is_null() ? std::nullopt : std::optional<usize>(index_of(ids, v.get<std::string>()));
}

std::vector<usize> refs(const Json &v, const std::vector<std::string> &ids) {
  std::vector<usize> out;
  for (const Json &id : v) {
    out.push_back(index_of(ids, id.get<std::string>()));
  }
  return out;
}

// Ruling P12: equal to 1e-12, relative above 1.
bool tie(f64 a, f64 b) { return std::fabs(a - b) <= 1e-12 * std::max(1.0, std::fabs(b)); }

void expect_tie(const std::optional<f64> &got, const Json &want, const std::string &where) {
  ASSERT_EQ(got.has_value(), !want.is_null()) << where;
  if (got) {
    EXPECT_TRUE(tie(*got, want.get<f64>())) << where << ": " << *got << " vs " << want.dump();
  }
}

// The fitter's row as a ScreenRow (its own float bits).
adm::ScreenRow fitter_row(const Json &r, const std::vector<std::string> &ids) {
  adm::ScreenRow row;
  row.s_k = r.at("s_k").get<i32>();
  row.tau = r.at("tau").get<f64>();
  row.train_days = r.at("train_days").get<usize>();
  row.train_mean = optional_float(r.at("train_mean"));
  row.train_sharpe = optional_float(r.at("train_sharpe"));
  row.hac_t = optional_float(r.at("hac_t"));
  for (const Json &c : r.at("failed_checks")) {
    for (const adm::Check check : {adm::Check::NoPrior, adm::Check::Insufficient,
                                   adm::Check::Turnover, adm::Check::TurnoverCost,
                                   adm::Check::Veto}) {
      if (adm::check_name(check) == c.get<std::string>()) {
        row.failed_checks.push_back(check);
      }
    }
  }
  for (const adm::Status s :
       {adm::Status::Admitted, adm::Status::RejectNoPrior, adm::Status::RejectInsufficient,
        adm::Status::RejectTurnover, adm::Status::RejectTurnoverCost, adm::Status::RejectVeto,
        adm::Status::RejectRedundant}) {
    if (adm::status_name(s) == r.at("status").get<std::string>()) {
      row.status = s;
    }
  }
  row.redundant_with = optional_ref(r.at("redundant_with"), ids);
  row.redundant_rho = optional_float(r.at("redundant_rho"));
  row.admission_rank = r.at("admission_rank").is_null()
                           ? std::nullopt
                           : std::optional<usize>(r.at("admission_rank").get<usize>());
  row.low_overlap_with = refs(r.at("low_overlap_with"), ids);
  row.undefined_rho_with = refs(r.at("undefined_rho_with"), ids);
  row.max_abs_rho = optional_float(r.at("max_abs_rho"));
  row.max_abs_rho_with = optional_ref(r.at("max_abs_rho_with"), ids);
  return row;
}

void expect_rows(const std::vector<adm::ScreenRow> &got, const Json &want,
                 const std::vector<std::string> &ids, const std::string &screen) {
  ASSERT_EQ(got.size(), want.size()) << screen;
  for (usize k = 0; k < got.size(); ++k) {
    const std::string where = screen + " " + ids[k];
    const adm::ScreenRow &g = got[k];
    const adm::ScreenRow w = fitter_row(want.at(k), ids);
    EXPECT_EQ(adm::status_name(g.status), adm::status_name(w.status)) << where;
    EXPECT_EQ(g.failed_checks, w.failed_checks) << where;
    EXPECT_EQ(g.s_k, w.s_k) << where;
    EXPECT_EQ(g.train_days, w.train_days) << where;
    EXPECT_EQ(g.admission_rank, w.admission_rank) << where;
    EXPECT_EQ(g.redundant_with, w.redundant_with) << where;
    EXPECT_EQ(g.low_overlap_with, w.low_overlap_with) << where;
    EXPECT_EQ(g.undefined_rho_with, w.undefined_rho_with) << where;
    EXPECT_EQ(g.max_abs_rho_with, w.max_abs_rho_with) << where;
    EXPECT_EQ(std::bit_cast<atx::u64>(g.tau), std::bit_cast<atx::u64>(w.tau)) << where;
    const Json &r = want.at(k);
    expect_tie(g.train_mean, r.at("train_mean"), where + " train_mean");
    expect_tie(g.train_sharpe, r.at("train_sharpe"), where + " train_sharpe");
    expect_tie(g.hac_t, r.at("hac_t"), where + " hac_t");
    expect_tie(g.redundant_rho, r.at("redundant_rho"), where + " redundant_rho");
    expect_tie(g.max_abs_rho, r.at("max_abs_rho"), where + " max_abs_rho");
  }
}

// Two admission.csv texts under Ruling P12: decision cells byte for byte, float cells tied.
void expect_same_table(const std::string &got, const std::string &want) {
  const auto lines = [](const std::string &text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
      out.push_back(line);
    }
    return out;
  };
  const auto cells = [](const std::string &line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    for (std::string cell; std::getline(in, cell, ',');) {
      out.push_back(cell);
    }
    if (!line.empty() && line.back() == ',') {
      out.emplace_back();
    }
    return out;
  };
  const auto g = lines(got);
  const auto w = lines(want);
  ASSERT_EQ(g.size(), w.size());
  ASSERT_EQ(g.front(), w.front());
  const auto header = cells(w.front());
  for (usize n = 1; n < w.size(); ++n) {
    const auto gc = cells(g[n]);
    const auto wc = cells(w[n]);
    ASSERT_EQ(gc.size(), header.size()) << n;
    ASSERT_EQ(wc.size(), header.size()) << n;
    for (usize c = 0; c < header.size(); ++c) {
      const std::string &col = header[c];
      const bool is_float = col == "redundant_rho" || col == "tau" || col == "train_mean" ||
                            col == "train_sharpe" || col == "hac_t" || col == "max_abs_rho";
      if (is_float && !gc[c].empty() && !wc[c].empty()) {
        EXPECT_TRUE(tie(std::stod(gc[c]), std::stod(wc[c]))) << n << ' ' << col;
      } else {
        EXPECT_EQ(gc[c], wc[c]) << n << ' ' << col;
      }
    }
  }
}

} // namespace

// Both screens on the fixture's rows: every status, failed check, admission rank, named candidate
// and count is the fitter's; tau is carried bit for bit; the statistics tie to 1e-12.
TEST(ResearchAdmission, EqualsFitterFixture) {
  const Inputs in = inputs();
  const Json &expected = fixture().at("expected");
  for (const char *name : {"v4-prior-v1", "v4-prior-v2"}) {
    const auto id = adm::screen_from_name(name);
    ASSERT_TRUE(id.has_value());
    const auto rows = adm::screen_v4(in.input(), adm::screen_rules(*id));
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    expect_rows(*rows, expected.at(name).at("rows"), in.ids, name);
  }
}

// The writer turns the fitter's own rows into the fitter's admission.csv byte for byte (columns,
// cells, lists, booleans and Python's float repr), for both screens.
TEST(ResearchAdmission, CsvEqualsFitterBytes) {
  const Inputs in = inputs();
  const Json &expected = fixture().at("expected");
  for (const char *name : {"v4-prior-v1", "v4-prior-v2"}) {
    std::vector<adm::ScreenRow> rows;
    for (const Json &r : expected.at(name).at("rows")) {
      rows.push_back(fitter_row(r, in.ids));
    }
    const auto csv = adm::admission_csv(in.meta, rows);
    ASSERT_TRUE(csv.has_value()) << csv.error().message();
    EXPECT_EQ(*csv, expected.at(name).at("csv").get<std::string>()) << name;
  }
  // The fitter's "unsafe cell" refusal.
  std::vector<adm::CandidateMeta> meta = in.meta;
  meta[0].family = "a,b";
  std::vector<adm::ScreenRow> rows;
  for (const Json &r : expected.at("v4-prior-v1").at("rows")) {
    rows.push_back(fitter_row(r, in.ids));
  }
  EXPECT_FALSE(adm::admission_csv(meta, rows).has_value());
}

// Python's repr(float), case by case as Python printed it.
TEST(ResearchAdmission, PythonFloatRepr) {
  for (const Json &c : fixture().at("repr_cases")) {
    const f64 x = c.at(0).get<f64>();
    EXPECT_EQ(adm::python_float_repr(x), c.at(1).get<std::string>()) << c.dump();
  }
  EXPECT_EQ(adm::python_float_repr(kNaN), "nan");
  EXPECT_EQ(adm::python_float_repr(std::numeric_limits<f64>::infinity()), "inf");
  EXPECT_EQ(adm::python_float_repr(-std::numeric_limits<f64>::infinity()), "-inf");
}

namespace {

// A K-P9-4 directory over the fixture's series (factor_h21 = the factor series itself, which the
// traded-horizon table only reports) and a candidates file shaped like the fitter's admission.json
// (decision keys present and ignored).
struct Written {
  fs::path factors;
  fs::path candidates;
};

std::string f64_bytes(const std::vector<f64> &values) {
  std::string out(values.size() * sizeof(f64), '\0');
  if (!values.empty()) {
    std::memcpy(out.data(), values.data(), out.size());
  }
  return out;
}

Written write_inputs(const fs::path &dir, const Inputs &in) {
  Written w{dir / "factors", dir / "candidates.json"};
  fs::create_directories(w.factors);
  const std::string factor = f64_bytes(in.factors);
  const std::string tau = f64_bytes(in.taus);
  write_bytes(w.factors / "factor.f64", factor);
  write_bytes(w.factors / "factor_h21.f64", factor);
  write_bytes(w.factors / "tau.f64", tau);
  Json candidates = Json::array();
  for (const std::string &id : in.ids) {
    candidates.push_back(Json{{"id", id}});
  }
  const auto sha = [](const std::string &bytes) { return atx::core::sha256_hex(bytes).value(); };
  const Json manifest{
      {"schema", "atx.factor-series/v1"},
      {"status", "complete"},
      {"role_manifest_sha256", std::string(64, 'e')},
      {"decisions", in.decisions},
      {"decision_sessions_ns", fixture().at("decision_sessions_ns")},
      {"candidates", std::move(candidates)},
      {"files", Json{{"factor.f64", Json{{"sha256", sha(factor)}}},
                     {"factor_h21.f64", Json{{"sha256", sha(factor)}}},
                     {"tau.f64", Json{{"sha256", sha(tau)}}}}}};
  write_bytes(w.factors / "manifest.json", manifest.dump(2) + "\n");
  Json rows = fixture().at("candidates");
  for (Json &r : rows) {
    r["status"] = "admitted"; // a decision key of admission.json: never read
  }
  write_bytes(w.candidates, Json{{"schema", "atx.dsl-admission/v1"}, {"candidates", rows}}.dump());
  return w;
}

int run_main(const std::vector<std::string> &argv, std::ostream &out, std::ostream &err) {
  std::vector<std::string_view> args{"atx-research-admission"};
  for (const auto &a : argv) {
    args.emplace_back(a);
  }
  return adm::research_admission_main(args, out, err);
}

} // namespace

// The executable on a K-P9-4 directory: admission.csv is the fitter's table (decision cells byte
// for byte, floats tied), the manifest names the fitter's admitted order, the counts and the ruled
// sign predicate's verdicts; traded_horizon.csv sits beside it; publication is exclusive and every
// refusal publishes nothing.
TEST(ResearchAdmission, CliScreensAFactorSeries) {
  const fs::path dir = scratch("cli");
  const Inputs in = inputs();
  const Written w = write_inputs(dir, in);
  const fs::path out = dir / "screen";
  std::ostringstream o;
  std::ostringstream e;
  const std::vector<std::string> args{"screen",         "--factors", w.factors.string(),
                                      "--candidates",   w.candidates.string(),
                                      "--output",       out.string(),
                                      "--sign-rule",    "gate-prior-v1"};
  ASSERT_EQ(run_main(args, o, e), 0) << e.str();
  const Json &v1 = fixture().at("expected").at("v4-prior-v1");
  expect_same_table(read_bytes(out / "admission.csv"), v1.at("csv").get<std::string>());
  const Json manifest = Json::parse(read_bytes(out / "manifest.json"));
  EXPECT_EQ(manifest.at("schema"), "atx.research-admission/v1");
  EXPECT_EQ(manifest.at("screen"), "v4-prior-v1");
  EXPECT_EQ(manifest.at("rules").at("hac").at("method"), "fitter-newey-west-v1");
  std::vector<std::pair<usize, std::string>> ranked;
  for (usize k = 0; k < in.ids.size(); ++k) {
    const Json &rank = v1.at("rows").at(k).at("admission_rank");
    if (!rank.is_null()) {
      ranked.emplace_back(rank.get<usize>(), in.ids[k]);
    }
  }
  std::sort(ranked.begin(), ranked.end());
  ASSERT_EQ(manifest.at("admitted").size(), ranked.size());
  for (usize i = 0; i < ranked.size(); ++i) {
    EXPECT_EQ(manifest.at("admitted").at(i).get<std::string>(), ranked[i].second) << i;
  }
  EXPECT_EQ(manifest.at("counts").at("admitted").get<usize>(), ranked.size());
  // The gate rule: the admitted string with runner sign 0 ("left") does not stand.
  const Json &sign = manifest.at("sign_rule");
  EXPECT_EQ(sign.at("name"), "gate-prior-v1");
  EXPECT_EQ(sign.at("admitted_sign_fails"), Json::array({"left"}));
  EXPECT_EQ(manifest.at("files").at("admission.csv").at("sha256"),
            atx::core::sha256_hex(read_bytes(out / "admission.csv")).value());
  const std::string horizon = read_bytes(out / "traded_horizon.csv");
  EXPECT_EQ(horizon.substr(0, horizon.find('\n')), "id,status,s_k,h21_days,ic_h21,ic_h21_hac_t");
  // Exclusive: a second run refuses and leaves the table untouched.
  const std::string before = read_bytes(out / "admission.csv");
  EXPECT_EQ(run_main(args, o, e), 1);
  EXPECT_EQ(read_bytes(out / "admission.csv"), before);
  // v4-prior-v2 through the flag; the wave rule keeps "left".
  const fs::path out2 = dir / "screen2";
  ASSERT_EQ(run_main({"screen", "--factors", w.factors.string(), "--candidates",
                      w.candidates.string(), "--output", out2.string(), "--screen", "v4-prior-v2"},
                     o, e),
            0)
      << e.str();
  expect_same_table(read_bytes(out2 / "admission.csv"),
                    fixture().at("expected").at("v4-prior-v2").at("csv").get<std::string>());
  const Json manifest2 = Json::parse(read_bytes(out2 / "manifest.json"));
  EXPECT_EQ(manifest2.at("sign_rule").at("name"), "wave-zero-kept-v1");
  EXPECT_TRUE(manifest2.at("sign_rule").at("admitted_sign_fails").empty());
  // Refusals: a payload whose SHA differs, candidates in another order; usage errors exit 2.
  write_bytes(w.factors / "tau.f64", f64_bytes(std::vector<f64>(in.taus.size(), 0.5)));
  EXPECT_EQ(run_main({"screen", "--factors", w.factors.string(), "--candidates",
                      w.candidates.string(), "--output", (dir / "x").string()},
                     o, e),
            1);
  EXPECT_FALSE(fs::exists(dir / "x"));
  EXPECT_EQ(run_main({"screen", "--factors", w.factors.string()}, o, e), 2);
  EXPECT_EQ(run_main({"fit"}, o, e), 2);
  EXPECT_EQ(run_main({"screen", "--factors", "a", "--candidates", "b", "--output", "c",
                      "--screen", "v3-admit-v1"},
                     o, e),
            2);
}

// The candidates contract: metadata keys typed as the fitter writes them; -1 prior refused (v4
// embeds the prior sign in the DSL), grades and integers never mixed, ids unique.
TEST(ResearchAdmission, CandidatesContract) {
  const auto row = [](Json tier, int prior) {
    return Json{{"id", "x"},          {"family", "f"},        {"theme", "value"},
                {"tier", std::move(tier)}, {"prior_sign", prior}, {"runner_sign", 1},
                {"cache_entry", "base"}, {"cache_payload_sha256", std::string(64, 'a')}};
  };
  const auto parse = [](const Json &candidates) {
    return adm::parse_candidates(Json{{"candidates", candidates}}.dump());
  };
  const auto good = parse(Json::array({row("B+", 1)}));
  ASSERT_TRUE(good.has_value()) << good.error().message();
  EXPECT_EQ(good->front().tier.rank, 3U);
  const auto integer = parse(Json::array({row(7, 0)}));
  ASSERT_TRUE(integer.has_value());
  EXPECT_EQ(integer->front().tier.text, "7");
  EXPECT_EQ(integer->front().tier.rank, 7U);
  EXPECT_FALSE(parse(Json::array({row("B+", -1)})).has_value());
  EXPECT_FALSE(parse(Json::array({row("Z", 1)})).has_value());
  Json mixed = Json::array({row("B", 1), row(2, 1)});
  mixed[1]["id"] = "y";
  EXPECT_FALSE(parse(mixed).has_value());
  EXPECT_FALSE(parse(Json::array({row("B", 1), row("A", 1)})).has_value()); // duplicate id
  Json no_family = Json::array({row("B", 1)});
  no_family[0].erase("family");
  EXPECT_FALSE(parse(no_family).has_value());
  EXPECT_FALSE(adm::parse_candidates("[]").has_value());
}
