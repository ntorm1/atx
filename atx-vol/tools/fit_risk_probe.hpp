#pragma once

// fit_risk_probe — lane F (wave 3, risk-engine sprint 2026-10-24): price and Greek every
// SERVED row of one quote-slice fit run (atx-vol-quote-bench predictions) through the
// risk engine (RiskKernel family + cold) at the FITTED vol, and report routes, flags,
// availability, Greek sanity and the reprice identity per (uid, root, bucket, expiry)
// chain. Design: .superpowers/sdd/2026-10-24-risk-engine/wave3-fit-robustness-design.md §4
// note 7; CLI and JSON schema: docs/universe-fit-bench.md.
//
// ## The shadow clock (why the probe owns the clock and the carry)
//
// The fitter prices an expiry on TWO clocks: volatility on the vendor VARIANCE clock
// (`years`, T_v) and carry on the CALENDAR clock (`years_calendar`, T_c), with the rate and
// yield rescaled by T_c / T_v and an aggregate cash-dividend PV folded into an equivalent
// yield (src/fitting/quote_slice.cpp `converted_yield`). A MarketState has ONE TimeSpec, so
// it cannot hold both clocks. The probe therefore maps every chain onto a synthetic
// Calendar365 expiry `expiry_ns' = ts + llround(T_v * kCalendarYearNs)` whose ExpiryCarry is
// {rate = r * (T_c / T_v), q = converted_yield, pv_divs = 0}: the engine then prices on
// exactly the fitter's basis. Each chain is its own synthetic uid (its own spot: the store
// carries several per-expiry spots per underlier), and each distinct supplied carry inside a
// chain gets its own expiry offset of +j ns (|T' - T_v| stays far below 1e-12 years).
// Under `snapshot.one_spot` the run's per-chain snapshot spot S* is applied through the
// library's own `rebase_quote_slice_spot` / `coalesce_quote_slice_carry`, so a rebased fit
// is reproduced too.
//
// ## Governance
//
// Labels (srPrc / srVol / vendor Greeks) are read ONLY when the dataset manifest says
// `rows_by_split == {"development": n}` (`labels_allowed`); every other manifest refuses
// them (PermissionDenied) and the probe never prints underlier membership of a split.
//
// ## Determinism
//
// Work is fanned out per underlier over `pricing_executor()`; each unit writes only its own
// rows; every chain and total is aggregated serially in input order (Neumaier sums), so the
// report is bit-identical for any thread count.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/risk/risk_types.hpp"

namespace atx::vol::fit_probe {

inline constexpr std::string_view kProbeSchema = "atx-vol-fit-risk-probe";
inline constexpr int kProbeSchemaVersion = 1;

// Reprice identity: a served row MISMATCHES when |P_engine - P_predicted| exceeds
// max(kRepriceRel * P_predicted, kRepriceAbs) (design §4 note 6, `reprice_mismatch`).
inline constexpr double kRepriceRel = 1.0e-6;
inline constexpr double kRepriceAbs = 1.0e-8;
// The reprice gap is also stated in vol bp (|dP| / vega) where vega is at least this: a
// deep-wing row with a vanishing vega has no meaningful vol-space gap.
inline constexpr double kRepriceVegaFloor = 1.0e-6;
// The shadow clock's rounding bound, asserted per expiry: |T' - T_v| <= this.
inline constexpr double kShadowClockTolerance = 1.0e-12;
// American slack of the call-put delta check. European parity makes the gap Dc - Dp exactly
// exp(-q T); an early-exercise premium raises a call's delta and lowers a put's, so an
// American gap is bounded BELOW by exp(-q T) only (the real panel measures it up to +0.23
// above on long tenors). A gap more than this below exp(-q T), with neither leg near the
// exercise edge or exercised, is a sanity failure.
inline constexpr double kCpDeltaSlack = 0.10;
// Rounding allowance of the per-row sign / range checks: deep in-the-money cold-analytic
// Greeks carry |gamma|, |vega| of 1e-13..1e-11 noise around zero.
inline constexpr double kSanityTolerance = 1.0e-9;

// ── Inputs ─────────────────────────────────────────────────────────────────

// One row of the label-isolated adapter's quotes.csv (tools/quote_benchmark.py
// QUOTE_SCHEMA): only the columns the probe needs.
struct QuoteRow {
  std::uint64_t row_id{0};
  std::uint64_t chain_id{0};
  std::int64_t snapshot_ns{0};
  std::string underlier;
  std::string root;
  std::int32_t expiry_days{0};
  double strike{0.0};
  bool is_call{false};
  std::string exercise;
  double spot{0.0};
  double bid{0.0};
  double ask{0.0};
  double years{0.0};          // variance clock T_v
  double years_calendar{0.0}; // calendar clock T_c
  double rate{0.0};
  double sdiv{0.0};           // supplied continuous yield (calendar clock)
  double ddiv_pv{0.0};        // aggregate cash-dividend PV
  std::string input_status;
};

// One row of atx-vol-quote-bench's predictions CSV.
struct PredictionRow {
  std::uint64_t row_id{0};
  std::uint64_t chain_id{0};
  double price{0.0}; // NaN when blank
  double vol{0.0};   // NaN when blank
  std::string status;
};

// One development-split label row: srPrc, srVol and the vendor Greeks in VENDOR units
// (de per unit spot; ve per vol point = dP/dsigma * kSpiderRockVegaScale).
struct LabelRow {
  std::uint64_t row_id{0};
  double reference_price{0.0};
  double reference_vol{0.0};
  double vendor_delta{0.0};
  double vendor_vega{0.0};
};

// The fit's snapshot spot S* for one chain (native report `one_spot.chains[]`).
struct ChainSpot {
  std::uint64_t chain_id{0};
  double spot{0.0};
};

// CSV readers (RFC-4180 quoting; header names select columns, order-free). ParseError on
// a missing column, a malformed number or a ragged record; InvalidArgument on a duplicate
// row_id.
[[nodiscard]] Result<std::vector<QuoteRow>> parse_quotes_csv(std::string_view text);
[[nodiscard]] Result<std::vector<PredictionRow>> parse_predictions_csv(std::string_view text);
[[nodiscard]] Result<std::vector<LabelRow>> parse_labels_csv(std::string_view text);
[[nodiscard]] Result<std::vector<ChainSpot>> parse_basis_csv(std::string_view text);

// Ok iff the dataset manifest's `rows_by_split` is exactly {"development": n} with n > 0.
// PermissionDenied for any other split map (universe, validation, mixed); ParseError on a
// manifest that is not a JSON object carrying rows_by_split.
[[nodiscard]] Status labels_allowed(std::string_view manifest_json);

// ── The shadow clock ────────────────────────────────────────────────────────

// r * (T_c / T_v): the fitter's variance-clock rate (QuoteSliceDiagnostics::effective_rate).
[[nodiscard]] double shadow_rate(const QuoteSliceMarket &market) noexcept;
// The fitter's variance-clock yield for a calendar-clock yield (quote_slice.cpp
// `converted_yield`, restated): equals QuoteSliceDiagnostics::effective_yield bit for bit
// at calendar_yield == market.continuous_yield. NaN when an aggregate PV is not below the
// (funding-discounted) spot.
[[nodiscard]] double shadow_yield(const QuoteSliceMarket &market, double calendar_yield,
                                  QuoteSliceDividendModel model) noexcept;
// ts + llround(T_v * kCalendarYearNs) (+ offset ns): the synthetic Calendar365 expiry.
[[nodiscard]] std::int64_t shadow_expiry_ns(std::int64_t ts_ns, double variance_years,
                                            std::int64_t offset_ns = 0) noexcept;

// ── Sanity ──────────────────────────────────────────────────────────────────

enum class SanityBit : std::uint8_t {
  None = 0,
  DeltaRange = 1u << 0, // call delta outside [0, B] / put outside [-B, 0], B = max(1, e^-qT)
  GammaNeg = 1u << 1,   // gamma < -kSanityTolerance
  VegaNeg = 1u << 2,    // vega < -kSanityTolerance
  CpDelta = 1u << 3,    // call-put delta gap below exp(-q T) by more than kCpDeltaSlack
};
inline constexpr std::size_t kSanityKinds = 4;
[[nodiscard]] constexpr std::uint8_t sanity_mask(SanityBit bit) noexcept {
  return static_cast<std::uint8_t>(bit);
}
// The per-row bits of one Greek triple (CpDelta is a pair check, never set here). The delta
// range is carry-aware: |delta| may reach max(1, exp(-q T)) (a q < 0 call is European, with
// delta exp(-q T) N(d1)); every bound allows kSanityTolerance. A non-finite input sets no
// bit: availability counts it instead.
[[nodiscard]] std::uint8_t greek_sanity(Side side, double delta, double gamma, double vega,
                                        double q, double T) noexcept;
// True when (Dc - Dp) < exp(-q T) - kCpDeltaSlack (the American lower bound, above).
[[nodiscard]] bool cp_delta_violation(double call_delta, double put_delta, double q,
                                      double T) noexcept;

// ── Per row ─────────────────────────────────────────────────────────────────

enum class RowState : std::uint8_t {
  NotServed = 0,    // prediction status neither ok nor ok_price_only (or no prediction)
  PriceOnly = 1,    // ok_price_only: a price without an identified vol; counted, not evaluated
  InvalidInput = 2, // served, but the vol mark or the chain's carry basis is unusable
  Evaluated = 3,    // priced and Greeked by the engine (status may still be non-Ok)
};
inline constexpr std::size_t kRowStateCount = 4;
inline constexpr std::size_t kRouteCount = 7; // RiskRoute None..Refused

struct ProbeRow {
  std::uint64_t row_id{0};
  std::uint64_t chain_id{0};
  RowState state{RowState::NotServed};
  Side side{Side::Call};
  RiskRoute route{RiskRoute::None};
  RiskFlags flags{0};
  RiskLaneStatus status{RiskLaneStatus::Refused};
  double strike{0.0};
  double sigma{0.0};           // the fitted (predicted) vol the row was priced at
  double predicted_price{0.0};
  double price{0.0};           // primary kernel (family when run, else cold)
  double delta{0.0};
  double gamma{0.0};
  double vega{0.0};
  double theta{0.0};           // shadow-clock theta (variance time); not a calendar theta
  double identity_price{0.0};  // cold price when the cold kernel ran, else `price`
  double reprice_abs{0.0};     // |identity_price - predicted_price|; NaN unless Ok
  double reprice_vol_bp{0.0};  // reprice_abs / vega in vol bp; NaN unless vega >= floor
  double T{0.0};               // shadow T' (variance years)
  double rate{0.0};            // variance-clock rate
  double q{0.0};               // variance-clock yield
  std::uint8_t sanity{0};
  double vendor_delta{0.0};    // NaN unless labels; vendor units converted to library units
  double vendor_vega{0.0};
};

// ── Per chain and totals ────────────────────────────────────────────────────

// Neumaier compensated sum (the 10-line idea of api/analytics NeumaierSum, restated so
// the probe does not include that header). Totals are serial, in input order.
struct CompensatedSum {
  double sum{0.0};
  double comp{0.0};
  void add(double x) noexcept;
  [[nodiscard]] double value() const noexcept { return sum + comp; }
};

struct ProbeCounts {
  std::uint64_t rows{0};
  std::array<std::uint64_t, kRowStateCount> state{};     // by RowState
  std::array<std::uint64_t, kRouteCount> route{};        // evaluated rows by RiskRoute
  std::array<std::uint64_t, kRiskLaneStatusCount> lane{}; // evaluated rows by RiskLaneStatus
  std::uint64_t near_edge{0};
  std::uint64_t cold_fallback{0};
  std::uint64_t outside_box{0};
  std::uint64_t nonfinite_greek{0};  // Ok rows with a non-finite delta / gamma / vega
  std::array<std::uint64_t, kSanityKinds> sanity_fail{}; // by SanityBit order
  std::uint64_t reprice_checked{0};
  std::uint64_t reprice_mismatch{0};
  double reprice_max_abs{0.0};
  double reprice_max_rel{0.0};
  double reprice_max_vol_bp{0.0};
  std::uint64_t vendor_n{0};
  CompensatedSum vendor_delta_abs{};   // sum |delta - vendor delta|
  CompensatedSum vendor_vega_sym_rel{}; // sum |v - V| / ((|v| + |V|) / 2)
};

// Fold one row into a count block: the probe's only aggregation step (exposed so a test
// can inject rows). Rows are folded serially in input order. Sanity bits count only on
// Evaluated rows with lane status Ok; a non-finite Greek counts in nonfinite_greek and
// never enters a sum; the reprice identity counts only finite pairs.
void accumulate(ProbeCounts &counts, const ProbeRow &row) noexcept;

struct ChainProbe {
  std::uint64_t chain_id{0};
  std::string underlier;
  std::string root;
  std::int64_t snapshot_ns{0};
  std::int32_t expiry_days{0};
  double variance_years{0.0}; // NaN when the chain has no eligible row
  std::string basis;          // own | snapshot_spot | invalid:<reason> | none
  ProbeCounts counts{};
};

enum class ProbeKernel : std::uint8_t { Family = 0, Cold = 1, Both = 2 };
[[nodiscard]] const char *to_string(ProbeKernel kernel) noexcept;
[[nodiscard]] const char *to_string(RowState state) noexcept;
[[nodiscard]] const char *to_string(SanityBit bit) noexcept;

struct ProbeOptions {
  ProbeKernel kernel{ProbeKernel::Both};
  unsigned threads{0}; // 0 => executor's auto degree (honours ATX_VOL_FIT_WORKERS)
  QuoteSliceDividendModel dividend_model{QuoteSliceDividendModel::AggregateFundingPresentValue};
  // snapshot.carry_price_tolerance of the fit; > 0 re-runs coalesce_quote_slice_carry on a
  // rebased chain exactly as the fitter did.
  double carry_price_tolerance{0.0};
  // Set only after labels_allowed() passed on the dataset manifest.
  bool labels_allowed{false};
};

struct ProbeProvenance {
  std::string kernel;          // family | cold | both
  std::string identity_kernel; // which kernel's price the reprice identity reads
  std::string al_opts;         // "accurate-fast-profile": al_fast_profile(nullopt)
  std::string dividend_model;
  double carry_price_tolerance{0.0};
  unsigned family_n_sigma{0};
  double family_sigma_box_pad{0.0};
  unsigned threads{0};
  bool labels{false};
};

struct ProbeReport {
  std::string schema{kProbeSchema};
  int schema_version{kProbeSchemaVersion};
  ProbeProvenance provenance{};
  ProbeCounts totals{};
  std::vector<ChainProbe> chains{}; // first-appearance order of chain_id in the quotes
};

// Run the probe. `quotes` is the whole dataset (every row is accounted for); predictions
// join on row_id (a row without one is NotServed); `labels` must be empty unless
// options.labels_allowed (PermissionDenied otherwise); `basis` lists S* per rebased chain
// (empty for a fit without snapshot.one_spot). `rows`, when non-null, receives one
// ProbeRow per quote row in input order. InvalidArgument on duplicate / unknown row ids or a
// prediction whose chain_id disagrees with its quote row.
[[nodiscard]] Result<ProbeReport> run_fit_risk_probe(std::span<const QuoteRow> quotes,
                                                     std::span<const PredictionRow> predictions,
                                                     std::span<const LabelRow> labels,
                                                     std::span<const ChainSpot> basis,
                                                     const ProbeOptions &options,
                                                     std::vector<ProbeRow> *rows = nullptr);

// Mean |delta - vendor delta| and mean symmetric relative vega gap of a count block; NaN
// when vendor_n == 0.
[[nodiscard]] double vendor_delta_mae(const ProbeCounts &counts) noexcept;
[[nodiscard]] double vendor_vega_sym_rel(const ProbeCounts &counts) noexcept;

// Schema-v1 JSON (docs/universe-fit-bench.md "probe.json"). Non-finite doubles are written
// as null and read back as NaN; chain_id is a JSON integer (64-bit exact on both sides).
[[nodiscard]] std::string probe_report_to_json(const ProbeReport &report);
// ParseError on malformed JSON or a missing field; InvalidArgument on a foreign schema or
// a schema_version other than kProbeSchemaVersion.
[[nodiscard]] Result<ProbeReport> probe_report_from_json(std::string_view text);
// One CSV line per ProbeRow (docs/universe-fit-bench.md "probe rows").
[[nodiscard]] std::string probe_rows_to_csv(std::span<const ProbeRow> rows);

} // namespace atx::vol::fit_probe
