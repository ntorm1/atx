#include "strategy_factors_verb.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/combine/group_rerank.hpp"
#include "atx/engine/data/research_window.hpp"
#include "strategy_ic_detail.hpp"
#include "strategy_research_role.hpp"

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
namespace ed = atx::engine::data;
namespace icd = ic_detail;
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 kMinResidualFraction = 1e-9; // fit_composition_weights.py MIN_RESIDUAL_FRACTION
constexpr u64 kSignalsLimit = 16ULL << 20;
constexpr usize kMaxCandidates = 20000;
constexpr const char* signals_schema = "atx.factor-signals/v1";
constexpr const char* series_schema = "atx.factor-series/v1";
// fit_composition_weights.py FACTOR_SEMANTICS, which these files hold.
constexpr const char* factor_semantics =
    "unsigned-centered-tied-rank;used-rows-finite-signal;ols-residual-2-pass;gross1;"
    "residual>1e-9*entry;f=sum(q*fwd);flat=NaN;tau=mean-consecutive-no-drift;v1";
// The context: the K2 export's (strategy_exposures_verb.cpp), forward returns read as 0 when
// invalid, as the fitter's Context.
constexpr const char* context_semantics =
    "price-risk-v1;beta252-min126-all-instrument-market;vol63-min32;ladv63;"
    "used=member&present&ok;z-sample-sd-clip5;min50;pivot1e-8;"
    "qr-basis-householder-lapack-sign;fwd=r[d+2]-valid-else-0;v1";
constexpr const char* horizon_semantics =
    "report-only;h=sum(q*sum_{j<21}fwd[d+j]);intervals-d+2..d+22;flat-or-past-window=NaN;v1";

co::Error refusal(co::ErrorCode code, const std::string& message) {
  return co::Error{code, "factors: " + message};
}
// Printable ASCII without the CSV separators, quotes or backslash: the id lands in CSV cells.
bool safe_id(std::string_view s) {
  return !s.empty() && s.size() <= 256 && std::all_of(s.begin(), s.end(), [](char c) {
    return c > ' ' && c <= '~' && c != ',' && c != ';' && c != '"' && c != '\\';
  });
}
// y - Q Q'y on the used rows of one decision (q: rows x kBasisColumns, row-major), the sums in
// ascending order as the fitter's einsum states them (coefficients, then each row's fit).
void project_out(std::span<const f64> q, std::span<f64> x) noexcept {
  std::array<f64, kBasisColumns> coef{};
  const usize m = x.size();
  for (usize k = 0; k < kBasisColumns; ++k)
    for (usize r = 0; r < m; ++r) coef[k] += q[r * kBasisColumns + k] * x[r];
  for (usize r = 0; r < m; ++r) {
    f64 fit = 0.0;
    for (usize k = 0; k < kBasisColumns; ++k) fit += q[r * kBasisColumns + k] * coef[k];
    x[r] -= fit;
  }
}
void write_f64(std::ofstream& file, std::span<const f64> values) {
  const auto bytes = std::as_bytes(values);
  // SAFETY: char writes the object representation of contiguous f64 data (the role reader
  // admits little-endian hosts only, so the files are little-endian).
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
}

struct SignalEntry {
  std::string id;
  fs::path payload;
  std::string sha256;
};
struct SignalIndex {
  std::vector<SignalEntry> entries;
  std::string sha256;
};
// DIR/signals.json, bound to the pinned role.
co::Result<SignalIndex> read_signals(const fs::path& dir, const std::string& role_sha) {
  const fs::path path = dir / "signals.json";
  ATX_TRY(const std::string text, icd::metadata_text(path.string(), kSignalsLimit));
  SignalIndex out;
  ATX_TRY(out.sha256, co::sha256_hex(text));
  const Json j = Json::parse(text);
  if (!j.is_object() || j.value("schema", std::string{}) != signals_schema)
    return co::Err(refusal(co::ErrorCode::InvalidArgument, "not an atx.factor-signals/v1 index"));
  if (j.value("role_manifest_sha256", std::string{}) != role_sha)
    return co::Err(refusal(co::ErrorCode::InvalidArgument,
                           "the signals index is bound to another role manifest"));
  const Json& list = j.at("candidates");
  if (!list.is_array() || list.empty() || list.size() > kMaxCandidates)
    return co::Err(refusal(co::ErrorCode::InvalidArgument, "signals: candidates list"));
  std::set<std::string> seen;
  for (const Json& c : list) {
    if (!c.is_object())
      return co::Err(refusal(co::ErrorCode::InvalidArgument, "signals: an entry is no object"));
    SignalEntry e{c.at("id").get<std::string>(), fs::path(c.at("payload").get<std::string>()),
                  c.at("payload_sha256").get<std::string>()};
    if (!safe_id(e.id) || e.payload.empty() || !icd::hash_valid(e.sha256) ||
        !seen.insert(e.id).second)
      return co::Err(refusal(co::ErrorCode::InvalidArgument,
                             "signals: bad or repeated entry " + e.id));
    if (e.payload.is_relative()) e.payload = dir / e.payload;
    out.entries.push_back(std::move(e));
  }
  return co::Ok(std::move(out));
}
Json file_entry(const std::string& sha, u64 bytes, Json shape, const char* layout) {
  return Json{{"bytes", bytes}, {"sha256", sha}, {"dtype", "<f8"}, {"shape", std::move(shape)},
              {"layout", layout}};
}

// The computed series of every candidate, decision-major.
struct FactorTable {
  std::vector<f64> factor, h21, tau;
  std::vector<usize> live;
};
co::Result<FactorTable> factor_table(const FactorContext& ctx, const SignalIndex& index,
                                     usize cells) {
  const usize k_count = index.entries.size(), d_count = ctx.decisions;
  FactorTable out;
  out.factor.assign(d_count * k_count, kNaN);
  out.h21.assign(d_count * k_count, kNaN);
  std::vector<f64> signal;
  FactorScratch scratch;
  for (usize k = 0; k < k_count; ++k) {
    const SignalEntry& e = index.entries[k];
    const auto loaded = icd::load_pinned_f64(e.payload, e.sha256, cells, signal,
                                             "factors signal payload " + e.id, nullptr, true);
    if (!loaded)
      return co::Err(refusal(loaded.error().code(), "candidate " + e.id + ": " +
                                                        loaded.error().message()));
    ATX_TRY(const FactorSeries series, factor_series(ctx, signal, scratch));
    for (usize j = 0; j < d_count; ++j) {
      out.factor[j * k_count + k] = series.f[j];
      out.h21[j * k_count + k] = series.h21[j];
    }
    out.tau.push_back(series.tau);
    out.live.push_back(series.live_decisions);
  }
  return co::Ok(std::move(out));
}
// Writes `values` to root/name; returns its manifest entry.
co::Result<Json> publish_payload(const fs::path& root, const char* name,
                                 std::span<const f64> values, Json shape, const char* layout) {
  {
    std::ofstream file(root / name, std::ios::binary);
    write_f64(file, values);
    file.close();
    if (!file) return co::Err(refusal(co::ErrorCode::IoError, std::string("write ") + name));
  }
  ATX_TRY(auto sha, co::sha256_file((root / name).string()));
  return co::Ok(file_entry(sha, values.size() * sizeof(f64), std::move(shape), layout));
}
} // namespace

co::Result<FactorContext> build_factor_context(const PriceExposureInput& panel,
                                               std::span<const u8> member,
                                               const PriceExposureConfig& cfg, usize begin,
                                               usize end) {
  const usize n = panel.instruments;
  if (!n || begin >= end || end + 2 > panel.dates || member.size() != panel.present.size())
    return co::Err(co::ErrorCode::InvalidArgument, "factors: context geometry");
  try {
    FactorContext ctx;
    ctx.instruments = n;
    ctx.begin = begin;
    ctx.decisions = end - begin;
    const usize d_count = ctx.decisions;
    ctx.row_begin.reserve(d_count + 1);
    ctx.row_begin.push_back(0);
    ctx.forward.resize(d_count * n);
    ctx.outcome.reserve(d_count);
    ExposureExportScratch scratch;
    enable_session_ring(scratch.exposure, panel); // each session logged once
    std::vector<f64> basis(n * kBasisColumns), forward(n);
    for (usize j = 0; j < d_count; ++j) {
      ATX_TRY(const auto decision,
              export_decision(panel, member, cfg, begin + j, scratch, basis, forward));
      for (usize i = 0; i < n; ++i) {
        // basis[i][0] != 0 iff name i is used at the decision (strategy_exposures_verb.hpp).
        if (basis[i * kBasisColumns] != 0.0) {
          const auto q = std::span<const f64>(basis).subspan(i * kBasisColumns, kBasisColumns);
          ctx.rows.push_back(i);
          ctx.basis.insert(ctx.basis.end(), q.begin(), q.end());
        }
        ctx.forward[j * n + i] = std::isnan(forward[i]) ? 0.0 : forward[i];
      }
      ctx.row_begin.push_back(ctx.rows.size());
      ctx.outcome.push_back(decision);
    }
    ctx.forward_h21.assign(d_count * n, kNaN);
    for (usize j = 0; j + kFactorHorizonSessions <= d_count; ++j)
      for (usize i = 0; i < n; ++i) {
        f64 total = 0.0;
        for (usize h = 0; h < kFactorHorizonSessions; ++h) total += ctx.forward[(j + h) * n + i];
        ctx.forward_h21[j * n + i] = total;
      }
    return co::Ok(std::move(ctx));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "factors: allocation failed");
  }
}

co::Result<FactorSeries> factor_series(const FactorContext& ctx, std::span<const f64> signal,
                                       FactorScratch& s) {
  const usize n = ctx.instruments, d_count = ctx.decisions;
  if (!n || signal.size() % n != 0 || signal.size() / n < ctx.begin + d_count ||
      ctx.row_begin.size() != d_count + 1 || ctx.forward.size() != d_count * n ||
      ctx.forward_h21.size() != d_count * n)
    return co::Err(co::ErrorCode::InvalidArgument, "factors: signal or context geometry");
  try {
    FactorSeries out;
    out.f.assign(d_count, kNaN);
    out.h21.assign(d_count, kNaN);
    s.q_prev.assign(n, 0.0);
    s.q_cur.assign(n, 0.0);
    s.ranked.reserve(n);
    f64 turnover = 0.0;
    for (usize j = 0; j < d_count; ++j) {
      const usize lo = ctx.row_begin[j], m = ctx.row_begin[j + 1] - lo;
      const auto rows = std::span<const usize>(ctx.rows).subspan(lo, m);
      const auto q = std::span<const f64>(ctx.basis).subspan(lo * kBasisColumns, m * kBasisColumns);
      const auto row = signal.subspan((ctx.begin + j) * n, n);
      // Centred tied ranks over the used names with a finite signal (the fitter's
      // centered_tied_ranks: a tie block [b, e) of n ranked gets (b + e - 1) / (2 (n - 1)) - 0.5).
      s.x.assign(m, 0.0);
      s.ranked.clear();
      for (usize r = 0; r < m; ++r)
        if (std::isfinite(row[rows[r]])) s.ranked.emplace_back(row[rows[r]], r);
      engine::combine::for_each_centered_rank(s.ranked, [&](usize r, f64 rank) { s.x[r] = rank; });
      f64 entry = 0.0;
      for (const f64 v : s.x) entry += std::abs(v);
      project_out(q, s.x);
      project_out(q, s.x); // one refinement step, as the fitter
      f64 gross = 0.0;
      for (const f64 v : s.x) gross += std::abs(v);
      std::fill(s.q_cur.begin(), s.q_cur.end(), 0.0);
      if (entry > 0.0 && std::isfinite(gross) && gross > kMinResidualFraction * entry) {
        const bool horizon = j + kFactorHorizonSessions <= d_count;
        f64 f = 0.0, h = 0.0;
        for (usize r = 0; r < m; ++r) {
          const f64 w = s.x[r] / gross;
          s.q_cur[rows[r]] = w;
          f += w * ctx.forward[j * n + rows[r]];
          if (horizon) h += w * ctx.forward_h21[j * n + rows[r]];
        }
        out.f[j] = f;
        if (horizon) out.h21[j] = h;
        ++out.live_decisions;
      }
      if (j > 0) {
        f64 step = 0.0;
        for (usize i = 0; i < n; ++i) step += std::abs(s.q_cur[i] - s.q_prev[i]);
        turnover += step;
      }
      std::swap(s.q_prev, s.q_cur);
    }
    out.tau = d_count > 1 ? turnover / static_cast<f64>(d_count - 1) : kNaN;
    return co::Ok(std::move(out));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "factors: allocation failed");
  }
}

co::Status run_factor_export(const FactorExportConfig& cfg, std::ostream& progress) {
  try {
    if (cfg.role_path.empty() || !icd::hash_valid(cfg.role_sha256) ||
        cfg.signals_directory.empty() || cfg.output_directory.empty())
      return co::Err(refusal(co::ErrorCode::InvalidArgument,
                             "--role, a --role-sha256 SHA-256, --signals and --output are "
                             "required"));
    const fs::path root(cfg.output_directory);
    if (fs::exists(root))
      return co::Err(refusal(co::ErrorCode::AlreadyExists, "output must not exist"));
    ATX_TRY(const SignalIndex index, read_signals(fs::path(cfg.signals_directory),
                                                  cfg.role_sha256));
    // The shared loader refuses a pin mismatch, a sealed score window and a delisting-returns role
    // from the manifest alone, before any payload is opened.
    ResearchRoleSpec spec;
    spec.manifest = cfg.role_path;
    spec.manifest_sha256 = cfg.role_sha256;
    ATX_TRY(const auto geometry, ResearchRole::geometry(spec));
    const PriceExposureConfig price{};
    const u64 cells = u64{geometry.dates} * geometry.instruments;
    // Beside the role: the presence copy, one signal, the forward and horizon planes, the
    // compact basis at most (cells x 4), the session ring and fixed slack.
    const u64 extra = cells * (1 + sizeof(f64) * (3 + kBasisColumns)) +
                      session_ring_bytes(price, geometry.instruments) + (16ULL << 20);
    if (cfg.max_working_bytes <= extra)
      return co::Err(refusal(co::ErrorCode::OutOfRange, "--max-bytes below the working set"));
    spec.max_bytes = cfg.max_working_bytes - extra;
    ATX_TRY(const auto role, ResearchRole::load(spec));
    const auto& data = role->data();
    if (data.score_end < data.score_begin + 4)
      return co::Err(refusal(co::ErrorCode::InvalidArgument,
                             "fewer than two scored decisions"));
    const usize d_count = data.panel.dates(), n = data.panel.instruments();
    const usize begin = data.score_begin, end = data.score_end - 2;
    ATX_TRY(const auto close_id, data.panel.field_id("close"));
    ATX_TRY(const auto raw_id, data.panel.field_id("raw_close"));
    ATX_TRY(const auto volume_id, data.panel.field_id("volume"));
    std::vector<u8> present(d_count * n);
    for (usize t = 0; t < d_count; ++t)
      for (usize i = 0; i < n; ++i)
        present[t * n + i] = static_cast<u8>(data.panel.in_universe(t, i));
    const PriceExposureInput panel{d_count, n, data.panel.field_all(close_id),
                                   data.panel.field_all(raw_id), data.panel.field_all(volume_id),
                                   present};
    ATX_TRY(const FactorContext ctx,
            build_factor_context(panel, role->member(), price, begin, end));
    ATX_TRY(const FactorTable table, factor_table(ctx, index, d_count * n));
    // Every input is read and every series computed: publish (manifest last).
    if (!fs::create_directory(root))
      return co::Err(refusal(co::ErrorCode::AlreadyExists, "output must not exist"));
    const usize decisions = end - begin, k_count = index.entries.size();
    const Json grid = Json::array({decisions, k_count});
    ATX_TRY(Json factor_file,
            publish_payload(root, "factor.f64", table.factor, grid,
                            "decision-major, then candidate (signals.json order): f_k(d), NaN on "
                            "a flat decision"));
    ATX_TRY(Json tau_file, publish_payload(root, "tau.f64", table.tau, Json::array({k_count}),
                                           "candidate (signals.json order): tau_k"));
    ATX_TRY(Json h21_file,
            publish_payload(root, "factor_h21.f64", table.h21, grid,
                            "decision-major, then candidate: report-only h_k(d), NaN when flat "
                            "or past the window"));
    Json candidates = Json::array(), used_rows = Json::array(), refused = Json::array(),
         sessions = Json::array();
    for (usize k = 0; k < k_count; ++k)
      candidates.push_back(Json{{"id", index.entries[k].id},
                                {"payload_sha256", index.entries[k].sha256},
                                {"live_decisions", table.live[k]}});
    for (usize j = 0; j < decisions; ++j) {
      const auto& outcome = ctx.outcome[j];
      const bool has_basis = outcome.refusal == BasisRefusal::None;
      used_rows.push_back(has_basis ? outcome.used_rows : usize{0});
      sessions.push_back(data.session_keys[begin + j]);
      if (!has_basis)
        refused.push_back(Json{{"decision_index", begin + j},
                               {"reason", basis_refusal_id(outcome.refusal)},
                               {"used_rows", outcome.used_rows}});
    }
    const auto refused_count = refused.size();
    const Json out{
        {"schema", series_schema}, {"status", "complete"}, {"contract", "K-P9-4"},
        {"role_manifest_sha256", cfg.role_sha256},
        {"research_window", std::string(ed::kResearchWindowId)},
        {"signals", {{"schema", signals_schema}, {"index_sha256", index.sha256}}},
        {"dates", d_count}, {"instruments", n}, {"decision_begin", begin},
        {"decision_end_exclusive", end}, {"decisions", decisions},
        {"decision_sessions_ns", std::move(sessions)},
        {"candidates", std::move(candidates)},
        {"traded_horizon_sessions", kFactorHorizonSessions},
        {"semantics", {{"factor", factor_semantics}, {"context", context_semantics},
                       {"traded_horizon", horizon_semantics}}},
        {"used_rows", std::move(used_rows)}, {"refused", std::move(refused)},
        {"files", {{"factor.f64", std::move(factor_file)}, {"tau.f64", std::move(tau_file)},
                   {"factor_h21.f64", std::move(h21_file)}}}};
    {
      std::ofstream file(root / "manifest.json", std::ios::binary);
      file << out.dump(2) << '\n';
      file.close();
      if (!file) return co::Err(refusal(co::ErrorCode::IoError, "manifest write"));
    }
    progress << "factors: " << decisions << " decisions x " << k_count << " candidates, "
             << refused_count << " refused -> " << root.string() << '\n';
    return co::Ok();
  } catch (const std::bad_alloc&) {
    return co::Err(refusal(co::ErrorCode::OutOfRange, "allocation failed"));
  } catch (const std::exception& e) {
    return co::Err(refusal(co::ErrorCode::InvalidArgument, e.what()));
  }
}

int dispatch_factors(int argc, char** argv, std::ostream& out, std::ostream& err) {
  try {
    FactorExportConfig cfg;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--help") {
        out << "factors --role PATH/manifest.json --role-sha256 SHA --signals DIR --output NEWDIR "
               "[--max-bytes 1400000000]\n"
               "Writes each candidate's unsigned factor series (contract K-P9-4) for the role's "
               "decisions [score_begin, score_end - 2): factor.f64 (decisions x candidates), "
               "tau.f64, factor_h21.f64 (report only: the 21-session hold), manifest.json last. "
               "DIR/signals.json (atx.factor-signals/v1) lists {id, payload, payload_sha256}.\n";
        return 0;
      }
      if (!seen.insert(key).second || i + 1 >= argc)
        throw std::invalid_argument("duplicate/missing flag");
      const std::string value = argv[++i];
      if (key == "--role") cfg.role_path = value;
      else if (key == "--role-sha256") cfg.role_sha256 = value;
      else if (key == "--signals") cfg.signals_directory = value;
      else if (key == "--output") cfg.output_directory = value;
      else if (key == "--max-bytes") {
        u64 x = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), x);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
          throw std::invalid_argument("invalid integer");
        cfg.max_working_bytes = x;
      } else throw std::invalid_argument("unknown flag: " + key);
    }
    if (cfg.role_path.empty() || cfg.role_sha256.empty() || cfg.signals_directory.empty() ||
        cfg.output_directory.empty())
      throw std::invalid_argument("--role, --role-sha256, --signals and --output are required");
    const auto status = run_factor_export(cfg, out);
    if (!status) { err << status.error().to_string() << '\n'; return 1; }
    return 0;
  } catch (const std::exception& e) { err << "factors: " << e.what() << '\n'; return 2; }
}
} // namespace atx::impl::strategy
