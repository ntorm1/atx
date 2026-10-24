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
// may ADD.
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.

#include <cstddef>
#include <cstdint>
#include <memory>
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

struct RiskRunOptions {
  std::uint64_t segment_bytes{64ull << 20};
  bool frames{true};
  bool buckets{true};
  std::int64_t created_unix_ns{0};
};

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

// ExplainResult's per-leg f64 columns, in struct order (pv_base .. d_rate).
enum class RiskExplainColumn : std::uint8_t {
  PvBase = 0, PvTarget, PnlTotal, PnlDelta, PnlGamma, PnlVega, PnlVolga, PnlVanna, PnlTheta,
  PnlRho, PnlCharm, PnlUnexplained, VolLevel, VolSkew, VolCurv, VolResid, DSpot, DVol, DTime,
  DRate,
};
inline constexpr std::size_t kRiskExplainColumnCount = 20;

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
  // Empty span when the step / column is absent.
  [[nodiscard]] std::span<const double> frame(RiskFrameColumn column, std::size_t step) const noexcept;
  [[nodiscard]] std::span<const double> explain(RiskExplainColumn column,
                                                std::size_t step) const noexcept;
  // nullptr when the step has no explain.
  [[nodiscard]] const ExplainTotals *totals(std::size_t step) const noexcept;
  [[nodiscard]] std::span<const std::uint64_t> leg_ids() const noexcept;

private:
  struct Impl;
  explicit RiskRunArchive(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

// Merge `dir`'s segments into one (bit-exact columns), optionally zstd-compressed at
// `level` (the cold tier). Returns the number of segments merged.
[[nodiscard]] PersistResult<std::size_t> compact_risk_run(std::string_view dir, PersistCodec codec,
                                                          int level);

} // namespace atx::vol
