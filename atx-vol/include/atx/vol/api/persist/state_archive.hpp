#pragma once

// MarketState archive (kind MarketState, `.atxms`) and the SpiderRock-derived state
// store. One file holds ONE `MarketState` instant: per-underlier scalars, per-expiry
// carry, cash dividends, yield-curve pillars, every contract mark as a column in
// global (uid, expiry_ns, side, strike) row order, optional per-mark vendor extras
// (bid / ask / vendor Greeks / vendor T / row uPrc) and optional embedded fitted
// surfaces (one verbatim ATXVSA2 image). Spec: docs/storage/PERSIST_FORMAT.md
// "MarketState kind" (design wave3-persist D10 / D11).
//
// ZERO-COPY OPEN. `MarketStateArchive::open` maps the file, validates it, rebuilds the
// small owned parts (UnderlierState rows, YieldCurves, PricedSurfaceViews over the
// embedded records) and calls `MarketState::create` over spans INTO THE MAPPING: no
// mark column is copied. The measured point of the format: a licensed bucket that
// costs ~1.1 s to load from Parquet opens in milliseconds.
//
// LIFETIME (persist D14). `MarketStateArchive` is move-only PIMPL: its internal
// vectors keep stable addresses across moves, so a state taken before a move stays
// valid after it. A `MarketState` from `state()` (and every copy of it) BORROWS the
// archive and must not outlive it; spans from `extras()` / `ticker()` likewise. A
// RiskStream replay therefore keeps two archives rolling (s_{k-1} alive until
// `step(s_k)` returns). `materialize()` deep-copies when an owned state is needed.
//
// FROZEN API CONTRACT (wave 3, lane P scaffold 2026-10-24). Lane P1 may ADD.
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/backtest/portfolio_pricer.hpp"      // SurfaceRef
#include "atx/vol/api/core/types.hpp"                     // Result
#include "atx/vol/api/marketdata/spiderrock_store.hpp"    // SpiderRockStoreSpec / Bucket
#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/risk/market_state.hpp"

namespace atx::vol {

// ── Kind vocabulary ──────────────────────────────────────────────────────────

inline constexpr std::string_view kMarketStateSalt = "ATXVMS01";

// Section types of the MarketState kind (the private on-disk records are pinned in
// src/persist/state_archive_format.hpp; PERSIST_FORMAT.md lists every size).
enum class MarketStateSection : std::uint32_t {
  Meta = 1,          // POD 256 B: ts, TimeSpec, counts, loader report, date / bucket
  Underliers = 2,    // POD 96 B per uid, sorted by uid
  Strings = 3,       // ticker bytes
  TermCarry = 4,     // ExpiryCarry rows (direct POD)
  CashDivs = 5,      // DividendEvent rows (direct POD)
  YieldPillars = 6,  // (t_years, zero_rate) f64 pairs
  UidTable = 7,      // POD 8 B per uid (Lane P1 addition): the writer's whole uid -> ticker
                     // table, so a store build can replay its uid assignment (optional)
  Strike = 16,       // f64 per mark
  Expiry = 17,       // i64 per mark
  Side = 18,         // u8 per mark, validated in {0, 1}
  Sigma = 19,        // f64 per mark
  VendorPrice = 20,  // f64 per mark (optional)
  VendorDelta = 21,  // f64 per mark (optional)
  Bid = 32,          // optional extras, f64 per mark
  Ask = 33,
  VendorGamma = 34,
  VendorVega = 35,
  VendorTheta = 36,
  VendorT = 37,
  RowUprc = 38,
  SurfaceIndex = 48,   // POD 16 B per embedded surface: uid -> symbol (optional)
  SurfaceArchive = 49, // one verbatim ATXVSA2 archive image (optional)
};

// The one schema fingerprint this build reads and writes for the MarketState kind.
[[nodiscard]] std::uint64_t market_state_fingerprint() noexcept;

// Open defaults for the MarketState kind (persist D7): Full, because the open touches
// every core column anyway (mark order and side validation).
inline constexpr PersistOpenOptions kMarketStateOpenDefaults{true, PersistVerify::Full, false, 0};

// ── Writer ───────────────────────────────────────────────────────────────────

// Per-mark vendor extras for ONE `MarketStateStorage::marks` entry, aligned with its
// rows (every vector empty, or as long as that entry's `sigma`).
struct MarkExtrasColumns {
  std::vector<double> bid, ask, vendor_gamma, vendor_vega, vendor_theta, vendor_T, uprc;
};

// The pillars of one `MarketStateStorage::yield_curves` entry. ADDITIVE (lane P1):
// `YieldCurve` exposes no pillar accessor, so a writer cannot recover them from the
// curve; the encoder rebuilds a curve from these pillars and refuses
// (InvalidArgument) unless it reproduces the stored curve bit-for-bit at every pillar.
struct YieldCurvePillars {
  std::vector<double> t_years;
  std::vector<double> zero_rates;
};

struct MarketStateWriteInput {
  const MarketStateStorage *storage{nullptr};
  // tickers_by_uid[uid - 1] names uid (UnderlierTable::tickers()); a uid past the end
  // is stored with an empty ticker. The whole table is also stored (section UidTable).
  std::span<const std::string> tickers_by_uid{};
  // Empty, or one per `storage->marks` entry (same order).
  std::span<const MarkExtrasColumns> extras{};
  // Surfaces to embed (owned `PricedSurface` refs only: a view has no writer). Empty
  // embeds `storage->surfaces`. Each must name a uid of the storage, at most one per uid.
  std::span<const SurfaceRef> surfaces{};
  // ── Lane P1 additions ──
  std::span<const YieldCurvePillars> yield_pillars{}; // one per storage->yield_curves
  const BucketStateReport *report{nullptr};           // loader counters -> Meta (optional)
  std::string_view date{};                            // "YYYY-MM-DD" provenance (<= 15 bytes)
  std::string_view bucket_et{};                       // "HHMM" provenance (<= 7 bytes)
  std::uint32_t conventions_version{0};               // loader conventions the values follow
  std::uint64_t source_fingerprint{0};
  std::int64_t created_unix_ns{0};
  unsigned n_threads{1};                              // codec fan-out (none in the hot tier)
};

// Encode one state into a complete PersistFile image (deterministic: the same input
// gives the same bytes). InvalidArgument on a null storage, inconsistent column
// lengths, a uid listed twice, unsorted marks, a side byte outside
// {0, 1}, a yield pointer that does not point into `storage->yield_curves`, missing /
// non-reproducing pillars, a view-backed or foreign-uid surface, or an oversized
// date / bucket string. The round trip is canonical: rows come back sorted by uid.
[[nodiscard]] PersistResult<std::vector<std::byte>>
encode_market_state(const MarketStateWriteInput &input);

// ── Reader ───────────────────────────────────────────────────────────────────

// Views of one uid's extras rows (aligned with its ContractMarks). Empty spans when
// the archive carries no extras or the uid has no marks.
struct MarkExtras {
  std::span<const double> bid, ask, vendor_gamma, vendor_vega, vendor_theta, vendor_T, uprc;
};

class MarketStateArchive {
public:
  // Open + validate + build the state (see the file comment). On top of the
  // container refusals: RecordInvalid (meta counts, a side byte outside {0, 1}, an
  // enum / flag byte, a ticker range, an embedded surface whose uid disagrees),
  // IndexUnsorted (underliers not strictly ascending by uid, marks not ascending by
  // (expiry, side, strike) within a uid or a duplicate key), IndexBounds (a row range
  // outside its column), SectionStride (a column whose length is not n_marks).
  [[nodiscard]] static PersistResult<MarketStateArchive> open(std::string_view path,
                                                              const PersistOpenOptions &options);
  // ── Lane P1 addition: validate an image already in memory (tests, the tool) ──
  [[nodiscard]] static PersistResult<MarketStateArchive>
  open_bytes(std::span<const std::byte> bytes, std::shared_ptr<const void> owner,
             PersistVerify verify = PersistVerify::Full);

  MarketStateArchive(MarketStateArchive &&) noexcept;
  MarketStateArchive &operator=(MarketStateArchive &&) noexcept;
  MarketStateArchive(const MarketStateArchive &) = delete;
  MarketStateArchive &operator=(const MarketStateArchive &) = delete;
  ~MarketStateArchive();

  [[nodiscard]] const MarketState &state() const noexcept;
  // The ticker of a uid of the state, else of the stored uid table; "" when absent.
  [[nodiscard]] std::string_view ticker(std::uint32_t uid) const noexcept;
  [[nodiscard]] MarkExtras extras(std::uint32_t uid) const noexcept;
  [[nodiscard]] std::uint64_t source_fingerprint() const noexcept;
  // Owned deep copy. Its `surfaces` still BORROW this archive's embedded views (a
  // MarketStateStorage cannot own a surface); everything else is independent.
  [[nodiscard]] PersistResult<MarketStateStorage> materialize() const;

  // ── Lane P1 additions ──
  [[nodiscard]] const PersistFile &file() const noexcept;
  [[nodiscard]] bool has_extras() const noexcept;
  [[nodiscard]] std::uint32_t conventions_version() const noexcept;
  [[nodiscard]] std::string_view date() const noexcept;
  [[nodiscard]] std::string_view bucket_et() const noexcept;
  // The loader counters stored at encode (zeros when none were given).
  [[nodiscard]] BucketStateReport report() const noexcept;
  [[nodiscard]] std::size_t n_marks() const noexcept;
  // Entries of the stored uid table (uids 1..n; 0 when the writer gave none).
  [[nodiscard]] std::uint32_t uid_table_size() const noexcept;

private:
  struct Impl;
  explicit MarketStateArchive(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

// ── SpiderRock-derived state store ───────────────────────────────────────────
//
// Layout: <out_root>/date=YYYY-MM-DD/ is a PersistDirectory whose keys are the
// bucket HHMM strings; files are bucket_et=HHMM-g<seq>.atxms (persist D11). The store
// is a DERIVED, fingerprinted cache of the Parquet ingest of record: a bucket is
// rebuilt when its source fingerprint (loader conventions version, atx-vol version,
// every SpiderRockStoreSpec field, the bucket, and (name, size, last-write time) of
// each source parquet file) or its extras choice changed, or its stored uids no
// longer match the build's uid assignment.
//
// UIDS. One build call assigns uids exactly as a serial `load_spiderrock_bucket` over
// `buckets_et` IN THE GIVEN ORDER with ONE UnderlierTable would (the risk bench's
// assignment), even though buckets load in parallel: a day's archives built in one
// call share one uid space, so a RiskStream can replay them.

struct StateStoreBuildOptions {
  std::string out_root;                 // must not lie under the spec's store_root
  unsigned max_parallel_buckets{2};     // an uncapped Parquet decode peaks near 1 GB
  bool include_extras{true};
  std::int64_t created_unix_ns{0};
  bool force{false};                    // rebuild even when up to date
  // ── Lane P1 additions ──
  // REQUIRED (nonzero): the loader-conventions version the archived values follow
  // (orchestrator ruling Q2; becomes kSpiderRockConventionsVersion at integration).
  std::uint32_t conventions_version{0};
};

struct StateStoreBucketReport {
  std::string bucket_et;
  std::size_t marks{0}, bytes{0};
  bool skipped{false};
  double load_s{0}, encode_s{0}, publish_s{0};
  // ── Lane P1 additions ──
  std::uint64_t source_fingerprint{0};
  std::string path; // the published (or up-to-date) file
};

[[nodiscard]] PersistResult<std::vector<StateStoreBucketReport>>
build_state_store_from_spiderrock(const SpiderRockStoreSpec &spec,
                                  std::span<const std::string> buckets_et,
                                  const StateStoreBuildOptions &options);

// The source fingerprint of one bucket (see the store comment). NotFound when the
// bucket directory holds no parquet file. `conventions_version` is an explicit input
// (orchestrator ruling Q2).
[[nodiscard]] PersistResult<std::uint64_t>
spiderrock_state_source_fingerprint(const SpiderRockStoreSpec &spec, std::string_view bucket_et,
                                    std::uint32_t conventions_version);

class StateStore {
public:
  // NotFound when `root` is not a directory.
  [[nodiscard]] static PersistResult<StateStore> open(std::string_view root);
  [[nodiscard]] std::string_view root() const noexcept { return root_; }
  // The bucket keys published for `date` (ascending). NotFound when the date is absent.
  [[nodiscard]] PersistResult<std::vector<std::string>> buckets(std::string_view date) const;
  // Open the current generation of (date, bucket); SegmentMismatch when the file
  // disagrees with its manifest entry (size or header CRC).
  [[nodiscard]] PersistResult<MarketStateArchive>
  open_bucket(std::string_view date, std::string_view bucket_et,
              const PersistOpenOptions &options = kMarketStateOpenDefaults) const;

private:
  std::string root_;
};

// The bench book selector's input rebuilt from an archive: one row per archived mark
// with uid / strike / expiry / side / sigma / vendor price & delta and, when the
// archive carries extras, bid / ask / vendor gamma / vega / theta / T / uPrc. rate /
// sdiv / ddiv are NOT stored per row (the state carries them per expiry) and come back
// empty. InvalidArgument when the archive has no extras (the selector needs bid / ask)
// or no vendor delta.
[[nodiscard]] Result<SpiderRockBucket>
selection_bucket_from_archive(const MarketStateArchive &archive);

} // namespace atx::vol
