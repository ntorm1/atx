#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "../src/strategy_factors_verb.hpp"
#include "../src/strategy_price_exposures.hpp"

// P9 B1 (contract K-P9-4): the `factors` verb's factor series on the K2 basis. The golden values
// are fit_composition_weights.py's own factor_record (Context.build, book, factor_returns,
// standalone_turnover) on the role factor_fixture() builds, written to
// fixtures/factors_verb_v1.json by atx-impl/tools/test_factor_series_admission.py, which pins the
// committed file against the fitter's code (the report-only 21-session series against a numpy
// reference over the fitter's own book and forward returns).
namespace {
using namespace atx;
namespace st = atx::impl::strategy;
namespace co = atx::core;
using Json = nlohmann::json;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();

struct Lcg {
  u64 state{};
  f64 next() { // [0, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) * 0x1.0p-53;
  }
};
// test_factor_series_admission.py factor_panel(), operation for operation: a one-factor walk with
// per-name loadings, an adjusted-only doubling of name 5 from session 395 (interval 395 guarded:
// decision 393's label is 0), scattered absent rows, 15 names absent at session 400 (fewer than 50
// used names: decision 400 refused), scattered nonmembers, members from session 63, and three
// signals: s1 noise with NaN cells, s2 loading on the factor loading, s3 four tied levels with
// every 9th session all NaN (flat decisions).
constexpr usize kN = 60, kRoleDates = 420, kScoreBegin = 383, kMemberFrom = 63;
constexpr usize kRefusedSession = 400, kDoubledFrom = 395;
constexpr i64 kFirstDay = 17879; // 2018-12-14: decision 383 is 2020-01-01
constexpr usize kEnd = kRoleDates - 2, kDecisions = kEnd - kScoreBegin;
constexpr usize kSignals = 3;
constexpr std::array<const char*, kSignals> kSignalNames{"s1", "s2", "s3"};
constexpr i64 kDayNs = 86'400'000'000'000LL;

struct Fixture {
  std::vector<f64> close, raw, volume;
  std::array<std::vector<f64>, kSignals> signals;
  std::vector<u8> present, member;
  [[nodiscard]] st::PriceExposureInput input() const {
    return {kRoleDates, kN, close, raw, volume, present};
  }
};
Fixture factor_fixture() {
  Fixture f;
  const usize cells = kRoleDates * kN;
  f.close.assign(cells, 0.0);
  Lcg rng{2027};
  for (usize i = 0; i < kN; ++i) f.close[i] = 20.0 + static_cast<f64>(i);
  for (usize t = 1; t < kRoleDates; ++t) {
    const f64 common = 0.03 * (rng.next() - 0.5);
    for (usize i = 0; i < kN; ++i) {
      const f64 loading = 0.5 + 0.02 * static_cast<f64>(i);
      const f64 idio = 0.005 + 0.0002 * static_cast<f64>((i * 7) % kN);
      f.close[t * kN + i] =
          f.close[(t - 1) * kN + i] * (1 + loading * common + idio * (rng.next() - 0.5));
    }
  }
  f.raw = f.close;
  for (usize t = kDoubledFrom; t < kRoleDates; ++t) f.close[t * kN + 5] = f.close[t * kN + 5] * 2.0;
  f.volume.assign(cells, 0.0);
  for (usize t = 0; t < kRoleDates; ++t)
    for (usize i = 0; i < kN; ++i)
      f.volume[t * kN + i] = 1e4 * static_cast<f64>(1 + (i * 11) % kN) * (0.5 + rng.next());
  f.present.assign(cells, 1);
  f.member.assign(cells, 0);
  for (usize t = 0; t < kRoleDates; ++t)
    for (usize i = 0; i < kN; ++i) {
      const usize k = t * kN + i;
      if ((t * 7 + i * 3) % 97 == 0 || (t == kRefusedSession && i < 15)) f.present[k] = 0;
      f.member[k] =
          static_cast<u8>(f.present[k] && (t + 2 * i) % 19 != 0 && t >= kMemberFrom);
      if (!f.present[k]) f.close[k] = f.raw[k] = f.volume[k] = missing;
    }
  for (auto& s : f.signals) s.assign(cells, 0.0);
  Lcg draws{78};
  for (usize t = 0; t < kRoleDates; ++t)
    for (usize i = 0; i < kN; ++i) {
      const usize k = t * kN + i;
      f.signals[0][k] = draws.next() - 0.5;
      if ((t * 3 + i) % 23 == 0) f.signals[0][k] = missing;
      f.signals[1][k] = (0.5 + 0.02 * static_cast<f64>(i)) + 0.2 * (draws.next() - 0.5);
      f.signals[2][k] = std::floor(4.0 * draws.next());
      if (t % 9 == 0) f.signals[2][k] = missing;
    }
  return f;
}
// The verb's series of every signal, in memory.
struct Series {
  st::FactorContext ctx;
  std::array<st::FactorSeries, kSignals> series;
};
Series compute(const Fixture& f) {
  Series out;
  auto ctx = st::build_factor_context(f.input(), f.member, st::PriceExposureConfig{}, kScoreBegin,
                                      kEnd);
  EXPECT_TRUE(ctx) << (ctx ? std::string{} : ctx.error().to_string());
  if (!ctx) return out;
  out.ctx = std::move(*ctx);
  st::FactorScratch scratch;
  for (usize k = 0; k < kSignals; ++k) {
    auto s = st::factor_series(out.ctx, f.signals[k], scratch);
    EXPECT_TRUE(s) << (s ? std::string{} : s.error().to_string());
    if (s) out.series[k] = std::move(*s);
  }
  return out;
}
bool same_bits(f64 a, f64 b) { return std::bit_cast<u64>(a) == std::bit_cast<u64>(b); }
bool same_bits(std::span<const f64> a, std::span<const f64> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), b.end(),
                                            [](f64 x, f64 y) { return same_bits(x, y); });
}
Json read_json(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path.string());
  return Json::parse(in);
}
Json fixture_json() {
  return read_json(std::filesystem::path(ATX_IMPL_TESTS_DIR) / "fixtures" / "factors_verb_v1.json");
}
f64 value_or_nan(const Json& v) { return v.is_null() ? missing : v.get<f64>(); }
} // namespace

// The verb's series equal the fitter's factor_record on the same role: the live decisions exactly,
// f and tau to 1e-12 (the K2 basis is numpy's QR to rounding and the sums run in another order),
// the used rows and the refused decision (too-few-usable-names) exactly, and the report-only
// 21-session series to 1e-12 against its numpy reference (NaN past the window).
TEST(FactorsVerb, EqualsFixture) {
  const auto f = factor_fixture();
  const auto got = compute(f);
  const Json want = fixture_json();
  ASSERT_EQ(want.at("geometry").at("decision_begin"), kScoreBegin);
  ASSERT_EQ(want.at("geometry").at("decision_end_exclusive"), kEnd);
  ASSERT_EQ(got.ctx.outcome.size(), kDecisions);
  const Json& used = want.at("used_rows");
  ASSERT_EQ(used.size(), kDecisions);
  usize refused = 0;
  for (usize j = 0; j < kDecisions; ++j) {
    const auto& outcome = got.ctx.outcome[j];
    const bool has_basis = outcome.refusal == st::BasisRefusal::None;
    EXPECT_EQ(has_basis ? outcome.used_rows : usize{0}, used[j].get<usize>()) << j;
    if (has_basis) continue;
    const Json& r = want.at("refused").at(refused++);
    EXPECT_EQ(r.at("decision_index").get<usize>(), kScoreBegin + j);
    EXPECT_EQ(r.at("reason").get<std::string>(), st::basis_refusal_id(outcome.refusal));
    EXPECT_EQ(r.at("used_rows").get<usize>(), outcome.used_rows);
  }
  EXPECT_EQ(refused, want.at("refused").size());
  EXPECT_EQ(refused, 1U);
  for (usize k = 0; k < kSignals; ++k) {
    const char* name = kSignalNames[k];
    const auto& s = got.series[k];
    const Json& wf = want.at("factor").at(name);
    const Json& wh = want.at("factor_h21").at(name);
    ASSERT_EQ(s.f.size(), kDecisions);
    ASSERT_EQ(wf.size(), kDecisions);
    for (usize j = 0; j < kDecisions; ++j) {
      const f64 ef = value_or_nan(wf[j]), eh = value_or_nan(wh[j]);
      ASSERT_EQ(std::isnan(s.f[j]), std::isnan(ef)) << name << ' ' << j;
      if (!std::isnan(ef)) EXPECT_NEAR(s.f[j], ef, 1e-12) << name << ' ' << j;
      ASSERT_EQ(std::isnan(s.h21[j]), std::isnan(eh)) << name << ' ' << j;
      if (!std::isnan(eh)) EXPECT_NEAR(s.h21[j], eh, 1e-12) << name << ' ' << j;
    }
    EXPECT_NEAR(s.tau, want.at("tau").at(name).get<f64>(), 1e-12) << name;
    EXPECT_EQ(s.live_decisions, want.at("live_decisions").at(name).get<usize>()) << name;
  }
  // Interval 395 of name 5 is guarded (adjusted-only doubling): decision 393 reads it as 0.
  EXPECT_EQ(got.ctx.forward[(393 - kScoreBegin) * kN + 5], 0.0);
  EXPECT_NE(got.ctx.forward[(392 - kScoreBegin) * kN + 5], 0.0);
}

// No decision reads a session after its label: changing every close (adjusted and raw alike, so
// the guard keeps the intervals valid) and every signal from session T on leaves f(d) bit for bit
// for d + 2 < T and the 21-session series for d + 22 < T, while later decisions do move (teeth).
TEST(FactorsVerb, FutureReturnDoesNotChangePast) {
  constexpr usize kT = 412;
  const auto base = factor_fixture();
  auto future = base;
  for (usize t = kT; t < kRoleDates; ++t)
    for (usize i = 0; i < kN; ++i) {
      const usize k = t * kN + i;
      const f64 bump = 1.0 + 0.003 * static_cast<f64>((t * 13 + i * 5) % 11);
      future.close[k] *= bump;
      future.raw[k] *= bump;
      for (auto& s : future.signals)
        if (i % 2 == 0) s[k] += 0.25;
    }
  const auto a = compute(base);
  const auto b = compute(future);
  for (usize k = 0; k < kSignals; ++k) {
    bool f_moved = false, h_moved = false;
    for (usize j = 0; j < kDecisions; ++j) {
      const usize d = kScoreBegin + j;
      const auto& fa = a.series[k];
      const auto& fb = b.series[k];
      if (d + 2 < kT) EXPECT_TRUE(same_bits(fa.f[j], fb.f[j])) << kSignalNames[k] << ' ' << d;
      else f_moved = f_moved || !same_bits(fa.f[j], fb.f[j]);
      if (d + 22 < kT) EXPECT_TRUE(same_bits(fa.h21[j], fb.h21[j])) << kSignalNames[k] << ' ' << d;
      else h_moved = h_moved || !same_bits(fa.h21[j], fb.h21[j]);
    }
    EXPECT_TRUE(f_moved) << kSignalNames[k];
    EXPECT_TRUE(h_moved) << kSignalNames[k];
  }
  for (usize j = 0; j + 2 + kScoreBegin < kT; ++j) {
    const auto row = std::span<const f64>(a.ctx.forward).subspan(j * kN, kN);
    EXPECT_TRUE(same_bits(row, std::span<const f64>(b.ctx.forward).subspan(j * kN, kN))) << j;
  }
}

namespace {
struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<u64> counter{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
        ("atx_factors_" + std::to_string(tick) + "_" + std::to_string(counter.fetch_add(1)));
    if (!std::filesystem::create_directory(path)) throw std::runtime_error("fixture directory");
  }
  ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
};
template <class T> Json payload(const std::filesystem::path& file, const std::vector<T>& data) {
  std::ofstream out(file, std::ios::binary);
  const auto bytes = std::as_bytes(std::span(data));
  // SAFETY: char writes the object representation of contiguous arithmetic fixture data.
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) throw std::runtime_error("fixture payload");
  return Json{{"bytes", bytes.size()}, {"sha256", co::sha256_file(file.string()).value()}};
}
std::string write_json(const std::filesystem::path& path, const Json& j) {
  std::ofstream out(path, std::ios::binary);
  out << j.dump(2) << '\n';
  out.close();
  if (!out) throw std::runtime_error("fixture JSON");
  return co::sha256_file(path.string()).value();
}
template <class T> std::vector<T> read_all(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  std::vector<T> out(static_cast<usize>(in.tellg()) / sizeof(T));
  in.seekg(0);
  // SAFETY: char reads the object representation of a trivially copyable array.
  in.read(reinterpret_cast<char*>(out.data()),
          static_cast<std::streamsize>(out.size() * sizeof(T)));
  return out;
}
struct Role {
  std::string path, sha;
  Json manifest;
};
// An engine-conformant research role (read_strategy_role's contract) on the fixture's arrays over
// kRoleDates daily sessions from kFirstDay, score window [383, kRoleDates).
Role write_role(const std::filesystem::path& dir, const Fixture& f) {
  if (!std::filesystem::create_directory(dir)) throw std::runtime_error("role directory");
  std::vector<i64> sessions(kRoleDates);
  for (usize t = 0; t < kRoleDates; ++t) sessions[t] = (kFirstDay + static_cast<i64>(t)) * kDayNs;
  std::vector<u64> ids(kN);
  for (usize i = 0; i < kN; ++i) ids[i] = 1000 + 7 * i;
  Json files;
  files["sessions.i64"] = payload(dir / "sessions.i64", sessions);
  files["ids.u64"] = payload(dir / "ids.u64", ids);
  files["close.f64"] = payload(dir / "close.f64", f.close);
  files["raw_close.f64"] = payload(dir / "raw_close.f64", f.raw);
  files["volume.f64"] = payload(dir / "volume.f64", f.volume);
  files["present.u8"] = payload(dir / "present.u8", f.present);
  files["member.u8"] = payload(dir / "member.u8", f.member);
  const Json membership{{"rule", "research-prior63-usd-adv-topn-v1"}, {"top_n", kN},
      {"lookback_sessions", 63}, {"lag_sessions", 1}, {"min_raw_price_exclusive", 5},
      {"min_adv_exclusive", 5000000}, {"ties", "securityID-ascending"},
      {"missing", "complete-prior-calendar-window-required"}, {"common_stock_verified", false}};
  Role role;
  role.manifest = Json{{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
      {"instrument_namespace", "spiderrock.securityID"}, {"dates", kRoleDates},
      {"instruments", kN}, {"score_begin", kScoreBegin}, {"score_end", kRoleDates},
      {"score_start_ns", sessions[kScoreBegin]}, {"score_end_ns", sessions.back() + kDayNs},
      {"source_sha256", std::string(64, 'a')}, {"membership_recipe", membership.dump()},
      {"clock_recipe", "modeled-session+22h-mark+23h-decision-v1"},
      {"close_basis", "f64(raw-f32-close)*f64-cumulReturnFactor"},
      {"volume_basis", "raw-share-volume"}, {"common_stock_verified", false},
      {"historical_vintage_verified", false},
      {"declared_output_bytes", kRoleDates * kN * 26 + kRoleDates * 8 + kN * 8},
      {"files", std::move(files)}};
  role.path = (dir / "manifest.json").string();
  role.sha = write_json(role.path, role.manifest);
  return role;
}
// DIR/signals.json over the fixture's signals: s1 by an absolute path, s2 and s3 relative to DIR.
Json write_signals(const std::filesystem::path& dir, const Fixture& f,
                   const std::string& role_sha) {
  if (!std::filesystem::create_directory(dir)) throw std::runtime_error("signals directory");
  Json entries = Json::array();
  for (usize k = 0; k < kSignals; ++k) {
    const std::string file = std::string(kSignalNames[k]) + ".f64";
    const Json written = payload(dir / file, f.signals[k]);
    entries.push_back(Json{{"id", kSignalNames[k]},
                           {"payload", k == 0 ? (dir / file).string() : file},
                           {"payload_sha256", written.at("sha256")},
                           {"layout", "v2"}}); // other entry keys are ignored
  }
  Json index{{"schema", "atx.factor-signals/v1"}, {"role_manifest_sha256", role_sha},
             {"candidates", std::move(entries)}};
  write_json(dir / "signals.json", index);
  return index;
}
int dispatch(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  return st::dispatch_factors(static_cast<int>(argv.size()), argv.data(), out, err);
}
} // namespace

// The verb on a pinned engine role writes the K-P9-4 layout: factor.f64 and factor_h21.f64
// (decisions x candidates, decision-major) and tau.f64 hold factor_series' bits, and
// manifest.json (last) the role pin, the window, the decision sessions, the candidates in index
// order, the used rows, the refusal and the file SHAs.
TEST(FactorsVerb, VerbWritesTheKP94Layout) {
  const auto f = factor_fixture();
  Directory dir;
  const auto role = write_role(dir.path / "role", f);
  const Json index = write_signals(dir.path / "signals", f, role.sha);
  const auto out = dir.path / "factors";
  std::ostringstream o, e;
  ASSERT_EQ(dispatch({"factors", "--role", role.path, "--role-sha256", role.sha, "--signals",
                      (dir.path / "signals").string(), "--output", out.string()}, o, e), 0)
      << e.str();
  const auto expected = compute(f);
  const auto factor = read_all<f64>(out / "factor.f64");
  const auto h21 = read_all<f64>(out / "factor_h21.f64");
  const auto tau = read_all<f64>(out / "tau.f64");
  ASSERT_EQ(factor.size(), kDecisions * kSignals);
  ASSERT_EQ(h21.size(), kDecisions * kSignals);
  ASSERT_EQ(tau.size(), kSignals);
  for (usize k = 0; k < kSignals; ++k) {
    EXPECT_TRUE(same_bits(tau[k], expected.series[k].tau)) << k;
    for (usize j = 0; j < kDecisions; ++j) {
      EXPECT_TRUE(same_bits(factor[j * kSignals + k], expected.series[k].f[j])) << k << ' ' << j;
      EXPECT_TRUE(same_bits(h21[j * kSignals + k], expected.series[k].h21[j])) << k << ' ' << j;
    }
  }
  const Json manifest = read_json(out / "manifest.json");
  EXPECT_EQ(manifest.at("schema"), "atx.factor-series/v1");
  EXPECT_EQ(manifest.at("status"), "complete");
  EXPECT_EQ(manifest.at("contract"), "K-P9-4");
  EXPECT_EQ(manifest.at("role_manifest_sha256"), role.sha);
  EXPECT_EQ(manifest.at("research_window"), "research-window-v2");
  EXPECT_EQ(manifest.at("decision_begin"), kScoreBegin);
  EXPECT_EQ(manifest.at("decision_end_exclusive"), kEnd);
  EXPECT_EQ(manifest.at("decisions"), kDecisions);
  EXPECT_EQ(manifest.at("traded_horizon_sessions"), 21);
  EXPECT_EQ(manifest.at("signals").at("index_sha256"),
            co::sha256_file((dir.path / "signals" / "signals.json").string()).value());
  ASSERT_EQ(manifest.at("decision_sessions_ns").size(), kDecisions);
  EXPECT_EQ(manifest.at("decision_sessions_ns")[0].get<i64>(),
            (kFirstDay + static_cast<i64>(kScoreBegin)) * kDayNs);
  ASSERT_EQ(manifest.at("candidates").size(), kSignals);
  for (usize k = 0; k < kSignals; ++k) {
    EXPECT_EQ(manifest.at("candidates")[k].at("id"), kSignalNames[k]);
    EXPECT_EQ(manifest.at("candidates")[k].at("payload_sha256"),
              index.at("candidates")[k].at("payload_sha256"));
    EXPECT_EQ(manifest.at("candidates")[k].at("live_decisions"),
              expected.series[k].live_decisions);
  }
  ASSERT_EQ(manifest.at("used_rows").size(), kDecisions);
  ASSERT_EQ(manifest.at("refused").size(), 1U);
  EXPECT_EQ(manifest.at("refused")[0].at("decision_index"), kRefusedSession);
  EXPECT_EQ(manifest.at("refused")[0].at("reason"), "too-few-usable-names");
  const auto& files = manifest.at("files");
  for (const char* name : {"factor.f64", "tau.f64", "factor_h21.f64"})
    EXPECT_EQ(files.at(name).at("sha256"), co::sha256_file((out / name).string()).value()) << name;
  EXPECT_EQ(files.at("factor.f64").at("shape"), Json::array({kDecisions, kSignals}));
  EXPECT_NE(o.str().find("factors: 35 decisions x 3 candidates, 1 refused"), std::string::npos)
      << o.str();
}

// Refusals before any output: a wrong role pin, a signals index bound to another role, a payload
// whose SHA-256 differs, a role whose score window reaches the seal (no payload is opened), an
// existing output; usage errors exit 2.
TEST(FactorsVerb, RefusesBeforeAnyOutput) {
  const auto f = factor_fixture();
  Directory dir;
  const auto role = write_role(dir.path / "role", f);
  const auto signals = dir.path / "signals";
  Json index = write_signals(signals, f, role.sha);
  const auto out = dir.path / "never";
  std::ostringstream o, e;
  const auto run = [&](const std::string& path, const std::string& sha) {
    return dispatch({"factors", "--role", path, "--role-sha256", sha, "--signals",
                     signals.string(), "--output", out.string()}, o, e);
  };
  EXPECT_EQ(run(role.path, std::string(64, 'b')), 1); // the index is bound to role.sha
  EXPECT_FALSE(std::filesystem::exists(out));
  index["role_manifest_sha256"] = std::string(64, 'b');
  write_json(signals / "signals.json", index);
  e.str("");
  EXPECT_EQ(run(role.path, std::string(64, 'b')), 1); // the role manifest is not bbb...
  EXPECT_NE(e.str().find("pin differs"), std::string::npos) << e.str();
  EXPECT_FALSE(std::filesystem::exists(out));
  index["role_manifest_sha256"] = std::string(64, 'c');
  write_json(signals / "signals.json", index);
  EXPECT_EQ(run(role.path, role.sha), 1);
  EXPECT_FALSE(std::filesystem::exists(out));
  index["role_manifest_sha256"] = role.sha;
  index["candidates"][1]["payload_sha256"] = std::string(64, 'd');
  write_json(signals / "signals.json", index);
  e.str("");
  EXPECT_EQ(run(role.path, role.sha), 1);
  EXPECT_NE(e.str().find("candidate s2"), std::string::npos) << e.str();
  EXPECT_FALSE(std::filesystem::exists(out));
  auto sealed = role.manifest;
  sealed["score_end_ns"] = 1'704'153'600'000'000'000LL; // 2024-01-02
  const auto sealed_path = (dir.path / "role" / "sealed.json").string();
  const auto sealed_sha = write_json(sealed_path, sealed);
  index["role_manifest_sha256"] = sealed_sha;
  write_json(signals / "signals.json", index);
  e.str("");
  EXPECT_EQ(run(sealed_path, sealed_sha), 1);
  EXPECT_NE(e.str().find("research-window-v2"), std::string::npos) << e.str();
  EXPECT_FALSE(std::filesystem::exists(out));
  ASSERT_TRUE(std::filesystem::create_directory(out));
  EXPECT_EQ(run(role.path, role.sha), 1);
  EXPECT_TRUE(std::filesystem::is_empty(out));
  EXPECT_EQ(dispatch({"factors", "--role", role.path, "--output", (dir.path / "x").string()}, o,
                     e), 2);
  EXPECT_EQ(dispatch({"factors", "--role", role.path, "--bogus", "1"}, o, e), 2);
  EXPECT_EQ(dispatch({"factors", "--max-bytes", "x"}, o, e), 2);
  EXPECT_FALSE(std::filesystem::exists(dir.path / "x"));
}
