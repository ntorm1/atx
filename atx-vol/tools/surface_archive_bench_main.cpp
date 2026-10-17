// atx-vol-fit-archive-bench: ATXVFA write / open / random-read benchmark over a
// real graded quote-slice payload (docs/storage/ARCHIVE_FORMAT.md). The target
// is not called atx-vol-surface-archive-bench: bench/ owns that name (ATXVSA2).
//
// Reproduces the graded admission path of atx-vol-quote-bench for one cohort
// (same closed CSV schema, chain-median market, ambiguity refusal, policy
// resolution, fit_quote_surface per exact snapshot; snapshots with more than
// kQuoteSurfaceMaxExpiries expiries take the static path there and are skipped
// here), checks the payload against that run's predictions CSV row by row, then
// optionally builds each served expiry's ChainRecords (ExpiryRecords) and
// archives everything. Timings are wall clock on whatever host runs it: report
// them as unqualified unless the host is quiet and pinned.
//
//   atx-vol-fit-archive-bench --input quotes.csv --policy graded-v1.json
//       --reference <graded run predictions.csv> --out-dir <new folder>
//       [--dividends funding-pv] [--workers N] [--reps N] [--lookups N]
//       [--ladders on|off]

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp"
#include "atx/vol/api/storage/fit_archive.hpp"
#include "atx/vol/api/storage/fit_archive_records.hpp"
#include "quote_bench_policy.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

using namespace atx::vol;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

struct Args {
  fs::path input, policy, reference, out_dir;
  std::string dividends{"funding-pv"};
  unsigned workers{3};
  std::size_t reps{9};
  std::size_t lookups{262144};
  bool ladders{true};
};

Args parse(int argc, char **argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string_view k{argv[i]};
    if (i + 1 >= argc) throw std::runtime_error("missing value for " + std::string(k));
    const std::string v{argv[++i]};
    if (k == "--input") a.input = v;
    else if (k == "--policy") a.policy = v;
    else if (k == "--reference") a.reference = v;
    else if (k == "--out-dir") a.out_dir = v;
    else if (k == "--dividends") a.dividends = v;
    else if (k == "--workers") a.workers = static_cast<unsigned>(std::stoul(v));
    else if (k == "--reps") a.reps = std::stoul(v);
    else if (k == "--lookups") a.lookups = std::stoul(v);
    else if (k == "--ladders") a.ladders = v == "on";
    else throw std::runtime_error("unknown argument " + std::string(k));
  }
  if (a.input.empty() || a.policy.empty() || a.out_dir.empty()) {
    throw std::runtime_error("--input, --policy and --out-dir are required");
  }
  if (a.workers == 0 || a.reps == 0) throw std::runtime_error("--workers and --reps must be positive");
  return a;
}

// ── Closed quote CSV schema (as tools/quote_bench_main.cpp reads it) ────────

std::vector<std::string> csv_record(std::string_view line) {
  std::vector<std::string> out;
  std::string field;
  bool quoted = false;
  bool closed = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c != '"') field.push_back(c);
      else if (i + 1 < line.size() && line[i + 1] == '"') {
        field.push_back('"');
        ++i;
      } else {
        quoted = false;
        closed = true;
      }
    } else if (c == ',') {
      out.push_back(std::move(field));
      field.clear();
      closed = false;
    } else if (c == '"' && field.empty() && !closed) {
      quoted = true;
    } else if (closed || c == '"') {
      throw std::runtime_error("malformed CSV quoting");
    } else {
      field.push_back(c);
    }
  }
  if (quoted) throw std::runtime_error("unterminated CSV quote");
  out.push_back(std::move(field));
  return out;
}

template <class T> T number(std::string_view s) {
  T v{};
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || end != s.data() + s.size()) throw std::runtime_error("bad number: " + std::string(s));
  return v;
}

struct Row {
  std::uint64_t id{};
  std::uint64_t chain{};
  QuoteSliceMarket market{};
  QuoteSliceQuote quote{};
  std::string status;
  std::array<std::string, 4> identity; // underlier, root, expiry day, snapshot ns
};

std::vector<Row> read_rows(const fs::path &path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open input");
  std::string line;
  std::getline(in, line);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::map<std::string, std::size_t, std::less<>> col;
  const auto names = csv_record(line);
  for (std::size_t i = 0; i < names.size(); ++i) col[names[i]] = i;
  std::vector<Row> rows;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto f = csv_record(line);
    const auto get = [&](std::string_view k) -> std::string_view { return f.at(col.at(std::string(k))); };
    Row r;
    r.id = number<std::uint64_t>(get("row_id"));
    r.chain = number<std::uint64_t>(get("chain_id"));
    r.identity = {std::string(get("underlier")), std::string(get("root")),
                  std::to_string(number<std::int32_t>(get("expiry_days"))),
                  std::to_string(number<std::int64_t>(get("snapshot_ns")))};
    r.status = std::string(get("input_status"));
    if (r.status == "ok") {
      const auto &root = r.identity[1];
      if (!root.empty() && root.back() >= '1' && root.back() <= '9') r.status = "unsupported_deliverable";
      else if (get("exercise") != "American") r.status = "unsupported_exercise";
    }
    if (r.status == "ok") {
      r.market = {number<double>(get("spot")), number<double>(get("years")),
                  number<double>(get("years_calendar")), number<double>(get("rate")), number<double>(get("sdiv")),
                  number<double>(get("ddiv_pv"))};
      const auto side = number<unsigned>(get("is_call"));
      r.quote = {number<double>(get("strike")), side == 1 ? Side::Call : Side::Put, number<double>(get("bid")),
                 number<double>(get("ask"))};
      r.quote.continuous_yield = r.market.continuous_yield;
    }
    rows.push_back(std::move(r));
  }
  return rows;
}

double median_of(std::vector<double> v) {
  if (v.empty() || !std::all_of(v.begin(), v.end(), [](double x) { return std::isfinite(x); })) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::sort(v.begin(), v.end());
  const std::size_t m = v.size() / 2;
  return v.size() % 2 ? v[m] : 0.5 * v[m - 1] + 0.5 * v[m];
}

QuoteSliceMarket chain_market(const std::vector<Row> &rows, const std::vector<std::size_t> &idx) {
  const auto col = [&](double QuoteSliceMarket::*m) {
    std::vector<double> v;
    for (const auto i : idx) v.push_back(rows[i].market.*m);
    return median_of(std::move(v));
  };
  return {col(&QuoteSliceMarket::spot), col(&QuoteSliceMarket::variance_years),
          col(&QuoteSliceMarket::calendar_years), col(&QuoteSliceMarket::rate),
          col(&QuoteSliceMarket::continuous_yield), col(&QuoteSliceMarket::dividend_pv)};
}

bool consistent_chain(const std::vector<Row> &rows, const std::vector<std::size_t> &idx) {
  const auto &first = rows[idx.front()];
  std::set<std::pair<double, Side>> contracts;
  for (const auto i : idx) {
    const auto &r = rows[i];
    if (r.identity != first.identity || !std::isfinite(r.quote.strike)) return false;
    if (!contracts.emplace(r.quote.strike, r.quote.side).second) return false;
    for (const auto m : {&QuoteSliceMarket::variance_years, &QuoteSliceMarket::calendar_years}) {
      const double a = r.market.*m;
      const double b = first.market.*m;
      if (!std::isfinite(a) || !std::isfinite(b) ||
          std::abs(a - b) > 1.0e-10 * std::max({1.0, std::abs(a), std::abs(b)})) {
        return false;
      }
    }
  }
  return true;
}

struct Snapshot {
  std::string underlier, root;
  std::int64_t snapshot_ns{};
  std::vector<std::uint64_t> chains; // members, in chain order
  std::vector<std::int32_t> days;
  std::shared_ptr<const QuoteFitProfile> profile;
  std::string source;
};

struct Prepared {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// One served expiry's chain: its contracts at the fitted vols and the fit's carry.
struct Columns {
  std::vector<double> K, T, sigma, rate, carry;
  std::vector<Side> side;
  [[nodiscard]] ChainContractsView view() const noexcept { return {K, T, sigma, rate, carry, side}; }
};

struct Fitted {
  std::vector<QuoteSurfaceSlice> slices;
  std::vector<std::optional<ChainRecords>> records; // per slice, when built
  std::vector<Columns> columns;                     // per slice
  std::size_t ladder_failures{0};
  bool ok{false};
};

std::string describe(const ArchiveError &e) {
  return std::string(to_string(e.status)) + " section=" + std::to_string(e.section) + " detail=" + std::to_string(e.detail);
}

double seconds_since(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

Json stats(std::vector<double> v, double scale = 1.0) {
  if (v.empty()) return nullptr;
  std::sort(v.begin(), v.end());
  const auto q = [&](double p) {
    const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size())) - 1));
    return v[i] * scale;
  };
  return {{"n", v.size()}, {"median", q(0.5)}, {"p99", q(0.99)}, {"min", v.front() * scale},
          {"max", v.back() * scale}};
}

std::array<std::uint8_t, 32> digest_of(const fs::path &file) {
  const auto hex = atx::core::sha256_file(file.string());
  if (!hex || hex->size() != 64) throw std::runtime_error("sha256 failed");
  std::array<std::uint8_t, 32> out{};
  for (std::size_t i = 0; i < 32; ++i) out[i] = static_cast<std::uint8_t>(std::stoul(hex->substr(2 * i, 2), nullptr, 16));
  return out;
}

#if defined(_WIN32)
// Read the whole file bypassing the OS cache (FILE_FLAG_NO_BUFFERING) into a
// page-aligned buffer: a cold-read proxy where dropping the standby list needs
// administrator rights.
std::pair<std::vector<std::byte>, double> unbuffered_read(const fs::path &path, double &open_seconds) {
  const auto t0 = Clock::now();
  HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("unbuffered open failed");
  LARGE_INTEGER size{};
  GetFileSizeEx(h, &size);
  const auto bytes = static_cast<std::size_t>(size.QuadPart);
  const std::size_t rounded = (bytes + 4095u) / 4096u * 4096u;
  void *buf = VirtualAlloc(nullptr, rounded, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  DWORD got = 0;
  std::size_t total = 0;
  while (total < rounded) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(rounded - total, 1u << 24));
    if (!ReadFile(h, static_cast<char *>(buf) + total, chunk, &got, nullptr) || got == 0) break;
    total += got;
  }
  CloseHandle(h);
  const double read_s = seconds_since(t0);
  std::vector<std::byte> copy(bytes);
  std::memcpy(copy.data(), buf, bytes);
  VirtualFree(buf, 0, MEM_RELEASE);
  const auto t1 = Clock::now();
  auto a = FitArchive::open_bytes(copy, nullptr, FitArchiveVerify::Full);
  open_seconds = seconds_since(t1);
  if (!a) throw std::runtime_error(std::string("unbuffered open refused: ") + to_string(a.error().status));
  return {std::move(copy), read_s};
}
#endif

} // namespace

int main(int argc, char **argv) {
  try {
    const Args args = parse(argc, argv);
    if (fs::exists(args.out_dir) && !fs::is_empty(args.out_dir)) {
      throw std::runtime_error("--out-dir exists and is not empty (outputs are never overwritten)");
    }
    fs::create_directories(args.out_dir);
    Json report;
    report["caveat"] = "unqualified timings (shared, loaded host; nothing pinned)";
    report["host"] = {{"hardware_threads", std::thread::hardware_concurrency()}, {"workers", args.workers}};
    report["inputs"] = {{"input", args.input.string()}, {"policy", args.policy.string()},
                        {"reference", args.reference.string()}, {"dividends", args.dividends}};

    // ── Load + prepare (graded admission path) ──
    const auto t_load = Clock::now();
    const auto rows = read_rows(args.input);
    const auto dividends = args.dividends == "funding-pv" ? QuoteSliceDividendModel::AggregateFundingPresentValue
                           : args.dividends == "escrowed-pv" ? QuoteSliceDividendModel::AggregatePresentValue
                                                              : QuoteSliceDividendModel::ContinuousYield;
    const auto policy = quote_bench_policy::load(args.policy.string(), dividends);
    if (!policy.graded) throw std::runtime_error("the policy is not graded admission");
    std::map<std::uint64_t, std::vector<std::size_t>> chains;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].status == "ok") chains[rows[i].chain].push_back(i);
    }
    std::map<std::uint64_t, Prepared> prepared;
    std::map<std::array<std::string, 3>, std::vector<std::uint64_t>> by_snapshot;
    for (const auto &[id, idx] : chains) {
      if (!consistent_chain(rows, idx)) continue;
      const auto m = chain_market(rows, idx);
      if (!(std::isfinite(m.spot) && m.spot > 0.0 && std::isfinite(m.calendar_years) && m.calendar_years > 0.0 &&
            std::isfinite(m.variance_years) && m.variance_years > 0.0 && std::isfinite(m.rate) &&
            std::isfinite(m.continuous_yield) && std::isfinite(m.dividend_pv) && m.dividend_pv >= 0.0)) {
        continue;
      }
      Prepared p{m, {}};
      for (const auto i : idx) p.quotes.push_back(rows[i].quote);
      prepared.emplace(id, std::move(p));
      const auto &key = rows[idx.front()].identity;
      by_snapshot[{key[0], key[1], key[3]}].push_back(id);
    }
    std::vector<Snapshot> snapshots;
    std::size_t static_fallback = 0;
    for (const auto &[key, ids] : by_snapshot) {
      std::map<std::string, std::vector<std::uint64_t>> expiries;
      std::map<double, std::vector<std::uint64_t>> maturities;
      for (const auto id : ids) {
        expiries[rows[chains.at(id).front()].identity[2]].push_back(id);
        maturities[prepared.at(id).market.calendar_years].push_back(id);
      }
      std::set<std::uint64_t> ambiguous;
      for (const auto &[k, g] : expiries) if (g.size() > 1) ambiguous.insert(g.begin(), g.end());
      for (const auto &[k, g] : maturities) if (g.size() > 1) ambiguous.insert(g.begin(), g.end());
      Snapshot s{key[0], key[1], number<std::int64_t>(key[2]), {}, {}, {}, {}};
      std::vector<QuoteFitExpirySnapshot> features;
      for (const auto id : ids) {
        if (ambiguous.contains(id)) continue;
        s.chains.push_back(id);
        s.days.push_back(number<std::int32_t>(rows[chains.at(id).front()].identity[2]));
        features.push_back({prepared.at(id).market, prepared.at(id).quotes});
      }
      if (s.chains.empty()) continue;
      const auto resolved = policy.catalog.resolve(s.underlier, features);
      if (!resolved) continue;
      s.profile = resolved->profile;
      s.source = to_string(resolved->source);
      if (s.chains.size() > kQuoteSurfaceMaxExpiries) {
        ++static_fallback;
        continue;
      }
      snapshots.push_back(std::move(s));
    }
    report["prepare_seconds"] = seconds_since(t_load);

    // ── Fit (and build ChainRecords) in parallel over snapshots ──
    std::vector<Fitted> fitted(snapshots.size());
    std::atomic<std::size_t> next{0};
    const auto t_fit = Clock::now();
    std::vector<double> ladder_seconds(snapshots.size(), 0.0);
    {
      std::vector<std::jthread> pool;
      for (unsigned w = 0; w < args.workers; ++w) {
        pool.emplace_back([&] {
          for (std::size_t i = next++; i < snapshots.size(); i = next++) {
            const Snapshot &s = snapshots[i];
            std::vector<QuoteSurfaceExpiry> expiries;
            for (const auto id : s.chains) expiries.push_back({prepared.at(id).market, prepared.at(id).quotes, {}});
            auto out = fit_quote_surface(expiries, s.profile->config, policy.surface);
            if (!out) continue;
            Fitted &f = fitted[i];
            f.slices = std::move(*out);
            f.ok = true;
            f.records.resize(f.slices.size());
            f.columns.resize(f.slices.size());
            if (!args.ladders) continue;
            const auto t_ladder = Clock::now();
            for (std::size_t e = 0; e < f.slices.size(); ++e) {
              const QuoteSurfaceSlice &slice = f.slices[e];
              if (slice.grade == QuoteSliceGrade::Refused) continue;
              const auto &idx = chains.at(s.chains[e]);
              Columns &c = f.columns[e];
              for (std::size_t j = 0; j < idx.size(); ++j) {
                const QuoteSliceRow &row = slice.fit.rows[j];
                if (row.evaluation_status != QuoteSliceEvaluationStatus::Success || !(row.fitted_iv > 0.0)) continue;
                c.K.push_back(rows[idx[j]].quote.strike);
                c.T.push_back(slice.fit.market.variance_years);
                c.sigma.push_back(row.fitted_iv);
                c.rate.push_back(slice.fit.diagnostics.effective_rate);
                c.carry.push_back(slice.fit.diagnostics.effective_yield);
                c.side.push_back(rows[idx[j]].quote.side);
              }
              if (c.K.empty()) continue;
              auto built = ChainRecords::build(slice.fit.market.spot, c.view());
              if (built) f.records[e] = std::move(*built);
              else ++f.ladder_failures;
            }
            ladder_seconds[i] = seconds_since(t_ladder);
          }
        });
      }
    }
    const double fit_wall = seconds_since(t_fit);

    // ── Parity of the payload against the graded run's predictions ──
    Json parity = nullptr;
    if (!args.reference.empty()) {
      std::map<std::uint64_t, std::array<std::string, 3>> ref; // price, vol, grade
      std::ifstream in(args.reference);
      std::string line;
      std::getline(in, line);
      while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto f = csv_record(line);
        ref[number<std::uint64_t>(f[0])] = {f[1], f[2], f.size() > 13 ? f[13] : std::string{}};
      }
      std::size_t compared = 0, price_equal = 0, vol_equal = 0, grade_equal = 0, refused_rows = 0, ref_ok = 0;
      for (std::size_t i = 0; i < snapshots.size(); ++i) {
        if (!fitted[i].ok) continue;
        for (std::size_t e = 0; e < fitted[i].slices.size(); ++e) {
          const auto &slice = fitted[i].slices[e];
          const bool served = slice.grade != QuoteSliceGrade::Refused;
          const auto &idx = chains.at(snapshots[i].chains[e]);
          for (std::size_t j = 0; j < idx.size(); ++j) {
            const auto &r = ref.at(rows[idx[j]].id);
            if (!r[0].empty()) ++ref_ok;
            const QuoteSliceRow &row = slice.fit.rows[j];
            if (!served || row.evaluation_status != QuoteSliceEvaluationStatus::Success) {
              refused_rows += r[0].empty() ? 1u : 0u;
              continue;
            }
            ++compared;
            price_equal += !r[0].empty() && number<double>(r[0]) == row.fitted_price ? 1u : 0u;
            vol_equal += std::isfinite(row.fitted_iv) ? (!r[1].empty() && number<double>(r[1]) == row.fitted_iv ? 1u : 0u)
                                                      : (r[1].empty() ? 1u : 0u);
            grade_equal += r[2] == to_string(slice.grade) ? 1u : 0u;
          }
        }
      }
      parity = {{"served_rows_compared", compared}, {"price_exact_equal", price_equal},
                {"vol_exact_equal", vol_equal}, {"grade_equal", grade_equal},
                {"unserved_rows_also_unserved_in_reference", refused_rows},
                {"reference_ok_rows_in_these_snapshots", ref_ok}};
    }

    // ── Encode ──
    const auto policy_digest = digest_of(args.policy);
    FitArchiveWriter writer({std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch()).count(),
                             0x2026101700000001ull, 0});
    const std::string provenance = Json{{"policy", policy.definition}, {"inputs", report["inputs"]}}.dump();
    writer.set_provenance(std::as_bytes(std::span{provenance.data(), provenance.size()}));
    std::size_t surfaces = 0, fits = 0, served_slices = 0, ladders = 0, expiry_records = 0, ladder_failures = 0;
    std::size_t refused_surfaces = 0;
    const auto t_encode = Clock::now();
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
      const Snapshot &s = snapshots[i];
      Fitted &f = fitted[i];
      if (!f.ok) continue;
      const auto added = writer.add_surface({s.root, s.snapshot_ns, f.slices, s.days, policy.surface, policy_digest,
                                             s.profile->name, s.source});
      if (!added) {
        std::cerr << "add_surface " << s.root << " " << s.snapshot_ns << ": " << describe(added.error()) << "\n";
        ++refused_surfaces;
        continue;
      }
      ++surfaces;
      fits += f.slices.size();
      ladder_failures += f.ladder_failures;
      for (std::size_t e = 0; e < f.slices.size(); ++e) {
        served_slices += f.slices[e].grade != QuoteSliceGrade::Refused ? 1u : 0u;
        if (!f.records[e]) continue;
        const auto ok = writer.add_chain_records({s.root, s.snapshot_ns, s.days[e], 0, 0, f.slices[e].fit.market.spot,
                                                  f.columns[e].view(), &*f.records[e], {}, {}, {}});
        if (!ok) throw std::runtime_error("add_chain_records " + s.root + ": " + describe(ok.error()));
        ++ladders;
        expiry_records += f.records[e]->bank().size();
      }
    }
    const double encode_s = seconds_since(t_encode);
    if (refused_surfaces != 0) throw std::runtime_error(std::to_string(refused_surfaces) + " surfaces refused");

    // ── Write: finish (frame + checksums) and publish (temp + fsync + rename) ──
    // Repetitions publish into a private temp directory (same volume as the
    // usual cache) so no file under --out-dir is ever replaced; the archive is
    // published into --out-dir exactly once, as a new file.
    const fs::path archive = args.out_dir / "dev200-graded-v1.atxvfa";
    const fs::path rep_dir = fs::temp_directory_path() / ("atx-fit-archive-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directories(rep_dir);
    std::vector<double> finish_s, publish_s;
    std::vector<std::byte> image;
    for (std::size_t rep = 0; rep < args.reps; ++rep) {
      const auto t0 = Clock::now();
      auto built = writer.finish();
      finish_s.push_back(seconds_since(t0));
      if (!built) throw std::runtime_error("finish: " + describe(built.error()));
      const auto t1 = Clock::now();
      if (!publish_fit_archive((rep_dir / "rep.atxvfa").string(), *built)) throw std::runtime_error("publish failed");
      publish_s.push_back(seconds_since(t1));
      if (rep > 0 && *built != image) throw std::runtime_error("finish is not deterministic across repetitions");
      image = std::move(*built);
    }
    fs::remove_all(rep_dir);
    {
      const auto t0 = Clock::now();
      if (!publish_fit_archive(archive.string(), image)) throw std::runtime_error("publish failed");
      report["final_publish_ms"] = seconds_since(t0) * 1.0e3;
    }

    // ── Open + validate (warm page cache) ──
    Json open = Json::object();
    const auto time_open = [&](FitArchiveOpenMode mode, FitArchiveVerify verify) {
      std::vector<double> v;
      for (std::size_t rep = 0; rep < args.reps; ++rep) {
        const auto t0 = Clock::now();
        auto a = FitArchive::open(archive.string(), {mode, verify});
        v.push_back(seconds_since(t0));
        if (!a) throw std::runtime_error(std::string("open refused: ") + to_string(a.error().status));
      }
      return stats(v, 1.0e3);
    };
    open["mapped_framing_ms"] = time_open(FitArchiveOpenMode::Mapped, FitArchiveVerify::Framing);
    open["mapped_full_ms"] = time_open(FitArchiveOpenMode::Mapped, FitArchiveVerify::Full);
    open["mapped_paranoid_ms"] = time_open(FitArchiveOpenMode::Mapped, FitArchiveVerify::Paranoid);
    open["read_full_ms"] = time_open(FitArchiveOpenMode::Read, FitArchiveVerify::Full);
#if defined(_WIN32)
    {
      std::vector<double> read_v, validate_v;
      for (std::size_t rep = 0; rep < args.reps; ++rep) {
        double validate = 0.0;
        const auto [bytes, read] = unbuffered_read(archive, validate);
        read_v.push_back(read);
        validate_v.push_back(validate);
      }
      open["unbuffered_read_ms"] = stats(read_v, 1.0e3);
      open["unbuffered_then_validate_full_ms"] = stats(validate_v, 1.0e3);
      open["cache_drop"] = "not feasible without administrator rights; FILE_FLAG_NO_BUFFERING read is the cold proxy";
    }
#endif

    // ── Random access ──
    auto a = FitArchive::open(archive.string());
    if (!a) throw std::runtime_error("reopen failed");
    std::mt19937_64 rng(20261017);
    const auto fit_rows = a->fits();
    struct Probe {
      std::string_view name;
      FitArchiveKey key;
    };
    std::vector<Probe> probes(args.lookups);
    for (auto &p : probes) {
      const auto &f = fit_rows[std::uniform_int_distribution<std::size_t>(0, fit_rows.size() - 1)(rng)];
      p = {a->underlier_name(f.key.underlier), f.key};
    }
    constexpr std::size_t kBlock = 256;
    double sink = 0.0;
    std::vector<double> fit_lookup_ns, fit_lookup_cold_ns;
    for (std::size_t b = 0; b + kBlock <= probes.size(); b += kBlock) {
      const auto t0 = Clock::now();
      for (std::size_t i = b; i < b + kBlock; ++i) {
        const auto id = a->find_underlier(probes[i].name);
        FitArchiveKey k = probes[i].key;
        k.underlier = *id;
        const FitArchiveFitRecord *r = a->find_fit(k);
        for (const auto &row : a->quote_rows(*r)) sink += row.fitted_price;
        for (const auto &node : price_nodes(*a, *r)) sink += node.put;
      }
      fit_lookup_ns.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / kBlock);
    }
    std::vector<double> decode_us;
    for (std::size_t i = 0; i < std::min<std::size_t>(probes.size(), 4096); ++i) {
      const FitArchiveFitRecord *r = a->find_fit(probes[i].key);
      const auto t0 = Clock::now();
      auto slice = decode_surface_slice(*a, *r);
      decode_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
      if (!slice) throw std::runtime_error("decode refused");
      sink += static_cast<double>(slice->fit.rows.size());
    }
    std::vector<double> record_ns, record_single_ns, fit_single_ns;
    const auto metas = a->expiry_meta();
    if (!metas.empty()) {
      std::vector<std::pair<std::string_view, FitArchiveKey>> keys(args.lookups);
      for (auto &k : keys) {
        const auto &m = metas[std::uniform_int_distribution<std::size_t>(0, metas.size() - 1)(rng)];
        k = {a->underlier_name(m.key.underlier), m.key};
      }
      for (std::size_t b = 0; b + kBlock <= keys.size(); b += kBlock) {
        const auto t0 = Clock::now();
        for (std::size_t i = b; i < b + kBlock; ++i) {
          FitArchiveKey k = keys[i].second;
          k.underlier = *a->find_underlier(keys[i].first);
          const FitArchiveExpiryMeta *m = a->find_expiry(k);
          const ExpiryRecord *rec = expiry_record(*a, *m);
          const double F = 100.0;
          sink += rec->price(F, F * std::exp(0.5 * (m->k_min + m->k_max)), 0.5 * (m->sigma_min + m->sigma_max));
        }
        record_ns.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / kBlock);
      }
      // Single-op timings (steady_clock granularity, 100 ns on Windows) for the tail.
      for (const auto &[name, key] : keys) {
        const auto t0 = Clock::now();
        FitArchiveKey k = key;
        k.underlier = *a->find_underlier(name);
        const FitArchiveExpiryMeta *m = a->find_expiry(k);
        const ExpiryRecord *rec = expiry_record(*a, *m);
        sink += rec->price(100.0, 100.0 * std::exp(0.5 * (m->k_min + m->k_max)), 0.5 * (m->sigma_min + m->sigma_max));
        record_single_ns.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
      }
    }
    for (const auto &p : probes) {
      const auto t0 = Clock::now();
      FitArchiveKey k = p.key;
      k.underlier = *a->find_underlier(p.name);
      const FitArchiveFitRecord *r = a->find_fit(k);
      sink += r->market.spot;
      fit_single_ns.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
    }

    // ── Report ──
    Json sizes = Json::object();
    for (const auto &e : a->sections()) sizes[to_string(static_cast<FitArchiveSectionType>(e.type))] = e.length;
    report["payload"] = {{"snapshots", snapshots.size()}, {"static_fallback_snapshots_skipped", static_fallback},
                         {"surfaces", surfaces}, {"expiry_fits", fits}, {"served_expiries", served_slices},
                         {"ladders", ladders}, {"expiry_records", expiry_records},
                         {"chain_record_build_failures", ladder_failures},
                         {"symbols", a->symbols().size()}};
    report["parity_vs_reference"] = parity;
    report["fit_wall_seconds"] = fit_wall;
    std::vector<double> nonzero;
    for (const double s : ladder_seconds) if (s > 0.0) nonzero.push_back(s);
    report["chain_record_build_cpu_seconds_sum"] = std::accumulate(nonzero.begin(), nonzero.end(), 0.0);
    report["archive"] = {{"path", archive.string()}, {"bytes", image.size()}, {"sections", sizes},
                         {"bytes_per_expiry_fit", fits ? static_cast<double>(image.size()) / static_cast<double>(fits) : 0.0}};
    report["write"] = {{"encode_seconds_all_records", encode_s}, {"finish_ms", stats(finish_s, 1.0e3)},
                       {"publish_temp_fsync_rename_ms", stats(publish_s, 1.0e3)}};
    report["open"] = open;
    report["random_access"] = {
        {"fit_lookup_plus_touch_rows_and_nodes_ns_per_op", stats(fit_lookup_ns)},
        {"fit_decode_owned_us", stats(decode_us)},
        {"expiry_record_lookup_plus_price_ns_per_op", stats(record_ns)},
        {"expiry_record_lookup_plus_price_single_op_ns", stats(record_single_ns)},
        {"fit_lookup_single_op_ns", stats(fit_single_ns)},
        {"block", kBlock},
        {"note", "*_per_op are block means over 256 random lookups (median/p99 across blocks); *_single_op are "
                 "individually timed operations at steady_clock granularity (100 ns on Windows)"}};
    report["sink"] = sink;
    const fs::path report_path = args.out_dir / "report.json";
    {
      std::ofstream out(report_path);
      out << report.dump(2) << "\n";
    }
    {
      std::ofstream sums(args.out_dir / "SHA256SUMS");
      for (const auto &file : {archive, report_path}) {
        const auto h = atx::core::sha256_file(file.string());
        if (!h) throw std::runtime_error("sha256 failed");
        sums << *h << "  " << file.filename().string() << "\n";
      }
    }
    std::cout << report.dump(2) << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "fit-archive-bench: " << e.what() << "\n";
    return 1;
  }
}
