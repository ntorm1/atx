#pragma once

// risk_bench — the testable core of `atx-vol-risk-bench` (design 2026-10-24 §5.8;
// CLI, report schema and conventions in atx-vol/docs/risk-bench.md).
//
// Two halves, one header:
//
//   ENGINE-FREE core (tools/risk_bench_core.cpp; compiled into atx-vol-tests as a
//   support TU): CLI parsing, synthetic-store preparation, bucket range selection,
//   book selection over a loaded SpiderRockBucket (SoA), the explain-quality
//   statistics (Spearman, two-sample KS, the FRTB PLA zone used as a DIAGNOSTIC),
//   vendor parity, run summaries, the versioned JSON report (write AND read) and the
//   markdown summary. Nothing here references a risk-engine symbol, so the core links
//   without the engine.
//
//   ENGINE driver (tools/risk_bench.cpp): `run_risk_bench` drives RiskStream over the
//   buckets and fills the report through the core. It links only where the engine
//   (lane A's src/risk/*) is compiled in: the tool target, and the end-to-end test
//   behind ATX_VOL_RISK_E2E.
//
// The licensed SpiderRock store and the synthetic store go through the IDENTICAL
// loader and engine path; only the directory differs (design D7).

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/marketdata/spiderrock_store.hpp"
#include "atx/vol/api/risk/risk_book.hpp"  // OptionLeg, ShareLeg (aggregates only)
#include "atx/vol/api/risk/risk_types.hpp" // RiskRoute, RiskFlag, kRiskLaneStatusCount

namespace atx::vol::riskbench {

// v2 (follow-up 2): inputs.time / inputs.carry_mode and the BucketLoad framework-filter and
// implied-carry fields; the reader requires every v2 field and refuses v1.
inline constexpr int kReportSchemaVersion = 2;
inline constexpr std::size_t kRouteCount = 7; // RiskRoute::None .. RiskRoute::Refused
inline constexpr std::size_t kFlagCount = 11; // RiskFlag bits 0 .. 10
inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ═══ CLI ═════════════════════════════════════════════════════════════════════

enum class BenchKernel : std::uint8_t { Family = 0, Cold = 1, Both = 2 };

struct RiskBenchArgs {
  std::string store;            // --store <root> (the dir holding date=<d>/)
  std::string date{"2026-10-14"}; // the licensed 30-min drop's trading date
  std::string from{"1000"};     // --from HHMM (first bucket, inclusive)
  std::string to{"1500"};       // --to HHMM (last bucket, inclusive)
  std::string synthetic_dir;    // --synthetic <dir>: write the synthetic store there first
  std::size_t underliers{100};
  std::size_t options{10000};
  std::size_t shares{100};
  BenchKernel kernel{BenchKernel::Family};
  unsigned threads{8};          // 0 = executor auto
  bool needs_rho{false};        // --needs rho
  bool trapezoid{false};        // --trapezoid (ExplainOptions::trapezoid_second_order)
  bool determinism{false};      // --determinism (threads=1 shadow stream, bit-compare)
  std::string out_json;         // --out <file>  (stdout when empty)
  std::string out_md;           // --md <file>   (not written when empty)
  std::string dump_legs;        // --dump-legs <file.csv>: per-option-leg CSV (none when empty)
  std::size_t dump_steps{1};    // --dump-steps N: dump the first N steps (0 = every step)
  std::uint64_t seed{1};
  bool help{false};             // --help / -h
  // ── Follow-up 2 additions (appended) ──
  // --time voltime|calendar365: the clock every state (and so the kernel) measures T on.
  // Calendar365 under both stores: VolTime reproduces SpiderRock's `years` only up to the
  // first NYSE early close (docs/risk-bench.md "Clock"), and the synthetic store's vendor
  // columns are priced on the calendar clock.
  TimeConvention time{TimeConvention::Calendar365};

  // The store the run reads: the synthetic dir when set, else --store.
  [[nodiscard]] const std::string &store_root() const noexcept {
    return synthetic_dir.empty() ? store : synthetic_dir;
  }
};

// `argv[0]` is the program name and is skipped. InvalidArgument (a usage error,
// exit 2) on an unknown flag, a missing / malformed value, from > to, or neither
// --store nor --synthetic (unless --help).
[[nodiscard]] Result<RiskBenchArgs> parse_risk_bench_args(std::span<const char *const> argv);
[[nodiscard]] std::string risk_bench_usage();
[[nodiscard]] const char *to_string(BenchKernel kernel) noexcept;
// "calendar365" | "voltime" (the --time spelling).
[[nodiscard]] const char *time_name(TimeConvention time) noexcept;
// "published" | "implied_from_calls".
[[nodiscard]] const char *carry_mode_name(SpiderRockCarryMode mode) noexcept;
// The carry mode a run builds its states with (no flag): Published under --synthetic (its
// vendor columns are priced on exactly the D4 carry, design D20), ImpliedFromCalls under
// --store (SpiderRock's escrow horizon is not a column; spiderrock_store.hpp).
[[nodiscard]] SpiderRockCarryMode bench_carry_mode(const RiskBenchArgs &args) noexcept;

// ═══ Report ══════════════════════════════════════════════════════════════════

enum class PlaZone : std::uint8_t { Green = 0, Amber = 1, Red = 2, Undefined = 3 };
[[nodiscard]] const char *to_string(PlaZone zone) noexcept;

// Vendor parity of one column: MAE and the mean SYMMETRIC relative error
// |m - o| / max(|m|, |o|, floor) over finite pairs (bench/oracle/CONVENTIONS.md's
// bounded convention). NaN statistics when n == 0.
struct ParityStat {
  std::size_t n{0};
  double mae{kNaN};
  double sym_rel{kNaN};
  double max_abs{kNaN};
};

// Position-scaled P&L per explain axis (ExplainTotals' columns).
struct AxisTotals {
  double delta{0.0};
  double gamma{0.0};
  double vega{0.0};
  double volga{0.0};
  double vanna{0.0};
  double theta{0.0};
  double rho{0.0};
  double charm{0.0};
  double unexplained{0.0};
  double vol_level{0.0};
  double vol_skew{0.0};
  double vol_curv{0.0};
  double vol_resid{0.0};
  double shares{0.0};
};

// `--kernel both`: the cold kernel on the same states, compared row by row.
struct ColdComparison {
  bool ran{false};
  double evaluate_seconds{0.0};
  double speedup{kNaN};        // cold evaluate / main evaluate
  std::size_t n_compared{0};   // unique options Ok in both frames
  double max_abs_price{kNaN};
  double max_abs_delta{kNaN};
  double max_abs_gamma{kNaN};
  double max_abs_vega{kNaN};
  double max_abs_theta{kNaN};
  double max_abs_vanna{kNaN};
  double max_abs_volga{kNaN};
  double max_abs_charm{kNaN};
  double max_abs_pnl_total{kNaN}; // per leg, legs Ok in both explains
};

// One explain step (bucket k-1 -> bucket k).
struct StepReport {
  std::string bucket_from;
  std::string bucket_to;
  std::int64_t ts_from{0};
  std::int64_t ts_to{0};
  double dt_years{0.0};
  // Clocks (seconds, steady_clock; evaluate / family build from FrameCounters).
  double load_seconds{0.0};
  double state_seconds{0.0};
  double evaluate_seconds{0.0};
  double family_build_seconds{0.0};
  double explain_seconds{0.0};
  double step_seconds{0.0}; // RiskStream::step wall clock (evaluate + explain + roll)
  // Shifted-frame counters.
  std::uint64_t families_built{0};
  std::uint64_t carry_sets_built{0};
  std::uint64_t family_rows{0};
  std::uint64_t cold_rows{0};
  std::uint64_t exercised_rows{0};
  std::uint64_t refused_rows{0};
  std::array<std::uint32_t, kRouteCount> routes{}; // per unique option, by RiskRoute
  std::array<std::uint32_t, kFlagCount> flags{};   // unique options with each flag bit
  // Explain counts.
  std::size_t n_legs{0};
  std::size_t n_ok{0};
  std::size_t n_excluded{0};
  double excluded_premium{0.0};
  std::array<std::uint32_t, kRiskLaneStatusCount> excluded_by_reason{};
  // Totals and quality.
  double pnl_total{0.0};
  double explained{0.0};           // pnl_total - pnl_unexplained (totals)
  double sum_abs_unexplained{0.0}; // over Ok legs
  double sum_abs_pnl_total{0.0};
  double explained_ratio{kNaN};    // sum_abs_unexplained / sum_abs_pnl_total
  AxisTotals axes{};
  AxisTotals axis_share{};         // axis / sum of |axes| (delta..charm + unexplained)
  double spearman{kNaN};           // per-leg explained vs actual (diagnostic)
  double ks{kNaN};
  PlaZone zone{PlaZone::Undefined};
  ParityStat parity_price{};
  ParityStat parity_delta{};
  ParityStat parity_vega{};
  bool determinism_checked{false};
  bool determinism_bit_equal{false};
  ColdComparison cold{};
};

struct StageSummary {
  double p50{kNaN};
  double p95{kNaN};
  double mean{kNaN};
  double total{0.0};
};

struct RunSummary {
  StageSummary load{};
  StageSummary state{};
  StageSummary evaluate{};
  StageSummary explain{};
  StageSummary step{};
  double contracts_per_second{kNaN}; // n_unique_options / evaluate p50
  double legs_per_second{kNaN};      // (n_option_legs + n_share_legs) / explain p50
  double run_seconds_excl_io{0.0};   // start + every step: state + evaluate + explain
  double explained_ratio{kNaN};      // over all steps' Ok legs
  double spearman_steps{kNaN};       // per-step explained vs actual TOTALS series
  double ks_steps{kNaN};
  PlaZone zone_steps{PlaZone::Undefined};
  double cold_lane_fraction{kNaN};   // cold_rows / (family_rows + cold_rows) over steps
  bool determinism_checked{false};
  bool determinism_bit_equal{false};
  double cold_speedup_p50{kNaN};
};

// Per bucket loaded: the loader's and the state builder's accounting.
struct BucketLoad {
  std::string bucket;
  std::int64_t rows_read{0};
  std::int64_t rows_kept{0};
  std::int64_t rows_dropped_null{0};
  std::int64_t rows_dropped_bad_input{0};
  std::int64_t rows_not_selected{0};
  std::int64_t dropped_non_american{0};
  std::int64_t dropped_nonstandard_root{0};
  std::int64_t rows_null_vendor{0};
  std::size_t n_uids{0};
  std::size_t n_marks{0};
  std::size_t n_uids_with_divs{0};
  double max_ddiv_spread{0.0};
  // ── Follow-up 2 additions (appended; schema v2) ──
  std::int64_t dropped_forward_framework{0};
  bool pv_divs_from_ddiv_pv{false}; // BucketStateReport::pv_divs_from_ddiv_pv
  // BucketStateReport's carry accounting (n_expiries_implied + n_expiries_published ==
  // the ExpiryCarry rows; max_forward_gap_rel = max_implied_vs_published_forward_rel).
  std::size_t n_expiries_implied{0};
  std::size_t n_expiries_published{0};
  double max_forward_gap_rel{0.0};
  double implied_carry_seconds{0.0};
};

struct RiskBenchReport {
  int schema_version{kReportSchemaVersion};
  std::string tool{"atx-vol-risk-bench"};
  // Inputs (echo of the CLI).
  std::string store;
  std::string date;
  std::string from;
  std::string to;
  bool synthetic{false};
  std::string kernel;
  unsigned threads{0};
  bool needs_rho{false};
  bool trapezoid{false};
  bool determinism{false};
  std::uint64_t seed{0};
  std::size_t requested_underliers{0};
  std::size_t requested_options{0};
  std::size_t requested_shares{0};
  unsigned host_threads{0};
  std::string time;       // time_name(args.time)
  std::string carry_mode; // carry_mode_name(bench_carry_mode(args))
  // Synthetic store preparation.
  bool synthetic_generated{false};
  double synthetic_seconds{0.0};
  std::size_t synthetic_rows{0};
  // Book.
  std::size_t n_option_legs{0};
  std::size_t n_unique_options{0};
  std::size_t n_share_legs{0};
  std::size_t n_underliers{0};
  std::size_t book_shortfall{0};
  // Buckets and the first frame.
  std::vector<std::string> buckets;
  std::vector<BucketLoad> bucket_loads;
  double start_state_seconds{0.0};    // state build of the first bucket
  double start_evaluate_seconds{0.0}; // RiskStream::start (first frame)
  std::vector<StepReport> steps;
  RunSummary summary{};
};

// Echo `args` into a fresh report (inputs only).
[[nodiscard]] RiskBenchReport report_skeleton(const RiskBenchArgs &args);

// Fills `report.summary` from `report.steps` and the book counts. Nearest-rank
// percentiles over steps; the step-series Spearman / KS need >= 2 steps.
void summarize(RiskBenchReport &report);

// Versioned JSON (schema_version 1; every field documented in docs/risk-bench.md).
// Non-finite doubles are written as null and read back as NaN.
[[nodiscard]] std::string report_to_json(const RiskBenchReport &report);
[[nodiscard]] Result<RiskBenchReport> report_from_json(std::string_view text);
[[nodiscard]] std::string report_to_markdown(const RiskBenchReport &report);
// Writes `text` to `path` (IoError on failure).
[[nodiscard]] Status write_text_file(const std::string &path, std::string_view text);

// ═══ Statistics ══════════════════════════════════════════════════════════════

// Spearman rank correlation with average ranks for ties; NaN when n < 2, the
// lengths differ, or either side has zero rank variance.
[[nodiscard]] double spearman_rho(std::span<const double> x, std::span<const double> y);
// Two-sample Kolmogorov-Smirnov statistic sup |F_a - F_b| over the ECDFs; NaN when
// either sample is empty.
[[nodiscard]] double ks_statistic(std::span<const double> a, std::span<const double> b);
// FRTB PLA cutoffs verbatim (design D9): Green = Spearman > 0.80 AND KS < 0.09;
// Red = Spearman < 0.70 OR KS > 0.12; Amber otherwise; Undefined on NaN.
[[nodiscard]] PlaZone pla_zone(double spearman, double ks) noexcept;
[[nodiscard]] ParityStat parity(std::span<const double> ours, std::span<const double> vendor,
                                double floor);
// Nearest-rank percentile (q in (0, 1]) of `values`; NaN when empty.
[[nodiscard]] double nearest_rank(std::span<const double> values, double q);

// ═══ Data selection ══════════════════════════════════════════════════════════

// Buckets of `all` (ascending HHMM) inside [from, to].
[[nodiscard]] std::vector<std::string> select_buckets(std::span<const std::string> all,
                                                      std::string_view from, std::string_view to);

// `--synthetic <dir>`: writes the synthetic store for `args.date` (n_underliers =
// args.underliers, the default 11 buckets 10:00..15:00, seed = args.seed) unless the
// dir already holds a bucket of that date. Records the generation in `report` and
// prints the time to `log`. No-op without --synthetic.
[[nodiscard]] Status prepare_synthetic_store(const RiskBenchArgs &args, RiskBenchReport &report,
                                             std::ostream &log);

// Row of `bucket` (sorted by (uid, expiry_ns, side, strike)) with that exact key.
[[nodiscard]] std::optional<std::size_t> find_bucket_row(const SpiderRockBucket &bucket,
                                                         std::uint32_t uid, std::int64_t expiry_ns,
                                                         Side side, double strike) noexcept;

// The benchmark book, deterministic from `rng`:
//   uids     top `args.underliers` by row count in `bucket` (ties: lower uid), ascending;
//   options  args.options split evenly over the uids (remainder to the first ones);
//            per uid, eligible rows have |vendor_delta| in [0.05, 0.95], sigma in
//            (0.01, 3.0] (the licensed drop carries penny-name srVol >= 5 rows) and a
//            two-sided quote (bid > 0, ask >= bid); picks go round-robin over the
//            uid's expiries (starting at an rng-chosen one); the side alternates inside
//            each expiry (call and put at every |delta| target) and across expiries on
//            the first pass (so a quota <= the expiry count still holds both sides);
//            each pick is the unused contract nearest the expiry's cycling |delta|
//            target grid;
//            qty uniform in {-10..-1, 1..10}, multiplier 100, ids 1..n;
//   shares   one leg per uid for the first min(args.shares, n_uids) uids, qty =
//            -round(sum(qty * 100 * vendor_delta) / 100) * 100 (100 when that is 0),
//            ids continuing after the options.
// A uid with fewer eligible rows than its quota contributes what it has
// (`shortfall` counts the gap). InvalidArgument when the bucket is empty, when
// `args.underliers` is 0, or when a column the selector reads (uid, expiry_ns, side,
// strike, vendor_delta, bid, ask) does not have one value per row.
struct BenchBook {
  std::vector<OptionLeg> options;
  std::vector<ShareLeg> shares;
  std::vector<std::uint32_t> uids;      // selected, ascending
  std::size_t n_candidates{0};          // eligible rows over the selected uids
  std::size_t min_expiries_per_uid{0};  // fewest distinct expiries picked for a uid
  std::size_t uids_with_both_sides{0};
  std::size_t shortfall{0};
};
[[nodiscard]] Result<BenchBook> select_bench_book(const SpiderRockBucket &bucket,
                                                  const RiskBenchArgs &args, std::mt19937_64 &rng);

// ═══ Per-leg dump (--dump-legs) ══════════════════════════════════════════════
//
// One CSV row per OPTION leg per dumped step (share legs are not dumped), in PREPARED
// row order (ties: leg input order). Every model column is the step's BASE side: the
// base frame (RiskStream::previous() after the step), the base state's carry and the
// base bucket's raw vendor row; the P&L columns are the step's per-leg explain.
// Header row, LF line endings, doubles in shortest round-trip form, a non-finite value
// is an EMPTY cell. Columns are listed in docs/risk-bench.md.

// Vendor parity floors (the symmetric-relative denominator's minimum): one tick in
// price, 1e-3 in delta and in vega per unit vol.
inline constexpr double kParityPriceFloor = 0.01;
inline constexpr double kParityDeltaFloor = 1e-3;
inline constexpr double kParityVegaFloor = 1e-3;

// Where the carry a contract was priced with came from in the base state (the
// MarketState precedence, market_state.hpp):
//   TermCarry        the uid's ExpiryCarry row with the contract's exact expiry_ns
//   Flat             no row: flat_rate / flat_q, no cash dividends (pv_divs = 0)
//   FlatCurveOrDivs  no row, and a yield curve or cash dividends apply: the rate /
//                    pv_divs the engine derived are not reproduced (empty cells; the
//                    frame's rate / q_eff columns are what the kernel used)
//   Surface          CarryFrom::Surface (carry read from the uid's surface)
//   Absent           the uid is not in the base state
enum class CarrySource : std::uint8_t {
  TermCarry = 0,
  Flat = 1,
  FlatCurveOrDivs = 2,
  Surface = 3,
  Absent = 4
};
[[nodiscard]] const char *to_string(CarrySource source) noexcept;

struct LegDumpRow {
  std::string step;            // "<from>-><to>"
  std::size_t leg_index{0};    // option-leg input index
  std::uint64_t leg_id{0};
  std::uint32_t prepared_row{0};
  std::string ticker;
  std::uint32_t uid{0};
  std::uint32_t uid_slot{0};   // index into PreparedRiskBook::uids()
  double strike{kNaN};
  std::int64_t expiry_ns{0};   // written as its UTC date YYYY-MM-DD
  Side side{Side::Call};
  double qty{kNaN};
  double multiplier{kNaN};
  // The base frame's resolved inputs (per share).
  double T{kNaN};
  double sigma{kNaN};
  double spot{kNaN};           // the base state's UnderlierState spot
  double forward{kNaN};
  double rate{kNaN};           // the frame's rate column (what the kernel priced at)
  double q_eff{kNaN};          // the frame's q_eff column
  CarrySource carry_source{CarrySource::Absent};
  double carry_rate{kNaN};     // the ExpiryCarry row's rate (Flat: flat_rate)
  double carry_q{kNaN};        // the row's q (Flat: flat_q)
  double pv_divs{kNaN};        // the row's pv_divs (Flat: 0)
  std::string route;           // to_string(RiskRoute) of the base frame row
  std::string flags;           // to_string(RiskFlag) names of the set bits, '|'-joined
  std::string status;          // to_string(RiskLaneStatus) of the base frame row
  std::string leg_status;      // the explain's status for this leg
  // Ours: the base frame (per share, unscaled; risk_frame.hpp conventions).
  double price{kNaN};
  double delta{kNaN};
  double gamma{kNaN};
  double vega{kNaN};
  double theta{kNaN};
  double vanna{kNaN};
  double volga{kNaN};
  double charm{kNaN};
  // The vendor as the engine carried it (the base frame's marks columns; NaN unless
  // VendorParityAvailable).
  double vendor_price{kNaN};
  double vendor_delta{kNaN};
  // The base bucket's row, RAW SpiderRock units (set_leg_dump_vendor).
  double sr_prc{kNaN};
  double sr_de{kNaN};
  double sr_ga{kNaN};
  double sr_ve{kNaN};
  double sr_th{kNaN};
  double sr_years{kNaN};
  double sr_uprc{kNaN};
  double sr_rate{kNaN};
  double sr_sdiv{kNaN};
  double sr_ddiv{kNaN};
  double sr_bid{kNaN};
  double sr_ask{kNaN};
  // The step's per-leg explain (position-scaled).
  double pnl_total{kNaN};
  double pnl_delta{kNaN};
  double pnl_gamma{kNaN};
  double pnl_vega{kNaN};
  double pnl_volga{kNaN};
  double pnl_vanna{kNaN};
  double pnl_theta{kNaN};
  double pnl_rho{kNaN};
  double pnl_charm{kNaN};
  double pnl_unexplained{kNaN};
};

// Copies row `r` of `bucket` into the sr_* columns of `row`.
void set_leg_dump_vendor(LegDumpRow &row, const SpiderRockBucket &bucket, std::size_t r);
// The header line (LF-terminated).
[[nodiscard]] std::string leg_dump_csv_header();
// Appends one LF-terminated line. Derived columns, computed here:
//   q_eff_d4       spiderrock_q_eff(spot, pv_divs, carry_rate, carry_q, T): the D4 carry
//                  the row implies (compare with q_eff, what the kernel used)
//   vendor_vega    sr_ve / kSpiderRockVegaScale   (per unit vol, as parity compares)
//   vendor_theta   sr_th / kSpiderRockThetaScale  (per year)
//   price_abs_err  |price - vendor_price|
//   price_sym_rel  price_abs_err / max(|price|, |vendor_price|, kParityPriceFloor)
// A string cell holding ',' '"' CR or LF is quoted (RFC 4180).
void append_leg_dump_csv(std::string &out, const LegDumpRow &row);
// Option-leg indices ordered by prepared row, ties by leg index (a stable sort).
[[nodiscard]] std::vector<std::size_t>
leg_dump_order(std::span<const std::uint32_t> leg_prepared_row);
// Whether 1-based step `step` is dumped under `--dump-steps limit` (0 = every step).
[[nodiscard]] bool dump_step_selected(std::size_t limit, std::size_t step) noexcept;

// ═══ Engine driver (tools/risk_bench.cpp) ════════════════════════════════════

// Runs the benchmark: prepares the synthetic store when asked, loads buckets
// [from, to] (top-N underliers fixed at the first bucket), selects the book on the
// first bucket, drives a RiskStream (plus the threads=1 shadow for --determinism and
// the cold kernel for --kernel both) and returns the filled, summarized report.
// With --dump-legs it writes the per-leg CSV (opened before any bucket is read).
// Progress goes to `log`. NotFound / InvalidArgument / IoError / engine errors propagate.
[[nodiscard]] Result<RiskBenchReport> run_risk_bench(const RiskBenchArgs &args, std::ostream &log);

} // namespace atx::vol::riskbench
