#pragma once

// atx::engine::data — DataContext (S6.8): the data-layer FACADE the BookPipeline
// consumes in place of a hand-built fixed Panel.
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  DataContext wraps a DatasetCatalog + the required price-dataset name and
//  exposes the FOUR data-plane artifacts the operating-book pipeline needs:
//
//    price_panel()             -> the alpha::Panel the pipeline mines / combines /
//                                 reports over. By DEFAULT (empty adv_windows) it
//                                 is a RAW lowering: Panel::create from the price
//                                 Dataset's columns + mask VERBATIM, so a price-only
//                                 context reproduces ANY user panel byte-identically
//                                 (the S6.8 boundary pin). Every Role::Feature dataset
//                                 in the catalog (ascending name) is then merged in as
//                                 extra named fields. NON-empty adv_windows instead
//                                 takes the with_datafields OHLCV augmentation path
//                                 (price_to_panel).
//    factor_model_override()   -> a BYO FactorModel lowered from a set FactorModelArtifact,
//                                 or nullopt (=> the pipeline builds the price-derived model).
//    signal_admit_candidates() -> the external-signal candidates (one SignalAdmission per
//                                 Role::Signal dataset, ascending name), as a flat candidate
//                                 view. The candidates' spans point INTO the OWNED admissions
//                                 held in *this — valid while the DataContext lives.
//    reference_spans_at()      -> the market_cap / group_id cross-section from the first
//                                 Role::Reference dataset, as-of a date.
//
// ===========================================================================
//  Ownership / lifetime (the load-bearing design)
// ===========================================================================
//  DataContext borrows the DatasetCatalog (a const&) for its lifetime — the catalog
//  MUST outlive the context. The context OWNS its lazily-built artifacts:
//    * a cached alpha::Panel (price_panel returns a const ref into it),
//    * a cached std::optional<risk::FactorModel> override,
//    * a std::vector<SignalAdmission> (move-only owners whose candidate spans the
//      flat candidate view aliases).
//  Because it owns the move-only SignalAdmissions, DataContext is itself MOVE-ONLY
//  (copy = delete). Every accessor returning a reference/span is valid only while
//  the DataContext lives.
//
// Cold-path (once per backtest window); allocation is intentional. Deterministic.

#include <functional> // std::reference_wrapper (price_panel return — tl::expected rejects ref T)
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp" // Result, Status
#include "atx/core/types.hpp" // f64, u16, u32, usize

#include "atx/engine/alpha/panel.hpp"                // alpha::Panel
#include "atx/engine/data/adapt_factor.hpp"          // RefSpans
#include "atx/engine/data/adapt_signal.hpp"          // SignalAdmission
#include "atx/engine/data/catalog.hpp"               // DatasetCatalog
#include "atx/engine/data/dataset_schema.hpp"        // DateKey, Role
#include "atx/engine/data/factor_model_artifact.hpp" // FactorModelArtifact
#include "atx/engine/exec/execution_sim.hpp"         // exec::ExecutionSimulator
#include "atx/engine/library/library.hpp"            // library::AlphaCandidate
#include "atx/engine/loop/weight_policy.hpp"         // WeightPolicy
#include "atx/engine/risk/factor_model.hpp" // risk::FactorModel (complete: std::optional member)

namespace atx::engine::data {

// ===========================================================================
//  DataContext — the price/feature/signal/reference/factor data facade.
// ===========================================================================
class DataContext {
public:
  // Build from a catalog + the required price-dataset name. Validates that the
  // price name is registered AND carries Role::Price (Err otherwise).
  //   adv_windows EMPTY    => RAW lowering (Panel::create from the price columns).
  //   adv_windows NON-empty => with_datafields OHLCV augmentation (price_to_panel).
  // The catalog is BORROWED for the context's lifetime (must outlive *this).
  [[nodiscard]] static atx::core::Result<DataContext>
  create(const DatasetCatalog &catalog, std::string price_name,
         std::vector<atx::u16> adv_windows = {});

  // Move-only (owns move-only SignalAdmissions + a cached Panel).
  DataContext(DataContext &&) noexcept = default;
  DataContext &operator=(DataContext &&) noexcept = default;
  DataContext(const DataContext &) = delete;
  DataContext &operator=(const DataContext &) = delete;
  ~DataContext() = default;

  // Attach a BYO factor model (not a Dataset, so not in the catalog).
  void set_factor_model(FactorModelArtifact artifact);

  // Lazy + cached. RAW Panel::create from the price columns (default), then merges
  // every Role::Feature dataset in the catalog (ascending name) as extra fields.
  // A price-only context (no Feature datasets) => exactly the user's panel. Returns a
  // reference_wrapper into a cached member (the vendored tl::expected rejects a reference
  // value type, so we follow the catalog::resolve convention) — the DataContext must
  // outlive the returned reference.
  [[nodiscard]] atx::core::Result<std::reference_wrapper<const alpha::Panel>> price_panel();

  // The BYO factor model lowered (artifact_to_factor_model), or nullopt (=> the
  // pipeline builds the price-derived model). Lazy + cached.
  [[nodiscard]] atx::core::Result<std::optional<risk::FactorModel>> factor_model_override();

  // Builds + OWNS the SignalAdmission(s) for every Role::Signal dataset (ascending
  // name), returns a flat view of all candidates. The candidates' spans point into
  // the OWNED SignalAdmissions held in *this — valid while the DataContext lives.
  [[nodiscard]] atx::core::Result<std::span<const library::AlphaCandidate>>
  signal_admit_candidates(const exec::ExecutionSimulator &sim,
                          const atx::engine::WeightPolicy &policy, atx::usize as_of);

  // Reference spans as-of a date (first Role::Reference dataset, ascending name), or
  // Err(NotFound) if the catalog carries no Role::Reference dataset.
  [[nodiscard]] atx::core::Result<RefSpans> reference_spans_at(DateKey as_of_date,
                                                               atx::u32 default_group = 0U);

private:
  DataContext(const DatasetCatalog &catalog, std::string price_name,
              std::vector<atx::u16> adv_windows) noexcept
      : catalog_{&catalog}, price_name_{std::move(price_name)},
        adv_windows_{std::move(adv_windows)} {}

  // Names of every registered dataset whose role == `role`, ascending (catalog order).
  [[nodiscard]] std::vector<std::string> names_with_role(Role role) const;

  const DatasetCatalog *catalog_; // borrowed; must outlive *this
  std::string price_name_;
  std::vector<atx::u16> adv_windows_; // empty => raw lowering

  std::optional<alpha::Panel> panel_cache_;              // lazily built by price_panel()
  std::optional<FactorModelArtifact> factor_artifact_;   // set via set_factor_model
  std::optional<risk::FactorModel> factor_cache_;        // lazily lowered override
  bool factor_lowered_ = false;                          // factor_cache_ is materialized
  std::vector<SignalAdmission> admissions_;              // owns signal candidate streams
  std::vector<library::AlphaCandidate> flat_candidates_; // flat view over admissions_
  bool signals_built_ = false;
};

} // namespace atx::engine::data
