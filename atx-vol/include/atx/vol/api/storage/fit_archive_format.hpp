#pragma once

// ATXVFA on-disk vocabulary: constants, the typed refusal currency and every
// fixed-layout record of the fitted-surface archive. The authoritative layout
// spec is atx-vol/docs/storage/ARCHIVE_FORMAT.md; every size and offset that
// document states is pinned below by static_assert, so a layout drift is a
// compile error in every consumer rather than a silently misread file.
//
// Every struct here is trivially copyable, standard layout, little-endian,
// naturally aligned and free of implicit padding: each gap is an explicit
// `reserved` member that writers zero and readers verify. Pure vocabulary: no
// IO, no library types (the records layer, fit_archive_records.hpp, maps these
// onto QuoteSliceFit / ExpiryRecord / AmericanMarkovSpec).
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <tl/expected.hpp>

namespace atx::vol {

// ── Constants ────────────────────────────────────────────────────────────────

// "\x89VFA\r\n\x1A\n": the high bit, CRLF and ^Z catch 7-bit and text-mode
// transfer damage (the PNG signature idea).
inline constexpr std::array<std::uint8_t, 8> kFitArchiveMagic{0x89, 0x56, 0x46, 0x41,
                                                              0x0D, 0x0A, 0x1A, 0x0A};
inline constexpr std::array<std::uint8_t, 8> kFitArchiveFooterMagic{0x56, 0x46, 0x41, 0x2D,
                                                                    0x45, 0x4E, 0x44, 0x0A};
inline constexpr std::uint16_t kFitArchiveFormatMajor = 1;
// Minor 1 (lane C0, 2026-10-22): the PriceOnlyNodes section and the minor-1 tails of
// FitArchiveQuoteRow / FitArchiveFitRecord (identifiability and snapshot provenance).
inline constexpr std::uint16_t kFitArchiveFormatMinor = 1;
inline constexpr std::uint32_t kFitArchiveEndianTag = 0x01020304u;
inline constexpr std::uint64_t kFitArchiveSectionAlign = 64;
inline constexpr std::uint32_t kFitArchiveMaxSections = 256;
inline constexpr std::size_t kFitArchiveSymbolMax = 40;
// "No row": a standalone fit has no surface, a standalone ExpiryRecord no ladder.
inline constexpr std::uint32_t kFitArchiveNone = 0xFFFFFFFFu;
// SectionEntry::flags bit 0: a reader that does not know the type must refuse.
inline constexpr std::uint32_t kFitArchiveSectionRequired = 1u;

enum class FitArchiveSectionType : std::uint32_t {
  Symbols = 1,
  Strings = 2,
  Provenance = 3,
  SurfaceRecords = 16,
  SurfaceIndex = 17,
  FitRecords = 32,
  FitIndex = 33,
  PriceNodes = 34,
  QuoteRows = 35,
  MovedNodes = 36,
  Dividends = 37,
  CarryPoints = 38,
  CurveWords = 39,
  FitTimings = 40, // optional: wall-clock fit diagnostics, written only on request
  PriceOnlyNodes = 41, // u8[] QuoteSliceFit::price_only_nodes (file minor 1)
  ExpiryMeta = 48,
  ExpiryData = 49,
  ExpiryIndex = 50,
  Ladders = 51,
  LadderIndex = 52,
  LadderContracts = 53,
  MarkovRecords = 64,
  MarkovIndex = 65,
  MarkovIntervals = 66,
  MarkovLocalVol = 67,
  MarkovWords = 68,
  SeriesSegments = 80,
};

// Canonical name ("fit-records", ...); "unknown" for a value this build does not know.
[[nodiscard]] const char *to_string(FitArchiveSectionType type) noexcept;

// ── Typed refusal currency (no strings: a refusal never allocates) ──────────

enum class ArchiveStatus : std::uint16_t {
  Ok = 0,
  InvalidArgument,          // writer input or API misuse
  NotFound,                 // file or key absent
  IoError,                  // OS read / write / rename failure
  Truncated,                // shorter than the header claims, or cut before the footer
  SizeMismatch,             // longer than the header claims
  BadMagic,                 // not an ATXVFA file
  UnsupportedEndian,        // endian tag does not read back
  UnsupportedMajor,         // format major differs from this reader's
  HeaderInvalid,            // header sizes / offsets / reserved bytes
  HeaderChecksum,
  FooterInvalid,            // footer magic / reserved bytes
  FooterChecksum,
  FooterMismatch,           // footer copies disagree with the header
  SectionTableChecksum,
  SectionBounds,            // a section outside the payload area
  SectionOverlap,           // sections out of order or overlapping
  SectionAlignment,         // offset / stride / align not multiples
  SectionStride,            // length != count * stride, or stride below minimum
  DuplicateSection,
  MissingSection,           // a record section without its index (or a needed pool)
  UnsupportedSection,       // unknown type flagged Required
  UnsupportedRecordVersion, // known type with a different type_major
  SectionChecksum,
  PaddingNotZero,
  FileChecksum,             // whole-file CRC (verify level Paranoid)
  IndexUnsorted,            // symbols or keys not strictly increasing
  IndexBounds,              // index entry names no record, or a different key
  RecordBounds,             // a reference outside its pool
  RecordInvalid,            // enum / flag / reserved byte / payload shape invalid
  LayoutMismatch,           // ExpiryRecord object representation check failed
  Misaligned,               // backing buffer not 8-byte aligned
  DuplicateKey,             // writer: two records with one key
  SegmentMismatch,          // series: segment differs from its manifest entry
  Busy,                     // series: lock held by a live writer
};

[[nodiscard]] const char *to_string(ArchiveStatus status) noexcept;

struct ArchiveError {
  ArchiveStatus status{ArchiveStatus::InvalidArgument};
  // FitArchiveSectionType value involved, or kFitArchiveNone.
  std::uint32_t section{kFitArchiveNone};
  // Element index or byte offset that failed (status-specific; 0 when unused).
  std::uint64_t detail{0};
};

template <class T> using ArchiveResult = tl::expected<T, ArchiveError>;
using ArchiveOutcome = ArchiveResult<void>;

[[nodiscard]] inline tl::unexpected<ArchiveError>
archive_error(ArchiveStatus status, std::uint32_t section = kFitArchiveNone,
              std::uint64_t detail = 0) noexcept {
  return tl::unexpected<ArchiveError>(ArchiveError{status, section, detail});
}

[[nodiscard]] inline tl::unexpected<ArchiveError>
archive_error(ArchiveStatus status, FitArchiveSectionType section,
              std::uint64_t detail = 0) noexcept {
  return archive_error(status, static_cast<std::uint32_t>(section), detail);
}

// ── Framing ──────────────────────────────────────────────────────────────────

struct FitArchiveHeader {
  std::array<std::uint8_t, 8> magic{};
  std::uint16_t format_major{};
  std::uint16_t format_minor{};
  std::uint16_t header_size{};
  std::uint16_t section_entry_size{};
  std::uint32_t endian_tag{};
  std::uint32_t footer_size{};
  std::uint64_t file_size{};
  std::uint64_t section_table_offset{};
  std::uint32_t section_count{};
  std::uint32_t flags{};
  std::int64_t created_unix_ns{};
  std::uint64_t writer_version{};
  std::uint64_t archive_id{};
  std::uint64_t segment_sequence{};
  std::array<char, 32> writer_name{};
  std::uint32_t section_table_crc32c{};
  std::uint32_t header_crc32c{}; // over these 256 bytes with this field zero
  std::array<std::uint8_t, 136> reserved{};
};

struct FitArchiveSectionEntry {
  std::uint32_t type{};
  std::uint16_t type_major{};
  std::uint16_t type_minor{};
  std::uint64_t offset{};
  std::uint64_t length{};
  std::uint64_t element_count{};
  std::uint32_t element_stride{};
  std::uint32_t element_align{};
  std::uint32_t flags{};
  std::uint32_t crc32c{};
  std::array<std::uint8_t, 16> reserved{};
};

struct FitArchiveFooter {
  std::array<std::uint8_t, 8> magic{};
  std::uint64_t header_offset{};
  std::uint64_t file_size{};
  std::uint64_t section_table_offset{};
  std::uint32_t section_count{};
  std::uint32_t header_crc32c{};
  std::uint32_t file_crc32c{};   // over [0, file_size - 64)
  std::uint32_t footer_crc32c{}; // over these 64 bytes with this field zero
  std::uint64_t segment_sequence{};
  std::array<std::uint8_t, 8> reserved{};
};

// ── Shared value types ───────────────────────────────────────────────────────

// Order: (underlier, snapshot_ns, expiry_day, sub). expiry_day counts days since
// 1970-01-01 (0 for snapshot-level records); snapshot_ns is Unix nanoseconds.
struct FitArchiveKey {
  std::uint32_t underlier{};
  std::int32_t expiry_day{};
  std::int64_t snapshot_ns{};
  std::uint32_t sub{};
  std::uint32_t reserved{};
};

struct FitArchiveIndexEntry {
  std::uint32_t underlier{};
  std::int32_t expiry_day{};
  std::int64_t snapshot_ns{};
  std::uint32_t sub{};
  std::uint32_t record{}; // row in the indexed record section
};

struct FitArchiveStringRef {
  std::uint32_t offset{};
  std::uint32_t length{};
};

struct FitArchiveArrayRef {
  std::uint32_t first{};
  std::uint32_t count{};
};

struct FitArchiveSymbol {
  std::uint32_t length{};
  std::uint32_t reserved{};
  std::array<char, kFitArchiveSymbolMax> name{};
};

// Strict weak order on keys; also used for index entries.
template <class A, class B>
[[nodiscard]] constexpr bool fit_archive_key_less(const A &a, const B &b) noexcept {
  if (a.underlier != b.underlier) return a.underlier < b.underlier;
  if (a.snapshot_ns != b.snapshot_ns) return a.snapshot_ns < b.snapshot_ns;
  if (a.expiry_day != b.expiry_day) return a.expiry_day < b.expiry_day;
  return a.sub < b.sub;
}

template <class A, class B>
[[nodiscard]] constexpr bool fit_archive_key_equal(const A &a, const B &b) noexcept {
  return a.underlier == b.underlier && a.snapshot_ns == b.snapshot_ns &&
         a.expiry_day == b.expiry_day && a.sub == b.sub;
}

// ── Fit records (one per expiry fit) ─────────────────────────────────────────

struct FitArchiveMarket {
  double spot{};
  double variance_years{};
  double calendar_years{};
  double rate{};
  double continuous_yield{};
  double dividend_pv{};
};

inline constexpr std::uint8_t kFitDiagRevertedToSeed = 1u;
inline constexpr std::uint8_t kFitDiagWarmStarted = 2u;
inline constexpr std::uint8_t kFitDiagHasGradNorm = 4u;

struct FitArchiveFitDiag {
  double rmse_vol_vega_weighted{};
  double max_residual_vol{};
  double final_grad_norm{}; // meaningful only with kFitDiagHasGradNorm
  std::uint32_t n_quotes_used{};
  std::uint32_t n_butterfly_viol{};
  std::uint32_t n_calendar_viol{};
  std::uint16_t outer_iters{};
  std::uint16_t inner_iters_total{};
  std::uint8_t termination{};
  std::uint8_t projection{};
  std::uint8_t flags{};
  std::uint8_t reserved0{};
  std::uint32_t reserved1{};
};

inline constexpr std::uint8_t kChannelUsed = 1u;
inline constexpr std::uint8_t kChannelTermPrior = 2u;
inline constexpr std::uint8_t kChannelConverged = 4u;

struct FitArchiveChannelDiag {
  std::uint64_t channel_quotes{};
  std::uint64_t mid_quotes{};
  std::uint64_t mid_strikes{};
  std::uint64_t violations{};
  std::uint64_t iterations{};
  std::uint64_t repaired_nodes{};
  double max_violation_half_widths{};
  double prior_strength{};
  // Always 0. The channel solve's wall-clock time differs run to run, so it is
  // not part of the record: it lives in the optional FitTimings section.
  double reserved_timing{};
  double repair_max_price{};
  double repair_max_half_spreads{};
  std::uint8_t flags{};
  std::array<std::uint8_t, 7> reserved{};
};

struct FitArchiveRepair {
  std::uint64_t nodes{};
  std::uint64_t material_nodes{};
  std::uint64_t unidentified{};
  double max_price_move{};
  double max_vol_move{};
  double max_unquoted_vol_move{};
  std::uint8_t outcome{};
  std::array<std::uint8_t, 7> reserved{};
};

struct FitArchiveDiagnostics {
  std::uint64_t quotes_seen{};
  std::uint64_t iv_attempted{};
  std::uint64_t quotes_admitted{};
  std::uint64_t distinct_strikes{};
  std::uint64_t prices_produced{};
  std::uint64_t weights_capped{};
  double forward{};
  double discount{};
  double effective_rate{};
  double effective_yield{};
  FitArchiveFitDiag curve_fit{};
  FitArchiveChannelDiag channel{};
  FitArchiveRepair repair{};
};

struct FitArchiveCarry {
  double supplied_forward{};
  double supplied_yield{};
  double implied_forward{};
  double implied_yield{};
  double implied_total_yield{};
  double parity_forward{};
  double forward_lower{};
  double forward_upper{};
  double dispersion_sd{};
  double exercise_shift_sd{};
  double atm_total_vol{};
  std::uint64_t pairs_valid{};
  std::uint64_t pairs_selected{};
  std::uint64_t pairs_retained{};
  std::uint64_t pairs_contaminated{};
  std::uint64_t pairs_unidentified{};
  FitArchiveStringRef reason{};
  std::uint8_t mode{};
  std::uint8_t applied{};
  std::uint8_t fallback{};
  std::uint8_t status{};
  std::uint32_t reserved{};
};

struct FitArchiveCurve {
  double T{};
  double F{};
  double df{};
  FitArchiveArrayRef payload{}; // into CurveWords
  std::uint32_t node_count{};
  std::uint8_t present{};
  std::uint8_t kind{}; // VolCurveKind
  std::array<std::uint8_t, 10> reserved{};
};

struct FitArchivePrices {
  double spot{};
  double discount_min{};
  double discount_max{};
  double prepaid_min{};
  double prepaid_max{};
  FitArchiveArrayRef nodes{}; // into PriceNodes
  std::uint8_t present{};
  std::array<std::uint8_t, 7> reserved{};
};

struct FitArchiveAlOpts {
  double tol{};
  std::uint16_t n_collocation{};
  std::uint16_t n_quadrature{};
  std::uint16_t n_quad_price{};
  std::uint16_t max_newton_iter{};
};

inline constexpr std::uint8_t kSliceChannelRefit = 1u;
inline constexpr std::uint8_t kSliceCalendarRetry = 2u;
inline constexpr std::uint8_t kSliceCalendarRepaired = 4u;
inline constexpr std::uint8_t kSliceRepairRung = 8u;
inline constexpr std::uint8_t kSliceCalendarChannelRepaired = 16u;
inline constexpr std::uint8_t kSlicePointGated = 32u;
inline constexpr std::uint8_t kSlicePointFallback = 64u;
inline constexpr std::uint8_t kSliceCalendarReadmitted = 128u;

struct FitArchiveSlice {
  double calendar_gap{};
  double calendar_gap_strike{};
  double calendar_repair_required{};
  double point_outside_fraction{};
  std::uint64_t point_outside_quotes{};
  FitArchiveRepair rung_repair{};
  FitArchiveRepair calendar_channel_repair{};
  FitArchiveStringRef refusal{};
  FitArchiveStringRef error{};
  FitArchiveStringRef point_error{};
  std::uint8_t grade{};        // QuoteSliceGrade
  std::uint8_t point_status{}; // QuoteSliceFitStatus
  std::uint8_t flags{};        // kSlice*
  std::uint8_t present{};      // 1: a QuoteSurfaceSlice; 0: a standalone QuoteSliceFit
  std::uint32_t reserved{};
};

struct FitArchiveFitRecord {
  FitArchiveKey key{};
  FitArchiveMarket market{};
  FitArchiveDiagnostics diagnostics{};
  FitArchiveCarry carry{};
  FitArchiveCurve curve{};
  FitArchivePrices prices{};
  FitArchiveAlOpts al_opts{};
  FitArchiveSlice slice{};
  FitArchiveArrayRef rows{};        // into QuoteRows
  FitArchiveArrayRef moved_nodes{}; // into MovedNodes
  FitArchiveArrayRef dividends{};   // into Dividends
  FitArchiveArrayRef carry_curve{}; // into CarryPoints
  FitArchiveStringRef fit_error{};
  std::int32_t discrete_div_steps{};
  std::uint32_t surface{kFitArchiveNone}; // owning SurfaceRecords row
  std::uint8_t fit_status{};
  std::uint8_t dividend_model{};
  std::uint8_t discrete_div_scheme{};
  std::uint8_t has_al_opts{};
  std::array<std::uint8_t, 44> reserved{};
  // ── minor 1 (lane C0): snapshot provenance and the identifiability policy. A minor-0
  // record reads these as zero: no provenance, Legacy policy, no price-only nodes.
  std::int64_t quote_ns{};       // QuoteSliceSnapshotInfo::quote_ns
  std::int64_t underlying_ns{};  // QuoteSliceSnapshotInfo::underlying_ns
  double raw_spot{};             // meaningful when snapshot_flags has kFitSnapshotHasRawSpot
  std::uint32_t spot_version{};
  std::uint32_t carry_version{};
  FitArchiveArrayRef price_only_nodes{}; // into PriceOnlyNodes
  std::uint8_t carry_convention{}; // QuoteSliceCarryConvention
  std::uint8_t floor_policy{};     // QuoteSliceFloorPolicy
  std::uint8_t snapshot_flags{};   // kFitSnapshot*
  // QuoteSliceSpotRule (lane C0 fix round 1, from a reserved byte: an earlier minor-1
  // writer wrote 0 = Own).
  std::uint8_t spot_rule{};
  std::array<std::uint8_t, 4> reserved1{};
};

inline constexpr std::uint8_t kFitSnapshotSpotStale = 1u;
inline constexpr std::uint8_t kFitSnapshotHasRawSpot = 2u;

inline constexpr std::uint8_t kQuoteRowHasInterval = 1u;

struct FitArchiveQuoteRow {
  double market_iv{};
  double fitted_iv{};
  double fitted_price{};
  double spread_vol{};
  double weight_w{};
  std::uint8_t quote_status{};
  std::uint8_t evaluation_status{};
  std::uint8_t admitted{};
  std::uint8_t weight_capped{};
  std::uint32_t reserved{};
  // ── minor 1 (lane C0): identifiability. A minor-0 row reads these as zero: no reason,
  // no convention recorded, no interval.
  double iv_lower{};               // meaningful when flags has kQuoteRowHasInterval
  double iv_upper{};
  std::uint8_t price_only_reason{}; // QuoteSlicePriceOnlyReason
  std::uint8_t vol_source{};        // QuoteSliceVolConvention
  std::uint8_t flags{};             // kQuoteRowHasInterval
  std::array<std::uint8_t, 5> reserved1{};
};

// ── Surface records (one per underlier x snapshot) ──────────────────────────

struct FitArchiveSurfaceRecord {
  FitArchiveKey key{};
  double term_prior_weight{};
  double prior_decay_log_time{};
  double prior_quote_halflife{};
  double channel_node_weight{};
  double calendar_retry_factor{};
  double calendar_repair_half_spreads{};
  std::uint64_t grade_b_min_mid_strikes{};
  std::array<std::uint8_t, 32> policy_digest{};
  FitArchiveStringRef profile{};
  FitArchiveStringRef policy_source{};
  std::uint32_t first_fit{};
  std::uint32_t fit_count{};
  std::array<std::uint32_t, 4> grade_counts{}; // A, B, C, Refused
  std::uint32_t flags{};
  std::uint32_t reserved0{};
  std::array<std::uint8_t, 32> reserved{};
};

// ── ExpiryRecord meta, chain (ladder) sources ───────────────────────────────

struct FitArchiveExpiryMeta {
  FitArchiveKey key{}; // sub = side << 16 | ordinal
  double expiry{};
  double rate{};
  double carry{};
  double k_min{};
  double k_max{};
  double sigma_min{};
  double sigma_max{};
  double forward_factor{};
  double certified_max_abs_error_fwd{};
  std::uint32_t certified_points{};
  std::uint32_t uncertified_rows{};
  std::uint8_t side{};
  std::uint8_t populated{};
  std::uint8_t american{};
  std::uint8_t has_exercise_edge{};
  std::uint32_t ladder{kFitArchiveNone}; // owning Ladders row
  std::uint32_t data_index{};            // row in ExpiryData
  std::array<std::uint8_t, 12> reserved{};
};

struct FitArchiveLadderRecord {
  FitArchiveKey key{};
  double spot{};
  double spot_move{};
  double sigma_lo_mult{};
  double sigma_hi_mult{};
  FitArchiveAlOpts sample_opts{};
  double window_sigmas{};
  double window_min{};
  double window_max{};
  double max_cert_usd{};
  double max_cert_rel{};
  double worst_certified_error_fwd{};
  FitArchiveArrayRef contracts{}; // into LadderContracts
  std::uint32_t first_record{};   // ExpiryMeta rows, in ChainRecords::bank() order
  std::uint32_t record_count{};
  std::uint32_t max_rebuilds_per_tick{};
  std::uint32_t build_workers{};
  std::uint8_t has_sample_opts{};
  std::uint8_t certify{};
  std::uint8_t rebuild_on_exit{};
  std::uint8_t rebuild_order{};
  std::array<std::uint8_t, 44> reserved{};
};

struct FitArchiveLadderContract {
  double strike{};
  double expiry{};
  double sigma{};
  double rate{};
  double carry{};
  std::uint32_t record{}; // bank index (relative to the ladder's first_record)
  std::uint8_t side{};
  std::array<std::uint8_t, 3> reserved{};
};

// ── Common Markov model specs ────────────────────────────────────────────────

struct FitArchiveMarkovRecord {
  FitArchiveKey key{};
  double spot{};
  double upper_spot_multiple{};
  double max_calendar_step{};
  double max_variance_step{};
  double cfl_fraction{};
  std::uint64_t grid_nodes{};
  std::uint64_t max_states{};
  std::uint64_t max_transitions{};
  std::uint64_t max_stored_rows{};
  std::uint64_t max_stored_volatility_values{};
  std::uint64_t max_transition_work{};
  FitArchiveArrayRef intervals{};           // into MarkovIntervals
  FitArchiveArrayRef state_grid{};          // into MarkovWords
  FitArchiveArrayRef sigmas{};              // into MarkovWords (fit summary)
  FitArchiveArrayRef log_state_knots{};     // into MarkovWords
  FitArchiveArrayRef variance_time_knots{}; // into MarkovWords
  FitArchiveArrayRef quote_prices{};        // into MarkovWords
  FitArchiveStringRef label{};
  std::uint8_t diffusion_method{};
  std::uint8_t time_shape{};
  std::uint8_t has_fit{};
  std::array<std::uint8_t, 21> reserved{};
};

struct FitArchiveMarkovInterval {
  double calendar_end{};
  double variance_end{};
  double rate{};
  double continuous_yield{};
  double volatility{};
  double cash_dividend_at_end{};
  FitArchiveArrayRef local_volatility{}; // into MarkovLocalVol
};

// ── Series manifest ──────────────────────────────────────────────────────────

struct FitArchiveSeriesSegment {
  std::uint64_t sequence{};
  std::uint64_t file_size{};
  std::uint32_t file_crc32c{};
  std::uint32_t header_crc32c{};
  std::int64_t min_snapshot_ns{};
  std::int64_t max_snapshot_ns{};
  std::uint64_t surface_records{};
  std::uint64_t fit_records{};
  std::uint64_t expiry_records{};
  std::uint64_t ladder_records{};
  std::uint64_t markov_records{};
  // NUL padded. Exactly the writer's name for `sequence`: "seg-", the decimal
  // sequence zero-padded to at least 12 digits, ".atxvfa". Anything else (a
  // directory part, "..", a drive letter, another sequence) is RecordInvalid.
  std::array<char, 40> file_name{};
  std::uint64_t reserved{};
};

// One per FitRecords element, same order (FitTimings section, optional).
struct FitArchiveFitTimings {
  double channel_solve_seconds{}; // QuoteSliceChannelDiagnostics::solve_seconds
};

// ── Layout pins (ARCHIVE_FORMAT.md) ──────────────────────────────────────────

template <class T> inline constexpr bool kFitArchivePod =
    std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

static_assert(kFitArchivePod<FitArchiveHeader> && sizeof(FitArchiveHeader) == 256);
static_assert(offsetof(FitArchiveHeader, file_size) == 24);
static_assert(offsetof(FitArchiveHeader, created_unix_ns) == 48);
static_assert(offsetof(FitArchiveHeader, writer_name) == 80);
static_assert(offsetof(FitArchiveHeader, section_table_crc32c) == 112);
static_assert(offsetof(FitArchiveHeader, header_crc32c) == 116);
static_assert(kFitArchivePod<FitArchiveSectionEntry> && sizeof(FitArchiveSectionEntry) == 64);
static_assert(offsetof(FitArchiveSectionEntry, element_stride) == 32);
static_assert(offsetof(FitArchiveSectionEntry, crc32c) == 44);
static_assert(kFitArchivePod<FitArchiveFooter> && sizeof(FitArchiveFooter) == 64);
static_assert(offsetof(FitArchiveFooter, file_crc32c) == 40);
static_assert(offsetof(FitArchiveFooter, footer_crc32c) == 44);
static_assert(kFitArchivePod<FitArchiveKey> && sizeof(FitArchiveKey) == 24);
static_assert(kFitArchivePod<FitArchiveIndexEntry> && sizeof(FitArchiveIndexEntry) == 24);
static_assert(offsetof(FitArchiveIndexEntry, record) == 20);
static_assert(kFitArchivePod<FitArchiveSymbol> && sizeof(FitArchiveSymbol) == 48);
static_assert(sizeof(FitArchiveStringRef) == 8 && sizeof(FitArchiveArrayRef) == 8);
static_assert(kFitArchivePod<FitArchiveMarket> && sizeof(FitArchiveMarket) == 48);
static_assert(kFitArchivePod<FitArchiveFitDiag> && sizeof(FitArchiveFitDiag) == 48);
static_assert(kFitArchivePod<FitArchiveChannelDiag> && sizeof(FitArchiveChannelDiag) == 96);
static_assert(kFitArchivePod<FitArchiveRepair> && sizeof(FitArchiveRepair) == 56);
static_assert(kFitArchivePod<FitArchiveDiagnostics> && sizeof(FitArchiveDiagnostics) == 280);
static_assert(kFitArchivePod<FitArchiveCarry> && sizeof(FitArchiveCarry) == 144);
static_assert(kFitArchivePod<FitArchiveCurve> && sizeof(FitArchiveCurve) == 48);
static_assert(kFitArchivePod<FitArchivePrices> && sizeof(FitArchivePrices) == 56);
static_assert(kFitArchivePod<FitArchiveAlOpts> && sizeof(FitArchiveAlOpts) == 16);
static_assert(kFitArchivePod<FitArchiveSlice> && sizeof(FitArchiveSlice) == 184);
static_assert(kFitArchivePod<FitArchiveFitRecord> && sizeof(FitArchiveFitRecord) == 944);
static_assert(offsetof(FitArchiveFitRecord, market) == 24);
static_assert(offsetof(FitArchiveFitRecord, diagnostics) == 72);
static_assert(offsetof(FitArchiveFitRecord, carry) == 352);
static_assert(offsetof(FitArchiveFitRecord, curve) == 496);
static_assert(offsetof(FitArchiveFitRecord, prices) == 544);
static_assert(offsetof(FitArchiveFitRecord, al_opts) == 600);
static_assert(offsetof(FitArchiveFitRecord, slice) == 616);
static_assert(offsetof(FitArchiveFitRecord, rows) == 800);
static_assert(offsetof(FitArchiveFitRecord, fit_error) == 832);
static_assert(offsetof(FitArchiveFitRecord, discrete_div_steps) == 840);
static_assert(offsetof(FitArchiveFitRecord, fit_status) == 848);
static_assert(offsetof(FitArchiveFitRecord, reserved) == 852);
static_assert(offsetof(FitArchiveFitRecord, quote_ns) == 896); // minor-1 tail
static_assert(offsetof(FitArchiveFitRecord, price_only_nodes) == 928);
static_assert(offsetof(FitArchiveFitRecord, spot_rule) == 939);
static_assert(offsetof(FitArchiveFitRecord, reserved1) == 940);
static_assert(kFitArchivePod<FitArchiveQuoteRow> && sizeof(FitArchiveQuoteRow) == 72);
static_assert(offsetof(FitArchiveQuoteRow, iv_lower) == 48); // minor-1 tail
static_assert(offsetof(FitArchiveQuoteRow, reserved1) == 67);
static_assert(kFitArchivePod<FitArchiveSurfaceRecord> && sizeof(FitArchiveSurfaceRecord) == 192);
static_assert(offsetof(FitArchiveSurfaceRecord, policy_digest) == 80);
static_assert(offsetof(FitArchiveSurfaceRecord, first_fit) == 128);
static_assert(offsetof(FitArchiveSurfaceRecord, grade_counts) == 136);
static_assert(kFitArchivePod<FitArchiveExpiryMeta> && sizeof(FitArchiveExpiryMeta) == 128);
static_assert(offsetof(FitArchiveExpiryMeta, side) == 104);
static_assert(offsetof(FitArchiveExpiryMeta, data_index) == 112);
static_assert(kFitArchivePod<FitArchiveLadderRecord> && sizeof(FitArchiveLadderRecord) == 192);
static_assert(offsetof(FitArchiveLadderRecord, contracts) == 120);
static_assert(offsetof(FitArchiveLadderRecord, has_sample_opts) == 144);
static_assert(kFitArchivePod<FitArchiveLadderContract> &&
              sizeof(FitArchiveLadderContract) == 48);
static_assert(kFitArchivePod<FitArchiveMarkovRecord> && sizeof(FitArchiveMarkovRecord) == 192);
static_assert(offsetof(FitArchiveMarkovRecord, intervals) == 112);
static_assert(offsetof(FitArchiveMarkovRecord, label) == 160);
static_assert(offsetof(FitArchiveMarkovRecord, diffusion_method) == 168);
static_assert(kFitArchivePod<FitArchiveMarkovInterval> &&
              sizeof(FitArchiveMarkovInterval) == 56);
static_assert(kFitArchivePod<FitArchiveSeriesSegment> && sizeof(FitArchiveSeriesSegment) == 128);
static_assert(kFitArchivePod<FitArchiveFitTimings> && sizeof(FitArchiveFitTimings) == 8);

// Curve payload word counts per VolCurveKind (ARCHIVE_FORMAT.md section 6). The
// fixed counts are the native parameter-struct sizes / 8, pinned again against
// sizeof(EssviParams) etc. in the records translation unit.
inline constexpr std::uint32_t kFitCurveConvexFixedWords = 13;
inline constexpr std::uint32_t kFitCurveEssviWords = 31;
inline constexpr std::uint32_t kFitCurveSviWords = 9;
inline constexpr std::uint32_t kFitCurveC8Words = 22;
inline constexpr std::uint32_t kFitCurveSplineFixedWords = 6;

// sizeof(ExpiryRecord), and the bytes of it that are declared members (the rest
// is tail padding the writer zeroes). Pinned against the class in the
// implementation; see ARCHIVE_FORMAT.md section 7.
inline constexpr std::uint32_t kFitArchiveExpiryRecordBytes = 1824;
inline constexpr std::uint32_t kFitArchiveExpiryRecordValueBytes = 1820;

} // namespace atx::vol
