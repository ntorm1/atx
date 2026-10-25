#pragma once

// RiskRun (kind RiskRun): a directory of self-contained `seg-<seq>.atxrr` segments
// behind a PersistDirectory manifest, recording a risk run step by step — the
// prepared book once, then per step the RiskFrame columns, the ExplainResult columns
// and totals, the explain buckets and historical-VaR frames / points (design
// wave3-persist D13). `RiskRunWriter` stages steps in memory and seals a segment at
// `segment_bytes` or on `finish()`; `compact_risk_run` merges segments bit-exact,
// optionally into the zstd cold tier.
//
// LIFETIME. Writer and archive are move-only PIMPL. Spans returned by the archive
// borrow its segments and die with it.
//
// FROZEN API CONTRACT (wave 3, lane P scaffold 2026-10-24). Lane P2 implements it and
// may ADD. Added by the lane-P2 fix round (2026-10-24, approved): SealDurability and
// the two RiskRunOptions durability fields, RiskRunStepInfo, RiskRunArchive::n_var /
// var / materialize_frame / materialize_explain / step_info, and the compact_risk_run
// overload taking a SealDurability.
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/risk/historical_var.hpp" // HistoricalVarResult
#include "atx/vol/api/risk/pnl_explain.hpp"    // ExplainResult, ExplainTotals
#include "atx/vol/api/risk/risk_book.hpp"      // PreparedRiskBook
#include "atx/vol/api/risk/risk_frame.hpp"     // RiskFrame

namespace atx::vol {

inline constexpr std::string_view kRiskRunSalt = "ATXVRR01";

[[nodiscard]] std::uint64_t risk_run_fingerprint() noexcept;

// How durable one segment seal (or a compaction) is when it returns. Both publish the
// same bytes the same way — write a unique temp beside the target, rename it into
// place, then replace the manifest likewise under the directory's writer lock — so a
// reader never observes a partial file, and a crash between the segment and the
// manifest leaves an unreferenced orphan, never a dangling entry.
//   Publish  no flush: the bytes are in the OS cache when the seal returns. A process
//            crash loses nothing that was sealed; a power loss / OS crash before the
//            OS writes the pages back can leave a torn segment or manifest. Open at
//            Full / Paranoid refuses such a run (header / file / section CRC); at
//            Lazy the index sections are verified at open and a column on its first
//            touch, so a torn column surfaces later (an empty span from frame() /
//            explain(), SectionChecksum from materialize_*); Framing verifies no
//            payload.
//   Flush    Publish + FlushFileBuffers on the segment and on the manifest before
//            their renames: the seal survives a power loss once it returns. A Flush
//            finish() first flushes every segment this writer sealed at Publish, so a
//            power loss after it returns cannot leave the flushed manifest naming an
//            unflushed segment. That flush needs write access to the files: while a
//            reader holds one open without write sharing (a mapping), finish() returns
//            Busy, changes nothing and may be retried. A reader that keeps the run
//            mapped makes that Busy indefinite: for runs read while they are written,
//            set seal_durability = Flush (finish() then has nothing earlier to flush),
//            or close the readers before finish(). A size-triggered Flush seal makes
//            only its own segment and the manifest durable.
//            POSIX only: the Flush path also fsyncs the directory after each rename. If
//            that fsync fails, the seal reports IoError although the manifest may
//            already list the segment; the writer's next seal then finds a row it did
//            not record and refuses (SegmentMismatch: the writer is lost), and the run
//            stays consistent (the manifest lists only complete files).
// The writer never starts a thread: a caller that wants a Flush seal off its hot path
// runs the appending (or finish()) on its own thread.
enum class SealDurability : std::uint8_t { Publish = 0, Flush = 1 };

struct RiskRunOptions {
  // Seal threshold. 16 MiB (was 64 MiB; ruling 2026-10-24): the Publish seal's <= 80 ms
  // latency contract holds at 16 MiB on the reference host and not at 64 MiB, where the
  // OS's cached write of a ~70 MB file alone takes ~80 ms (docs/persist-history.md).
  std::uint64_t segment_bytes{16ull << 20};
  bool frames{true};
  bool buckets{true};
  std::int64_t created_unix_ns{0};
  // Seals triggered by `segment_bytes` while streaming (the latency-sensitive path).
  SealDurability seal_durability{SealDurability::Publish};
  // The last seal, in finish().
  SealDurability finish_durability{SealDurability::Flush};
};

// SINGLE WRITER. A run directory has one writer for its whole life. create() refuses a
// directory that already lists a run (AlreadyExists, checked under the directory's
// writer lock). Every seal re-checks ownership under the lock: the manifest must still
// end with the row this writer published last (or still be empty, for its first
// segment). A second writer on the same directory, or a compaction that replaced the
// manifest under a live writer, makes the next seal refuse with SegmentMismatch (a
// listed key is never replaced: DuplicateKey). From then on every append and finish()
// returns that refusal, and the writer's unsealed steps are lost; the run stays
// openable on whatever the manifest now lists.
class RiskRunWriter {
public:
  [[nodiscard]] static PersistResult<RiskRunWriter> create(std::string_view dir,
                                                           const PreparedRiskBook &book,
                                                           const RiskRunOptions &options);

  RiskRunWriter(RiskRunWriter &&) noexcept;
  RiskRunWriter &operator=(RiskRunWriter &&) noexcept;
  RiskRunWriter(const RiskRunWriter &) = delete;
  RiskRunWriter &operator=(const RiskRunWriter &) = delete;
  ~RiskRunWriter();

  [[nodiscard]] PersistOutcome append_frame(const RiskFrame &frame);
  [[nodiscard]] PersistOutcome append_explain(const ExplainResult &result);
  [[nodiscard]] PersistOutcome append_var(const HistoricalVarResult &result);
  [[nodiscard]] PersistOutcome finish();

private:
  struct Impl;
  explicit RiskRunWriter(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

// RiskFrame's per-unique f64 columns, in struct order (T .. vendor_delta).
enum class RiskFrameColumn : std::uint8_t {
  T = 0, Sigma, Forward, QEff, Rate, Price, Delta, Gamma, Vega, Theta, Rho, Vanna, Volga,
  Charm, VendorPrice, VendorDelta,
};
inline constexpr std::size_t kRiskFrameColumnCount = 16;

// ExplainResult's per-leg f64 columns, in struct order (pv_base .. d_rate, then pnl_carry,
// appended by lane E after the status vectors).
enum class RiskExplainColumn : std::uint8_t {
  PvBase = 0, PvTarget, PnlTotal, PnlDelta, PnlGamma, PnlVega, PnlVolga, PnlVanna, PnlTheta,
  PnlRho, PnlCharm, PnlUnexplained, VolLevel, VolSkew, VolCurv, VolResid, DSpot, DVol, DTime,
  DRate, PnlCarry,
};
inline constexpr std::size_t kRiskExplainColumnCount = 21;

// One recorded step's metadata (a value: nothing is borrowed).
struct RiskRunStepInfo {
  std::uint64_t step{0};            // global step index
  std::int64_t ts_ns{0};            // the frame's instant / the explain's shifted instant
  std::int64_t base_ts_ns{0};       // the explain's base instant (0 without an explain)
  double dt_years{0.0};             // the explain's horizon (0 without an explain)
  std::uint64_t book_logical_id{0}; // the frame's book identity as recorded (process-local)
  bool has_frame{false};
  bool has_explain{false};
  std::size_t explain_rows{0};      // explained legs (0 without an explain)
  std::size_t segment{0};           // ordinal of the segment that holds the step
  RiskNeeds needs{};                // the frame's greek needs
  FrameCounters counters{};         // the frame's counters
};

class RiskRunArchive {
public:
  [[nodiscard]] static PersistResult<RiskRunArchive> open(std::string_view dir,
                                                          const PersistOpenOptions &options = {});

  RiskRunArchive(RiskRunArchive &&) noexcept;
  RiskRunArchive &operator=(RiskRunArchive &&) noexcept;
  RiskRunArchive(const RiskRunArchive &) = delete;
  RiskRunArchive &operator=(const RiskRunArchive &) = delete;
  ~RiskRunArchive();

  [[nodiscard]] std::size_t n_steps() const noexcept;
  // Empty span when the step or column is absent, AND when the column failed its lazy
  // CRC (a Lazy open; see SealDurability): step_info() says whether the step has a
  // frame / explain, and materialize_frame / materialize_explain tell NotFound from
  // SectionChecksum.
  [[nodiscard]] std::span<const double> frame(RiskFrameColumn column, std::size_t step) const noexcept;
  [[nodiscard]] std::span<const double> explain(RiskExplainColumn column,
                                                std::size_t step) const noexcept;
  // nullptr when the step has no explain.
  [[nodiscard]] const ExplainTotals *totals(std::size_t step) const noexcept;
  [[nodiscard]] std::span<const std::uint64_t> leg_ids() const noexcept;

  // std::nullopt past the end.
  [[nodiscard]] std::optional<RiskRunStepInfo> step_info(std::size_t step) const noexcept;
  // Owned deep copies of a step's frame / explain (buckets included), every enum byte
  // validated (RecordInvalid otherwise; a column failing its lazy CRC is
  // SectionChecksum). NotFound when the step has no frame / explain or is past the end.
  [[nodiscard]] PersistResult<RiskFrame> materialize_frame(std::size_t step) const;
  [[nodiscard]] PersistResult<ExplainResult> materialize_explain(std::size_t step) const;
  // Historical-VaR results in append order (owned deep copies); NotFound past the end.
  [[nodiscard]] std::size_t n_var() const noexcept;
  [[nodiscard]] PersistResult<HistoricalVarResult> var(std::size_t index) const;

private:
  struct Impl;
  explicit RiskRunArchive(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

// Merge `dir`'s segments into one (bit-exact columns), optionally zstd-compressed at
// `level` (the cold tier). Returns the number of segments merged.
//
// COMPACTION CONTRACT. The run is read fully verified into memory (no mapping is held
// on the files the manifest stops naming); the merged segment is published under a
// fresh name; then, holding the directory's writer lock, the manifest is re-read and
// replaced by one naming only the merged segment — refused with SegmentMismatch when
// anything was published since the run was read (every row's seq and file CRC are
// compared). Compact only finished runs: if a writer is still live, a seal it lands
// during the compaction makes the compaction refuse, and a compaction that lands first
// makes the writer's next seal refuse (SINGLE WRITER above), losing its unsealed steps.
// When nothing would change (the run is one segment that re-frames to its own bytes:
// compacted again at the same codec and level, or a writer's lone segment) the call
// publishes nothing and returns 1: no superseded file, no new manifest row, so a live
// writer keeps owning the run, and nothing is flushed (the file keeps the durability
// it was sealed with). The superseded files of a real compaction stay on disk (a
// reader may still map them); removing them is offline gc. The 3-argument form seals
// with SealDurability::Flush.
[[nodiscard]] PersistResult<std::size_t> compact_risk_run(std::string_view dir, PersistCodec codec,
                                                          int level);
[[nodiscard]] PersistResult<std::size_t> compact_risk_run(std::string_view dir, PersistCodec codec,
                                                          int level, SealDurability durability);

} // namespace atx::vol
