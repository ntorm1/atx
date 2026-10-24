#pragma once

// ATXVFA typed records: encode the library's fitted objects into an archive
// (FitArchiveWriter) and read them back, either as zero-copy views typed as the
// library structs or as owned, bit-identical reconstructions (decode_*).
// Spec: atx-vol/docs/storage/ARCHIVE_FORMAT.md (sections 5-7).
//
// What is stored, per record type:
//   * a QuoteSurfaceSlice (graded admission) or a standalone QuoteSliceFit:
//     supported persisted fields, including the polymorphic curve (all six VolCurveKinds), the
//     certified AmericanPriceSlice nodes and bounds, row provenance, repair
//     records, carry resolution and dividends;
//   * a cross-expiry quote surface: QuoteSurfaceConfig, a caller policy digest,
//     the resolved profile and its expiries;
//   * ExpiryRecords (object representation, ARCHIVE_FORMAT.md section 7) and the
//     chain they were built for (the inputs of ChainRecords::build and
//     ChainSpotLadder::build, whose own state is not readable);
//   * an AmericanMarkovSpec (the deterministic input of compile_american_markov)
//     plus an optional fit summary.
//
// Runtime policies/provenance absent from this record version refuse with
// UnsupportedRecordVersion: terminal-floor recovery, economic repricing budgets,
// any nonempty withheld-node mask, and final-price projection rows/diagnostics.
// Both fit and surface additions roll back completely on that refusal. Legacy
// full-support archives retain their original empty-mask semantics.
//
// Adapters, not library changes: nothing here touches fitting / pricing
// internals. Where a type is not trivially serialisable (curves, price slices,
// QuoteSliceFit, the Markov model) the archive stores public fields and the
// decoder rebuilds the object through its public constructor / factory.
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "atx/vol/api/fitting/american_markov_fit.hpp"
#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/fitting/expiry_record.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american_markov.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp"
#include "atx/vol/api/storage/fit_archive.hpp"

namespace atx::vol {

// Library structs read in place from pool sections: their layout IS the format.
static_assert(sizeof(AmericanPriceNode) == 24 && offsetof(AmericanPriceNode, put) == 16);
static_assert(sizeof(CashDividend) == 16 && offsetof(CashDividend, amount) == 8);
static_assert(sizeof(QuoteSliceCarryPoint) == 16 &&
              offsetof(QuoteSliceCarryPoint, continuous_yield) == 8);
static_assert(sizeof(AmericanMarkovLocalVolNode) == 16 &&
              offsetof(AmericanMarkovLocalVolNode, volatility_multiplier) == 8);

// ── Writer inputs (all borrowed for the add_* call only) ────────────────────

// One fit_quote_surface result: `slices[i]` is expiry `expiry_days[i]`. Slices
// are stored in expiry order whatever their input order; days must be distinct.
struct FitArchiveSurfaceInput {
  std::string_view underlier{};
  std::int64_t snapshot_ns{0};
  std::span<const QuoteSurfaceSlice> slices{};
  std::span<const std::int32_t> expiry_days{};
  QuoteSurfaceConfig config{};
  std::array<std::uint8_t, 32> policy_digest{};
  std::string_view profile{};
  std::string_view policy_source{};
};

// One standalone (static-path) expiry fit.
struct FitArchiveFitInput {
  std::string_view underlier{};
  std::int64_t snapshot_ns{0};
  std::int32_t expiry_day{0};
  std::uint32_t sub{0};
  const QuoteSliceFit *fit{nullptr};
};

// One populated ExpiryRecord outside any chain. Key sub = side << 16 | ordinal.
struct FitArchiveExpiryInput {
  std::string_view underlier{};
  std::int64_t snapshot_ns{0};
  std::int32_t expiry_day{0};
  std::uint16_t ordinal{0};
  const ExpiryRecord *record{nullptr};
};

// A chain's records with everything a spot ladder is built from. Each bank
// record is keyed (underlier, snapshot, record_expiry_days[i] or expiry_day
// when that span is empty, side << 16 | ordinal); ordinals are assigned in bank
// order per (expiry day, side), starting at ordinal_base (give a second chain on
// the same expiries a distinct base, or its record keys collide).
struct FitArchiveChainInput {
  std::string_view underlier{};
  std::int64_t snapshot_ns{0};
  std::int32_t expiry_day{0}; // ladder key day (0: whole chain)
  std::uint32_t sub{0};       // ladder key ordinal
  std::uint16_t ordinal_base{0};
  double spot{0.0};
  ChainContractsView chain{};
  const ChainRecords *records{nullptr};
  ChainRecordPolicy record_policy{};
  SpotLadderPolicy ladder_policy{};
  std::span<const std::int32_t> record_expiry_days{};
};

// A common Markov model spec (the input of compile_american_markov) and,
// optionally, the fit that produced it.
struct FitArchiveMarkovInput {
  std::string_view underlier{};
  std::int64_t snapshot_ns{0};
  std::uint32_t sub{0};
  const AmericanMarkovSpec *spec{nullptr};
  const AmericanMarkovFit *fit{nullptr};
  std::string_view label{};
};

// ── Writer ───────────────────────────────────────────────────────────────────

// Copies every input on add_* (inputs are never retained), assigns symbol ids,
// sorts each index, refuses duplicate keys and frames the archive on finish().
// Records are stored in insertion order (a surface's expiries contiguous and in
// expiry order); lookups go through the sorted index. Single-threaded object;
// allocates.
//
// Determinism: the archive is a function of the added values and the
// FitArchiveFileInfo only. Wall-clock fit diagnostics (today
// QuoteSliceChannelDiagnostics::solve_seconds) differ run to run, so by default
// they are not stored and decode as 0: two archives of the same inputs and info
// are byte-identical, and an archive's sha256 can pin a parity check. With
// record_timings(true) they are kept in the optional FitTimings section and
// decode bit-exactly.
class FitArchiveWriter {
public:
  explicit FitArchiveWriter(FitArchiveFileInfo info = {});
  ~FitArchiveWriter();
  FitArchiveWriter(FitArchiveWriter &&) noexcept;
  FitArchiveWriter &operator=(FitArchiveWriter &&) noexcept;
  FitArchiveWriter(const FitArchiveWriter &) = delete;
  FitArchiveWriter &operator=(const FitArchiveWriter &) = delete;

  // InvalidArgument for an empty / over-long (> 40 byte) underlier, mismatched
  // spans, a null pointer, an unpopulated ExpiryRecord or an unknown curve kind.
  // A failed add leaves the writer unchanged.
  [[nodiscard]] ArchiveOutcome add_surface(const FitArchiveSurfaceInput &input);
  [[nodiscard]] ArchiveOutcome add_fit(const FitArchiveFitInput &input);
  [[nodiscard]] ArchiveOutcome add_expiry_record(const FitArchiveExpiryInput &input);
  [[nodiscard]] ArchiveOutcome add_chain_records(const FitArchiveChainInput &input);
  [[nodiscard]] ArchiveOutcome add_markov(const FitArchiveMarkovInput &input);
  // Transfer every record of `source` as stored (only symbol ids and pool
  // references are rebased): merge and compaction never re-encode a fit. A
  // source carrying FitTimings turns record_timings on, so its timings survive.
  [[nodiscard]] ArchiveOutcome copy_from(const FitArchive &source);
  void set_provenance(std::span<const std::byte> bytes);
  // Keep wall-clock fit diagnostics in a FitTimings section (default off; see
  // the class comment). Applies to every fit in the writer, added before or after.
  void record_timings(bool on) noexcept;

  [[nodiscard]] std::size_t surface_count() const noexcept;
  [[nodiscard]] std::size_t fit_count() const noexcept;
  [[nodiscard]] std::size_t expiry_count() const noexcept;
  [[nodiscard]] std::size_t ladder_count() const noexcept;
  [[nodiscard]] std::size_t markov_count() const noexcept;

  // The complete image (DuplicateKey when two records share a key). The writer
  // is unchanged and may keep accepting records. The overload stamps `info`
  // instead of the constructor's (series segments set their sequence this way).
  [[nodiscard]] ArchiveResult<std::vector<std::byte>> finish() const;
  [[nodiscard]] ArchiveResult<std::vector<std::byte>> finish(const FitArchiveFileInfo &info) const;
  // finish() + publish_fit_archive(path).
  [[nodiscard]] ArchiveOutcome write_file(std::string_view path) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ── Zero-copy typed views (allocation-free; borrow the archive) ─────────────

[[nodiscard]] std::span<const AmericanPriceNode> price_nodes(const FitArchive &archive,
                                                             const FitArchiveFitRecord &fit) noexcept;
[[nodiscard]] std::span<const CashDividend> fit_dividends(const FitArchive &archive,
                                                          const FitArchiveFitRecord &fit) noexcept;
[[nodiscard]] std::span<const QuoteSliceCarryPoint>
fit_carry_curve(const FitArchive &archive, const FitArchiveFitRecord &fit) noexcept;
// Parallel to archive.expiry_meta(): row i is the record meta row i describes.
[[nodiscard]] std::span<const ExpiryRecord> expiry_records(const FitArchive &archive) noexcept;
[[nodiscard]] const ExpiryRecord *expiry_record(const FitArchive &archive,
                                                const FitArchiveExpiryMeta &meta) noexcept;
[[nodiscard]] std::span<const AmericanMarkovLocalVolNode>
markov_local_vol(const FitArchive &archive, const FitArchiveMarkovInterval &interval) noexcept;

// ── Owned reconstructions (allocate; independent of the archive) ────────────

// Bit-identical to the object that was written, except that wall-clock timings
// decode as 0 unless the archive has a FitTimings section and `fit` is an
// element of archive.fits(). RecordInvalid when a stored curve or price slice
// does not rebuild, or when the curve fails the shape check of open's record
// pass (node count against payload length in 64-bit arithmetic, kind, reserved
// and native enum / bool bytes). A record of a validated archive always passes;
// the check guards record copies edited by the caller, which open never saw.
[[nodiscard]] ArchiveResult<std::unique_ptr<IVolCurve>> decode_curve(const FitArchive &archive,
                                                                    const FitArchiveFitRecord &fit);
[[nodiscard]] ArchiveResult<QuoteSliceFit> decode_fit(const FitArchive &archive,
                                                      const FitArchiveFitRecord &fit);
// A standalone fit decodes with every QuoteSurfaceSlice-only field at its default.
[[nodiscard]] ArchiveResult<QuoteSurfaceSlice> decode_surface_slice(const FitArchive &archive,
                                                                    const FitArchiveFitRecord &fit);
[[nodiscard]] ArchiveResult<std::vector<QuoteSurfaceSlice>>
decode_surface(const FitArchive &archive, const FitArchiveSurfaceRecord &surface);
[[nodiscard]] QuoteSurfaceConfig decode_surface_config(const FitArchiveSurfaceRecord &surface) noexcept;

struct FitArchiveChainSource {
  double spot{0.0};
  std::vector<double> strike, expiry, sigma, rate, carry;
  std::vector<Side> side;
  std::vector<std::uint32_t> record_of_contract; // bank indices
  ChainRecordPolicy record_policy{};
  SpotLadderPolicy ladder_policy{};
  double worst_certified_error_fwd{0.0};
  [[nodiscard]] ChainContractsView view() const noexcept {
    return {strike, expiry, sigma, rate, carry, side};
  }
};
[[nodiscard]] ArchiveResult<FitArchiveChainSource>
decode_chain_source(const FitArchive &archive, const FitArchiveLadderRecord &ladder);
// The chain's records as an ExpiryRecordBank, in bank order.
[[nodiscard]] ArchiveResult<ExpiryRecordBank> decode_expiry_bank(const FitArchive &archive,
                                                                 const FitArchiveLadderRecord &ladder);

// An AmericanMarkovSpec whose interval local-volatility spans BORROW the
// archive's MarkovLocalVol pool (zero copy): the spec must not outlive it.
struct FitArchiveMarkovSpec {
  double spot{0.0};
  AmericanMarkovConfig config{};
  std::vector<double> state_grid;
  std::vector<AmericanMarkovInterval> intervals;
  [[nodiscard]] AmericanMarkovSpec spec() const noexcept {
    return {spot, intervals, state_grid, config};
  }
};
[[nodiscard]] ArchiveResult<FitArchiveMarkovSpec> decode_markov_spec(const FitArchive &archive,
                                                                     const FitArchiveMarkovRecord &record);

} // namespace atx::vol
