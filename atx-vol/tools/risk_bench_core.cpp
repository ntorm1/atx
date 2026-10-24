// risk_bench_core — the ENGINE-FREE half of atx-vol-risk-bench (see risk_bench.hpp):
// CLI, synthetic-store preparation, book selection over SoA bucket columns,
// explain-quality statistics, summaries, the versioned JSON report (write + read)
// and the markdown summary. Nothing here references a risk-engine symbol, so this
// TU links into atx-vol-tests without lane A's engine.

#include "risk_bench.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <optional>
#include <ostream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::vol::riskbench {

using atx::core::Err;
using atx::core::Error;
using atx::core::Ok;

namespace {

constexpr double kMinAbsDelta = 0.05;
constexpr double kMaxAbsDelta = 0.95;
constexpr double kMinSigma = 0.01; // exclusive
constexpr double kMaxSigma = 3.0;  // inclusive
constexpr double kMultiplier = 100.0;
constexpr std::uint64_t kQtyChoices = 20; // {-10..-1, 1..10}
// |delta| targets the per-(expiry, side) picks cycle through: ATM first, then the
// wings, so a short quota still spans the smile.
constexpr std::array<double, 19> kDeltaTargets{0.50, 0.25, 0.75, 0.10, 0.90, 0.40, 0.60,
                                               0.15, 0.85, 0.30, 0.70, 0.20, 0.80, 0.35,
                                               0.65, 0.05, 0.95, 0.45, 0.55};

constexpr std::array<const char *, kRouteCount> kRouteNames{
    "None", "Family", "ColdAnalytic", "ColdFd", "Exercised", "Share", "Refused"};
constexpr std::array<const char *, kFlagCount> kFlagNames{
    "TenorExtrapolated", "MoneynessExtrapolated", "NearExerciseEdge", "MarkMissing",
    "SurfaceMissing",    "OutsideFamilyBox",      "Rolled",           "PivotFromMarks",
    "VendorParityAvailable", "ColdFallback",      "CarryFromSurface",
    "FamilyUneconomic",  "ShortTenor"};
constexpr std::array<const char *, kRiskLaneStatusCount> kStatusNames{
    "Ok", "ModelUnavailable", "NumericError", "InvalidContract", "Rolled", "Refused"};

static_assert(static_cast<std::size_t>(RiskRoute::Refused) + 1 == kRouteCount,
              "kRouteCount must cover every RiskRoute");
static_assert(static_cast<std::uint32_t>(RiskFlag::ShortTenor) == (1u << (kFlagCount - 1)),
              "kFlagCount must cover every RiskFlag bit");
static_assert(static_cast<std::size_t>(RiskLaneStatus::Refused) + 1 == kRiskLaneStatusCount,
              "kStatusNames must cover every RiskLaneStatus");

[[nodiscard]] Error usage_error(const std::string &msg) {
  return Error{ErrorCode::InvalidArgument, msg};
}

template <class T> [[nodiscard]] std::optional<T> parse_unsigned(std::string_view s) noexcept {
  T value{};
  const char *first = s.data();
  const char *last = first + s.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (s.empty() || ec != std::errc{} || ptr != last) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] std::optional<BenchKernel> parse_kernel(std::string_view s) noexcept {
  if (s == "family") {
    return BenchKernel::Family;
  }
  if (s == "cold") {
    return BenchKernel::Cold;
  }
  if (s == "both") {
    return BenchKernel::Both;
  }
  return std::nullopt;
}

// The argv walk: `i` is the current flag; value() advances past its value.
struct ArgCursor {
  std::span<const char *const> argv;
  std::size_t i{1};
  [[nodiscard]] std::optional<std::string_view> value() {
    if (i + 1 >= argv.size() || argv[i + 1] == nullptr) {
      return std::nullopt;
    }
    ++i;
    return std::string_view{argv[i]};
  }
};

template <class T>
[[nodiscard]] Status set_number(ArgCursor &c, std::string_view flag, T &out) {
  const std::optional<std::string_view> v = c.value();
  const std::optional<T> n = v.has_value() ? parse_unsigned<T>(*v) : std::nullopt;
  if (!n.has_value()) {
    return Err(usage_error("bad or missing value for " + std::string{flag}));
  }
  out = *n;
  return Ok();
}

[[nodiscard]] Status set_string(ArgCursor &c, std::string_view flag, std::string &out) {
  const std::optional<std::string_view> v = c.value();
  if (!v.has_value() || v->empty()) {
    return Err(usage_error("missing value for " + std::string{flag}));
  }
  out = std::string{*v};
  return Ok();
}

struct StringFlag {
  std::string_view name;
  std::string RiskBenchArgs::*field;
};
constexpr std::array<StringFlag, 8> kStringFlags{{{"--store", &RiskBenchArgs::store},
                                                  {"--date", &RiskBenchArgs::date},
                                                  {"--from", &RiskBenchArgs::from},
                                                  {"--to", &RiskBenchArgs::to},
                                                  {"--synthetic", &RiskBenchArgs::synthetic_dir},
                                                  {"--out", &RiskBenchArgs::out_json},
                                                  {"--md", &RiskBenchArgs::out_md},
                                                  {"--dump-legs", &RiskBenchArgs::dump_legs}}};

struct SizeFlag {
  std::string_view name;
  std::size_t RiskBenchArgs::*field;
};
constexpr std::array<SizeFlag, 4> kSizeFlags{{{"--underliers", &RiskBenchArgs::underliers},
                                              {"--options", &RiskBenchArgs::options},
                                              {"--shares", &RiskBenchArgs::shares},
                                              {"--dump-steps", &RiskBenchArgs::dump_steps}}};

struct BoolFlag {
  std::string_view name;
  bool RiskBenchArgs::*field;
};
constexpr std::array<BoolFlag, 4> kBoolFlags{{{"--help", &RiskBenchArgs::help},
                                              {"-h", &RiskBenchArgs::help},
                                              {"--trapezoid", &RiskBenchArgs::trapezoid},
                                              {"--determinism", &RiskBenchArgs::determinism}}};

[[nodiscard]] Status parse_choice(ArgCursor &c, std::string_view flag, RiskBenchArgs &a) {
  const std::optional<std::string_view> v = c.value();
  if (flag == "--kernel") {
    const std::optional<BenchKernel> k = v.has_value() ? parse_kernel(*v) : std::nullopt;
    if (!k.has_value()) {
      return Err(usage_error("--kernel must be family|cold|both"));
    }
    a.kernel = *k;
    return Ok();
  }
  if (!v.has_value() || (*v != "rho" && *v != "none")) {
    return Err(usage_error("--needs must be rho|none"));
  }
  a.needs_rho = *v == "rho";
  return Ok();
}

// ── Lane A2: family-kernel policy overrides ────────────────────────────────

[[nodiscard]] std::optional<double> parse_finite(std::string_view s) noexcept {
  double value{};
  const char *first = s.data();
  const char *last = first + s.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (s.empty() || ec != std::errc{} || ptr != last || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

// One of the on|off / static|dynamic switches: `yes` names the true spelling.
[[nodiscard]] Status set_switch(ArgCursor &c, std::string_view flag, std::string_view yes,
                                std::string_view no, std::optional<bool> &out) {
  const std::optional<std::string_view> v = c.value();
  if (!v.has_value() || (*v != yes && *v != no)) {
    return Err(usage_error(std::string{flag} + " must be " + std::string{yes} + "|" +
                           std::string{no}));
  }
  out = *v == yes;
  return Ok();
}

// The family-policy flag `flag` into `a`, or nullopt when `flag` is not one of them.
[[nodiscard]] std::optional<Status> parse_family_flag(ArgCursor &c, std::string_view flag,
                                                      RiskBenchArgs &a) {
  if (flag == "--n-sigma" || flag == "--n-sigma-min") {
    std::uint16_t n{0};
    if (Status st = set_number(c, flag, n); !st.has_value()) {
      return st;
    }
    (flag == "--n-sigma" ? a.n_sigma : a.n_sigma_min) = n;
    return Ok();
  }
  constexpr std::array<std::pair<std::string_view, std::optional<double> RiskBenchArgs::*>, 4>
      kDoubles{{{"--sigma-box-pad", &RiskBenchArgs::sigma_box_pad},
                {"--sigma-node-spacing", &RiskBenchArgs::sigma_node_spacing},
                {"--family-node-cost-us", &RiskBenchArgs::family_node_cost_us},
                {"--cold-lane-cost-us", &RiskBenchArgs::cold_lane_cost_us}}};
  for (const auto &[name, field] : kDoubles) {
    if (flag != name) {
      continue;
    }
    const std::optional<std::string_view> v = c.value();
    const std::optional<double> x = v.has_value() ? parse_finite(*v) : std::nullopt;
    if (!x.has_value()) {
      return Status{Err(usage_error("bad or missing value for " + std::string{flag}))};
    }
    a.*field = *x;
    return Ok();
  }
  if (flag == "--family-economics") {
    return set_switch(c, flag, "on", "off", a.family_economics);
  }
  if (flag == "--family-schedule") {
    return set_switch(c, flag, "dynamic", "static", a.family_dynamic_schedule);
  }
  return std::nullopt;
}

[[nodiscard]] Status parse_one(ArgCursor &c, RiskBenchArgs &a) {
  const std::string_view flag = c.argv[c.i] != nullptr ? c.argv[c.i] : "";
  if (std::optional<Status> st = parse_family_flag(c, flag, a); st.has_value()) {
    return *st;
  }
  for (const BoolFlag &f : kBoolFlags) {
    if (flag == f.name) {
      a.*f.field = true;
      return Ok();
    }
  }
  for (const StringFlag &f : kStringFlags) {
    if (flag == f.name) {
      return set_string(c, flag, a.*f.field);
    }
  }
  for (const SizeFlag &f : kSizeFlags) {
    if (flag == f.name) {
      return set_number(c, flag, a.*f.field);
    }
  }
  if (flag == "--threads") {
    return set_number(c, flag, a.threads);
  }
  if (flag == "--seed") {
    return set_number(c, flag, a.seed);
  }
  if (flag == "--kernel" || flag == "--needs") {
    return parse_choice(c, flag, a);
  }
  return Err(usage_error("unknown flag: " + std::string{flag}));
}

[[nodiscard]] Status validate_args(const RiskBenchArgs &a) {
  if (a.store.empty() && a.synthetic_dir.empty()) {
    return Err(usage_error("one of --store or --synthetic is required"));
  }
  if (!a.store.empty() && !a.synthetic_dir.empty() && a.store != a.synthetic_dir) {
    return Err(usage_error("--store and --synthetic name different directories"));
  }
  if (!spiderrock_bucket_instant_ns(a.date, a.from).has_value() ||
      !spiderrock_bucket_instant_ns(a.date, a.to).has_value()) {
    return Err(usage_error("--date must be YYYY-MM-DD and --from/--to HHMM"));
  }
  if (a.from > a.to) {
    return Err(usage_error("--from must not be after --to"));
  }
  if (a.underliers == 0 || a.options == 0) {
    return Err(usage_error("--underliers and --options must be >= 1"));
  }
  return Ok();
}

} // namespace

// ═══ CLI ═════════════════════════════════════════════════════════════════════

Result<RiskBenchArgs> parse_risk_bench_args(std::span<const char *const> argv) {
  RiskBenchArgs a;
  ArgCursor c{argv, 1};
  for (; c.i < argv.size(); ++c.i) { // bounded: parse_one only moves c.i forward
    ATX_TRY_VOID(parse_one(c, a));
  }
  if (a.help) {
    return Ok(std::move(a));
  }
  ATX_TRY_VOID(validate_args(a));
  return Ok(std::move(a));
}

std::string risk_bench_usage() {
  return "usage: atx-vol-risk-bench (--store <root> | --synthetic <dir>) [options]\n"
         "  --store <root>       SpiderRock store root (holds date=<d>/bucket_et=<HHMM>/)\n"
         "  --synthetic <dir>    write the synthetic SpiderRock-shaped store there first\n"
         "                       (skipped when it already holds the date) and read it\n"
         "  --date YYYY-MM-DD    trading date (default 2026-10-14)\n"
         "  --from HHMM          first bucket, inclusive (default 1000)\n"
         "  --to HHMM            last bucket, inclusive (default 1500)\n"
         "  --underliers N       top-N underliers by row count at the first bucket (100)\n"
         "  --options N          option legs over those underliers (10000)\n"
         "  --shares N           delta-hedge share legs, one per underlier (100)\n"
         "  --kernel K           family | cold | both (family)\n"
         "  --threads N          kernel / explain fan-out degree, 0 = auto (8)\n"
         "  --needs rho|none     evaluate the rho axis (none)\n"
         "  --trapezoid          trapezoid second-order explain terms\n"
         "  --determinism        shadow threads=1 stream; totals must be bit-equal\n"
         "  --out <file>         JSON report (stdout when absent)\n"
         "  --md <file>          markdown summary\n"
         "  --dump-legs <file>   per-option-leg CSV of the base frame, vendor row and\n"
         "                       per-leg explain (columns in docs/risk-bench.md)\n"
         "  --dump-steps N       steps written to --dump-legs, the first N; 0 = all (1)\n"
         "  --seed N             book selection seed (1)\n"
         "  family-kernel policy overrides (FamilyKernelPolicy defaults when absent):\n"
         "  --n-sigma N          sigma-node ceiling n_sigma\n"
         "  --n-sigma-min N      adaptive node floor n_sigma_min (= n_sigma: fixed count)\n"
         "  --sigma-box-pad X    sigma box pad\n"
         "  --sigma-node-spacing X  adaptive log-sigma node spacing\n"
         "  --family-economics on|off  route uneconomic groups cold-analytic\n"
         "  --family-node-cost-us X / --cold-lane-cost-us X  the economics cost constants\n"
         "  --family-schedule static|dynamic  block fan-out: run_blocks | run_dynamic\n"
         "  --help               this text\n"
         "exit: 0 ok, 1 runtime error, 2 usage error\n";
}

const char *to_string(BenchKernel kernel) noexcept {
  switch (kernel) {
  case BenchKernel::Family:
    return "family";
  case BenchKernel::Cold:
    return "cold";
  case BenchKernel::Both:
    return "both";
  }
  return "family";
}

const char *to_string(PlaZone zone) noexcept {
  switch (zone) {
  case PlaZone::Green:
    return "green";
  case PlaZone::Amber:
    return "amber";
  case PlaZone::Red:
    return "red";
  case PlaZone::Undefined:
    return "undefined";
  }
  return "undefined";
}

// ═══ Statistics ══════════════════════════════════════════════════════════════

namespace {

// Average ranks (1-based) of `v`; ties share the mean of their positions.
[[nodiscard]] std::vector<double> average_ranks(std::span<const double> v) {
  std::vector<std::size_t> idx(v.size());
  for (std::size_t i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  std::stable_sort(idx.begin(), idx.end(),
                   [&](std::size_t a, std::size_t b) { return v[a] < v[b]; });
  std::vector<double> rank(v.size());
  for (std::size_t lo = 0; lo < idx.size();) {
    std::size_t hi = lo + 1;
    while (hi < idx.size() && v[idx[hi]] == v[idx[lo]]) {
      ++hi;
    }
    const double r = 0.5 * static_cast<double>(lo + hi - 1) + 1.0;
    for (std::size_t k = lo; k < hi; ++k) {
      rank[idx[k]] = r;
    }
    lo = hi;
  }
  return rank;
}

[[nodiscard]] double pearson(std::span<const double> x, std::span<const double> y) noexcept {
  const auto n = static_cast<double>(x.size());
  double mx = 0.0;
  double my = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    mx += x[i];
    my += y[i];
  }
  mx /= n;
  my /= n;
  double sxy = 0.0;
  double sxx = 0.0;
  double syy = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    sxy += (x[i] - mx) * (y[i] - my);
    sxx += (x[i] - mx) * (x[i] - mx);
    syy += (y[i] - my) * (y[i] - my);
  }
  return (sxx > 0.0 && syy > 0.0) ? sxy / std::sqrt(sxx * syy) : kNaN;
}

} // namespace

double spearman_rho(std::span<const double> x, std::span<const double> y) {
  if (x.size() != y.size()) {
    return kNaN;
  }
  std::vector<double> fx;
  std::vector<double> fy;
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (std::isfinite(x[i]) && std::isfinite(y[i])) {
      fx.push_back(x[i]);
      fy.push_back(y[i]);
    }
  }
  if (fx.size() < 2) {
    return kNaN;
  }
  return pearson(average_ranks(fx), average_ranks(fy));
}

double ks_statistic(std::span<const double> a, std::span<const double> b) {
  std::vector<double> sa;
  std::vector<double> sb;
  for (const double v : a) {
    if (std::isfinite(v)) {
      sa.push_back(v);
    }
  }
  for (const double v : b) {
    if (std::isfinite(v)) {
      sb.push_back(v);
    }
  }
  if (sa.empty() || sb.empty()) {
    return kNaN;
  }
  std::sort(sa.begin(), sa.end());
  std::sort(sb.begin(), sb.end());
  const auto na = static_cast<double>(sa.size());
  const auto nb = static_cast<double>(sb.size());
  std::size_t i = 0;
  std::size_t j = 0;
  double d = 0.0;
  while (i < sa.size() && j < sb.size()) { // bounded: every pass advances i or j
    const double x = std::min(sa[i], sb[j]);
    while (i < sa.size() && sa[i] <= x) {
      ++i;
    }
    while (j < sb.size() && sb[j] <= x) {
      ++j;
    }
    d = std::max(d, std::abs(static_cast<double>(i) / na - static_cast<double>(j) / nb));
  }
  return d;
}

PlaZone pla_zone(double spearman, double ks) noexcept {
  if (!std::isfinite(spearman) || !std::isfinite(ks)) {
    return PlaZone::Undefined;
  }
  if (spearman > 0.80 && ks < 0.09) {
    return PlaZone::Green;
  }
  if (spearman < 0.70 || ks > 0.12) {
    return PlaZone::Red;
  }
  return PlaZone::Amber;
}

ParityStat parity(std::span<const double> ours, std::span<const double> vendor, double floor) {
  ParityStat p;
  const std::size_t n = std::min(ours.size(), vendor.size());
  double sum_abs = 0.0;
  double sum_rel = 0.0;
  double max_abs = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!std::isfinite(ours[i]) || !std::isfinite(vendor[i])) {
      continue;
    }
    const double d = std::abs(ours[i] - vendor[i]);
    sum_abs += d;
    sum_rel += d / std::max({std::abs(ours[i]), std::abs(vendor[i]), floor});
    max_abs = std::max(max_abs, d);
    ++p.n;
  }
  if (p.n > 0) {
    p.mae = sum_abs / static_cast<double>(p.n);
    p.sym_rel = sum_rel / static_cast<double>(p.n);
    p.max_abs = max_abs;
  }
  return p;
}

double nearest_rank(std::span<const double> values, double q) {
  std::vector<double> v;
  for (const double x : values) {
    if (std::isfinite(x)) {
      v.push_back(x);
    }
  }
  if (v.empty()) {
    return kNaN;
  }
  std::sort(v.begin(), v.end());
  const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(v.size())));
  return v[std::clamp<std::size_t>(rank, 1, v.size()) - 1];
}

// ═══ Data selection ══════════════════════════════════════════════════════════

std::vector<std::string> select_buckets(std::span<const std::string> all, std::string_view from,
                                        std::string_view to) {
  std::vector<std::string> out;
  for (const std::string &b : all) {
    if (std::string_view{b} >= from && std::string_view{b} <= to) {
      out.push_back(b);
    }
  }
  return out;
}

Status prepare_synthetic_store(const RiskBenchArgs &args, RiskBenchReport &report,
                               std::ostream &log) {
  if (args.synthetic_dir.empty()) {
    return Ok();
  }
  const auto existing = list_spiderrock_buckets(args.synthetic_dir, args.date);
  if (existing.has_value() && !existing->empty()) {
    log << "[risk-bench] synthetic store reused: " << args.synthetic_dir << " (" << existing->size()
        << " buckets of " << args.date << ")\n";
    return Ok();
  }
  SyntheticSpiderRockSpec spec;
  spec.date = args.date;
  spec.n_underliers = args.underliers;
  spec.seed = args.seed;
  spec.n_threads = args.threads;
  SyntheticStoreReport sr;
  ATX_TRY_VOID(write_synthetic_spiderrock_store(spec, args.synthetic_dir, sr));
  report.synthetic_generated = true;
  report.synthetic_seconds = sr.seconds;
  report.synthetic_rows = sr.rows_written;
  log << "[risk-bench] synthetic store written: " << sr.rows_written << " rows, "
      << sr.files_written << " buckets in " << sr.seconds << " s (pricing " << sr.pricing_seconds
      << " s, " << sr.pricing_failures << " pricer refusals)\n";
  return Ok();
}

std::optional<std::size_t> find_bucket_row(const SpiderRockBucket &bucket, std::uint32_t uid,
                                           std::int64_t expiry_ns, Side side,
                                           double strike) noexcept {
  std::size_t lo = 0;
  std::size_t hi = bucket.size();
  auto less_than_key = [&](std::size_t r) {
    if (bucket.uid[r] != uid) {
      return bucket.uid[r] < uid;
    }
    if (bucket.expiry_ns[r] != expiry_ns) {
      return bucket.expiry_ns[r] < expiry_ns;
    }
    if (bucket.side[r] != side) {
      return bucket.side[r] < side;
    }
    return bucket.strike[r] < strike;
  };
  while (lo < hi) { // bounded: the interval halves
    const std::size_t mid = lo + (hi - lo) / 2;
    if (less_than_key(mid)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  const bool hit = lo < bucket.size() && bucket.uid[lo] == uid &&
                   bucket.expiry_ns[lo] == expiry_ns && bucket.side[lo] == side &&
                   bucket.strike[lo] == strike;
  return hit ? std::optional<std::size_t>{lo} : std::nullopt;
}

// ── Book selection ──────────────────────────────────────────────────────────

namespace {

struct UidRun {
  std::uint32_t uid{0};
  std::size_t begin{0};
  std::size_t end{0};
};

// One expiry of one uid: eligible rows per side (strike order), and picks so far.
struct ExpiryPool {
  std::array<std::vector<std::size_t>, 2> rows{};
  std::array<std::vector<std::uint8_t>, 2> used{};
  std::size_t picks{0};
  std::array<bool, 2> side_picked{};
};

[[nodiscard]] bool eligible(const SpiderRockBucket &b, std::size_t r) noexcept {
  const double ad = std::abs(b.vendor_delta[r]);
  return std::isfinite(ad) && ad >= kMinAbsDelta && ad <= kMaxAbsDelta &&
         std::isfinite(b.sigma[r]) && b.sigma[r] > kMinSigma && b.sigma[r] <= kMaxSigma &&
         std::isfinite(b.bid[r]) && std::isfinite(b.ask[r]) && b.bid[r] > 0.0 &&
         b.ask[r] >= b.bid[r];
}

[[nodiscard]] Result<std::vector<UidRun>> uid_runs(const SpiderRockBucket &b) {
  std::vector<UidRun> runs;
  for (std::size_t lo = 0; lo < b.size();) {
    std::size_t hi = lo + 1;
    while (hi < b.size() && b.uid[hi] == b.uid[lo]) {
      ++hi;
    }
    if (!runs.empty() && b.uid[lo] <= runs.back().uid) {
      return Err(ErrorCode::InvalidArgument,
                 "select_bench_book: bucket rows are not sorted by uid");
    }
    runs.push_back(UidRun{b.uid[lo], lo, hi});
    lo = hi;
  }
  return Ok(std::move(runs));
}

[[nodiscard]] std::vector<ExpiryPool> expiry_pools(const SpiderRockBucket &b, const UidRun &run,
                                                   std::size_t &n_candidates) {
  std::vector<ExpiryPool> pools;
  std::int64_t current = 0;
  for (std::size_t r = run.begin; r < run.end; ++r) {
    if (pools.empty() || b.expiry_ns[r] != current) {
      current = b.expiry_ns[r];
      pools.emplace_back();
    }
    if (!eligible(b, r)) {
      continue;
    }
    const std::size_t s = b.side[r] == Side::Call ? 0 : 1;
    pools.back().rows[s].push_back(r);
    pools.back().used[s].push_back(0);
    ++n_candidates;
  }
  std::erase_if(pools, [](const ExpiryPool &p) { return p.rows[0].empty() && p.rows[1].empty(); });
  return pools;
}

// One pick from `pool`: side `preferred` (0 = call) when it has an unused contract,
// else the other side; the unused contract nearest the pool's cycling |delta| target,
// lowest strike on ties. nullopt when the pool is exhausted.
[[nodiscard]] std::optional<std::size_t> pick_one(const SpiderRockBucket &b, ExpiryPool &pool,
                                                  std::size_t preferred) {
  const double target = kDeltaTargets[(pool.picks / 2) % kDeltaTargets.size()];
  for (const std::size_t s : {preferred, 1 - preferred}) {
    std::optional<std::size_t> best;
    double best_d = 0.0;
    for (std::size_t k = 0; k < pool.rows[s].size(); ++k) {
      if (pool.used[s][k] != 0) {
        continue;
      }
      const double d = std::abs(std::abs(b.vendor_delta[pool.rows[s][k]]) - target);
      if (!best.has_value() || d < best_d) {
        best = k;
        best_d = d;
      }
    }
    if (best.has_value()) {
      pool.used[s][*best] = 1;
      pool.side_picked[s] = true;
      ++pool.picks;
      return pool.rows[s][*best];
    }
  }
  return std::nullopt;
}

[[nodiscard]] double draw_qty(std::mt19937_64 &rng) {
  const auto v = static_cast<std::int64_t>(rng() % kQtyChoices); // 0..19
  return static_cast<double>(v < 10 ? v - 10 : v - 9);
}

// Picks up to `quota` legs for one uid; returns the picked rows in pick order.
[[nodiscard]] std::vector<std::size_t> pick_uid(const SpiderRockBucket &b, const UidRun &run,
                                                std::size_t quota, std::mt19937_64 &rng,
                                                BenchBook &book) {
  std::vector<ExpiryPool> pools = expiry_pools(b, run, book.n_candidates);
  std::vector<std::size_t> picked;
  if (pools.empty() || quota == 0) {
    book.min_expiries_per_uid = 0;
    return picked;
  }
  const std::size_t start = static_cast<std::size_t>(rng() % pools.size());
  const std::size_t bound = pools.size() * (run.end - run.begin + 1);
  // The side is chosen per pool: (rank + picks) % 2. Inside a pool the sides alternate,
  // so every |delta| target is taken as a call AND a put; the first pass over the pools
  // (picks == 0) alternates across them, so even a quota no larger than the expiry
  // count holds both calls and puts.
  for (std::size_t iter = 0; picked.size() < quota && iter < bound; ++iter) {
    const std::size_t rank = iter % pools.size();
    ExpiryPool &pool = pools[(start + rank) % pools.size()];
    if (const auto r = pick_one(b, pool, (rank + pool.picks) % 2); r.has_value()) {
      picked.push_back(*r);
    }
  }
  const auto n_exp = static_cast<std::size_t>(
      std::count_if(pools.begin(), pools.end(), [](const ExpiryPool &p) { return p.picks > 0; }));
  book.min_expiries_per_uid = std::min(book.min_expiries_per_uid, n_exp);
  const bool calls = std::any_of(pools.begin(), pools.end(),
                                 [](const ExpiryPool &p) { return p.side_picked[0]; });
  const bool puts = std::any_of(pools.begin(), pools.end(),
                                [](const ExpiryPool &p) { return p.side_picked[1]; });
  book.uids_with_both_sides += (calls && puts) ? 1u : 0u;
  return picked;
}

} // namespace

Result<BenchBook> select_bench_book(const SpiderRockBucket &bucket, const RiskBenchArgs &args,
                                    std::mt19937_64 &rng) {
  if (bucket.size() == 0) {
    return Err(ErrorCode::InvalidArgument, "select_bench_book: empty bucket");
  }
  if (args.underliers == 0) {
    return Err(ErrorCode::InvalidArgument, "select_bench_book: underliers must be >= 1");
  }
  // Every column the selector reads must cover every row (market_state_from_bucket
  // accepts empty vendor columns, so a legal bucket can lack them).
  const std::size_t n = bucket.size();
  if (bucket.uid.size() != n || bucket.expiry_ns.size() != n || bucket.side.size() != n ||
      bucket.strike.size() != n || bucket.vendor_delta.size() != n || bucket.bid.size() != n ||
      bucket.ask.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "select_bench_book: uid / expiry / side / strike / vendor delta / bid / ask "
               "columns must all have one value per row");
  }
  ATX_TRY(std::vector<UidRun> runs, uid_runs(bucket));
  std::stable_sort(runs.begin(), runs.end(), [](const UidRun &a, const UidRun &b) {
    return (a.end - a.begin) > (b.end - b.begin); // runs are uid-ascending: ties keep it
  });
  runs.resize(std::min(args.underliers, runs.size()));
  std::sort(runs.begin(), runs.end(),
            [](const UidRun &a, const UidRun &b) { return a.uid < b.uid; });

  BenchBook book;
  book.min_expiries_per_uid = std::numeric_limits<std::size_t>::max();
  const std::size_t base = args.options / runs.size();
  const std::size_t extra = args.options % runs.size();
  std::uint64_t next_id = 1;
  for (std::size_t u = 0; u < runs.size(); ++u) {
    const std::size_t quota = base + (u < extra ? 1 : 0);
    const std::vector<std::size_t> rows = pick_uid(bucket, runs[u], quota, rng, book);
    book.shortfall += quota - rows.size();
    book.uids.push_back(runs[u].uid);
    double net_delta = 0.0; // shares-equivalent vendor delta of this uid's legs
    for (const std::size_t r : rows) {
      const OptionLeg leg{next_id++,       runs[u].uid, bucket.strike[r], bucket.expiry_ns[r],
                          bucket.side[r], draw_qty(rng), kMultiplier};
      net_delta += leg.qty * leg.multiplier * bucket.vendor_delta[r];
      book.options.push_back(leg);
    }
    if (u < args.shares) {
      double qty = -std::round(net_delta / 100.0) * 100.0;
      qty = qty == 0.0 ? 100.0 : qty;
      book.shares.push_back(ShareLeg{0, runs[u].uid, qty});
    }
  }
  for (ShareLeg &s : book.shares) {
    s.id = next_id++;
  }
  return Ok(std::move(book));
}

// ═══ Report ══════════════════════════════════════════════════════════════════

RiskBenchReport report_skeleton(const RiskBenchArgs &args) {
  RiskBenchReport r;
  r.store = args.store_root();
  r.date = args.date;
  r.from = args.from;
  r.to = args.to;
  r.synthetic = !args.synthetic_dir.empty();
  r.kernel = to_string(args.kernel);
  r.threads = args.threads;
  r.needs_rho = args.needs_rho;
  r.trapezoid = args.trapezoid;
  r.determinism = args.determinism;
  r.seed = args.seed;
  r.requested_underliers = args.underliers;
  r.requested_options = args.options;
  r.requested_shares = args.shares;
  r.host_threads = std::thread::hardware_concurrency();
  return r;
}

namespace {

template <class F>
[[nodiscard]] StageSummary stage(const std::vector<StepReport> &steps, F field) {
  StageSummary s;
  std::vector<double> v;
  for (const StepReport &st : steps) {
    v.push_back(field(st));
    s.total += field(st);
  }
  s.p50 = nearest_rank(v, 0.50);
  s.p95 = nearest_rank(v, 0.95);
  s.mean = v.empty() ? kNaN : s.total / static_cast<double>(v.size());
  return s;
}

[[nodiscard]] double ratio_or_nan(double num, double den) noexcept {
  return den > 0.0 && std::isfinite(num) ? num / den : kNaN;
}

} // namespace

void summarize(RiskBenchReport &report) {
  const std::vector<StepReport> &steps = report.steps;
  RunSummary &s = report.summary;
  s = RunSummary{};
  s.load = stage(steps, [](const StepReport &t) { return t.load_seconds; });
  s.state = stage(steps, [](const StepReport &t) { return t.state_seconds; });
  s.evaluate = stage(steps, [](const StepReport &t) { return t.evaluate_seconds; });
  s.explain = stage(steps, [](const StepReport &t) { return t.explain_seconds; });
  s.step = stage(steps, [](const StepReport &t) { return t.step_seconds; });
  s.contracts_per_second =
      ratio_or_nan(static_cast<double>(report.n_unique_options), s.evaluate.p50);
  s.legs_per_second =
      ratio_or_nan(static_cast<double>(report.n_option_legs + report.n_share_legs), s.explain.p50);
  s.run_seconds_excl_io = report.start_state_seconds + report.start_evaluate_seconds;
  double abs_unexplained = 0.0;
  double abs_total = 0.0;
  double cold = 0.0;
  double served = 0.0;
  std::vector<double> explained;
  std::vector<double> actual;
  std::vector<double> speedups;
  for (const StepReport &t : steps) {
    s.run_seconds_excl_io += t.state_seconds + t.step_seconds;
    abs_unexplained += t.sum_abs_unexplained;
    abs_total += t.sum_abs_pnl_total;
    cold += static_cast<double>(t.cold_rows);
    served += static_cast<double>(t.family_rows + t.cold_rows);
    explained.push_back(t.explained);
    actual.push_back(t.pnl_total);
    s.determinism_checked = s.determinism_checked || t.determinism_checked;
    if (t.cold.ran) {
      speedups.push_back(t.cold.speedup);
    }
  }
  s.determinism_bit_equal =
      s.determinism_checked &&
      std::all_of(steps.begin(), steps.end(), [](const StepReport &t) {
        return !t.determinism_checked || t.determinism_bit_equal;
      });
  s.explained_ratio = ratio_or_nan(abs_unexplained, abs_total);
  s.spearman_steps = spearman_rho(explained, actual);
  s.ks_steps = steps.size() >= 2 ? ks_statistic(explained, actual) : kNaN;
  s.zone_steps = pla_zone(s.spearman_steps, s.ks_steps);
  s.cold_lane_fraction = ratio_or_nan(cold, served);
  s.cold_speedup_p50 = nearest_rank(speedups, 0.50);
}

// ── JSON ────────────────────────────────────────────────────────────────────
//
// A minimal, self-contained JSON DOM (write + read) for the report, so the core
// carries no third-party dependency into every test slice (the oracle cohort
// reader's precedent). Numbers keep their TEXT: doubles are written with
// std::to_chars' shortest round-trip form and read back with std::from_chars, so a
// report round-trips bit for bit; integers parse exactly from the same text.
// Non-finite doubles are written as null and read back as NaN.

namespace {

struct Json {
  enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
  Kind kind{Kind::Null};
  bool boolean{false};
  std::string text; // Number: the token; String: the decoded value
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> fields; // insertion order

  [[nodiscard]] const Json *find(std::string_view key) const noexcept {
    for (const auto &[k, v] : fields) {
      if (k == key) {
        return &v;
      }
    }
    return nullptr;
  }
};

[[nodiscard]] Json jnull() { return Json{}; }

[[nodiscard]] Json jbool(bool b) {
  Json j;
  j.kind = Json::Kind::Bool;
  j.boolean = b;
  return j;
}

template <class T> [[nodiscard]] Json jnumber(T v) {
  std::array<char, 40> buf{};
  const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  Json j;
  j.kind = Json::Kind::Number;
  j.text.assign(buf.data(), ec == std::errc{} ? ptr : buf.data());
  return j;
}

[[nodiscard]] Json num(double v) { return std::isfinite(v) ? jnumber(v) : jnull(); }

[[nodiscard]] Json jstr(std::string_view s) {
  Json j;
  j.kind = Json::Kind::String;
  j.text = std::string{s};
  return j;
}

[[nodiscard]] Json jarray() {
  Json j;
  j.kind = Json::Kind::Array;
  return j;
}

// An object builder: `obj({{"a", x}, {"b", y}})`.
[[nodiscard]] Json obj(std::initializer_list<std::pair<const char *, Json>> kv) {
  Json j;
  j.kind = Json::Kind::Object;
  for (const auto &[k, v] : kv) {
    j.fields.emplace_back(k, v);
  }
  return j;
}

// ── Serialization ──

void write_string(std::string &out, std::string_view s) {
  static constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  out.push_back('"');
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (u < 0x20) {
      out += "\\u00";
      out.push_back(kHex[u >> 4]);
      out.push_back(kHex[u & 0xF]);
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
}

void write_json(std::string &out, const Json &j, int depth) {
  const std::string pad(static_cast<std::size_t>(2 * (depth + 1)), ' ');
  const std::string close(static_cast<std::size_t>(2 * depth), ' ');
  switch (j.kind) {
  case Json::Kind::Null:
    out += "null";
    return;
  case Json::Kind::Bool:
    out += j.boolean ? "true" : "false";
    return;
  case Json::Kind::Number:
    out += j.text;
    return;
  case Json::Kind::String:
    write_string(out, j.text);
    return;
  case Json::Kind::Array:
    out += j.items.empty() ? "[" : "[\n";
    for (std::size_t i = 0; i < j.items.size(); ++i) {
      out += pad;
      write_json(out, j.items[i], depth + 1);
      out += (i + 1 < j.items.size()) ? ",\n" : "\n" + close;
    }
    out += "]";
    return;
  case Json::Kind::Object:
    out += j.fields.empty() ? "{" : "{\n";
    for (std::size_t i = 0; i < j.fields.size(); ++i) {
      out += pad;
      write_string(out, j.fields[i].first);
      out += ": ";
      write_json(out, j.fields[i].second, depth + 1);
      out += (i + 1 < j.fields.size()) ? ",\n" : "\n" + close;
    }
    out += "}";
    return;
  }
}

// ── Parsing (recursive descent, depth-bounded) ──

constexpr int kMaxJsonDepth = 32;

class JsonParser {
public:
  explicit JsonParser(std::string_view s) : s_{s} {}

  [[nodiscard]] Result<Json> document() {
    ATX_TRY(Json j, value(0));
    skip_ws();
    if (i_ != s_.size()) {
      return fail("trailing content");
    }
    return Ok(std::move(j));
  }

private:
  [[nodiscard]] Error fail_error(const char *what) const {
    return Error{ErrorCode::ParseError,
                 std::string("risk-bench report JSON: ") + what + " at byte " + std::to_string(i_)};
  }
  [[nodiscard]] tl::unexpected<Error> fail(const char *what) const { return Err(fail_error(what)); }

  [[nodiscard]] static bool is_ws(char c) noexcept {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
  }

  void skip_ws() noexcept {
    while (i_ < s_.size() && is_ws(s_[i_])) {
      ++i_;
    }
  }

  [[nodiscard]] bool literal(std::string_view word) noexcept {
    if (s_.substr(i_, word.size()) != word) {
      return false;
    }
    i_ += word.size();
    return true;
  }

  [[nodiscard]] Result<Json> value(int depth) {
    if (depth > kMaxJsonDepth) {
      return fail("nesting too deep");
    }
    skip_ws();
    if (i_ >= s_.size()) {
      return fail("unexpected end");
    }
    const char c = s_[i_];
    if (c == '{') {
      return object(depth);
    }
    if (c == '[') {
      return array(depth);
    }
    if (c == '"') {
      ATX_TRY(std::string str, string_token());
      return Ok(jstr(str));
    }
    if (literal("true")) {
      return Ok(jbool(true));
    }
    if (literal("false")) {
      return Ok(jbool(false));
    }
    if (literal("null")) {
      return Ok(jnull());
    }
    return number();
  }

  [[nodiscard]] Result<Json> number() {
    const std::size_t start = i_;
    while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) != 0 ||
                              s_[i_] == '-' || s_[i_] == '+' || s_[i_] == '.' || s_[i_] == 'e' ||
                              s_[i_] == 'E')) {
      ++i_;
    }
    double probe = 0.0;
    const char *first = s_.data() + start;
    const char *last = s_.data() + i_;
    const auto [ptr, ec] = std::from_chars(first, last, probe);
    if (start == i_ || ec != std::errc{} || ptr != last) {
      return fail("malformed number");
    }
    Json j;
    j.kind = Json::Kind::Number;
    j.text = std::string{s_.substr(start, i_ - start)};
    return Ok(std::move(j));
  }

  [[nodiscard]] Result<std::string> string_token() {
    ++i_; // opening quote
    std::string out;
    while (i_ < s_.size()) { // bounded: every pass consumes >= 1 byte
      const char c = s_[i_++];
      if (c == '"') {
        return Ok(std::move(out));
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (i_ >= s_.size()) {
        break;
      }
      ATX_TRY_VOID(escape(s_[i_++], out));
    }
    return fail("unterminated string");
  }

  [[nodiscard]] Status escape(char e, std::string &out) {
    switch (e) {
    case '"':
    case '\\':
    case '/':
      out.push_back(e);
      return Ok();
    case 'b':
      out.push_back('\b');
      return Ok();
    case 'f':
      out.push_back('\f');
      return Ok();
    case 'n':
      out.push_back('\n');
      return Ok();
    case 'r':
      out.push_back('\r');
      return Ok();
    case 't':
      out.push_back('\t');
      return Ok();
    case 'u': {
      unsigned code = 0;
      const char *first = s_.data() + i_;
      const char *last = first + std::min<std::size_t>(4, s_.size() - i_);
      const auto [ptr, ec] = std::from_chars(first, last, code, 16);
      if (ec != std::errc{} || ptr != first + 4 || code >= 0x80) {
        return fail("unsupported \\u escape (ASCII only)");
      }
      i_ += 4;
      out.push_back(static_cast<char>(code));
      return Ok();
    }
    default:
      return fail("bad escape");
    }
  }

  [[nodiscard]] Result<Json> array(int depth) {
    ++i_; // '['
    Json j = jarray();
    skip_ws();
    if (i_ < s_.size() && s_[i_] == ']') {
      ++i_;
      return Ok(std::move(j));
    }
    while (i_ < s_.size()) { // bounded: every element consumes input
      ATX_TRY(Json item, value(depth + 1));
      j.items.push_back(std::move(item));
      skip_ws();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        continue;
      }
      if (i_ < s_.size() && s_[i_] == ']') {
        ++i_;
        return Ok(std::move(j));
      }
      break;
    }
    return fail("malformed array");
  }

  [[nodiscard]] Result<Json> object(int depth) {
    ++i_; // '{'
    Json j;
    j.kind = Json::Kind::Object;
    skip_ws();
    if (i_ < s_.size() && s_[i_] == '}') {
      ++i_;
      return Ok(std::move(j));
    }
    while (i_ < s_.size()) { // bounded: every member consumes input
      skip_ws();
      if (i_ >= s_.size() || s_[i_] != '"') {
        break;
      }
      ATX_TRY(std::string key, string_token());
      skip_ws();
      if (i_ >= s_.size() || s_[i_] != ':') {
        break;
      }
      ++i_;
      ATX_TRY(Json v, value(depth + 1));
      j.fields.emplace_back(std::move(key), std::move(v));
      skip_ws();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        continue;
      }
      if (i_ < s_.size() && s_[i_] == '}') {
        ++i_;
        return Ok(std::move(j));
      }
      break;
    }
    return fail("malformed object");
  }

  std::string_view s_;
  std::size_t i_{0};
};

// Typed field access with first-error capture (no exceptions): a missing key or a
// wrong type records the path in `err` and returns a default; the caller checks
// `err` once at the end.
class Fields {
public:
  Fields(const Json &node, std::string path, std::string &err)
      : node_{node}, path_{std::move(path)}, err_{err} {}

  [[nodiscard]] double num(std::string_view key) const {
    const Json *v = get(key, Json::Kind::Number, /*nullable=*/true);
    if (v == nullptr || v->kind == Json::Kind::Null) {
      return kNaN;
    }
    double d = kNaN;
    const auto res = std::from_chars(v->text.data(), v->text.data() + v->text.size(), d);
    return res.ec == std::errc{} ? d : kNaN;
  }

  template <class T> [[nodiscard]] T integer(std::string_view key) const {
    const Json *v = get(key, Json::Kind::Number, false);
    T out{};
    if (v != nullptr) {
      const auto [ptr, ec] = std::from_chars(v->text.data(), v->text.data() + v->text.size(), out);
      if (ec != std::errc{} || ptr != v->text.data() + v->text.size()) {
        note(key, "not an integer of the expected range");
      }
    }
    return out;
  }

  [[nodiscard]] bool boolean(std::string_view key) const {
    const Json *v = get(key, Json::Kind::Bool, false);
    return v != nullptr && v->boolean;
  }

  [[nodiscard]] std::string text(std::string_view key) const {
    const Json *v = get(key, Json::Kind::String, false);
    return v != nullptr ? v->text : std::string{};
  }

  [[nodiscard]] Fields object(std::string_view key) const {
    const Json *v = get(key, Json::Kind::Object, false);
    return Fields{v != nullptr ? *v : empty_object(), path_ + "." + std::string{key}, err_};
  }

  [[nodiscard]] const std::vector<Json> &array(std::string_view key) const {
    static const std::vector<Json> kEmpty;
    const Json *v = get(key, Json::Kind::Array, false);
    return v != nullptr ? v->items : kEmpty;
  }

  [[nodiscard]] Fields element(const Json &item, std::size_t index) const {
    if (item.kind != Json::Kind::Object) {
      note(std::to_string(index), "is not an object");
      return Fields{empty_object(), path_, err_};
    }
    return Fields{item, path_ + "[" + std::to_string(index) + "]", err_};
  }

private:
  [[nodiscard]] static const Json &empty_object() {
    static const Json kEmpty = obj({});
    return kEmpty;
  }

  void note(std::string_view key, const char *what) const {
    if (err_.empty()) {
      err_ = "risk-bench report: " + path_ + "." + std::string{key} + " " + what;
    }
  }

  [[nodiscard]] const Json *get(std::string_view key, Json::Kind kind, bool nullable) const {
    const Json *v = node_.find(key);
    if (v == nullptr) {
      note(key, "missing");
      return nullptr;
    }
    if (v->kind != kind && !(nullable && v->kind == Json::Kind::Null)) {
      note(key, "has the wrong type");
      return nullptr;
    }
    return v;
  }

  const Json &node_;
  std::string path_;
  std::string &err_;
};

template <std::size_t N>
[[nodiscard]] Json named_counts(const std::array<std::uint32_t, N> &counts,
                                const std::array<const char *, N> &names) {
  Json o = obj({});
  for (std::size_t i = 0; i < N; ++i) {
    o.fields.emplace_back(names[i], jnumber(counts[i]));
  }
  return o;
}

template <std::size_t N>
void read_named_counts(const Fields &o, const std::array<const char *, N> &names,
                       std::array<std::uint32_t, N> &out) {
  for (std::size_t i = 0; i < N; ++i) {
    out[i] = o.integer<std::uint32_t>(names[i]);
  }
}

struct AxisField {
  const char *name;
  double AxisTotals::*field;
};
constexpr std::array<AxisField, 14> kAxisFields{{{"delta", &AxisTotals::delta},
                                                 {"gamma", &AxisTotals::gamma},
                                                 {"vega", &AxisTotals::vega},
                                                 {"volga", &AxisTotals::volga},
                                                 {"vanna", &AxisTotals::vanna},
                                                 {"theta", &AxisTotals::theta},
                                                 {"rho", &AxisTotals::rho},
                                                 {"charm", &AxisTotals::charm},
                                                 {"unexplained", &AxisTotals::unexplained},
                                                 {"vol_level", &AxisTotals::vol_level},
                                                 {"vol_skew", &AxisTotals::vol_skew},
                                                 {"vol_curv", &AxisTotals::vol_curv},
                                                 {"vol_resid", &AxisTotals::vol_resid},
                                                 {"shares", &AxisTotals::shares}}};

[[nodiscard]] Json axes_json(const AxisTotals &a) {
  Json o = obj({});
  for (const AxisField &f : kAxisFields) {
    o.fields.emplace_back(f.name, num(a.*f.field));
  }
  return o;
}

[[nodiscard]] AxisTotals axes_from(const Fields &o) {
  AxisTotals a;
  for (const AxisField &f : kAxisFields) {
    a.*f.field = o.num(f.name);
  }
  return a;
}

[[nodiscard]] Json parity_json(const ParityStat &p) {
  return obj({{"n", jnumber(p.n)},
              {"mae", num(p.mae)},
              {"sym_rel", num(p.sym_rel)},
              {"max_abs", num(p.max_abs)}});
}

[[nodiscard]] ParityStat parity_from(const Fields &o) {
  ParityStat p;
  p.n = o.integer<std::size_t>("n");
  p.mae = o.num("mae");
  p.sym_rel = o.num("sym_rel");
  p.max_abs = o.num("max_abs");
  return p;
}

struct ColdField {
  const char *name;
  double ColdComparison::*field;
};
constexpr std::array<ColdField, 12> kColdFields{
    {{"evaluate_seconds", &ColdComparison::evaluate_seconds},
     {"speedup", &ColdComparison::speedup},
     {"max_abs_price", &ColdComparison::max_abs_price},
     {"max_abs_delta", &ColdComparison::max_abs_delta},
     {"max_abs_gamma", &ColdComparison::max_abs_gamma},
     {"max_abs_vega", &ColdComparison::max_abs_vega},
     {"max_abs_theta", &ColdComparison::max_abs_theta},
     {"max_abs_vanna", &ColdComparison::max_abs_vanna},
     {"max_abs_volga", &ColdComparison::max_abs_volga},
     {"max_abs_charm", &ColdComparison::max_abs_charm},
     {"max_abs_pnl_total", &ColdComparison::max_abs_pnl_total},
     {"max_rel_price_family", &ColdComparison::max_rel_price_family}}};

[[nodiscard]] Json cold_json(const ColdComparison &c) {
  Json o = obj({{"ran", jbool(c.ran)}, {"n_compared", jnumber(c.n_compared)}});
  for (const ColdField &f : kColdFields) {
    o.fields.emplace_back(f.name, num(c.*f.field));
  }
  return o;
}

[[nodiscard]] ColdComparison cold_from(const Fields &o) {
  ColdComparison c;
  c.ran = o.boolean("ran");
  c.n_compared = o.integer<std::size_t>("n_compared");
  for (const ColdField &f : kColdFields) {
    c.*f.field = o.num(f.name);
  }
  return c;
}

[[nodiscard]] Json step_json(const StepReport &t) {
  Json clocks = obj({{"load_seconds", num(t.load_seconds)},
                     {"state_seconds", num(t.state_seconds)},
                     {"evaluate_seconds", num(t.evaluate_seconds)},
                     {"family_build_seconds", num(t.family_build_seconds)},
                     {"explain_seconds", num(t.explain_seconds)},
                     {"step_seconds", num(t.step_seconds)}});
  Json frame = obj({{"families_built", jnumber(t.families_built)},
                    {"carry_sets_built", jnumber(t.carry_sets_built)},
                    {"family_rows", jnumber(t.family_rows)},
                    {"cold_rows", jnumber(t.cold_rows)},
                    {"exercised_rows", jnumber(t.exercised_rows)},
                    {"refused_rows", jnumber(t.refused_rows)},
                    {"family_nodes", jnumber(t.family_nodes)},
                    {"routes", named_counts(t.routes, kRouteNames)},
                    {"flags", named_counts(t.flags, kFlagNames)}});
  Json explain = obj({{"n_legs", jnumber(t.n_legs)},
                      {"n_ok", jnumber(t.n_ok)},
                      {"n_excluded", jnumber(t.n_excluded)},
                      {"excluded_premium", num(t.excluded_premium)},
                      {"excluded_by_reason", named_counts(t.excluded_by_reason, kStatusNames)},
                      {"pnl_total", num(t.pnl_total)},
                      {"explained", num(t.explained)},
                      {"sum_abs_unexplained", num(t.sum_abs_unexplained)},
                      {"sum_abs_pnl_total", num(t.sum_abs_pnl_total)},
                      {"explained_ratio", num(t.explained_ratio)},
                      {"axes", axes_json(t.axes)},
                      {"axis_share", axes_json(t.axis_share)}});
  return obj({{"bucket_from", jstr(t.bucket_from)},
              {"bucket_to", jstr(t.bucket_to)},
              {"ts_from", jnumber(t.ts_from)},
              {"ts_to", jnumber(t.ts_to)},
              {"dt_years", num(t.dt_years)},
              {"clocks", clocks},
              {"frame", frame},
              {"explain", explain},
              {"quality", obj({{"spearman", num(t.spearman)},
                               {"ks", num(t.ks)},
                               {"zone", jstr(to_string(t.zone))}})},
              {"vendor_parity", obj({{"price", parity_json(t.parity_price)},
                                     {"delta", parity_json(t.parity_delta)},
                                     {"vega", parity_json(t.parity_vega)}})},
              {"determinism", obj({{"checked", jbool(t.determinism_checked)},
                                   {"bit_equal", jbool(t.determinism_bit_equal)}})},
              {"cold", cold_json(t.cold)}});
}

[[nodiscard]] PlaZone zone_from(const std::string &z) {
  for (const PlaZone p : {PlaZone::Green, PlaZone::Amber, PlaZone::Red}) {
    if (z == to_string(p)) {
      return p;
    }
  }
  return PlaZone::Undefined;
}

void clocks_from(const Fields &c, StepReport &t) {
  t.load_seconds = c.num("load_seconds");
  t.state_seconds = c.num("state_seconds");
  t.evaluate_seconds = c.num("evaluate_seconds");
  t.family_build_seconds = c.num("family_build_seconds");
  t.explain_seconds = c.num("explain_seconds");
  t.step_seconds = c.num("step_seconds");
}

void frame_from(const Fields &f, StepReport &t) {
  t.families_built = f.integer<std::uint64_t>("families_built");
  t.carry_sets_built = f.integer<std::uint64_t>("carry_sets_built");
  t.family_rows = f.integer<std::uint64_t>("family_rows");
  t.cold_rows = f.integer<std::uint64_t>("cold_rows");
  t.exercised_rows = f.integer<std::uint64_t>("exercised_rows");
  t.refused_rows = f.integer<std::uint64_t>("refused_rows");
  t.family_nodes = f.integer<std::uint64_t>("family_nodes");
  read_named_counts(f.object("routes"), kRouteNames, t.routes);
  read_named_counts(f.object("flags"), kFlagNames, t.flags);
}

void explain_from(const Fields &e, StepReport &t) {
  t.n_legs = e.integer<std::size_t>("n_legs");
  t.n_ok = e.integer<std::size_t>("n_ok");
  t.n_excluded = e.integer<std::size_t>("n_excluded");
  t.excluded_premium = e.num("excluded_premium");
  read_named_counts(e.object("excluded_by_reason"), kStatusNames, t.excluded_by_reason);
  t.pnl_total = e.num("pnl_total");
  t.explained = e.num("explained");
  t.sum_abs_unexplained = e.num("sum_abs_unexplained");
  t.sum_abs_pnl_total = e.num("sum_abs_pnl_total");
  t.explained_ratio = e.num("explained_ratio");
  t.axes = axes_from(e.object("axes"));
  t.axis_share = axes_from(e.object("axis_share"));
}

[[nodiscard]] StepReport step_from(const Fields &o) {
  StepReport t;
  t.bucket_from = o.text("bucket_from");
  t.bucket_to = o.text("bucket_to");
  t.ts_from = o.integer<std::int64_t>("ts_from");
  t.ts_to = o.integer<std::int64_t>("ts_to");
  t.dt_years = o.num("dt_years");
  clocks_from(o.object("clocks"), t);
  frame_from(o.object("frame"), t);
  explain_from(o.object("explain"), t);
  const Fields q = o.object("quality");
  t.spearman = q.num("spearman");
  t.ks = q.num("ks");
  t.zone = zone_from(q.text("zone"));
  const Fields vp = o.object("vendor_parity");
  t.parity_price = parity_from(vp.object("price"));
  t.parity_delta = parity_from(vp.object("delta"));
  t.parity_vega = parity_from(vp.object("vega"));
  t.determinism_checked = o.object("determinism").boolean("checked");
  t.determinism_bit_equal = o.object("determinism").boolean("bit_equal");
  t.cold = cold_from(o.object("cold"));
  return t;
}

[[nodiscard]] Json stage_json(const StageSummary &s) {
  return obj({{"p50", num(s.p50)}, {"p95", num(s.p95)}, {"mean", num(s.mean)},
              {"total", num(s.total)}});
}

[[nodiscard]] StageSummary stage_from(const Fields &o) {
  return StageSummary{o.num("p50"), o.num("p95"), o.num("mean"), o.num("total")};
}

[[nodiscard]] Json summary_json(const RunSummary &s) {
  return obj({{"load", stage_json(s.load)},
              {"state", stage_json(s.state)},
              {"evaluate", stage_json(s.evaluate)},
              {"explain", stage_json(s.explain)},
              {"step", stage_json(s.step)},
              {"contracts_per_second", num(s.contracts_per_second)},
              {"legs_per_second", num(s.legs_per_second)},
              {"run_seconds_excl_io", num(s.run_seconds_excl_io)},
              {"explained_ratio", num(s.explained_ratio)},
              {"spearman_steps", num(s.spearman_steps)},
              {"ks_steps", num(s.ks_steps)},
              {"zone_steps", jstr(to_string(s.zone_steps))},
              {"cold_lane_fraction", num(s.cold_lane_fraction)},
              {"determinism_checked", jbool(s.determinism_checked)},
              {"determinism_bit_equal", jbool(s.determinism_bit_equal)},
              {"cold_speedup_p50", num(s.cold_speedup_p50)}});
}

[[nodiscard]] RunSummary summary_from(const Fields &o) {
  RunSummary s;
  s.load = stage_from(o.object("load"));
  s.state = stage_from(o.object("state"));
  s.evaluate = stage_from(o.object("evaluate"));
  s.explain = stage_from(o.object("explain"));
  s.step = stage_from(o.object("step"));
  s.contracts_per_second = o.num("contracts_per_second");
  s.legs_per_second = o.num("legs_per_second");
  s.run_seconds_excl_io = o.num("run_seconds_excl_io");
  s.explained_ratio = o.num("explained_ratio");
  s.spearman_steps = o.num("spearman_steps");
  s.ks_steps = o.num("ks_steps");
  s.zone_steps = zone_from(o.text("zone_steps"));
  s.cold_lane_fraction = o.num("cold_lane_fraction");
  s.determinism_checked = o.boolean("determinism_checked");
  s.determinism_bit_equal = o.boolean("determinism_bit_equal");
  s.cold_speedup_p50 = o.num("cold_speedup_p50");
  return s;
}

[[nodiscard]] Json bucket_load_json(const BucketLoad &b) {
  return obj({{"bucket", jstr(b.bucket)},
              {"rows_read", jnumber(b.rows_read)},
              {"rows_kept", jnumber(b.rows_kept)},
              {"rows_dropped_null", jnumber(b.rows_dropped_null)},
              {"rows_dropped_bad_input", jnumber(b.rows_dropped_bad_input)},
              {"rows_not_selected", jnumber(b.rows_not_selected)},
              {"dropped_non_american", jnumber(b.dropped_non_american)},
              {"dropped_nonstandard_root", jnumber(b.dropped_nonstandard_root)},
              {"rows_null_vendor", jnumber(b.rows_null_vendor)},
              {"n_uids", jnumber(b.n_uids)},
              {"n_marks", jnumber(b.n_marks)},
              {"n_uids_with_divs", jnumber(b.n_uids_with_divs)},
              {"max_ddiv_spread", num(b.max_ddiv_spread)}});
}

[[nodiscard]] BucketLoad bucket_load_from(const Fields &o) {
  BucketLoad b;
  b.bucket = o.text("bucket");
  b.rows_read = o.integer<std::int64_t>("rows_read");
  b.rows_kept = o.integer<std::int64_t>("rows_kept");
  b.rows_dropped_null = o.integer<std::int64_t>("rows_dropped_null");
  b.rows_dropped_bad_input = o.integer<std::int64_t>("rows_dropped_bad_input");
  b.rows_not_selected = o.integer<std::int64_t>("rows_not_selected");
  b.dropped_non_american = o.integer<std::int64_t>("dropped_non_american");
  b.dropped_nonstandard_root = o.integer<std::int64_t>("dropped_nonstandard_root");
  b.rows_null_vendor = o.integer<std::int64_t>("rows_null_vendor");
  b.n_uids = o.integer<std::size_t>("n_uids");
  b.n_marks = o.integer<std::size_t>("n_marks");
  b.n_uids_with_divs = o.integer<std::size_t>("n_uids_with_divs");
  b.max_ddiv_spread = o.num("max_ddiv_spread");
  return b;
}

[[nodiscard]] Json inputs_json(const RiskBenchReport &r) {
  return obj({{"store", jstr(r.store)},
              {"date", jstr(r.date)},
              {"from", jstr(r.from)},
              {"to", jstr(r.to)},
              {"synthetic", jbool(r.synthetic)},
              {"kernel", jstr(r.kernel)},
              {"threads", jnumber(r.threads)},
              {"needs_rho", jbool(r.needs_rho)},
              {"trapezoid", jbool(r.trapezoid)},
              {"determinism", jbool(r.determinism)},
              {"seed", jnumber(r.seed)},
              {"requested_underliers", jnumber(r.requested_underliers)},
              {"requested_options", jnumber(r.requested_options)},
              {"requested_shares", jnumber(r.requested_shares)},
              {"host_threads", jnumber(r.host_threads)},
              {"family_policy", jstr(r.family_policy)}});
}

void inputs_from(const Fields &o, RiskBenchReport &r) {
  r.store = o.text("store");
  r.date = o.text("date");
  r.from = o.text("from");
  r.to = o.text("to");
  r.synthetic = o.boolean("synthetic");
  r.kernel = o.text("kernel");
  r.threads = o.integer<unsigned>("threads");
  r.needs_rho = o.boolean("needs_rho");
  r.trapezoid = o.boolean("trapezoid");
  r.determinism = o.boolean("determinism");
  r.seed = o.integer<std::uint64_t>("seed");
  r.requested_underliers = o.integer<std::size_t>("requested_underliers");
  r.requested_options = o.integer<std::size_t>("requested_options");
  r.requested_shares = o.integer<std::size_t>("requested_shares");
  r.host_threads = o.integer<unsigned>("host_threads");
  r.family_policy = o.text("family_policy");
}

void book_from(const Fields &syn, const Fields &book, RiskBenchReport &r) {
  r.synthetic_generated = syn.boolean("generated");
  r.synthetic_seconds = syn.num("seconds");
  r.synthetic_rows = syn.integer<std::size_t>("rows");
  r.n_option_legs = book.integer<std::size_t>("n_option_legs");
  r.n_unique_options = book.integer<std::size_t>("n_unique_options");
  r.n_share_legs = book.integer<std::size_t>("n_share_legs");
  r.n_underliers = book.integer<std::size_t>("n_underliers");
  r.book_shortfall = book.integer<std::size_t>("shortfall");
}

[[nodiscard]] RiskBenchReport report_from(const Fields &j) {
  RiskBenchReport r;
  r.schema_version = j.integer<int>("schema_version");
  r.tool = j.text("tool");
  inputs_from(j.object("inputs"), r);
  book_from(j.object("synthetic_store"), j.object("book"), r);
  const std::vector<Json> &buckets = j.array("buckets");
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    r.buckets.push_back(buckets[i].kind == Json::Kind::String ? buckets[i].text : std::string{});
  }
  const std::vector<Json> &loads = j.array("bucket_loads");
  for (std::size_t i = 0; i < loads.size(); ++i) {
    r.bucket_loads.push_back(bucket_load_from(j.element(loads[i], i)));
  }
  r.start_state_seconds = j.object("start").num("state_seconds");
  r.start_evaluate_seconds = j.object("start").num("evaluate_seconds");
  const std::vector<Json> &steps = j.array("steps");
  for (std::size_t i = 0; i < steps.size(); ++i) {
    r.steps.push_back(step_from(j.element(steps[i], i)));
  }
  r.summary = summary_from(j.object("summary"));
  return r;
}

} // namespace

std::string report_to_json(const RiskBenchReport &r) {
  Json buckets = jarray();
  for (const std::string &b : r.buckets) {
    buckets.items.push_back(jstr(b));
  }
  Json loads = jarray();
  for (const BucketLoad &b : r.bucket_loads) {
    loads.items.push_back(bucket_load_json(b));
  }
  Json steps = jarray();
  for (const StepReport &s : r.steps) {
    steps.items.push_back(step_json(s));
  }
  const Json j = obj({{"schema_version", jnumber(r.schema_version)},
                      {"tool", jstr(r.tool)},
                      {"inputs", inputs_json(r)},
                      {"synthetic_store", obj({{"generated", jbool(r.synthetic_generated)},
                                               {"seconds", num(r.synthetic_seconds)},
                                               {"rows", jnumber(r.synthetic_rows)}})},
                      {"book", obj({{"n_option_legs", jnumber(r.n_option_legs)},
                                    {"n_unique_options", jnumber(r.n_unique_options)},
                                    {"n_share_legs", jnumber(r.n_share_legs)},
                                    {"n_underliers", jnumber(r.n_underliers)},
                                    {"shortfall", jnumber(r.book_shortfall)}})},
                      {"buckets", buckets},
                      {"bucket_loads", loads},
                      {"start", obj({{"state_seconds", num(r.start_state_seconds)},
                                     {"evaluate_seconds", num(r.start_evaluate_seconds)}})},
                      {"steps", steps},
                      {"summary", summary_json(r.summary)}});
  std::string out;
  write_json(out, j, 0);
  out.push_back('\n');
  return out;
}

Result<RiskBenchReport> report_from_json(std::string_view text) {
  ATX_TRY(const Json root, JsonParser{text}.document());
  if (root.kind != Json::Kind::Object) {
    return Err(ErrorCode::ParseError, "risk-bench report: not a JSON object");
  }
  std::string err;
  RiskBenchReport r = report_from(Fields{root, "$", err});
  if (!err.empty()) {
    return Err(ErrorCode::ParseError, err);
  }
  if (r.schema_version != kReportSchemaVersion) {
    return Err(ErrorCode::ParseError, "risk-bench report: unsupported schema_version " +
                                          std::to_string(r.schema_version));
  }
  return Ok(std::move(r));
}

// ── Markdown ────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] std::string fmt_num(const char *format, double v) {
  if (!std::isfinite(v)) {
    return "n/a";
  }
  std::array<char, 64> buf{};
  const int n = std::snprintf(buf.data(), buf.size(), format, v);
  return std::string(buf.data(), static_cast<std::size_t>(std::clamp(n, 0, 63)));
}

[[nodiscard]] std::string ms(double seconds) { return fmt_num("%.2f", seconds * 1e3); }

void stage_row(std::string &md, const char *name, const StageSummary &s) {
  md += std::string("| ") + name + " | " + ms(s.p50) + " | " + ms(s.p95) + " | " + ms(s.mean) +
        " | " + ms(s.total) + " |\n";
}

void steps_table(std::string &md, const RiskBenchReport &r) {
  md += "\n## Steps\n\n| step | ok / legs | pnl_total | explained ratio | Spearman | KS | zone "
        "(diagnostic) | evaluate ms | explain ms | cold rows | delta parity sym rel |\n"
        "|---|---|---|---|---|---|---|---|---|---|---|\n";
  for (const StepReport &t : r.steps) {
    md += "| " + t.bucket_from + "->" + t.bucket_to + " | " + std::to_string(t.n_ok) + " / " +
          std::to_string(t.n_legs) + " | " + fmt_num("%.2f", t.pnl_total) + " | " +
          fmt_num("%.4f", t.explained_ratio) + " | " + fmt_num("%.3f", t.spearman) + " | " +
          fmt_num("%.3f", t.ks) + " | " + to_string(t.zone) + " | " + ms(t.evaluate_seconds) +
          " | " +
          ms(t.explain_seconds) + " | " + std::to_string(t.cold_rows) + " | " +
          fmt_num("%.2e", t.parity_delta.sym_rel) + " |\n";
  }
}

// "Name count, Name count" over the nonzero entries; "none" when all are zero.
template <std::size_t N>
[[nodiscard]] std::string nonzero_counts(const std::array<std::uint32_t, N> &counts,
                                         const std::array<const char *, N> &names) {
  std::string s;
  for (std::size_t i = 0; i < N; ++i) {
    if (counts[i] == 0) {
      continue;
    }
    if (!s.empty()) {
      s += ", ";
    }
    s += std::string(names[i]) + " " + std::to_string(counts[i]);
  }
  return s.empty() ? "none" : s;
}

// One line per step: the shifted frame's FrameCounters, routes and nonzero flags.
void frame_lines(std::string &md, const RiskBenchReport &r) {
  md += "\n## Frame routes and flags (shifted frame of each step)\n\n";
  for (const StepReport &t : r.steps) {
    md += "- " + t.bucket_from + "->" + t.bucket_to + ": families_built " +
          std::to_string(t.families_built) + ", carry_sets_built " +
          std::to_string(t.carry_sets_built) + ", family_rows " + std::to_string(t.family_rows) +
          ", cold_rows " + std::to_string(t.cold_rows) + ", exercised_rows " +
          std::to_string(t.exercised_rows) + ", refused_rows " + std::to_string(t.refused_rows) +
          "; routes " + nonzero_counts(t.routes, kRouteNames) + "; flags " +
          nonzero_counts(t.flags, kFlagNames) + "\n";
  }
}

} // namespace

std::string report_to_markdown(const RiskBenchReport &r) {
  const RunSummary &s = r.summary;
  std::string md = "# atx-vol-risk-bench\n\n";
  md += "- store: `" + r.store + "` (" + (r.synthetic ? "synthetic" : "licensed") + "), date " +
        r.date + ", buckets " + r.from + "..." + r.to + " (" + std::to_string(r.buckets.size()) +
        " states, " + std::to_string(r.steps.size()) + " steps)\n";
  md += "- kernel " + r.kernel + ", threads " + std::to_string(r.threads) + " (host " +
        std::to_string(r.host_threads) + "), needs_rho " + (r.needs_rho ? "yes" : "no") +
        ", trapezoid " + (r.trapezoid ? "yes" : "no") + ", seed " + std::to_string(r.seed) + "\n";
  if (!r.family_policy.empty()) {
    md += "- family policy: " + r.family_policy + "\n";
  }
  md += "- book: " + std::to_string(r.n_option_legs) + " option legs (" +
        std::to_string(r.n_unique_options) + " unique) + " + std::to_string(r.n_share_legs) +
        " share legs on " + std::to_string(r.n_underliers) + " underliers (shortfall " +
        std::to_string(r.book_shortfall) + ")\n";
  if (r.synthetic_generated) {
    md += "- synthetic store generated: " + std::to_string(r.synthetic_rows) + " rows in " +
          fmt_num("%.2f", r.synthetic_seconds) + " s\n";
  }
  md += "\n## Clocks (ms per step)\n\n| stage | p50 | p95 | mean | total |\n"
        "|---|---|---|---|---|\n";
  stage_row(md, "bucket load", s.load);
  stage_row(md, "state build", s.state);
  stage_row(md, "frame evaluate", s.evaluate);
  stage_row(md, "explain", s.explain);
  stage_row(md, "stream step", s.step);
  md += "\n- first frame evaluate: " + ms(r.start_evaluate_seconds) + " ms; run excl. I/O: " +
        fmt_num("%.3f", s.run_seconds_excl_io) + " s\n";
  md += "- throughput: " + fmt_num("%.0f", s.contracts_per_second) +
        " contracts/s (evaluate p50), " +
        fmt_num("%.0f", s.legs_per_second) + " legs/s (explain p50)\n";
  md += "- cold-lane fraction: " + fmt_num("%.4f", s.cold_lane_fraction) + "\n";
  md += "- explained ratio sum|unexplained| / sum|pnl_total|: " +
        fmt_num("%.5f", s.explained_ratio) +
        "\n";
  md += "- step-series Spearman " + fmt_num("%.3f", s.spearman_steps) + ", KS " +
        fmt_num("%.3f", s.ks_steps) + ", zone " + to_string(s.zone_steps) +
        " (FRTB PLA cutoffs, diagnostic only)\n";
  md += "- determinism: " +
        std::string(s.determinism_checked ? (s.determinism_bit_equal ? "bit-equal" : "MISMATCH")
                                          : "not checked") +
        "; cold speedup p50: " + fmt_num("%.1f", s.cold_speedup_p50) + "x\n";
  steps_table(md, r);
  frame_lines(md, r);
  return md;
}

// ── Per-leg dump (--dump-legs) ──────────────────────────────────────────────

namespace {

constexpr std::array<const char *, 5> kCarrySourceNames{"term_carry", "flat",
                                                        "flat_curve_or_divs", "surface",
                                                        "absent"};
static_assert(static_cast<std::size_t>(CarrySource::Absent) + 1 == kCarrySourceNames.size(),
              "kCarrySourceNames must cover every CarrySource");

// The columns in write order: append_leg_dump_csv writes exactly these, one cell each.
constexpr std::array<std::string_view, 63> kLegDumpColumns{
    "step",          "leg_index",     "leg_id",       "prepared_row", "ticker",
    "uid",           "uid_slot",      "strike",       "expiry",       "side",
    "qty",           "multiplier",    "T",            "sigma",        "spot",
    "forward",       "rate",          "q_eff",        "carry_source", "carry_rate",
    "carry_q",       "pv_divs",       "q_eff_d4",     "route",        "flags",
    "status",        "leg_status",    "price",        "delta",        "gamma",
    "vega",          "theta",         "vanna",        "volga",        "charm",
    "vendor_price",  "vendor_delta",  "vendor_vega",  "vendor_theta", "price_abs_err",
    "price_sym_rel", "sr_prc",        "sr_de",        "sr_ga",        "sr_ve",
    "sr_th",         "sr_years",      "sr_uprc",      "sr_rate",      "sr_sdiv",
    "sr_ddiv",       "sr_bid",        "sr_ask",       "pnl_total",    "pnl_delta",
    "pnl_gamma",     "pnl_vega",      "pnl_volga",    "pnl_vanna",    "pnl_theta",
    "pnl_rho",       "pnl_charm",     "pnl_unexplained"};

// One CSV line under construction: every cell call writes the separator first.
class CsvLine {
public:
  explicit CsvLine(std::string &out) : out_(out) {}

  void number(double v) {
    next();
    if (!std::isfinite(v)) {
      return; // non-finite => empty cell
    }
    std::array<char, 40> buf{};
    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    if (ec == std::errc{}) {
      out_.append(buf.data(), ptr);
    }
  }
  void count(std::uint64_t v) {
    next();
    out_ += std::to_string(v);
  }
  void text(std::string_view s) {
    next();
    if (s.find_first_of(",\"\r\n") == std::string_view::npos) {
      out_ += s;
      return;
    }
    out_.push_back('"');
    for (const char c : s) {
      if (c == '"') {
        out_.push_back('"');
      }
      out_.push_back(c);
    }
    out_.push_back('"');
  }
  void end() { out_.push_back('\n'); }

private:
  void next() {
    if (!first_) {
      out_.push_back(',');
    }
    first_ = false;
  }
  std::string &out_;
  bool first_{true};
};

[[nodiscard]] std::string utc_date(std::int64_t ns) {
  namespace chr = std::chrono;
  const chr::sys_time<chr::nanoseconds> t{chr::nanoseconds{ns}};
  const chr::year_month_day ymd{chr::floor<chr::days>(t)};
  std::array<char, 16> buf{};
  const int n = std::snprintf(buf.data(), buf.size(), "%04d-%02u-%02u",
                              static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                              static_cast<unsigned>(ymd.day()));
  return std::string(buf.data(), static_cast<std::size_t>(std::clamp(n, 0, 15)));
}

[[nodiscard]] double cell_at(const std::vector<double> &column, std::size_t r) noexcept {
  return r < column.size() ? column[r] : kNaN;
}

} // namespace

const char *to_string(CarrySource source) noexcept {
  const auto i = static_cast<std::size_t>(source);
  return i < kCarrySourceNames.size() ? kCarrySourceNames[i] : "absent";
}

void set_leg_dump_vendor(LegDumpRow &row, const SpiderRockBucket &b, std::size_t r) {
  row.sr_prc = cell_at(b.vendor_price, r);
  row.sr_de = cell_at(b.vendor_delta, r);
  row.sr_ga = cell_at(b.vendor_gamma, r);
  row.sr_ve = cell_at(b.vendor_vega, r);
  row.sr_th = cell_at(b.vendor_theta, r);
  row.sr_years = cell_at(b.vendor_T, r);
  row.sr_uprc = cell_at(b.uprc, r);
  row.sr_rate = cell_at(b.rate, r);
  row.sr_sdiv = cell_at(b.sdiv, r);
  row.sr_ddiv = cell_at(b.ddiv, r);
  row.sr_bid = cell_at(b.bid, r);
  row.sr_ask = cell_at(b.ask, r);
}

std::string leg_dump_csv_header() {
  std::string out;
  CsvLine line{out};
  for (const std::string_view name : kLegDumpColumns) {
    line.text(name);
  }
  line.end();
  return out;
}

void append_leg_dump_csv(std::string &out, const LegDumpRow &row) {
  const double q_eff_d4 = spiderrock_q_eff(row.spot, row.pv_divs, row.carry_rate, row.carry_q,
                                           row.T);
  const bool priced = std::isfinite(row.price) && std::isfinite(row.vendor_price);
  const double abs_err = priced ? std::abs(row.price - row.vendor_price) : kNaN;
  const double sym_rel =
      priced ? abs_err / std::max({std::abs(row.price), std::abs(row.vendor_price),
                                   kParityPriceFloor})
             : kNaN;
  CsvLine line{out};
  line.text(row.step);
  line.count(row.leg_index);
  line.count(row.leg_id);
  line.count(row.prepared_row);
  line.text(row.ticker);
  line.count(row.uid);
  line.count(row.uid_slot);
  line.number(row.strike);
  line.text(utc_date(row.expiry_ns));
  line.text(row.side == Side::Put ? "put" : "call");
  line.number(row.qty);
  line.number(row.multiplier);
  line.number(row.T);
  line.number(row.sigma);
  line.number(row.spot);
  line.number(row.forward);
  line.number(row.rate);
  line.number(row.q_eff);
  line.text(to_string(row.carry_source));
  line.number(row.carry_rate);
  line.number(row.carry_q);
  line.number(row.pv_divs);
  line.number(q_eff_d4);
  line.text(row.route);
  line.text(row.flags);
  line.text(row.status);
  line.text(row.leg_status);
  for (const double v : {row.price, row.delta, row.gamma, row.vega, row.theta, row.vanna,
                         row.volga, row.charm, row.vendor_price, row.vendor_delta}) {
    line.number(v);
  }
  line.number(row.sr_ve / kSpiderRockVegaScale);
  line.number(row.sr_th / kSpiderRockThetaScale);
  line.number(abs_err);
  line.number(sym_rel);
  for (const double v : {row.sr_prc, row.sr_de, row.sr_ga, row.sr_ve, row.sr_th, row.sr_years,
                         row.sr_uprc, row.sr_rate, row.sr_sdiv, row.sr_ddiv, row.sr_bid,
                         row.sr_ask}) {
    line.number(v);
  }
  for (const double v : {row.pnl_total, row.pnl_delta, row.pnl_gamma, row.pnl_vega,
                         row.pnl_volga, row.pnl_vanna, row.pnl_theta, row.pnl_rho, row.pnl_charm,
                         row.pnl_unexplained}) {
    line.number(v);
  }
  line.end();
}

std::vector<std::size_t> leg_dump_order(std::span<const std::uint32_t> leg_prepared_row) {
  std::vector<std::size_t> order(leg_prepared_row.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return leg_prepared_row[a] < leg_prepared_row[b];
  });
  return order;
}

bool dump_step_selected(std::size_t limit, std::size_t step) noexcept {
  return limit == 0 || step <= limit;
}

Status write_text_file(const std::string &path, std::string_view text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    return Err(ErrorCode::IoError, "cannot open for writing: " + path);
  }
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!f) {
    return Err(ErrorCode::IoError, "write failed: " + path);
  }
  return Ok();
}

} // namespace atx::vol::riskbench
