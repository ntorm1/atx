#pragma once

// P9 lane COV (cov-design 3.3, K-P9-12): the synthetic panel of the CovTruncation / CovEigen /
// CovBias / CovRecipe / CovStore / CovDiff suites and their harness. The panel plants factor
// returns (market, four FF49 industries, a value loading) with two IPOs, two delistings,
// membership churn, missing descriptors and one thin industry (pooled into the residual slot);
// truncate_after / perturb_after / add_future_instruments derive the counterfactual panels. The
// harness runs run_risk_model with an in-memory container sink (cov_dense_row -> cov_block_of ->
// encode_cov_block, exactly the verb's ContainerSink without the file) and compares block bytes.
// Synthetic inputs only.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/risk/cov_container.hpp"
#include "../src/strategy_risk_model.hpp"

namespace atx_cov_test {
using atx::f32;
using atx::f64;
using atx::i64;
using atx::u32;
using atx::u64;
using atx::u8;
using atx::usize;
namespace rk = atx::impl::strategy::risk;
namespace er = atx::engine::risk;
namespace co = atx::core;
namespace fs = std::filesystem;
using Json = nlohmann::json;

inline constexpr i64 day_ns = 86'400'000'000'000LL;
inline constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();
inline constexpr usize value_style = static_cast<usize>(rk::Style::Value);

struct Normal {
  u64 state;
  f64 uniform() { // (0, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<f64>(state >> 11U) + 0.5) * 0x1.0p-53;
  }
  f64 operator()() {
    const f64 a = uniform(), b = uniform();
    return std::sqrt(-2.0 * std::log(a)) * std::cos(2.0 * std::numbers::pi * b);
  }
};

inline std::vector<i64> weekday_sessions(usize count) {
  std::vector<i64> out;
  auto date = std::chrono::sys_days{std::chrono::year{2019} / 1 / 2};
  while (out.size() < count) {
    const std::chrono::weekday wd{date};
    if (wd != std::chrono::Saturday && wd != std::chrono::Sunday)
      out.push_back(static_cast<i64>(date.time_since_epoch().count()) * day_ns);
    date += std::chrono::days{1};
  }
  return out;
}

// The planted panel (cov-design 3.3). Names 0 / 1 list at sessions ipo[0] / ipo[1]; names 2 / 3
// delist after del[0] / del[1]; names i % 9 == 4 leave the membership on every second 25-session
// span; the value descriptor is missing where (i + t) % 11 == 0; names i % 25 == 7 sit in FF49
// industry 5 (fewer than 10 names: pooled into the residual slot), the rest in 1 + i % 4. Absent
// cells carry NaN prices and cap (the role contract). Instrument ids are 1000 + 2 i, so
// add_future_instruments can interleave odd ids.
struct CovPanel {
  usize d{}, n{};
  std::vector<i64> sessions;
  std::vector<u64> ids;
  std::vector<f64> close, raw, volume, cap;
  std::vector<u8> present, member, industry;
  std::vector<f32> value, earnings;
  std::array<usize, 2> ipo{}, del{};

  CovPanel() = default;
  CovPanel(usize dates, usize names, u64 seed)
      : d(dates), n(names), sessions(weekday_sessions(dates)), close(dates * names, missing),
        raw(dates * names, missing), volume(dates * names, missing), cap(dates * names, missing),
        present(dates * names, 0), member(dates * names, 0), industry(dates * names, 0),
        value(dates * names, std::numeric_limits<f32>::quiet_NaN()),
        earnings(dates * names, std::numeric_limits<f32>::quiet_NaN()) {
    ipo = {dates / 5, dates / 3};
    del = {dates / 2, (2 * dates) / 3};
    Normal rng{seed};
    std::vector<f64> caps(n), v(n), e(n), sigma(n);
    for (usize i = 0; i < n; ++i) {
      ids.push_back(1000 + 2 * i);
      caps[i] = 1e9 * std::exp(rng());
      v[i] = rng();
      e[i] = rng();
      sigma[i] = 0.01 + 0.015 * rng.uniform();
    }
    for (usize t = 0; t < d; ++t) {
      const f64 market = 0.01 * rng(), loading = 0.004 * rng(), carry = 0.002 * rng();
      std::array<f64, 5> ind{};
      for (usize g = 0; g < 4; ++g) ind[g] = 0.005 * rng();
      ind[4] = 0.003 * rng();
      for (usize i = 0; i < n; ++i) {
        const usize k = t * n + i;
        if (!listed(i, t)) {
          (void)rng();
          continue;
        }
        present[k] = 1;
        member[k] = (i % 9 == 4 && (t / 25) % 2 == 1) ? u8{0} : u8{1};
        industry[k] = static_cast<u8>(industry_of(i));
        cap[k] = caps[i] * (1.0 + 0.001 * static_cast<f64>(t % 7));
        value[k] = (i + t) % 11 == 0 ? std::numeric_limits<f32>::quiet_NaN() : static_cast<f32>(v[i]);
        earnings[k] = static_cast<f32>(e[i]);
        volume[k] = 2e5 * (1.0 + 0.1 * static_cast<f64>(i % 5));
        const bool first = t == 0 || !present[k - n];
        const f64 r = market + ind[industry_of(i) - 1] + loading * v[i] + carry * e[i] +
                      sigma[i] * rng();
        close[k] = first ? 40.0 + static_cast<f64>(i % 20) : close[k - n] * (1.0 + r);
        raw[k] = close[k];
      }
    }
  }
  [[nodiscard]] static usize industry_of(usize i) { return i % 25 == 7 ? 5 : 1 + i % 4; }
  [[nodiscard]] bool listed(usize i, usize t) const {
    if (i < 2) return t >= ipo[i];
    if (i < 4) return t <= del[i - 2];
    return true;
  }
  [[nodiscard]] rk::RiskPanel panel() const {
    rk::RiskPanel p;
    p.dates = d; p.instruments = n; p.sessions = sessions; p.ids = ids;
    p.close = close; p.raw_close = raw; p.volume = volume; p.present = present; p.member = member;
    p.cap = cap; p.industry = industry;
    p.descriptors[0] = value;
    p.descriptors[1] = earnings;
    return p;
  }
  // The panel through session c (c + 1 sessions; nothing after c exists).
  [[nodiscard]] CovPanel truncate_after(usize c) const {
    CovPanel out = *this;
    out.d = c + 1;
    out.sessions.resize(out.d);
    const usize cells = out.d * n;
    for (auto* v : {&out.close, &out.raw, &out.volume, &out.cap}) v->resize(cells);
    for (auto* v : {&out.present, &out.member, &out.industry}) v->resize(cells);
    for (auto* v : {&out.value, &out.earnings}) v->resize(cells);
    return out;
  }
  // Every value after c replaced (prices, volume, membership, caps, descriptors, industries) on
  // the same instrument axis; listing and the session axis kept.
  [[nodiscard]] CovPanel perturb_after(usize c, u64 seed) const {
    CovPanel out = *this;
    Normal rng{seed};
    for (usize t = c + 1; t < d; ++t)
      for (usize i = 0; i < n; ++i) {
        const usize k = t * n + i;
        if (!present[k]) continue;
        out.member[k] = i % 7 == 0 ? static_cast<u8>(1 - member[k]) : member[k];
        out.cap[k] = cap[k] * std::exp(0.5 * rng());
        out.volume[k] = volume[k] * (1.0 + rng.uniform());
        out.value[k] = static_cast<f32>(rng());
        out.earnings[k] = (i + t) % 5 == 0 ? std::numeric_limits<f32>::quiet_NaN()
                                           : static_cast<f32>(rng());
        out.industry[k] = static_cast<u8>(i % 6 == 0 ? 1 + (i / 6) % 4 : industry[k]);
        const bool first = t == 0 || !present[k - n];
        const f64 r = 0.03 * rng();
        out.close[k] = first ? 33.0 : out.close[k - n] * (1.0 + r);
        out.raw[k] = out.close[k];
      }
    return out;
  }
  // `count` instruments that first appear after c (odd ids interleaved with the panel's, so the
  // role index of every later name shifts): absent and not members through c, listed after.
  [[nodiscard]] CovPanel add_future_instruments(usize c, usize count) const {
    struct Column { u64 id; usize source; bool fresh; };
    std::vector<Column> columns;
    for (usize i = 0; i < n; ++i) columns.push_back({ids[i], i, false});
    for (usize j = 0; j < count; ++j) columns.push_back({1001 + 2 * (j * (n / count)), j, true});
    std::sort(columns.begin(), columns.end(),
              [](const Column& a, const Column& b) { return a.id < b.id; });
    CovPanel out;
    out.d = d; out.n = columns.size(); out.sessions = sessions; out.ipo = ipo; out.del = del;
    const usize cells = out.d * out.n;
    out.close.assign(cells, missing); out.raw.assign(cells, missing);
    out.volume.assign(cells, missing); out.cap.assign(cells, missing);
    out.present.assign(cells, 0); out.member.assign(cells, 0); out.industry.assign(cells, 0);
    out.value.assign(cells, std::numeric_limits<f32>::quiet_NaN());
    out.earnings.assign(cells, std::numeric_limits<f32>::quiet_NaN());
    Normal rng{991};
    for (usize col = 0; col < out.n; ++col) {
      const auto& c0 = columns[col];
      out.ids.push_back(c0.id);
      for (usize t = 0; t < d; ++t) {
        const usize to = t * out.n + col;
        if (!c0.fresh) {
          const usize from = t * n + c0.source;
          out.close[to] = close[from]; out.raw[to] = raw[from]; out.volume[to] = volume[from];
          out.cap[to] = cap[from]; out.present[to] = present[from]; out.member[to] = member[from];
          out.industry[to] = industry[from]; out.value[to] = value[from];
          out.earnings[to] = earnings[from];
          continue;
        }
        if (t <= c) continue;
        out.present[to] = 1; out.member[to] = 1;
        out.industry[to] = static_cast<u8>(1 + c0.source % 4);
        out.cap[to] = 2e9; out.volume[to] = 3e5;
        out.value[to] = static_cast<f32>(rng()); out.earnings[to] = static_cast<f32>(rng());
        out.close[to] = t == c + 1 ? 25.0 : out.close[to - out.n] * (1.0 + 0.02 * rng());
        out.raw[to] = out.close[to];
      }
    }
    return out;
  }
  // The full-sample mean of the value descriptor (every session: the planted leak's statistic).
  [[nodiscard]] f64 full_sample_value_mean() const {
    f64 sum = 0;
    usize count = 0;
    for (const f32 x : value)
      if (std::isfinite(x)) { sum += static_cast<f64>(x); ++count; }
    return count ? sum / static_cast<f64>(count) : 0.0;
  }
};

// Short windows so every stage (fully observed block, S2b, structural specific risk, VRA) runs
// within ~150 sessions on ~160 names; the estimator code is the recipe's, only the windows differ.
inline rk::RiskModelConfig small_config(rk::RiskRecipe recipe) {
  rk::RiskModelConfig c;
  c.recipe = recipe;
  c.vol_halflife = 21; c.correlation_halflife = 504; c.nw_halflife = 42; c.vra_halflife = 10;
  c.vra_min_dates = 5; c.min_factor_history = 20; c.specific_halflife = 21;
  c.structural_history = 30; c.beta_window = 40; c.min_beta_pairs = 20;
  c.momentum_window = 40; c.momentum_skip = 5; c.adv_window = 10;
  c.min_regression_names = 60;
  return c;
}

// The container spec of the in-memory sinks (geometry 62 / 50 / 11, f32 exposures).
inline er::CovContainerSpec memory_spec() {
  er::CovContainerSpec spec;
  spec.groups = rk::cov_groups;
  spec.dense = rk::cov_dense;
  spec.f32_exposures = true;
  spec.recipe_json = "{}";
  spec.inputs_json = "{}";
  spec.meta_json = "{}";
  for (usize k = 0; k < rk::factor_count; ++k) spec.factor_names.push_back(rk::factor_name(k));
  return spec;
}

// The in-memory container sink: one encoded block per session (the verb's ContainerSink without
// the file). leak != 0 plants a look-ahead: every nonzero value-style exposure is shifted by
// leak x the panel's full-sample value mean (a statistic over sessions after t).
struct BlockLog final : rk::RiskSink {
  std::vector<u64> ids;
  f64 leak{};
  std::vector<std::vector<u8>> blocks;
  std::vector<i64> sessions, history_first;
  std::vector<u32> flags;
  std::optional<i64> first;
  rk::CovDayScratch day;
  rk::CovBlockScratch scratch;
  er::CovContainerSpec spec{memory_spec()};
  co::Status on_day(const rk::RiskDay& d) override {
    if (d.returns_entered && !first) first = d.session;
    const rk::CovDenseRow row = rk::cov_dense_row(d, ids, first.value_or(d.session), day);
    if (leak != 0.0)
      for (usize i = 0; i < ids.size(); ++i) {
        f32& z = day.styles[i * rk::style_count + value_style];
        if (z != 0.0F) z = static_cast<f32>(static_cast<f64>(z) - leak);
      }
    ATX_TRY(const auto input, rk::cov_block_of(row, scratch));
    std::vector<u8> bytes;
    ATX_TRY_VOID(er::encode_cov_block(spec, input, bytes));
    blocks.push_back(std::move(bytes));
    sessions.push_back(d.session);
    history_first.push_back(input.history_first_ns);
    flags.push_back(input.flags);
    return co::Ok();
  }
};
inline BlockLog run_blocks(const CovPanel& p, const rk::RiskModelConfig& cfg, f64 leak_scale = 0.0,
                           std::span<rk::RiskSink* const> extra = {}) {
  BlockLog log;
  log.ids = p.ids;
  log.leak = leak_scale * p.full_sample_value_mean();
  const auto panel = p.panel();
  std::vector<rk::RiskSink*> sinks{&log};
  sinks.insert(sinks.end(), extra.begin(), extra.end());
  const auto status = rk::run_risk_model(panel, cfg, sinks);
  if (!status) throw std::runtime_error("run_risk_model: " + status.error().to_string());
  return log;
}
// The first session index t <= through whose block bytes (or stamp) differ; nullopt when none.
inline std::optional<usize> first_violation(const BlockLog& a, const BlockLog& b, usize through) {
  for (usize t = 0; t <= through; ++t) {
    if (t >= a.blocks.size() || t >= b.blocks.size()) return t;
    if (a.blocks[t] != b.blocks[t] || a.history_first[t] != b.history_first[t] ||
        a.flags[t] != b.flags[t] || a.sessions[t] != b.sessions[t])
      return t;
  }
  return std::nullopt;
}
inline er::CovBlockView parse_block(const std::vector<u8>& bytes) {
  auto v = er::parse_cov_block(bytes, static_cast<u32>(rk::factor_count), rk::cov_groups,
                               rk::cov_dense, true);
  if (!v) throw std::runtime_error("parse_cov_block: " + v.error().to_string());
  return *v;
}
template <class T>
bool same_bits(std::span<const T> a, std::span<const T> b) {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}
// Two blocks equal after projecting rows by instrument id (the role index may shift).
inline bool same_by_id(const std::vector<u8>& a_bytes, const std::vector<u8>& b_bytes) {
  const auto a = parse_block(a_bytes), b = parse_block(b_bytes);
  if (a.as_of != b.as_of || a.cutoff != b.cutoff || a.rows != b.rows || a.priced != b.priced ||
      !same_bits(a.factor_cov, b.factor_cov) || !same_bits(a.factor_return, b.factor_return) ||
      !same_bits(a.factor_flags, b.factor_flags) || !same_bits(a.diag, b.diag) ||
      !same_bits(a.eigen_gamma, b.eigen_gamma))
    return false;
  using Row = std::tuple<u32, u64, std::vector<u32>, u8>;
  const auto rows_of = [](const er::CovBlockView& v) {
    std::map<u64, Row> out;
    for (usize r = 0; r < v.rows; ++r) {
      std::vector<u32> x(rk::style_count);
      std::memcpy(x.data(), v.exposures_f32.data() + r * rk::style_count, rk::style_count * sizeof(f32));
      out[v.ids[r]] = Row{v.group[r], std::bit_cast<u64>(v.specific[r]), std::move(x),
                          v.row_flags.empty() ? u8{0} : v.row_flags[r]};
    }
    return out;
  };
  return rows_of(a) == rows_of(b);
}

// ---- the verb on a pinned synthetic role ------------------------------------------------------
struct Directory {
  fs::path path;
  Directory() {
    static std::atomic<u64> counter{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           ("atx_cov_impl_" + std::to_string(tick) + "_" + std::to_string(counter.fetch_add(1)));
    if (!fs::create_directory(path)) throw std::runtime_error("fixture directory");
  }
  ~Directory() { std::error_code ec; fs::remove_all(path, ec); }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
};
template <class T>
Json write_payload(const fs::path& file, const std::vector<T>& values) {
  std::ofstream out(file, std::ios::binary);
  const auto bytes = std::as_bytes(std::span(values));
  // SAFETY: char writes the object representation of contiguous arithmetic fixture data.
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) throw std::runtime_error("fixture payload write");
  return Json{{"bytes", bytes.size()}, {"sha256", co::sha256_file(file.string()).value()}};
}
inline std::string write_json(const fs::path& path, const Json& value) {
  std::ofstream out(path, std::ios::binary);
  out << value.dump(2) << '\n';
  out.close();
  if (!out) throw std::runtime_error("fixture JSON write");
  return co::sha256_file(path.string()).value();
}
inline std::vector<u8> file_bytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) throw std::runtime_error("fixture read " + path.string());
  std::vector<u8> out(static_cast<usize>(in.tellg()));
  in.seekg(0);
  // SAFETY: char reads the object representation of a byte buffer.
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  return out;
}
inline Json read_json(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Json::parse(in);
}
struct Pinned {
  std::string role, role_sha, fields, fields_sha;
};
// Role (atx.recent-research-role/v1) and fields (me_company, grp_ff49, be, ni_ttm) of `p`.
inline Pinned write_role_and_fields(const fs::path& dir, const CovPanel& p) {
  const auto role_dir = dir / "role", fields_dir = dir / "fields";
  fs::create_directories(role_dir);
  fs::create_directories(fields_dir);
  Json files = Json::object();
  files["sessions.i64"] = write_payload(role_dir / "sessions.i64", p.sessions);
  files["ids.u64"] = write_payload(role_dir / "ids.u64", p.ids);
  files["close.f64"] = write_payload(role_dir / "close.f64", p.close);
  files["raw_close.f64"] = write_payload(role_dir / "raw_close.f64", p.raw);
  files["volume.f64"] = write_payload(role_dir / "volume.f64", p.volume);
  files["present.u8"] = write_payload(role_dir / "present.u8", p.present);
  files["member.u8"] = write_payload(role_dir / "member.u8", p.member);
  const Json role{{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
      {"volume_basis", "raw-share-volume"}, {"dates", p.d}, {"instruments", p.n}, {"files", files}};
  Pinned out;
  out.role = (role_dir / "manifest.json").string();
  out.role_sha = write_json(role_dir / "manifest.json", role);
  const usize cells = p.d * p.n;
  std::vector<f64> industry(cells), be(cells, missing), ni(cells, missing);
  for (usize k = 0; k < cells; ++k) {
    industry[k] = p.industry[k] ? static_cast<f64>(p.industry[k]) : missing;
    if (std::isfinite(p.value[k]) && std::isfinite(p.cap[k])) be[k] = static_cast<f64>(p.value[k]) * p.cap[k];
    if (std::isfinite(p.earnings[k]) && std::isfinite(p.cap[k]))
      ni[k] = static_cast<f64>(p.earnings[k]) * p.cap[k] * 0.1;
  }
  Json field_list = Json::array(), field_files = Json::object();
  const auto add = [&](const std::string& name, const std::vector<f64>& values) {
    const auto receipt = write_payload(fields_dir / (name + ".f64"), values);
    field_files[name + ".f64"] = receipt;
    field_list.push_back(Json{{"name", name}, {"file", name + ".f64"}, {"dtype", "<f8"},
        {"layout", "date-major"}, {"shape", Json::array({p.d, p.n})},
        {"sha256", receipt.at("sha256")}, {"point_in_time", true}});
  };
  add("me_company", p.cap);
  add("grp_ff49", industry);
  add("be", be);
  add("ni_ttm", ni);
  const Json fields{{"schema", "atx.research-role-fields/v1"}, {"status", "complete"},
      {"role", {{"manifest_sha256", out.role_sha},
                {"sessions_sha256", files.at("sessions.i64").at("sha256")},
                {"ids_sha256", files.at("ids.u64").at("sha256")},
                {"member_sha256", files.at("member.u8").at("sha256")}}},
      {"fields", field_list}, {"files", field_files}};
  out.fields = (fields_dir / "manifest.json").string();
  out.fields_sha = write_json(fields_dir / "manifest.json", fields);
  return out;
}
inline int run_risk_verb(const Pinned& s, const fs::path& output, std::string& error,
                         std::initializer_list<const char*> extra = {}) {
  std::vector<std::string> args{"risk", "--role", s.role, "--role-sha256", s.role_sha,
      "--fields", s.fields, "--fields-sha256", s.fields_sha, "--output", output.string(),
      "--random-portfolios", "4"};
  args.insert(args.end(), extra.begin(), extra.end());
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  std::ostringstream out, err;
  const int code = rk::dispatch_risk_model(static_cast<int>(argv.size()), argv.data(), out, err);
  error = err.str();
  return code;
}
inline int run_tool(int (*verb)(int, char**, std::ostream&, std::ostream&),
                    std::vector<std::string> args, std::string& output) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  std::ostringstream out, err;
  const int code = verb(static_cast<int>(argv.size()), argv.data(), out, err);
  output = out.str() + err.str();
  return code;
}

// A legacy store (spo fixture write_risk_model, or any `risk` output with --emit-exposures all)
// given the container a `risk --emit-container` run would carry: every date's dense row (the
// legacy files, diagnostics.csv's session and forecast flag) through cov_block_of into
// model.atxcov, the manifest's `container` block and files entry added. The role pin must be 64
// lowercase hex. Returns the new manifest's SHA-256.
inline std::string add_container(const fs::path& dir) {
  Json m = read_json(dir / "manifest.json");
  const usize dates = m.at("geometry").at("dates").get<usize>();
  const usize names = m.at("geometry").at("instruments").get<usize>();
  constexpr usize kf = rk::factor_count, ks = rk::style_count;
  const auto cov = file_bytes(dir / "factor_covariance.f64");
  const auto spec_bytes = file_bytes(dir / "specific_variance.f64");
  const auto style_bytes = file_bytes(dir / "style_exposures.f32");
  const auto slots = file_bytes(dir / "industry_slot.u8");
  std::vector<i64> sessions;
  std::vector<u8> forecast;
  {
    std::ifstream csv(dir / "diagnostics.csv", std::ios::binary);
    std::string line;
    std::getline(csv, line);
    while (std::getline(csv, line)) {
      if (line.empty()) continue;
      const auto a = line.find(','), b = line.find(',', a + 1);
      sessions.push_back(std::stoll(line.substr(0, a)));
      forecast.push_back(line.substr(a + 1, b - a - 1) == "1" ? u8{1} : u8{0});
    }
  }
  er::CovContainerSpec spec = memory_spec();
  spec.role_sha256 = er::cov_parse_hex(m.at("role").at("manifest_sha256").get<std::string>()).value();
  auto writer = er::CovContainerWriter::create((dir / "model.atxcov").string(), spec);
  if (!writer) throw std::runtime_error(writer.error().to_string());
  std::vector<u64> ids(names);
  for (usize i = 0; i < names; ++i) ids[i] = 1 + i;
  std::vector<f64> c(kf * kf), s(names);
  std::vector<f32> x(names * ks);
  std::vector<u8> slot(names);
  rk::CovBlockScratch scratch;
  for (usize t = 0; t < dates; ++t) {
    std::memcpy(c.data(), cov.data() + t * kf * kf * sizeof(f64), kf * kf * sizeof(f64));
    std::memcpy(s.data(), spec_bytes.data() + t * names * sizeof(f64), names * sizeof(f64));
    std::memcpy(x.data(), style_bytes.data() + t * names * ks * sizeof(f32), names * ks * sizeof(f32));
    std::memcpy(slot.data(), slots.data() + t * names, names);
    rk::CovDenseRow row;
    row.date = t; row.session = sessions[t]; row.history_first = sessions[t];
    row.flags = forecast[t] ? er::cov::date_forecast : 0U;
    row.ids = ids; row.slot = slot; row.specific = s; row.styles = x; row.covariance = c;
    const auto input = rk::cov_block_of(row, scratch);
    if (!input) throw std::runtime_error(input.error().to_string());
    const auto appended = writer->append(*input);
    if (!appended) throw std::runtime_error(appended.error().to_string());
  }
  const auto root = writer->finalize();
  if (!root) throw std::runtime_error(root.error().to_string());
  const auto bytes = fs::file_size(dir / "model.atxcov");
  const auto sha = co::sha256_file((dir / "model.atxcov").string()).value();
  m["files"]["model.atxcov"] = Json{{"sha256", sha}, {"bytes", bytes}};
  m["container"] = Json{{"file", "model.atxcov"}, {"format", std::string(er::cov::format_name)},
                        {"bytes", bytes}, {"file_sha256", sha}, {"root_sha256", er::cov_hex(*root)}};
  return write_json(dir / "manifest.json", m);
}
} // namespace atx_cov_test
