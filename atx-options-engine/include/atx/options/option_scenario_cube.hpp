#pragma once

// Canonical, fail-closed full-reprice risk evidence for one point-in-time
// active options catalog.
//
// A cube is intentionally compiled from a decision-only active catalog.
// Every row declares why it is present: current candidate, filled position,
// working order, or pending cancel. This keeps memory O(active contracts x
// scenarios) instead of O(historical union contracts x dates x scenarios).
//
// Revision 2 implements FrozenStickyStrike dynamics only. Every scenario names
// an explicit simultaneous shock for every underlier in the active catalog.
// The compiler never invents constituent betas, correlations, or dispersion
// offsets, and never substitutes a Taylor approximation for a failed full
// reprice.

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/options/option_pretrade_risk.hpp"
#include "atx/options/option_research_panel.hpp"
#include "atx/vol/backtest.hpp"
#include "atx/vol/surface_archive.hpp"

namespace atx::options::risk {

inline constexpr std::uint64_t kOptionScenarioCubeSchemaVersion =
    0x4154584F53430002ULL; // "ATXOSC", revision 2
inline constexpr std::uint64_t kOptionScenarioCubePricerModelVersion =
    0x4154584F53430102ULL; // full reprice, frozen sticky strike, revision 2
inline constexpr std::uint64_t kOptionScenarioCubeGreekConventionVersion =
    0x4154584F53470201ULL; // spot-cash listed-contract Greeks, revision 1

enum class OptionScenarioSurfaceDynamics : std::uint8_t {
  FrozenStickyStrike = 0,
};

enum class OptionScenarioContractRole : std::uint8_t {
  None = 0,
  Candidate = 1U << 0U,
  FilledPosition = 1U << 1U,
  WorkingOrder = 1U << 2U,
  PendingCancel = 1U << 3U,
};

[[nodiscard]] constexpr OptionScenarioContractRole
operator|(OptionScenarioContractRole left, OptionScenarioContractRole right) noexcept {
  return static_cast<OptionScenarioContractRole>(static_cast<std::uint8_t>(left) |
                                                 static_cast<std::uint8_t>(right));
}

// Decision-only input row. Unlike OptionResearchPanel, it carries no execution
// timestamp or ex-post outcome label. role_mask must contain at least one known
// lifecycle role; the compiler rejects duplicate or noncanonical contract IDs.
struct OptionScenarioActiveContract {
  research::OptionInstrument instrument{};
  research::OptionDecisionAudit audit{};
  double market_mark{0.0};
  OptionScenarioContractRole role_mask{OptionScenarioContractRole::None};
};

// PIT attestation emitted by the lifecycle-state owner for the complete active
// union. The four counts must exactly match the role memberships in the
// supplied catalog. source_identity binds the persisted lifecycle artifact.
struct OptionScenarioActiveSetAttestation {
  std::int64_t observed_ts_ns{0};
  std::int64_t available_ts_ns{0};
  atx::vol::ArchiveContentIdentity source_identity{};
  std::size_t candidate_contract_count{0};
  std::size_t filled_position_contract_count{0};
  std::size_t working_order_contract_count{0};
  std::size_t pending_cancel_contract_count{0};
};

// Scenario-level fields shared by every underlier shock in the scenario.
// horizon_ns rolls calendar time forward and may cross expiry. rate_shift is
// an absolute continuously-compounded rate change.
struct OptionScenarioDefinition {
  std::uint64_t scenario_id{0};
  std::int64_t horizon_ns{0};
  double rate_shift{0.0};

  [[nodiscard]] bool operator==(const OptionScenarioDefinition &) const noexcept = default;
};

// Exactly one row is required for each (scenario_id, active underlier_uid).
// spot_return is a simple return and must be greater than -1.
// vol_level_shift is an absolute volatility change (0.05 = five vol points).
struct OptionScenarioUnderlierShock {
  std::uint64_t scenario_id{0};
  std::uint32_t underlier_uid{0};
  double spot_return{0.0};
  double vol_level_shift{0.0};

  [[nodiscard]] bool operator==(const OptionScenarioUnderlierShock &) const noexcept = default;
};

struct OptionScenarioManifest {
  OptionScenarioSurfaceDynamics surface_dynamics{OptionScenarioSurfaceDynamics::FrozenStickyStrike};
  // Strictly positive floor applied after the absolute vol shift.
  double minimum_implied_vol{1.0e-4};
  // Point-in-time clocks for the manifest artifact. observed <= available <=
  // decision and effective <= decision are enforced by the compiler.
  std::int64_t observed_ts_ns{0};
  std::int64_t available_ts_ns{0};
  std::int64_t effective_ts_ns{0};
  // Persisted identity of the canonical manifest artifact. The compiler also
  // computes and exposes its SHA-256 semantic digest.
  atx::vol::ArchiveContentIdentity artifact_identity{};
  std::vector<OptionScenarioDefinition> scenarios{};
  std::vector<OptionScenarioUnderlierShock> underlier_shocks{};
};

struct OptionScenarioCubeLimits {
  std::size_t max_contracts{100'000};
  std::size_t max_underliers{10'000};
  std::size_t max_scenarios{1'024};
  std::size_t max_scenario_cells{100'000'000};
  // Bounds the exact logical payload allocated by the compiler and generated
  // panel (base state, canonical rows, dense P&L, lineage, and panel axes).
  // Caller-owned panel/snapshot storage, moved-in manifest excess capacity,
  // container objects, and allocator metadata are excluded.
  std::size_t max_workspace_bytes{8'589'934'592ULL}; // 8 GiB
  // Surface evidence must be genuinely point-in-time and recent. Zero is
  // rejected; callers must choose an explicit policy.
  std::int64_t max_surface_age_ns{900'000'000'000LL}; // 15 minutes
  // Same fail-closed age policy for the option mark used by premium limits.
  std::int64_t max_market_age_ns{900'000'000'000LL}; // 15 minutes
  // Counted per contract-scenario lane. The default fails closed if an
  // absolute volatility shock would bind the configured floor.
  std::size_t max_vol_floor_hits{0};
};

struct OptionScenarioCubeBuildSpec {
  // Externally observed availability of the loaded archive.
  std::int64_t surface_available_ts_ns{0};
  // Zero requests the pricing executor's bounded automatic pool width.
  unsigned n_threads{0};
  OptionScenarioActiveSetAttestation active_set_attestation{};
  OptionScenarioCubeLimits limits{};
};

struct OptionScenarioCubeBuildReport {
  std::size_t contract_count{0};
  std::size_t underlier_count{0};
  std::size_t scenario_count{0};
  std::size_t scenario_cell_count{0};
  std::size_t american_contract_count{0};
  std::size_t european_contract_count{0};
  std::size_t candidate_contract_count{0};
  std::size_t filled_position_contract_count{0};
  std::size_t working_order_contract_count{0};
  std::size_t pending_cancel_contract_count{0};
  std::size_t vol_floor_hit_count{0};
  OptionRiskContentDigest scenario_manifest_digest{};
  OptionRiskContentDigest risk_snapshot_digest{};
};

class OptionScenarioCube {
public:
  OptionScenarioCube(OptionRiskPanel risk_panel, OptionScenarioManifest canonical_manifest,
                     OptionScenarioCubeBuildReport build_report)
      : panel{std::move(risk_panel)}, manifest{std::move(canonical_manifest)},
        report{build_report} {}

  OptionScenarioCube(const OptionScenarioCube &) = delete;
  OptionScenarioCube &operator=(const OptionScenarioCube &) = delete;
  OptionScenarioCube(OptionScenarioCube &&) noexcept = default;
  OptionScenarioCube &operator=(OptionScenarioCube &&) noexcept = default;

  OptionRiskPanel panel;
  // Canonical, sorted semantic manifest used to produce panel.
  OptionScenarioManifest manifest;
  OptionScenarioCubeBuildReport report;
};

// Compile authoritative full-reprice risk evidence for one active-set segment.
//
// Preconditions enforced by the function:
// - active_contracts is a canonical, duplicate-free union of candidates,
//   filled positions, working orders, and pending cancels at one decision,
//   with role counts matching a PIT lifecycle-source attestation;
// - every contract has a matching definition-audit row and nonzero role;
// - snapshot was observed no later than its availability, availability is no
//   later than the decision, and age is within the nonzero configured bound;
// - every referenced surface is a healthy, admitted, nonlegacy Risk surface;
// - the manifest is finite, canonicalizable, and dense over the active
//   underlier set.
//
// American lanes use the archived American method and its exact resolved
// options through the cold-reference route. European lanes use Black-76 with
// the surface-implied forward/carry. Expiry-crossing scenarios use exact spot
// intrinsic. Any
// missing surface, invalid provenance, or nonfinite base/shocked solve aborts
// the entire build; no failed lane is skipped and no Taylor fallback exists.
[[nodiscard]] atx::core::Result<OptionScenarioCube>
compile_option_scenario_cube(std::span<const OptionScenarioActiveContract> active_contracts,
                             const atx::vol::MarketSnapshot &snapshot,
                             OptionScenarioManifest manifest,
                             const OptionScenarioCubeBuildSpec &spec = {});

} // namespace atx::options::risk
