#pragma once

// atx::engine::eval — cross-sectional forecast evaluation (checkpoint 14).
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  The repository has never measured whether its frozen signal expressions
//  predict anything: `eval/breadth.hpp:71` takes `ic` as a caller-supplied
//  parameter and `combine/combiner.hpp:39-47` records that the IC-from-signal
//  form "is not computable here". This unit computes it — per-date
//  cross-sectional Pearson and Spearman information coefficients over a
//  pre-registered horizon set, ICIR with circular-block-bootstrap intervals,
//  signal autocorrelation with an implied-turnover proxy, decile spreads gross
//  and net of a frozen cost convention, and a per-date coverage table.
//
//  Binding design:
//  `atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md`.
//  Section references below (§3.x, §5.x, §6.x) are to that note. Every frozen
//  constant lives there, not here; this header declares the shapes the design
//  pins and nothing else.
//
//  PURE. No I/O, no logging, no clock, no exception on any success path, and no
//  dynamic allocation inside the per-date loop: every working buffer is sized
//  once by `plan_cross_section_ic` and owned by the caller (§6.1).
//
// ===========================================================================
//  The three contracts a caller must honour
// ===========================================================================
//  1. BORROWED SPANS. `CrossSectionIcConfig::horizons` and every span of
//     `CrossSectionIcInput` are non-owning. They must outlive both
//     `plan_cross_section_ic` and `compute_cross_section_ic` and must not be
//     modified between the two calls — the plan sizes scratch from `dates`,
//     `instruments` and `bootstrap_draws`, and the compute re-reads them.
//  2. NO DERIVED TERMINAL RETURNS. The engine never infers that a missing mark
//     is a terminal event. The caller supplies, per cell, a `terminal` flag, a
//     `terminal_evidenced` flag and a `terminal_value` amount built from the
//     audit's evidence table; an unevidenced cell is dropped and counted, never
//     repaired (§3.8, ruling AR-1/R-A).
//  3. THE MASK IS THE ADMISSION TRUTH. `mask` is the already-intersected
//     eligibility/readiness gate of the evaluated Panel. The engine does not
//     recompute eligibility and treats a zero byte as exclusion regardless of
//     value finiteness (§3.1) — `extract_streams` builds a static full-size
//     `Universe`, so a panel alone does not carry the mask downstream.
//
// ===========================================================================
//  The seal (§5)
// ===========================================================================
//  `apply_calendar_seal` is the structural exclusion of the 2023-01-01 sealed
//  period that the program previously had only as prose. It is a live gate that
//  the frozen 2013 slice does not trip: NON-VACUOUS BY CODE, VACUOUS BY DATA.
//  It masks and counts by session key only. The index-fraction carve of
//  `eval::reserve_lockbox` (`lockbox.hpp:319`, default frac 0.20) is NOT used
//  here — on a 2013-only panel it would reserve 2013 dates, not 2023-2025
//  (ruling R-2). A caller whose panel straddles a boundary routes the Panel
//  through `eval::reserve_window` itself and records the reservation's content
//  address into `SealReport`; this function has no Panel and never does so.

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/random.hpp" // Xoshiro256pp — the frozen bootstrap stream (§3.10)
#include "atx/core/types.hpp"

#if !defined(__SIZEOF_INT128__)
#include <intrin.h> // _umul128 — the non-__int128 half of detail::umul_64_to_128
#endif

namespace atx::engine::eval {

// ---------------------------------------------------------------------------
//  Bounded maxima (ruling R-7). Declared placeholders: they exist so every loop
//  in the unit has a statically obvious upper bound (JPL rule 2) and so a
//  nonsense configuration is rejected at the boundary rather than sized into a
//  multi-gigabyte scratch allocation.
//
//  There is deliberately no `kMaxIcSignals`: the API takes ONE signal span per
//  call and bounds no signal count. The caller's signal loop is bounded by its
//  own frozen list (§4.1).
//
//  `kMaxIcDates` / `kMaxIcInstruments` exist per parent ruling RR-1 (design
//  §11.5). §6.1 claims every loop bound is "validated against a compile-time
//  maximum at entry"; without these two that sentence was false for `dates` and
//  `instruments`, and `plan_cross_section_ic`'s `.assign()` calls could throw
//  `std::bad_alloc` on a span-consistent but enormous input, in a unit whose
//  contract is that no path throws. 4,096 clears Stage 1 (189 x 1,661) and
//  Stage 2 (~1,950 x ~1,661) with room to spare.
// ---------------------------------------------------------------------------
inline constexpr atx::usize kMaxIcHorizons = 8;
inline constexpr atx::usize kMaxIcQuantiles = 32;
inline constexpr atx::usize kMaxBootstrapDraws = 100'000;
inline constexpr atx::usize kMaxIcDates = 4096;
inline constexpr atx::usize kMaxIcInstruments = 4096;

// ---------------------------------------------------------------------------
//  Rejectable admission enums.
//
//  `Unknown = 0` is never a working default: a value-initialized config must be
//  REJECTED, not silently interpreted. Both fields below are validated to a
//  named enumerator at entry, so an implementer cannot acquire a policy by
//  forgetting to set one.
// ---------------------------------------------------------------------------
enum class ForwardReturnVariant : atx::u8 {
  Unknown = 0,              // rejects
  DropMissingForward,       // Shumway-biased UPWARD; must be reported beside the other
  IncludeAuditedTerminalV1, // caller-supplied evidenced terminal returns (§3.8)
};

enum class IcTieHandling : atx::u8 {
  Unknown = 0, // rejects
  AverageRanksV1,
};

enum class SealPolicy : atx::u8 {
  Unknown = 0,    // rejects
  RejectSealedV1, // any observation at or after the sealed boundary is an error
  MaskSealedV1,   // report the first sealed index; the CALLER truncates
};

// Frozen integer values: the bootstrap stream key XORs `statistic_id` into bits
// 8-15 (§3.10), so renumbering these invalidates every published interval.
enum class BootstrapStatisticId : atx::u16 {
  IcMean = 0,
  Icir = 1,
  RankIcMean = 2,
  RankIcir = 3,
  SpreadGross = 4,
  SpreadNet = 5,
};

// ---------------------------------------------------------------------------
//  CrossSectionIcConfig — the pre-registered recipe for one (signal, variant,
//  restriction, sample) evaluation. Every `stream_*` field is an input to the
//  six-field bootstrap stream key of §3.10 (ruling R-C); they are carried here
//  rather than derived so the engine never has to guess which configuration it
//  is evaluating.
// ---------------------------------------------------------------------------
struct CrossSectionIcConfig {
  // BORROWED: must outlive both plan_* and compute_*, and must not be modified
  // between them. Strictly increasing, each >= 1, size <= kMaxIcHorizons.
  std::span<const atx::usize> horizons;
  atx::usize quantiles{10};          // 2 <= Q <= kMaxIcQuantiles
  atx::usize min_names_per_date{2};  // >= 2 (matches linear_alpha.cpp:141 `pv.size() >= 2U`)
  atx::usize bootstrap_draws{2000};  // 0 => every interval unreportable; <= kMaxBootstrapDraws
  atx::usize block_len_floor{5};     // >= 1;  L_h = max(floor, (h + 1) / 2)
  atx::u64 bootstrap_seed{0};
  atx::u64 stream_signal_index{0};   // stream key input (§3.10); 0 when unused
  atx::u64 stream_variant_id{0};     // 0 = DropMissingForward, 1 = IncludeAuditedTerminalV1
  atx::u64 stream_restriction_id{0}; // 0 = full, 1 = _ex34
  atx::u64 stream_sample_id{0};      // 0 = full, 1 = common  (ruling R-C)
  atx::f64 trade_bps{0.0};           // >= 0, finite
  atx::f64 annual_borrow_bps{0.0};   // >= 0, finite
  atx::f64 short_leg_gross{1.0};     // > 0, finite; the gross-2.0 book's short leg
                                     // (§3.12, ruling AR-11)
  atx::i64 day_basis{365};           // 360 or 365
  atx::usize common_sample_dates{0}; // §3.13 PREFIX LENGTH: in_common_sample(t) ==
                                     // (t < common_sample_dates). 0 => no _common pass.
                                     // The frozen Stage-1 value is T - max(H) = 126.
  ForwardReturnVariant forward_variant{ForwardReturnVariant::Unknown};
  IcTieHandling ties{IcTieHandling::Unknown};
};

// ---------------------------------------------------------------------------
//  CrossSectionIcInput — the evaluated panel, flattened.
//
//  All numeric spans are DATE-MAJOR of length dates*instruments unless noted:
//  cell (t, i) is at flat index `t * instruments + i`, matching
//  `alpha/panel.hpp:69-71`. Indices are PANEL ROW indices throughout, never
//  calendar days.
//
//  BORROWED: the caller keeps every span alive and unchanged for the call; no
//  span is modified by this unit.
// ---------------------------------------------------------------------------
struct CrossSectionIcInput {
  atx::usize dates{};
  atx::usize instruments{};
  std::span<const atx::f64> signal;             // NaN = undefined
  std::span<const atx::f64> price;              // TRI close; NaN or <= 0 = no mark
  std::span<const atx::f64> raw_price;          // as-traded close; terminal legs only
  std::span<const atx::u8> mask;                // 1 = admitted at (t, i)
  std::span<const atx::u8> terminal;            // 1 = audited terminal coverage at (t, i)
  std::span<const atx::u8> terminal_evidenced;  // 1 = consideration evidenced; 0 = drop + count
  std::span<const atx::f64> terminal_value;     // consideration (+ special, already gated by
                                                // the caller on record date, §3.8 / AR-2)
  std::span<const atx::u8> excluded_audited;    // 1 = in the 34-ID _ex34 restriction set
  std::span<const atx::i64> session_keys;       // strictly increasing, size == dates
  // Optional auxiliary per-cell series (same dates x instruments layout as
  // `signal`; NaN = missing; empty span = none). Never enters the IC, the
  // spread or the bootstrap: it is only averaged over each decile's members so a
  // reader can see, e.g., the mean dollar ADV of the top and bottom deciles.
  std::span<const atx::f64> aux;
};

// ---------------------------------------------------------------------------
//  IcDatePoint — one evaluation date at one horizon, with the coverage counters
//  of §3.7. No IC number is ever reported without these beside it.
// ---------------------------------------------------------------------------
struct IcDatePoint {
  atx::usize date{};
  atx::i64 session_key{};
  atx::usize n_eligible{};
  atx::usize n_signal_finite{};
  atx::usize n_with_forward{};
  atx::usize n_dropped_missing_forward{};
  atx::usize n_terminal_applied{};
  atx::usize n_terminal_unevidenced{};
  atx::usize n_excluded_audited{};
  atx::usize n_used{};
  atx::f64 pearson_ic{};
  atx::f64 rank_ic{};
  atx::f64 spread_gross{};
  atx::f64 spread_net{};
  atx::i64 days_forward{};  // days_h(t), ACTUAL calendar days from session keys (§3.12)
  atx::f64 borrow_drag{};   // per-date; varies with the calendar span
  atx::f64 cost_drag{};     // trade_drag (horizon constant) + borrow_drag(t)
  atx::u8 emitted{};        // 0 -> pearson_ic / rank_ic are 0.0 and MUST be ignored
  atx::u8 spread_emitted{}; // 0 -> spread_* are 0.0 and MUST be ignored
  atx::u8 in_common_sample{}; // 1 -> the date contributes to the _common statistics
};

// ---------------------------------------------------------------------------
//  BootstrapInterval — a circular-block-bootstrap percentile interval (§3.10).
//  `reportable == 0` is a first-class state, not a NaN: the serializer emits
//  `lo`/`hi` as JSON null / CSV "" and names the reason.
// ---------------------------------------------------------------------------
struct BootstrapInterval {
  atx::f64 point{};
  atx::f64 lo{};
  atx::f64 hi{};
  atx::usize draws{};
  atx::usize block_len{};
  atx::usize blocks{};
  atx::usize series_len{};
  atx::usize modulo_fallbacks{}; // §3.10 bounded-loop fallback count; the receipt asserts 0
  atx::u8 reportable{};          // 0 -> lo/hi are 0.0 and MUST serialize as null / ""
  // §3.10's FROZEN five-code enum (ruling A-3, restated by the design's own
  // implementer note; an older two-code comment stood here and was wrong):
  //   0  reportable
  //   1  "series-shorter-than-twenty"            (n < 20)
  //   2  "common-prefix-gap"                     (§3.13, common block only)
  //   3  "series-too-short-for-block-length"     (floor(n / L_h) < 10)
  //   4  "bootstrap-draws-zero"                  (bootstrap_draws == 0)
  // Several conditions may hold at once: the LOWEST NONZERO code wins.
  atx::u8 unreportable_reason{};
};

struct QuantileBucketStat {
  atx::usize quantile{};
  atx::usize n_dates{};
  atx::f64 mean_forward_return{};
  atx::f64 mean_names{};
  atx::f64 mean_aux{};     // mean over dates of the bucket's mean finite `aux`; 0 when n_aux_dates == 0
  atx::usize n_aux_dates{}; // dates on which at least one member had a finite `aux`
};

// ---------------------------------------------------------------------------
//  IcSampleStats — one of these per (horizon, sample), sample in {full, common}.
//  The pair is carried side by side so a reader never has to join two files
//  (§3.13).
// ---------------------------------------------------------------------------
struct IcSampleStats {
  atx::usize dates_emitted{};
  atx::f64 ic_mean{};
  atx::f64 ic_sd{};
  atx::f64 icir{};
  atx::f64 naive_t{}; // EMITTED AND INVALID under overlapping horizons (§3.9)
  atx::f64 rank_ic_mean{};
  atx::f64 rank_ic_sd{};
  atx::f64 rank_icir{};
  atx::f64 rank_naive_t{};
  atx::f64 spread_gross_mean{};
  atx::f64 spread_gross_sd{};
  atx::f64 spread_net_mean{};
  atx::f64 spread_net_sd{};
  BootstrapInterval ic_mean_ci;
  BootstrapInterval icir_ci;
  BootstrapInterval rank_ic_mean_ci;
  BootstrapInterval rank_icir_ci;
  BootstrapInterval spread_gross_ci;
  BootstrapInterval spread_net_ci;
  // §3.13 / NEW-1: ONE counter per (horizon, sample), SHARED by this block's IC
  // and spread families. Counts prefix dates that failed to emit at this horizon
  // for EITHER reason (n < min_names_per_date, or n < Q). There is deliberately
  // no per-family split — a single scalar cannot express one, and a date that
  // emits an IC but not a spread must void both or the two families would be
  // computed over different date sets. Meaningful only on the common block; 0 on
  // the full block.
  atx::usize common_prefix_gaps{};
  // 0 when dates_emitted < 2, or (common block) when common_prefix_gaps > 0.
  // When 0, EVERY field of this block — MEANS INCLUDED, IC and spread alike —
  // serializes as null / "" (§3.9, §3.13). A point estimate over an incomplete
  // prefix is exactly the number a reader would mistake for a common-sample
  // result.
  atx::u8 summary_reportable{};
  // The SAME frozen five-code enum as `BootstrapInterval::unreportable_reason`
  // (§3.10, ruling A-3), so a reader never has to hold two tables:
  //   0 reportable, 1 short series (this block's n < 2 is the degenerate end of
  //   the same short-series condition, §3.9), 2 common-prefix-gap, 3 block
  //   length, 4 draws-zero. LOWEST NONZERO code wins.
  // Only 1 and 2 are reachable on a summary block: 3 and 4 are properties of an
  // interval, not of a point estimate.
  atx::u8 unreportable_reason{};
};

struct IcHorizonSummary {
  atx::usize horizon{};
  atx::usize block_len{};
  atx::usize dates_below_min_names{};
  atx::usize dates_below_quantile_count{};
  IcSampleStats full;   // all emitted dates
  IcSampleStats common; // §3.13 common sample; zeroed with summary_reportable = 0
                        // when cfg.common_sample_dates == 0
  // §3.12 cost-drag inputs and outputs. `decile_one_way_turnover` is the
  // DECILE-MEMBERSHIP one-way turnover at lag h, measured on the actual decile
  // portfolios, and it is the one `cost_drag` uses.
  //
  // SCALE (ruling NEW-4, stated here because this is where it is read): it is
  // expressed as a fraction of NAV for the GROSS-2.0 book (AR-11), so its range
  // is [0, 2] and a COMPLETE turnover reads 2.0, NOT 1.0. Not the same scale as
  // AutocorrSummary::implied_one_way_turnover, which lies in [0, 2] only via
  // 1 - rho_rank and is a different quantity entirely (§3.11, §12.9). Never
  // compare the two numbers directly. A "correction" of the decile weights to
  // +/-0.5/n to make a complete turnover read 1.0 is the exact pre-AR-11 bug and
  // halves every spread_net_* figure.
  atx::f64 decile_one_way_turnover{};
  atx::f64 trade_drag{};      // CONSTANT per horizon
  atx::f64 mean_borrow_drag{};
  atx::f64 mean_cost_drag{};
  atx::f64 mean_days_forward{}; // diagnostic for the calendar span
  // ---------------------------------------------------------------------------
  //  Parent ruling I-6 (T2T3 review; design §11.8). `dates_emitted`,
  //  `summary_reportable` and `unreportable_reason` on IcSampleStats describe the
  //  IC FAMILY only — they are gated on `n >= min_names_per_date`. The spread
  //  family has its own, strictly smaller emitted-date set (`n >= quantiles`), so
  //  a thin-universe horizon could publish `spread_gross_mean` computed over one
  //  date, or over none at all, while `summary_reportable == 1` told a reader the
  //  block was sound.
  //
  //  These two fields close that: they are `detail::series_reportable` applied to
  //  the SPREAD series' own length `n_spread` at this horizon's `block_len` and
  //  the configured draw count, and `spread_unreportable_reason` carries the SAME
  //  frozen five-code enum (§3.10 / A-3) under the same lowest-nonzero-wins rule.
  //  When `spread_reportable == 0` the engine emits `spread_gross_mean/sd` and
  //  `spread_net_mean/sd` as 0.0 and the serializer publishes them as null / "".
  //  Measured on the FULL sample; each IcSampleStats block additionally gates its
  //  own spread means on its own spread series (the common block adding code 2).
  // ---------------------------------------------------------------------------
  atx::u8 spread_reportable{};
  atx::u8 spread_unreportable_reason{};
  std::vector<QuantileBucketStat> buckets; // sized Q once per horizon, never per date
  std::vector<IcDatePoint> series;         // reserved to `dates` once per horizon
};

struct AutocorrSummary {
  atx::usize lag{1};
  atx::usize pairs_emitted{};
  atx::f64 rho_pearson{};              // REPORTED ONLY; defines nothing (§3.11 / AR-5)
  atx::f64 rho_rank{};                 // DEFINES the turnover proxy below
  // == 1 - rho_rank. A practitioner rule of thumb, not an accounting identity,
  // and NOT comparable to the replay's realized turnover. DISTINCT from
  // IcHorizonSummary::decile_one_way_turnover — a different quantity on a
  // different scale (that one is measured at gross-2.0 weights, where a complete
  // turnover reads 2.0); NEVER compare the two numbers directly (ruling NEW-4).
  atx::f64 implied_one_way_turnover{};
};

struct CrossSectionIcResult {
  std::vector<IcHorizonSummary> horizons; // one per configured horizon, same order
  AutocorrSummary autocorr;
  ForwardReturnVariant variant{ForwardReturnVariant::Unknown};
};

// ---------------------------------------------------------------------------
//  CrossSectionIcScratch — caller-owned working storage, sized ONCE by
//  `plan_cross_section_ic`. `compute_cross_section_ic` performs no allocation
//  after entry except the result vectors, which are sized before the date loop
//  begins; the per-date inner loop touches only scratch.
//
//  Contents after a call are UNSPECIFIED. Re-use across calls is permitted and
//  does not change results.
// ---------------------------------------------------------------------------
struct CrossSectionIcScratch {
  std::vector<atx::f64> x;      // sized `instruments`
  std::vector<atx::f64> r;      // sized `instruments`
  std::vector<atx::f64> rx;     // sized `instruments`
  std::vector<atx::f64> rr;     // sized `instruments`
  // sized `instruments`: the forward returns of one date in ORDERED decile
  // position, so the bucket sums of §3.12 walk contiguous memory rather than
  // dereferencing `order` a second time.
  std::vector<atx::f64> buf;
  std::vector<atx::usize> order; // sized `instruments`
  std::vector<atx::usize> perm;  // sized `instruments`
  std::vector<atx::f64> w_prev; // sized `instruments`
  std::vector<atx::f64> w_curr; // sized `instruments`
  // Sized `dates`: holds ONE per-(horizon, sample) source series — the emitted
  // IC, rank-IC, gross-spread or net-spread values, compacted to the front — for
  // the duration of that family's bootstrap. The resample itself is never
  // materialized: `resample_statistic` replays the draw's RNG from a saved copy
  // for its second pass, which is exactly §3.9's two-pass mean-then-sd code path
  // at zero extra storage (§3.10's "truncated to exactly n" is unchanged).
  std::vector<atx::f64> draw;
  std::vector<atx::f64> stat;   // sized `bootstrap_draws` — the B draw statistics,
                                // sorted in place for the percentile step
};

namespace detail {

// ---------------------------------------------------------------------------
//  pearson — Pearson correlation of two equal-length vectors.
//
//  SOURCE: a byte-for-byte algorithmic copy of
//  `atx-engine/include/atx/engine/learn/latent.hpp:151-178` (ruling R-6). It is
//  RESTATED here rather than included so that an `eval` translation unit does
//  not acquire a dependency on `learn/`, whose own header pulls in
//  `FeatureMatrix` and the PCA stack. The copy is deliberate and pinned: §9.1
//  case 2a asserts exact `f64` equality against `learn::detail::pearson` on a
//  fixed vector pair, so the two cannot drift silently.
//
//  Both guard branches are part of the contract, not defensive padding:
//    * n < 2  -> 0.0, matching the `pv.size() >= 2U` gate of
//      `learn/linear_alpha.cpp:141`;
//    * a constant series -> 0.0, the same all-zero convention
//      `core::stats::zscore` uses, so a flat cross-section contributes no
//      information rather than a NaN that would poison every downstream mean.
//
//  Accumulation order is ascending index — fixed, so the result is
//  bit-reproducible run to run.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::f64 pearson(std::span<const atx::f64> a,
                                      std::span<const atx::f64> b) noexcept {
  const atx::usize n = a.size();
  if (n < 2U) {
    return 0.0;
  }
  atx::f64 ma = 0.0;
  atx::f64 mb = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    ma += a[i];
    mb += b[i];
  }
  ma /= static_cast<atx::f64>(n);
  mb /= static_cast<atx::f64>(n);
  atx::f64 cov = 0.0;
  atx::f64 va = 0.0;
  atx::f64 vb = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    const atx::f64 da = a[i] - ma;
    const atx::f64 db = b[i] - mb;
    cov += da * db;
    va += da * da;
    vb += db * db;
  }
  if (va == 0.0 || vb == 0.0) {
    return 0.0; // a constant series has no linear relationship to anything
  }
  return cov / std::sqrt(va * vb);
}

// ---------------------------------------------------------------------------
//  block_len — the circular-block-bootstrap block length at horizon h (§3.10):
//
//      L_h = max(block_len_floor, ceil(h / 2))
//
//  `ceil(h / 2)` is spelled as the integer expression `(h + 1) / 2` in `usize`;
//  it is exact for both parities and cannot wrap, because h <= dates - 1 is
//  validated before any call. At the pre-registered floor of 5 and the frozen
//  horizon set {1, 5, 10, 21, 63} this yields the frozen {5, 5, 5, 11, 32}.
//
//  BOTH terms of the max are load-bearing and are pinned directly by tests: the
//  floor decides at h = 1 (5 beats 1) and the ceil decides at h = 63 (32 beats
//  5), which is the term that makes h = 63 the single horizon §4.4 predicts will
//  come back with a null interval.
//
//  In the header rather than the .cpp per parent ruling RR-3: T3's bootstrap and
//  T7a's oracle both need it, and a frozen value deserves a direct test.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr atx::usize block_len(atx::usize horizon, atx::usize floor_len) noexcept {
  const atx::usize half = (horizon + 1U) / 2U;
  return (floor_len > half) ? floor_len : half;
}

// ---------------------------------------------------------------------------
//  series_reportable — the frozen §3.10 reportability rule, one spelling:
//
//      draws >= 1  &&  n >= 20  &&  floor(n / L) >= 10
//
//  `n / len` is INTEGER division (M-7). `draws >= 1` is part of the rule and not
//  an afterthought: with draws == 0 the percentile step would read an empty
//  span, and `quantile_sorted`'s guard is `ATX_ASSERT`, which compiles to
//  `((void)0)` outside the checked build — an out-of-bounds read in `rel`, which
//  `.agents/cpp/agent.md` §0 forbids outright. Validation therefore rejects
//  draws == 0 whenever a horizon would otherwise have been reportable, so
//  "intervals disabled" is an explicit whole-run choice rather than a silently
//  degraded one (C-7 item 3).
//
//  The `n >= 20` clause is independent of the `n / len` clause and is pinned
//  separately: at len == 1 a 19-point series passes `n / len >= 10` and must
//  still be unreportable.
//
//  `len == 0` cannot occur at any current call site (`block_len` returns at
//  least `block_len_floor`, validated >= 1), but this is a `constexpr` predicate
//  T3 reuses and the guard is what keeps the division defined for every argument
//  a future caller can form — it is a precondition made total, not dead code.
//
//  RETURNS A BARE bool ON PURPOSE FOR NOW, and T3 must not infer
//  `BootstrapInterval::unreportable_reason` from it: reasons 1 (draws-zero),
//  2 (n < 20) and 3 (n/L < 10) are distinct and this predicate collapses them.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr bool series_reportable(atx::usize n, atx::usize len,
                                               atx::usize draws) noexcept {
  if (draws == 0U || len == 0U) {
    return false;
  }
  return (n >= 20U) && ((n / len) >= 10U);
}

// ---------------------------------------------------------------------------
//  splitmix64_next — RESTATED from `atx::core::detail::splitmix64_next`
//  (`atx-core/include/atx/core/random.hpp:57`).
//
//  SAFETY / provenance: a byte-for-byte algorithmic copy, restated locally per
//  ruling R-6's rationale — an `eval` unit does not reach into another module's
//  `detail` namespace (§3.10, M-2). §9.1 case 13b pins the two streams it
//  derives, so the copy cannot drift silently.
//
//  IN/OUT CONVENTION, which the whole bootstrap depends on: it ADVANCES `state`
//  FIRST and RETURNS the mixed value of the ADVANCED state. §3.10's seed
//  derivation uses the RETURN VALUE and discards the mutated `state`.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr atx::u64 splitmix64_next(atx::u64 &state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  atx::u64 z = state;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

// ---------------------------------------------------------------------------
//  umul_64_to_128 — the full 128-bit unsigned product of two u64 as hi:lo.
//
//  SAFETY / provenance: restated byte-for-byte from
//  `atx-core/include/atx/core/decimal.hpp:120-133`, including its `#if` split,
//  per §3.10 (N-5) and ruling R-6's rationale. `unsigned __int128` is a compiler
//  extension (clang-cl 18 takes this branch) used for a WIDENING MULTIPLY ONLY —
//  never a 128-bit divide, which would need the absent `__divti3`.
// ---------------------------------------------------------------------------
constexpr void umul_64_to_128(atx::u64 a, atx::u64 b, atx::u64 &hi, atx::u64 &lo) noexcept {
#if defined(__SIZEOF_INT128__)
  const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
  lo = static_cast<atx::u64>(product);
  hi = static_cast<atx::u64>(product >> 64);
#else
  // SAFETY (MSVC, untested in this toolchain, exactly as decimal.hpp states):
  // _umul128 returns the low 64 bits and writes the high 64 through the
  // out-parameter — an exact 64x64 -> 128 product.
  unsigned long long high = 0;
  lo = _umul128(a, b, &high);
  hi = high;
#endif
}

// ---------------------------------------------------------------------------
//  bootstrap_stream_key — §3.10's six-field, byte-aligned stream key (R-C).
//
//      seed ^ (statistic_id<<8) ^ (horizon_index<<16) ^ (signal_index<<24)
//           ^ (variant_id<<32) ^ (restriction_id<<40) ^ (sample_id<<48)
//
//  Each field occupies its own byte, so the XOR stays injective; bits 0-7 and
//  56-63 are unused. Every field is validated <= 255 at the API boundary, which
//  is what keeps the fields disjoint — a wider value would alias another field's
//  byte and silently merge two streams.
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr atx::u64 bootstrap_stream_key(BootstrapStatisticId statistic,
                                                      atx::u64 horizon_index,
                                                      atx::u64 signal_index, atx::u64 variant_id,
                                                      atx::u64 restriction_id, atx::u64 sample_id,
                                                      atx::u64 seed) noexcept {
  return seed ^ (static_cast<atx::u64>(statistic) << 8U) ^ (horizon_index << 16U) ^
         (signal_index << 24U) ^ (variant_id << 32U) ^ (restriction_id << 40U) ^
         (sample_id << 48U);
}

// ---------------------------------------------------------------------------
//  draw_below — Lemire's nearly-divisionless bounded draw, frozen by §3.10.
//
//  Returns a uniform value in [0, n). ONE 64-bit RNG output per attempt; the
//  rejection threshold is spelled `(0ULL - n) % n` because `-n` on an unsigned
//  operand is MSVC C4146, which `/WX` turns into an error; the HIGH half of the
//  widening product is the result. The retry loop carries an explicit cap of 64
//  (JPL rule 2) and a COUNTED fallback, so "unreachable" is measured rather than
//  asserted — for n <= 189 the per-attempt rejection probability is below 2^-56.
//
//  DEVIATION, deliberate: the design's spelling divides by `n` unguarded. `n < 2`
//  returns 0 without consuming an output — the only correct value for n == 1 and
//  the only way to keep `% n` defined for n == 0, which `.agents/cpp/agent.md` §0
//  requires. Unreachable at every call site (a reportable series has n >= 20), so
//  it changes no drawn sequence.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::u64 draw_below(atx::core::Xoshiro256pp &rng, atx::u64 n,
                                         atx::usize &modulo_fallbacks) noexcept {
  if (n < 2ULL) {
    return 0ULL;
  }
  atx::u64 hi = 0;
  atx::u64 lo = 0;
  atx::u64 x = rng.next_u64();
  umul_64_to_128(x, n, hi, lo);
  if (lo < n) {
    const atx::u64 thresh = (0ULL - n) % n; // (2^64 - n) mod n
    for (atx::usize tries = 0; lo < thresh; ++tries) {
      if (tries == 64U) {
        ++modulo_fallbacks;
        return x % n;
      }
      x = rng.next_u64();
      umul_64_to_128(x, n, hi, lo);
    }
  }
  return hi;
}

// ---------------------------------------------------------------------------
//  quantile_sorted_asc — the nearest-rank, round-half-up percentile of §3.10.
//
//  SAFETY / provenance: the same five-line rule as
//  `atx::core::stats::detail::quantile_sorted` (`cross_section.hpp:75-86`),
//  restated locally rather than reached across into another module's `detail`
//  namespace (§3.10, M-2).
//
//  DEVIATION, deliberate: the source guards emptiness with `ATX_ASSERT`, which
//  compiles to `((void)0)` outside the checked build and would be an
//  out-of-bounds read in `rel`. An empty span returns 0.0 here. Callers never
//  reach it — an interval is only drawn when `bootstrap_draws >= 1`.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::f64 quantile_sorted_asc(std::span<const atx::f64> sorted,
                                                  atx::f64 q) noexcept {
  const atx::usize n = sorted.size();
  if (n == 0U) {
    return 0.0;
  }
  if (n == 1U) {
    return sorted[0];
  }
  const atx::f64 pos = std::floor(q * static_cast<atx::f64>(n - 1U) + 0.5); // round-half-up
  const auto idx = static_cast<atx::usize>(pos);
  return sorted[(idx < n) ? idx : (n - 1U)];
}

} // namespace detail

// ===========================================================================
//  plan_cross_section_ic — validate (§6.2) and size the caller-owned scratch.
//
//  Runs EVERY validation branch of §6.2, so a configuration that survives the
//  plan is one `compute_cross_section_ic` may assume. Returns scratch sized for
//  exactly one (signal, variant, restriction, sample) evaluation of `in` under
//  `cfg`; re-use it across calls with the same shape.
//
//  Errors: InvalidArgument (unknown enum, out-of-contract scalar, non-monotonic
//  session keys, a `stream_*` field above 255 — §3.10's key gives each field one
//  byte, so a wider value would alias its neighbour and merge two streams,
//  bootstrap_draws == 0 while a horizon would otherwise be reportable),
//  OutOfRange (mis-sized span, horizon at or past `dates`, bootstrap_draws above
//  kMaxBootstrapDraws, and — ruling RR-1 — `dates` above kMaxIcDates or
//  `instruments` above kMaxIcInstruments, both checked BEFORE any sizing so no
//  `.assign()` can reach a throwing size; the dates*instruments overflow guard
//  is kept behind them because it is the branch that makes `cells` well-defined
//  for the span-shape checks and it survives any later raise of the two maxima).
// ===========================================================================
[[nodiscard]] atx::core::Result<CrossSectionIcScratch>
plan_cross_section_ic(const CrossSectionIcInput &in, const CrossSectionIcConfig &cfg);

// ===========================================================================
//  compute_cross_section_ic — the evaluation itself.
//
//  PURE in (in, cfg): same inputs -> bit-identical result. `scratch` must be one
//  `plan_cross_section_ic` returned for the same shape; a smaller one is a
//  caller-contract violation and returns `Internal` rather than reading out of
//  bounds. `scratch` contents afterwards are unspecified; re-use is permitted and
//  changes no result (§9.1 case 12).
//
//  WHICH (variant, restriction, sample) THIS CALL EVALUATES:
//    * `cfg.forward_variant` selects §3.8's variant A or B;
//    * `cfg.stream_restriction_id != 0` applies the `_ex34` restriction — a cell
//      whose `excluded_audited` byte is set is removed from the universe BEFORE
//      eligibility, exactly as §3.8/AR-7 defines the sub-universe. At 0 the full
//      universe is used and `n_excluded_audited` reports, on the ELIGIBLE basis
//      (ruling A-2), how many admitted cells the restriction would have removed;
//    * BOTH samples are produced in one call: `full` over every emitted date and
//      `common` over the §3.13 prefix `t < cfg.common_sample_dates`. The full
//      block's bootstrap streams carry `cfg.stream_sample_id` (the frozen value
//      is 0) and the common block's carry `1`, which is what ruling R-C's
//      `sample_id` field exists to separate.
//
//  Errors: the same set as `plan_cross_section_ic` (every §6.2 branch is
//  re-checked, so the function is total in its arguments), plus `Internal` when
//  `scratch` is smaller than the plan requires.
// ===========================================================================
[[nodiscard]] atx::core::Result<CrossSectionIcResult>
compute_cross_section_ic(const CrossSectionIcInput &in, const CrossSectionIcConfig &cfg,
                         CrossSectionIcScratch &scratch);

// ---------------------------------------------------------------------------
//  The calendar seal (§5.2).
//
//  The two boundaries are exported (parent ruling RR-2) so that the engine owns
//  the one spelling of each and every caller — the `equity-ic` stage, the §5.3
//  CLI window guard, the tests — refers to it rather than restating a 19-digit
//  literal. A transposed digit in a restated literal would seal the wrong period
//  while the Stage-1 receipt (`dates_at_or_after_sealed == 0`) still read as a
//  pass, which is a silent failure.
//
//  Each is written twice, once as a day count since the Unix epoch and once as
//  the nanosecond literal, and the two spellings are cross-checked below: a
//  typo in either form breaks the build rather than the seal.
// ---------------------------------------------------------------------------
inline constexpr atx::i64 kNanosPerDay = 86'400'000'000'000;
inline constexpr atx::i64 kValidationBeginNs = 18'262 * kNanosPerDay; // 2020-01-01T00:00:00Z
inline constexpr atx::i64 kSealedBeginNs = 19'358 * kNanosPerDay;     // 2023-01-01T00:00:00Z

static_assert(kValidationBeginNs == 1'577'836'800'000'000'000,
              "kValidationBeginNs must be 2020-01-01T00:00:00Z in epoch nanoseconds");
static_assert(kSealedBeginNs == 1'672'531'200'000'000'000,
              "kSealedBeginNs must be 2023-01-01T00:00:00Z in epoch nanoseconds");
static_assert(kValidationBeginNs < kSealedBeginNs,
              "the validation boundary precedes the sealed boundary");

struct CalendarSeal {
  // Defaulted to the frozen boundaries so a caller cannot seal the wrong period
  // by omission; `policy` still defaults to `Unknown`, so a default-constructed
  // seal is still rejected rather than silently applied (RR-2).
  atx::i64 validation_begin_ns{kValidationBeginNs}; // 2020-01-01
  atx::i64 sealed_begin_ns{kSealedBeginNs};         // 2023-01-01
  SealPolicy policy{SealPolicy::Unknown};
};

struct SealReport {
  atx::usize dates_total{};
  atx::usize dates_visible{};
  atx::usize dates_at_or_after_validation{};
  atx::usize dates_at_or_after_sealed{};
  atx::usize embargo_len{};
  atx::u64 content_address{}; // eval::SealedReservation::content_address, or 0
  atx::u8 used_reserve_window{}; // 1 iff a boundary fell strictly inside the panel
};

// ===========================================================================
//  apply_calendar_seal — the structural gate, by session key.
//
//  Counts `dates_at_or_after_validation` and `dates_at_or_after_sealed` ALWAYS,
//  even when both are zero, so the receipt can state that the gate ran and this
//  input did not trip it (§5.2). Under `RejectSealedV1` a single sealed
//  observation is `PermissionDenied` with the count in the message and in the
//  report. Under `MaskSealedV1` `dates_visible` is the first sealed index and
//  the CALLER truncates — that policy is not used in checkpoint 14; it exists so
//  the enum has a rejectable alternative rather than a silent one.
//
//  `content_address`, `embargo_len` and `used_reserve_window` are left at zero:
//  this function has no Panel, so it never calls `eval::reserve_window`, and it
//  never calls `eval::reserve_lockbox` at all (ruling R-2). A caller whose panel
//  straddles a boundary performs that carve and fills these fields itself.
//
//  Errors: InvalidArgument (`SealPolicy::Unknown`, non-monotonic session keys),
//  PermissionDenied (`RejectSealedV1` with a sealed observation present).
// ===========================================================================
[[nodiscard]] atx::core::Result<SealReport>
apply_calendar_seal(std::span<const atx::i64> session_keys, const CalendarSeal &seal);

} // namespace atx::engine::eval
