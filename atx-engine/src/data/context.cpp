#include "atx/engine/data/context.hpp"

#include <span>    // std::span
#include <string>  // std::string
#include <utility> // std::move
#include <vector>  // std::vector

#include "atx/engine/data/adapt_factor.hpp"  // artifact_to_factor_model, reference_spans, RefSpans
#include "atx/engine/data/adapt_feature.hpp" // merge_features_into_panel
#include "atx/engine/data/adapt_panel.hpp"   // price_to_panel (with_datafields augmentation)
#include "atx/engine/data/adapt_signal.hpp"  // signal_to_candidates, SignalAdmission
#include "atx/engine/data/dataset.hpp"       // Dataset

namespace atx::engine::data {

// ===========================================================================
//  create — validate the price dataset is registered AND Role::Price.
// ===========================================================================
atx::core::Result<DataContext> DataContext::create(const DatasetCatalog &catalog,
                                                   std::string price_name,
                                                   std::vector<atx::u16> adv_windows) {
  ATX_TRY(const auto role, catalog.role_of(price_name));
  if (role != Role::Price) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "DataContext::create: dataset '" + price_name + "' is not Role::Price");
  }
  return atx::core::Ok(DataContext{catalog, std::move(price_name), std::move(adv_windows)});
}

// ===========================================================================
//  set_factor_model — attach a BYO artifact; invalidate any lowered cache.
// ===========================================================================
void DataContext::set_factor_model(FactorModelArtifact artifact) {
  factor_artifact_ = std::move(artifact);
  factor_cache_.reset();
  factor_lowered_ = false;
}

// ===========================================================================
//  names_with_role — registered datasets of a given role, ascending (catalog order).
// ===========================================================================
std::vector<std::string> DataContext::names_with_role(Role role) const {
  std::vector<std::string> out;
  for (const std::string &name : catalog_->names()) {
    const auto r = catalog_->role_of(name);
    if (r.has_value() && *r == role) {
      out.push_back(name);
    }
  }
  return out;
}

// ===========================================================================
//  price_panel — RAW lowering (default) + Feature merge. Lazy + cached.
// ===========================================================================
atx::core::Result<std::reference_wrapper<const alpha::Panel>> DataContext::price_panel() {
  if (panel_cache_.has_value()) {
    return atx::core::Ok(std::cref(*panel_cache_));
  }
  ATX_TRY(const auto price_ref, catalog_->resolve(price_name_));
  const Dataset &price = price_ref.get();

  // 1. Lower the price Dataset into a base Panel (Panel has no public default ctor, so
  //    stage it in an std::optional and assign from the chosen lowering).
  //    adv_windows EMPTY  => RAW Panel::create from the price columns + mask verbatim
  //                          (reproduces ANY user panel byte-identically — the boundary pin).
  //    adv_windows non-empty => with_datafields OHLCV augmentation (price_to_panel).
  std::optional<alpha::Panel> base;
  if (adv_windows_.empty()) {
    std::vector<std::string> names{price.schema().columns};
    std::vector<std::vector<atx::f64>> cols;
    cols.reserve(price.schema().columns.size());
    for (atx::usize c = 0; c < price.schema().columns.size(); ++c) {
      const std::span<const atx::f64> col = price.column(c);
      cols.emplace_back(col.begin(), col.end());
    }
    const std::span<const std::uint8_t> m = price.mask();
    std::vector<std::uint8_t> mask(m.begin(), m.end());
    ATX_TRY(alpha::Panel p,
            alpha::Panel::create(price.num_dates(), price.num_instruments(), std::move(names),
                                 std::move(cols), std::move(mask)));
    base = std::move(p);
  } else {
    ATX_TRY(alpha::Panel p, price_to_panel(price, std::span<const atx::u16>{adv_windows_}));
    base = std::move(p);
  }

  // 2. Merge every Role::Feature dataset (ascending name) as extra named fields. A
  //    price-only context (no Feature datasets) leaves `base` untouched.
  for (const std::string &fname : names_with_role(Role::Feature)) {
    ATX_TRY(const auto feat_ref, catalog_->resolve(fname));
    ATX_TRY(alpha::Panel merged, merge_features_into_panel(*base, price, feat_ref.get()));
    base = std::move(merged);
  }

  panel_cache_ = std::move(base);
  return atx::core::Ok(std::cref(*panel_cache_));
}

// ===========================================================================
//  factor_model_override — lower the BYO artifact, or nullopt. Lazy + cached.
// ===========================================================================
atx::core::Result<std::optional<risk::FactorModel>> DataContext::factor_model_override() {
  if (!factor_artifact_.has_value()) {
    return atx::core::Ok(std::optional<risk::FactorModel>{});
  }
  if (!factor_lowered_) {
    ATX_TRY(risk::FactorModel fm, artifact_to_factor_model(*factor_artifact_));
    factor_cache_ = std::move(fm);
    factor_lowered_ = true;
  }
  return atx::core::Ok(std::optional<risk::FactorModel>{factor_cache_});
}

// ===========================================================================
//  signal_admit_candidates — OWN one SignalAdmission per Role::Signal dataset
//  (ascending name); return a flat view over all candidates. Lazy + cached.
// ===========================================================================
atx::core::Result<std::span<const library::AlphaCandidate>>
DataContext::signal_admit_candidates(const exec::ExecutionSimulator &sim,
                                     const atx::engine::WeightPolicy &policy, atx::usize as_of) {
  if (signals_built_) {
    return atx::core::Ok(std::span<const library::AlphaCandidate>{flat_candidates_});
  }
  ATX_TRY(const auto panel_ref, price_panel());
  const alpha::Panel &panel = panel_ref.get();
  ATX_TRY(const auto price_ref, catalog_->resolve(price_name_));
  const Dataset &price = price_ref.get();

  admissions_.clear();
  flat_candidates_.clear();
  for (const std::string &sname : names_with_role(Role::Signal)) {
    ATX_TRY(const auto sig_ref, catalog_->resolve(sname));
    ATX_TRY(SignalAdmission adm,
            signal_to_candidates(sig_ref.get(), price, panel, sim, policy, as_of));
    admissions_.push_back(std::move(adm));
  }
  // The owned admissions' addresses are stable (vector move preserves buffers); their
  // candidate spans point into each admission's AlphaStreams. Collect a flat copy of the
  // candidate descriptors (the spans still alias the OWNED admissions_, not a copy).
  for (const SignalAdmission &adm : admissions_) {
    for (const library::AlphaCandidate &c : adm.candidates) {
      flat_candidates_.push_back(c);
    }
  }
  signals_built_ = true;
  return atx::core::Ok(std::span<const library::AlphaCandidate>{flat_candidates_});
}

// ===========================================================================
//  reference_spans_at — first Role::Reference dataset, as-of a date.
// ===========================================================================
atx::core::Result<RefSpans> DataContext::reference_spans_at(DateKey as_of_date,
                                                            atx::u32 default_group) {
  const std::vector<std::string> refs = names_with_role(Role::Reference);
  if (refs.empty()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "DataContext::reference_spans_at: no Role::Reference dataset registered");
  }
  ATX_TRY(const auto ref_ref, catalog_->resolve(refs.front()));
  ATX_TRY(const auto price_ref, catalog_->resolve(price_name_));
  return reference_spans(ref_ref.get(), price_ref.get(), as_of_date, default_group);
}

} // namespace atx::engine::data
