#include "atx/engine/learn/learned_source.hpp"

#include <string>  // std::string (raw field name resolution)
#include <utility> // std::move

#include "atx/core/macro.hpp" // ATX_CHECK

namespace atx::engine::learn {

LearnedSignalSource::LearnedSignalSource(LearnedModel model, FeatureSpec spec,
                                         atx::usize universe_size)
    : model_{std::move(model)}, spec_{std::move(spec)}, universe_size_{universe_size} {
  ATX_CHECK(spec_.pool_alphas.empty());
  ATX_CHECK(static_cast<atx::usize>(model_.n_base_features) == spec_.raw_fields.size());
  // Resolve each raw field name to its PanelField slot ONCE (cold path), so
  // evaluate() never touches a string. An unknown name aborts here (a fixture
  // bug), not per-evaluate.
  field_slots_.reserve(spec_.raw_fields.size());
  for (const std::string &name : spec_.raw_fields) {
    field_slots_.push_back(panel_field_of(name));
  }
  // Pre-size all evaluate() scratch (M7: zero per-date allocation).
  signal_.assign(universe_size_, kNoOpinion);
  base_.assign(static_cast<atx::usize>(model_.n_base_features), 0.0);
  augmented_.assign(model_.augmented_dim(), 0.0);
  const atx::usize k =
      model_.aug.pca.has_value() ? static_cast<atx::usize>(model_.aug.pca->k) : 0U;
  latent_.assign(k, 0.0);
}

} // namespace atx::engine::learn
