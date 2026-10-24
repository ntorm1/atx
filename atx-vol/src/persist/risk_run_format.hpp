#pragma once

// risk_run_format.hpp — the RiskRun segment's on-disk records, its schema table and
// the full-featured reader behind RiskRunArchive (design wave3-persist D13; layout spec
// docs/storage/PERSIST_HISTORY_FORMAT.md §RiskRun). Private to atx-vol (tests and the
// atx-vol-history-store tool include it through the src/ root).
//
// Every record here is little-endian, trivially copyable, standard layout and free of
// implicit padding: each struct's field list tiles its sizeof (static_assert below),
// and the same lists are folded into the kind's schema fingerprint, so a layout drift —
// including ExplainTotals / FrameCounters growing under another lane — is a compile
// error here first and a SchemaMismatch for every old file after the re-pin.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/persist/persist_format.hpp"
#include "atx/vol/api/persist/risk_run_archive.hpp"
#include "atx/vol/api/risk/historical_var.hpp"
#include "atx/vol/api/risk/pnl_explain.hpp"
#include "atx/vol/api/risk/risk_frame.hpp"

namespace atx::vol::risk_run {

// ── Section types ─────────────────────────────────────────────────────────────

enum class Section : std::uint32_t {
  Meta = 1,
  BookLegs = 2,     // RiskRunLeg per explain row (option legs in book order, then shares)
  Steps = 3,        // RiskRunStep per step
  StepTotals = 4,   // ExplainTotals per step (zero row when the step has no explain)
  BookUnique = 5,   // RiskRunUnique per unique option, PREPARED order (frame rows)
  BookUids = 6,     // u32, PreparedRiskBook::uids() order (frame per-uid rows)
  LegIds = 7,       // u64 per explain row
  FrameF64 = 16,    // 16 .. 31: RiskFrame f64 columns in RiskFrameColumn order
  FrameRoute = 32,  // u8 RiskRoute
  FrameFlags = 33,  // u32 RiskFlags
  FrameStatus = 34, // u8 RiskLaneStatus
  FrameUidSpot = 35,
  FrameUidRate = 36,
  FrameUidStatus = 37,
  FrameShareMark = 38,
  FrameShareStatus = 39,
  ExplainF64 = 48,  // 48 .. 68: ExplainResult f64 columns in RiskExplainColumn order
                    // (68 = PnlCarry, lane E; the four sections below moved up by one)
  ExplainRouteBase = 69,
  ExplainRouteShifted = 70,
  ExplainFlags = 71,
  ExplainStatus = 72,
  BucketsByUid = 80,
  BucketsByExpiry = 81,
  VarIndex = 96,
  VarFrames = 97,
  VarPoints = 98,
};

[[nodiscard]] constexpr std::uint32_t type_of(Section s) noexcept {
  return static_cast<std::uint32_t>(s);
}

inline constexpr std::uint64_t kNone64 = 0xFFFFFFFFFFFFFFFFull;

// RiskRunMeta::flags
inline constexpr std::uint32_t kRunFrames = 1u;  // frame columns recorded
inline constexpr std::uint32_t kRunBuckets = 2u; // explain bucket rows recorded
// RiskRunStep::flags
inline constexpr std::uint32_t kStepHasFrame = 1u;         // frame columns stored for the step
inline constexpr std::uint32_t kStepHasExplain = 2u;
inline constexpr std::uint32_t kStepRateMoveWithoutRho = 4u;
inline constexpr std::uint32_t kStepNeedsSecondOrder = 8u; // the frame's RiskNeeds
inline constexpr std::uint32_t kStepNeedsRho = 16u;
inline constexpr std::uint32_t kStepNeedsCharm = 32u;
inline constexpr std::uint32_t kStepKnownFlags = 63u;

// ── On-disk records ───────────────────────────────────────────────────────────

struct RiskRunMeta {
  std::uint64_t book_fingerprint{}; // FNV-1a/64 + fmix64 over the four book sections
  std::uint64_t first_step{};       // global index of this segment's first step
  std::uint64_t n_steps{};
  std::uint64_t first_var{};        // global index of this segment's first VaR result
  std::uint64_t n_var{};
  std::uint32_t n_option_legs{};
  std::uint32_t n_share_legs{};
  std::uint32_t n_unique{};
  std::uint32_t n_uids{};
  std::uint32_t flags{};            // kRun*
  std::uint32_t segment_ordinal{};  // 0-based; a compacted run is one segment 0
  std::int64_t created_unix_ns{};
  std::uint64_t n_frames{};         // frames stored in this segment
  std::uint64_t n_explain_rows{};   // explain rows stored in this segment
  std::array<std::uint64_t, 5> reserved{};
};
static_assert(kPersistPod<RiskRunMeta> && sizeof(RiskRunMeta) == 128);

struct RiskRunLeg {
  std::uint32_t uid{};
  std::uint8_t kind{};       // ExplainLegKind
  std::uint8_t side{};       // Side (options); 0 for a share
  std::uint16_t reserved0{};
  std::uint32_t slot{};      // option: prepared row; share: uid slot
  std::uint32_t reserved1{};
  double strike{};           // 0 for a share
  std::int64_t expiry_ns{};  // 0 for a share
  double qty{};
  double multiplier{};       // as given on the OptionLeg; 1 for a share
};
static_assert(kPersistPod<RiskRunLeg> && sizeof(RiskRunLeg) == 48);

struct RiskRunUnique {
  std::uint32_t uid{};
  std::uint8_t side{};
  std::uint8_t reserved0{};
  std::uint16_t reserved1{};
  double strike{};
  std::int64_t expiry_ns{};
};
static_assert(kPersistPod<RiskRunUnique> && sizeof(RiskRunUnique) == 24);

struct RiskRunStep {
  std::uint64_t step{};                // global step index
  std::int64_t ts_ns{};                // the frame's instant / the explain's shifted instant
  std::int64_t base_ts_ns{};           // the explain's base instant (0 without explain)
  double dt_years{};
  std::uint64_t frame_index{};         // ordinal of the step's frame in the segment, or kNone64
  std::uint64_t explain_row{};         // first explain row in the segment, or kNone64
  std::uint64_t bucket_uid_begin{};    // into BucketsByUid
  std::uint64_t bucket_expiry_begin{}; // into BucketsByExpiry
  std::uint64_t book_logical_id{};     // the frame's (process-local identity, round-tripped)
  double explain_seconds{};
  std::uint32_t flags{};               // kStep*
  std::uint32_t explain_rows{};
  std::uint32_t n_pivot_groups{};
  std::uint32_t n_pivot_edge_fallback{};
  std::uint32_t n_bucket_uid{};
  std::uint32_t n_bucket_expiry{};
  std::array<std::uint32_t, kRiskLaneStatusCount> excluded_count{};
  std::array<double, kRiskLaneStatusCount> excluded_premium{};
  FrameCounters counters{};
};
static_assert(kRiskLaneStatusCount == 6, "RiskRunStep pins six exclusion reasons: re-pin");
static_assert(kPersistPod<RiskRunStep> && sizeof(RiskRunStep) == 288);
static_assert(offsetof(RiskRunStep, excluded_count) == 104);
static_assert(offsetof(RiskRunStep, excluded_premium) == 128);
static_assert(offsetof(RiskRunStep, counters) == 176);

struct RiskRunBucket {
  std::uint64_t step{}; // global step index
  std::uint64_t key{};
  std::int64_t expiry_ns{};
  ExplainTotals totals{};
};
static_assert(kPersistPod<RiskRunBucket> && sizeof(RiskRunBucket) == 184);

struct RiskRunVar {
  std::uint64_t step{};        // the last step appended before this result, or kNone64
  std::uint64_t frame_begin{}; // into VarFrames (segment-local)
  std::uint64_t frame_count{};
  std::uint64_t point_begin{}; // into VarPoints (segment-local)
  std::uint64_t point_count{};
  std::uint64_t n_admitted{};
  double reference_pv{};
  std::uint64_t reserved{};
};
static_assert(kPersistPod<RiskRunVar> && sizeof(RiskRunVar) == 64);

// HistoricalVarFrame carries implicit padding (persist D3), so it is re-laid explicitly.
struct RiskRunVarFrame {
  std::int64_t base_ts_ns{};
  std::int64_t shifted_ts_ns{};
  ExplainTotals totals{};
  std::uint32_t n_legs_resolved{};
  std::uint32_t n_legs_unresolved{};
  std::uint8_t admitted{};
  std::array<std::uint8_t, 7> reserved{};
};
static_assert(kPersistPod<RiskRunVarFrame> && sizeof(RiskRunVarFrame) == 192);

struct RiskRunVarPoint {
  double confidence{};
  double value_at_risk{};
  double expected_shortfall{};
  std::uint64_t tail_scenarios{};
  ExplainTotals tail_mean{};
};
static_assert(kPersistPod<RiskRunVarPoint> && sizeof(RiskRunVarPoint) == 192);

// ── Field lists (fingerprint + no-padding proofs) ─────────────────────────────

namespace detail {

[[nodiscard]] constexpr PersistFieldSpec fld(std::string_view name, std::size_t offset,
                                             std::size_t size) noexcept {
  return PersistFieldSpec{name, static_cast<std::uint32_t>(offset),
                          static_cast<std::uint32_t>(size)};
}

template <std::size_t N, std::size_t M>
[[nodiscard]] constexpr std::array<PersistFieldSpec, N + M>
cat(const std::array<PersistFieldSpec, N> &a, const std::array<PersistFieldSpec, M> &b) noexcept {
  std::array<PersistFieldSpec, N + M> out{};
  for (std::size_t i = 0; i < N; ++i) out[i] = a[i];
  for (std::size_t i = 0; i < M; ++i) out[N + i] = b[i];
  return out;
}

[[nodiscard]] constexpr std::array<PersistFieldSpec, 21> totals_fields(std::size_t at) noexcept {
  return {{
      fld("pv_base", at + offsetof(ExplainTotals, pv_base), 8),
      fld("pv_target", at + offsetof(ExplainTotals, pv_target), 8),
      fld("pnl_total", at + offsetof(ExplainTotals, pnl_total), 8),
      fld("pnl_delta", at + offsetof(ExplainTotals, pnl_delta), 8),
      fld("pnl_gamma", at + offsetof(ExplainTotals, pnl_gamma), 8),
      fld("pnl_vega", at + offsetof(ExplainTotals, pnl_vega), 8),
      fld("pnl_volga", at + offsetof(ExplainTotals, pnl_volga), 8),
      fld("pnl_vanna", at + offsetof(ExplainTotals, pnl_vanna), 8),
      fld("pnl_theta", at + offsetof(ExplainTotals, pnl_theta), 8),
      fld("pnl_rho", at + offsetof(ExplainTotals, pnl_rho), 8),
      fld("pnl_charm", at + offsetof(ExplainTotals, pnl_charm), 8),
      fld("pnl_unexplained", at + offsetof(ExplainTotals, pnl_unexplained), 8),
      fld("vol_level", at + offsetof(ExplainTotals, vol_level), 8),
      fld("vol_skew", at + offsetof(ExplainTotals, vol_skew), 8),
      fld("vol_curv", at + offsetof(ExplainTotals, vol_curv), 8),
      fld("vol_resid", at + offsetof(ExplainTotals, vol_resid), 8),
      fld("shares_pnl", at + offsetof(ExplainTotals, shares_pnl), 8),
      fld("n_ok", at + offsetof(ExplainTotals, n_ok), 4),
      fld("n_excluded", at + offsetof(ExplainTotals, n_excluded), 4),
      fld("excluded_premium", at + offsetof(ExplainTotals, excluded_premium), 8),
      fld("pnl_carry", at + offsetof(ExplainTotals, pnl_carry), 8), // lane E (D23a)
  }};
}

[[nodiscard]] constexpr std::array<PersistFieldSpec, 12> counters_fields(std::size_t at) noexcept {
  return {{
      fld("families_built", at + offsetof(FrameCounters, families_built), 8),
      fld("carry_sets_built", at + offsetof(FrameCounters, carry_sets_built), 8),
      fld("family_rows", at + offsetof(FrameCounters, family_rows), 8),
      fld("cold_rows", at + offsetof(FrameCounters, cold_rows), 8),
      fld("exercised_rows", at + offsetof(FrameCounters, exercised_rows), 8),
      fld("refused_rows", at + offsetof(FrameCounters, refused_rows), 8),
      fld("status_count", at + offsetof(FrameCounters, status_count), 4 * kRiskLaneStatusCount),
      fld("evaluate_seconds", at + offsetof(FrameCounters, evaluate_seconds), 8),
      fld("family_build_seconds", at + offsetof(FrameCounters, family_build_seconds), 8),
      // Lane A2's appended counters (re-pinned with lane E's merge of the integration tip).
      fld("family_nodes", at + offsetof(FrameCounters, family_nodes), 8),
      fld("uneconomic_rows", at + offsetof(FrameCounters, uneconomic_rows), 8),
      fld("short_tenor_rows", at + offsetof(FrameCounters, short_tenor_rows), 8),
  }};
}

} // namespace detail

inline constexpr auto kTotalsFields = detail::totals_fields(0);
static_assert(persist_fields_tile(kTotalsFields, sizeof(ExplainTotals)),
              "ExplainTotals changed: re-pin the RiskRun layout (wave3-persist D13)");
inline constexpr auto kCountersFields = detail::counters_fields(0);
static_assert(persist_fields_tile(kCountersFields, sizeof(FrameCounters)),
              "FrameCounters changed: re-pin the RiskRun layout (wave3-persist D13)");
// The sizes the records below are laid out against (lane E re-pin: ExplainTotals 152 -> 160
// with pnl_carry; FrameCounters 88 -> 112 with lane A2's three counters).
static_assert(sizeof(ExplainTotals) == 160 && sizeof(FrameCounters) == 112,
              "ExplainTotals / FrameCounters changed: re-pin the RiskRun layout");

inline constexpr std::array<PersistFieldSpec, 15> kMetaFields{{
    {"book_fingerprint", offsetof(RiskRunMeta, book_fingerprint), 8},
    {"first_step", offsetof(RiskRunMeta, first_step), 8},
    {"n_steps", offsetof(RiskRunMeta, n_steps), 8},
    {"first_var", offsetof(RiskRunMeta, first_var), 8},
    {"n_var", offsetof(RiskRunMeta, n_var), 8},
    {"n_option_legs", offsetof(RiskRunMeta, n_option_legs), 4},
    {"n_share_legs", offsetof(RiskRunMeta, n_share_legs), 4},
    {"n_unique", offsetof(RiskRunMeta, n_unique), 4},
    {"n_uids", offsetof(RiskRunMeta, n_uids), 4},
    {"flags", offsetof(RiskRunMeta, flags), 4},
    {"segment_ordinal", offsetof(RiskRunMeta, segment_ordinal), 4},
    {"created_unix_ns", offsetof(RiskRunMeta, created_unix_ns), 8},
    {"n_frames", offsetof(RiskRunMeta, n_frames), 8},
    {"n_explain_rows", offsetof(RiskRunMeta, n_explain_rows), 8},
    {"reserved", offsetof(RiskRunMeta, reserved), 40},
}};
static_assert(persist_fields_tile(kMetaFields, sizeof(RiskRunMeta)));

inline constexpr std::array<PersistFieldSpec, 10> kLegFields{{
    {"uid", offsetof(RiskRunLeg, uid), 4},
    {"kind", offsetof(RiskRunLeg, kind), 1},
    {"side", offsetof(RiskRunLeg, side), 1},
    {"reserved0", offsetof(RiskRunLeg, reserved0), 2},
    {"slot", offsetof(RiskRunLeg, slot), 4},
    {"reserved1", offsetof(RiskRunLeg, reserved1), 4},
    {"strike", offsetof(RiskRunLeg, strike), 8},
    {"expiry_ns", offsetof(RiskRunLeg, expiry_ns), 8},
    {"qty", offsetof(RiskRunLeg, qty), 8},
    {"multiplier", offsetof(RiskRunLeg, multiplier), 8},
}};
static_assert(persist_fields_tile(kLegFields, sizeof(RiskRunLeg)));

inline constexpr std::array<PersistFieldSpec, 6> kUniqueFields{{
    {"uid", offsetof(RiskRunUnique, uid), 4},
    {"side", offsetof(RiskRunUnique, side), 1},
    {"reserved0", offsetof(RiskRunUnique, reserved0), 1},
    {"reserved1", offsetof(RiskRunUnique, reserved1), 2},
    {"strike", offsetof(RiskRunUnique, strike), 8},
    {"expiry_ns", offsetof(RiskRunUnique, expiry_ns), 8},
}};
static_assert(persist_fields_tile(kUniqueFields, sizeof(RiskRunUnique)));

inline constexpr auto kStepFields = detail::cat(
    std::array<PersistFieldSpec, 18>{{
        {"step", offsetof(RiskRunStep, step), 8},
        {"ts_ns", offsetof(RiskRunStep, ts_ns), 8},
        {"base_ts_ns", offsetof(RiskRunStep, base_ts_ns), 8},
        {"dt_years", offsetof(RiskRunStep, dt_years), 8},
        {"frame_index", offsetof(RiskRunStep, frame_index), 8},
        {"explain_row", offsetof(RiskRunStep, explain_row), 8},
        {"bucket_uid_begin", offsetof(RiskRunStep, bucket_uid_begin), 8},
        {"bucket_expiry_begin", offsetof(RiskRunStep, bucket_expiry_begin), 8},
        {"book_logical_id", offsetof(RiskRunStep, book_logical_id), 8},
        {"explain_seconds", offsetof(RiskRunStep, explain_seconds), 8},
        {"flags", offsetof(RiskRunStep, flags), 4},
        {"explain_rows", offsetof(RiskRunStep, explain_rows), 4},
        {"n_pivot_groups", offsetof(RiskRunStep, n_pivot_groups), 4},
        {"n_pivot_edge_fallback", offsetof(RiskRunStep, n_pivot_edge_fallback), 4},
        {"n_bucket_uid", offsetof(RiskRunStep, n_bucket_uid), 4},
        {"n_bucket_expiry", offsetof(RiskRunStep, n_bucket_expiry), 4},
        {"excluded_count", offsetof(RiskRunStep, excluded_count), 4 * kRiskLaneStatusCount},
        {"excluded_premium", offsetof(RiskRunStep, excluded_premium), 8 * kRiskLaneStatusCount},
    }},
    detail::counters_fields(offsetof(RiskRunStep, counters)));
static_assert(persist_fields_tile(kStepFields, sizeof(RiskRunStep)));

inline constexpr auto kBucketFields = detail::cat(
    std::array<PersistFieldSpec, 3>{{
        {"step", offsetof(RiskRunBucket, step), 8},
        {"key", offsetof(RiskRunBucket, key), 8},
        {"expiry_ns", offsetof(RiskRunBucket, expiry_ns), 8},
    }},
    detail::totals_fields(offsetof(RiskRunBucket, totals)));
static_assert(persist_fields_tile(kBucketFields, sizeof(RiskRunBucket)));

inline constexpr std::array<PersistFieldSpec, 8> kVarFields{{
    {"step", offsetof(RiskRunVar, step), 8},
    {"frame_begin", offsetof(RiskRunVar, frame_begin), 8},
    {"frame_count", offsetof(RiskRunVar, frame_count), 8},
    {"point_begin", offsetof(RiskRunVar, point_begin), 8},
    {"point_count", offsetof(RiskRunVar, point_count), 8},
    {"n_admitted", offsetof(RiskRunVar, n_admitted), 8},
    {"reference_pv", offsetof(RiskRunVar, reference_pv), 8},
    {"reserved", offsetof(RiskRunVar, reserved), 8},
}};
static_assert(persist_fields_tile(kVarFields, sizeof(RiskRunVar)));

inline constexpr auto kVarFrameFields = detail::cat(
    detail::cat(std::array<PersistFieldSpec, 2>{{
                    {"base_ts_ns", offsetof(RiskRunVarFrame, base_ts_ns), 8},
                    {"shifted_ts_ns", offsetof(RiskRunVarFrame, shifted_ts_ns), 8},
                }},
                detail::totals_fields(offsetof(RiskRunVarFrame, totals))),
    std::array<PersistFieldSpec, 4>{{
        {"n_legs_resolved", offsetof(RiskRunVarFrame, n_legs_resolved), 4},
        {"n_legs_unresolved", offsetof(RiskRunVarFrame, n_legs_unresolved), 4},
        {"admitted", offsetof(RiskRunVarFrame, admitted), 1},
        {"reserved", offsetof(RiskRunVarFrame, reserved), 7},
    }});
static_assert(persist_fields_tile(kVarFrameFields, sizeof(RiskRunVarFrame)));

inline constexpr auto kVarPointFields = detail::cat(
    std::array<PersistFieldSpec, 4>{{
        {"confidence", offsetof(RiskRunVarPoint, confidence), 8},
        {"value_at_risk", offsetof(RiskRunVarPoint, value_at_risk), 8},
        {"expected_shortfall", offsetof(RiskRunVarPoint, expected_shortfall), 8},
        {"tail_scenarios", offsetof(RiskRunVarPoint, tail_scenarios), 8},
    }},
    detail::totals_fields(offsetof(RiskRunVarPoint, tail_mean)));
static_assert(persist_fields_tile(kVarPointFields, sizeof(RiskRunVarPoint)));

inline constexpr std::array<std::string_view, kRiskFrameColumnCount> kFrameColumnNames{
    "frame.T",     "frame.sigma", "frame.forward", "frame.q_eff",        "frame.rate",
    "frame.price", "frame.delta", "frame.gamma",   "frame.vega",         "frame.theta",
    "frame.rho",   "frame.vanna", "frame.volga",   "frame.charm",        "frame.vendor_price",
    "frame.vendor_delta"};
inline constexpr std::array<std::string_view, kRiskExplainColumnCount> kExplainColumnNames{
    "explain.pv_base",   "explain.pv_target", "explain.pnl_total",       "explain.pnl_delta",
    "explain.pnl_gamma", "explain.pnl_vega",  "explain.pnl_volga",       "explain.pnl_vanna",
    "explain.pnl_theta", "explain.pnl_rho",   "explain.pnl_charm",       "explain.pnl_unexplained",
    "explain.vol_level", "explain.vol_skew",  "explain.vol_curv",        "explain.vol_resid",
    "explain.d_spot",    "explain.d_vol",     "explain.d_time",          "explain.d_rate",
    "explain.pnl_carry"};

// The f64 columns in RiskFrameColumn / RiskExplainColumn order (== struct order).
inline constexpr std::array<std::vector<double> RiskFrame::*, kRiskFrameColumnCount> kFrameMembers{
    &RiskFrame::T,     &RiskFrame::sigma, &RiskFrame::forward, &RiskFrame::q_eff,
    &RiskFrame::rate,  &RiskFrame::price, &RiskFrame::delta,   &RiskFrame::gamma,
    &RiskFrame::vega,  &RiskFrame::theta, &RiskFrame::rho,     &RiskFrame::vanna,
    &RiskFrame::volga, &RiskFrame::charm, &RiskFrame::vendor_price, &RiskFrame::vendor_delta};
inline constexpr std::array<std::vector<double> ExplainResult::*, kRiskExplainColumnCount>
    kExplainMembers{&ExplainResult::pv_base,   &ExplainResult::pv_target,
                    &ExplainResult::pnl_total, &ExplainResult::pnl_delta,
                    &ExplainResult::pnl_gamma, &ExplainResult::pnl_vega,
                    &ExplainResult::pnl_volga, &ExplainResult::pnl_vanna,
                    &ExplainResult::pnl_theta, &ExplainResult::pnl_rho,
                    &ExplainResult::pnl_charm, &ExplainResult::pnl_unexplained,
                    &ExplainResult::vol_level, &ExplainResult::vol_skew,
                    &ExplainResult::vol_curv,  &ExplainResult::vol_resid,
                    &ExplainResult::d_spot,    &ExplainResult::d_vol,
                    &ExplainResult::d_time,    &ExplainResult::d_rate,
                    &ExplainResult::pnl_carry};

// A std::array initializer shorter than its extent zero-fills the tail, so a column count
// raised without its name / member would pass silently: refuse that at compile time. The
// explain f64 sections are contiguous types from ExplainF64 and must not reach the next.
[[nodiscard]] constexpr bool explain_columns_complete() noexcept {
  for (std::size_t c = 0; c < kRiskExplainColumnCount; ++c) {
    if (kExplainMembers[c] == nullptr || kExplainColumnNames[c].empty()) return false;
  }
  for (std::size_t c = 0; c < kRiskFrameColumnCount; ++c) {
    if (kFrameMembers[c] == nullptr || kFrameColumnNames[c].empty()) return false;
  }
  return true;
}
static_assert(explain_columns_complete(), "a RiskRun column lacks its member or name");
// The member lists above are hand-written, so a per-leg / per-row column added to
// ExplainResult or RiskFrame without them would be dropped SILENTLY (materialize_explain /
// materialize_frame would return it empty beside full columns, and a consumer indexing it per
// leg would read out of bounds). Pin both structs' shape — in units of a std::vector, so the
// pin holds at any iterator-debug level — so such an addition fails here first. ExplainResult:
// 30 vectors (id, uid, kind, 20 f64 columns, 4 route/flag/status, pnl_carry, 2 bucket lists)
// + 280 bytes of scalars, ExplainTotals and the exclusion arrays. RiskFrame: 24 vectors (16 f64,
// 3 lane, 3 per-uid, 2 per-share) + 136 bytes (instant, book id, RiskNeeds, FrameCounters).
static_assert(sizeof(ExplainResult) == 30 * sizeof(std::vector<double>) + 280,
              "ExplainResult changed: update kExplainMembers / kExplainColumnNames and "
              "RiskExplainColumn (or the RiskRun step records), then re-pin the RiskRun layout");
static_assert(sizeof(RiskFrame) == 24 * sizeof(std::vector<double>) + 136,
              "RiskFrame changed: update kFrameMembers / kFrameColumnNames and RiskFrameColumn "
              "(or the RiskRun frame sections), then re-pin the RiskRun layout");
static_assert(kRiskExplainColumnCount == 21 &&
                  kExplainMembers[static_cast<std::size_t>(RiskExplainColumn::PnlCarry)] ==
                      &ExplainResult::pnl_carry,
              "RiskExplainColumn changed: re-pin the RiskRun layout");
static_assert(type_of(Section::ExplainF64) + kRiskExplainColumnCount ==
                  type_of(Section::ExplainRouteBase),
              "explain f64 section types overlap the next section");
static_assert(type_of(Section::FrameF64) + kRiskFrameColumnCount <= type_of(Section::FrameRoute),
              "frame f64 section types overlap the next section");

// ── Schema (file order == this order) ────────────────────────────────────────

// meta + 4 book + steps + totals, the frame columns + 8, the explain columns + 4,
// two bucket sections, three VaR sections.
inline constexpr std::size_t kSectionCount =
    7 + kRiskFrameColumnCount + 8 + kRiskExplainColumnCount + 4 + 2 + 3;

[[nodiscard]] constexpr std::array<PersistSectionSpec, kSectionCount> make_schema() noexcept {
  std::array<PersistSectionSpec, kSectionCount> s{};
  std::size_t i = 0;
  const auto pod = [&](Section t, std::size_t stride, std::string_view name,
                       std::span<const PersistFieldSpec> fields) {
    s[i++] = PersistSectionSpec{type_of(t), 1, PersistDType::Pod,
                                static_cast<std::uint32_t>(stride), name, fields, true};
  };
  const auto col = [&](std::uint32_t t, PersistDType d, std::uint32_t stride,
                       std::string_view name) {
    s[i++] = PersistSectionSpec{t, 1, d, stride, name, {}, true};
  };
  pod(Section::Meta, sizeof(RiskRunMeta), "meta", kMetaFields);
  pod(Section::BookLegs, sizeof(RiskRunLeg), "book.legs", kLegFields);
  pod(Section::Steps, sizeof(RiskRunStep), "steps", kStepFields);
  pod(Section::StepTotals, sizeof(ExplainTotals), "step.totals", kTotalsFields);
  pod(Section::BookUnique, sizeof(RiskRunUnique), "book.unique", kUniqueFields);
  col(type_of(Section::BookUids), PersistDType::U32, 4, "book.uids");
  col(type_of(Section::LegIds), PersistDType::U64, 8, "book.leg_ids");
  for (std::uint32_t c = 0; c < kRiskFrameColumnCount; ++c) {
    col(type_of(Section::FrameF64) + c, PersistDType::F64, 8, kFrameColumnNames[c]);
  }
  col(type_of(Section::FrameRoute), PersistDType::U8, 1, "frame.route");
  col(type_of(Section::FrameFlags), PersistDType::U32, 4, "frame.flags");
  col(type_of(Section::FrameStatus), PersistDType::U8, 1, "frame.status");
  col(type_of(Section::FrameUidSpot), PersistDType::F64, 8, "frame.uid_spot");
  col(type_of(Section::FrameUidRate), PersistDType::F64, 8, "frame.uid_flat_rate");
  col(type_of(Section::FrameUidStatus), PersistDType::U8, 1, "frame.uid_status");
  col(type_of(Section::FrameShareMark), PersistDType::F64, 8, "frame.share_mark");
  col(type_of(Section::FrameShareStatus), PersistDType::U8, 1, "frame.share_status");
  for (std::uint32_t c = 0; c < kRiskExplainColumnCount; ++c) {
    col(type_of(Section::ExplainF64) + c, PersistDType::F64, 8, kExplainColumnNames[c]);
  }
  col(type_of(Section::ExplainRouteBase), PersistDType::U8, 1, "explain.route_base");
  col(type_of(Section::ExplainRouteShifted), PersistDType::U8, 1, "explain.route_shifted");
  col(type_of(Section::ExplainFlags), PersistDType::U32, 4, "explain.flags");
  col(type_of(Section::ExplainStatus), PersistDType::U8, 1, "explain.status");
  pod(Section::BucketsByUid, sizeof(RiskRunBucket), "buckets.by_uid", kBucketFields);
  pod(Section::BucketsByExpiry, sizeof(RiskRunBucket), "buckets.by_expiry", kBucketFields);
  pod(Section::VarIndex, sizeof(RiskRunVar), "var.index", kVarFields);
  pod(Section::VarFrames, sizeof(RiskRunVarFrame), "var.frames", kVarFrameFields);
  pod(Section::VarPoints, sizeof(RiskRunVarPoint), "var.points", kVarPointFields);
  return s;
}

inline constexpr std::array<PersistSectionSpec, kSectionCount> kSchema = make_schema();
inline constexpr std::uint64_t kFingerprint =
    persist_fingerprint(persist_salt(kRiskRunSalt), kSchema);

// Position of section `type` in kSchema (== its position in every segment's table).
[[nodiscard]] constexpr std::size_t slot_of(std::uint32_t type) noexcept {
  for (std::size_t i = 0; i < kSchema.size(); ++i) {
    if (kSchema[i].type == type) return i;
  }
  return kSchema.size();
}

// The manifest key of segment `ordinal`: 12 zero-padded digits, so key order is
// segment order (PersistDirectory keeps entries sorted by key).
[[nodiscard]] std::string segment_key(std::uint64_t ordinal);

// ── Publication (risk_run_archive_publish.cpp) ───────────────────────────────

// Compaction's publish: the merged `image` under a fresh name, then — under the lock,
// after re-reading the manifest — a manifest naming only it, refused (SegmentMismatch)
// when the manifest no longer equals `seen` (seq and file CRC of every entry).
[[nodiscard]] PersistOutcome publish_compacted(std::string_view dir,
                                               std::span<const PersistManifestEntry> seen,
                                               std::span<const std::byte> image,
                                               std::uint32_t records, SealDurability durability);

// ── Reader ────────────────────────────────────────────────────────────────────

// Everything a RiskRun directory holds, validated at open (every segment's framing,
// schema, structure and the book identity across segments; column payloads are
// CRC-verified lazily on first touch under Lazy). RiskRunArchive is a thin facade
// over this; the tool and the white-box tests use the segment-level accessors.
class Reader {
public:
  [[nodiscard]] static PersistResult<Reader> open(std::string_view dir,
                                                  const PersistOpenOptions &options = {});

  Reader(Reader &&) noexcept;
  Reader &operator=(Reader &&) noexcept;
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  ~Reader();

  [[nodiscard]] std::size_t n_steps() const noexcept;
  [[nodiscard]] std::size_t n_segments() const noexcept;
  [[nodiscard]] std::size_t n_var() const noexcept;
  [[nodiscard]] const RiskRunMeta &meta(std::size_t segment) const noexcept;
  [[nodiscard]] const PersistFile &segment(std::size_t segment) const noexcept;
  // nullptr past the end.
  [[nodiscard]] const RiskRunStep *step(std::size_t step) const noexcept;
  // std::nullopt past the end.
  [[nodiscard]] std::optional<RiskRunStepInfo> step_info(std::size_t step) const noexcept;
  [[nodiscard]] std::span<const RiskRunLeg> legs() const noexcept;
  [[nodiscard]] std::span<const std::uint64_t> leg_ids() const noexcept;
  [[nodiscard]] std::span<const RiskRunUnique> unique() const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> uids() const noexcept;

  // Empty when the step / column is absent (or its section fails its lazy CRC).
  [[nodiscard]] std::span<const double> frame(RiskFrameColumn column,
                                              std::size_t step) const noexcept;
  [[nodiscard]] std::span<const double> explain(RiskExplainColumn column,
                                                std::size_t step) const noexcept;
  [[nodiscard]] const ExplainTotals *totals(std::size_t step) const noexcept;

  // Owned deep copies, every enum byte validated (RecordInvalid otherwise). NotFound
  // when the step has no frame / explain, or `index` is past the end.
  [[nodiscard]] PersistResult<RiskFrame> materialize_frame(std::size_t step) const;
  [[nodiscard]] PersistResult<ExplainResult> materialize_explain(std::size_t step) const;
  [[nodiscard]] PersistResult<HistoricalVarResult> var(std::size_t index) const;

private:
  struct Impl;
  explicit Reader(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::vol::risk_run
