#include "atx/engine/learn/feature_matrix.hpp"

#include <cmath>  // std::isfinite
#include <algorithm>
#include <limits>
#include <span>   // std::span
#include <utility> // std::move
#include <vector> // std::vector

#include "atx/core/error.hpp" // Result, Ok, ATX_TRY
#include "atx/core/macro.hpp" // ATX_CHECK
#include "atx/core/types.hpp" // f64, u8, usize

#include "atx/engine/alpha/panel.hpp"   // alpha::Panel, FieldId
#include "atx/engine/combine/store.hpp" // combine::AlphaStore, combine::AlphaId
#include "atx/engine/data/panel_store.hpp"

namespace atx::engine::learn {

namespace detail {

atx::core::Result<std::vector<alpha::FieldId>>
resolve_raw_fields(const alpha::Panel &panel, const FeatureSpec &spec) {
  std::vector<alpha::FieldId> ids;
  ids.reserve(spec.raw_fields.size());
  for (const std::string &name : spec.raw_fields) {
    ATX_TRY(const alpha::FieldId fid, panel.field_id(name));
    ids.push_back(fid);
  }
  return atx::core::Ok(std::move(ids));
}

atx::f64 forward_return(std::span<const atx::f64> close_all,
                                             atx::usize n_dates, atx::usize n_instruments,
                                             atx::usize date, atx::usize inst, atx::u16 horizon) {
  const atx::usize ahead = date + static_cast<atx::usize>(horizon);
  if (ahead >= n_dates) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  const atx::f64 now = close_all[date * n_instruments + inst];
  const atx::f64 fut = close_all[ahead * n_instruments + inst];
  return fut / now - 1.0;
}

// Write the feature row for cell (date, inst) into X[row*n_features ..]: the raw
// fields (in id order) then the pool alphas (in id order). Returns true iff every
// written feature is finite (the row_valid flag). SAFETY: `raw_cs[f]` aliases the
// Panel's date-major column for date `date`; it is valid for the life of `panel`
// and is read at `inst` < instruments (the caller bounds the loop). Pool-alpha
// values come from store.positions(id, date)[inst]; `date` < n_periods and
// `inst` < n_instruments are bounded by the caller, so the deref is in range.
bool
write_feature_row(std::span<atx::f64> X, atx::usize row, atx::usize n_features,
                  const std::vector<std::span<const atx::f64>> &raw_cs,
                  const combine::AlphaStore &store, const FeatureSpec &spec, atx::usize date,
                  atx::usize inst) {
  const atx::usize base = row * n_features;
  ATX_CHECK(base + n_features <= X.size());
  atx::usize f = 0;
  bool all_finite = true;
  for (const std::span<const atx::f64> &cs : raw_cs) {
    const atx::f64 v = cs[inst];
    X[base + f] = v;
    all_finite = all_finite && std::isfinite(v);
    ++f;
  }
  for (const combine::AlphaId id : spec.pool_alphas) {
    // The stored stream is already PIT-aligned: its period axis IS the date axis,
    // so positions(id, date) is the cross-section knowable at date `date`.
    const std::span<const atx::f64> cs = store.positions(id, date);
    const atx::f64 v = cs[inst];
    X[base + f] = v;
    all_finite = all_finite && std::isfinite(v);
    ++f;
  }
  return all_finite;
}

} // namespace detail

atx::core::Result<FeatureMatrix>
build_features(const alpha::Panel &panel, const combine::AlphaStore &store,
               const FeatureSpec &spec) {
  ATX_TRY(const std::vector<alpha::FieldId> raw_ids, detail::resolve_raw_fields(panel, spec));
  ATX_TRY(const alpha::FieldId close_id, panel.field_id("close"));

  FeatureMatrix fm;
  fm.n_dates = panel.dates();
  fm.n_instruments = panel.instruments();
  fm.n_features = spec.raw_fields.size() + spec.pool_alphas.size();
  fm.Y.assign(spec.horizons.size(), {});
  fm.label_horizons = spec.horizons;

  const std::span<const atx::f64> close_all = panel.field_all(close_id);

  // Emit in (date, instrument) order — a single nested loop, no map iteration.
  for (atx::usize d = 0; d < fm.n_dates; ++d) {
    // The raw cross-sections for date d alias the Panel's columns (PIT: date d).
    std::vector<std::span<const atx::f64>> raw_cs;
    raw_cs.reserve(raw_ids.size());
    for (const alpha::FieldId fid : raw_ids) {
      raw_cs.push_back(panel.field_cross_section(fid, d));
    }
    for (atx::usize i = 0; i < fm.n_instruments; ++i) {
      if (!panel.in_universe(d, i)) {
        continue; // out-of-universe: NO row (M8 — not zero-filled)
      }
      const atx::usize row = fm.push_row(d, i);
      fm.X.resize((row + 1) * fm.n_features);
      const bool valid = detail::write_feature_row(std::span<atx::f64>{fm.X}, row, fm.n_features,
                                                   raw_cs, store, spec, d, i);
      fm.row_valid.push_back(static_cast<atx::u8>(valid ? 1 : 0));
      for (atx::usize h = 0; h < spec.horizons.size(); ++h) {
        fm.Y[h].push_back(detail::forward_return(close_all, fm.n_dates, fm.n_instruments, d, i,
                                                 spec.horizons[h]));
      }
    }
  }
  return atx::core::Ok(std::move(fm));
}

namespace {
using core::Err; using core::ErrorCode; using core::Ok; using core::Result; using core::Status;
class PanelSource final : public PanelDatasetSource {
public:
  PanelSource(const alpha::Panel& panel, const combine::AlphaStore& store, const FeatureSpec& spec,
      std::vector<alpha::FieldId> fields, alpha::FieldId close, std::span<const u8> member, std::span<const i64> clocks)
      : panel_(panel), store_(store), spec_(spec), fields_(std::move(fields)), close_(close), member_(member), clocks_(clocks) {}
  Status read_features(usize date, std::span<f64> out, std::span<u8> present, std::span<u8> member, i64& clock) override {
    const auto n = panel_.instruments(); usize f = 0;
    for (const auto field : fields_) { const auto row = panel_.field_cross_section(field, date); std::copy(row.begin(), row.end(), out.begin() + static_cast<std::ptrdiff_t>(f++ * n)); }
    for (const auto id : spec_.pool_alphas) { const auto row = store_.positions(id, date); std::copy(row.begin(), row.end(), out.begin() + static_cast<std::ptrdiff_t>(f++ * n)); }
    for (usize i = 0; i < n; ++i) { present[i] = panel_.in_universe(date, i) ? 1 : 0; member[i] = member_[date * n + i]; }
    clock = clocks_[date]; return Ok();
  }
  Status read_close(usize date, std::span<f64> out) override {
    const auto row = panel_.field_cross_section(close_, date);
    for (usize i = 0; i < out.size(); ++i)
      out[i] = panel_.in_universe(date, i) ? row[i] : std::numeric_limits<f64>::quiet_NaN();
    return Ok();
  }
private:
  const alpha::Panel& panel_; const combine::AlphaStore& store_; const FeatureSpec& spec_;
  std::vector<alpha::FieldId> fields_; alpha::FieldId close_;
  std::span<const u8> member_; std::span<const i64> clocks_;
};
class StoreSource final : public PanelDatasetSource {
public:
  StoreSource(const data::PanelStore& store, std::vector<usize> fields) : store_(store), fields_(std::move(fields)) {}
  Status read_features(usize date, std::span<f64> out, std::span<u8> present, std::span<u8> member, i64& clock) override {
    ATX_TRY(auto block, get(date)); const auto local = date - block.begin_date(), n = block.instruments();
    for (usize f = 0; f < fields_.size(); ++f) {
      if (store_.config().fields[fields_[f]].name == "close") { ATX_TRY_VOID(block.read_exact_close_row(local, out.subspan(f * n, n))); }
      else { ATX_TRY_VOID(block.read_field_row(fields_[f], local, out.subspan(f * n, n))); }
    }
    ATX_TRY(auto p, block.present(local)); ATX_TRY(auto m, block.tradable(local));
    std::copy(p.begin(), p.end(), present.begin()); std::copy(m.begin(), m.end(), member.begin());
    ATX_TRY(clock, block.membership_decision_key(local)); return Ok();
  }
  Status read_close(usize date, std::span<f64> out) override {
    ATX_TRY(auto block, get(date)); return block.read_exact_close_row(date - block.begin_date(), out);
  }
private:
  Result<data::PanelStoreChunk> get(usize date) {
    const auto index = date / store_.config().chunk_dates;
    for (auto it = cache_.begin(); it != cache_.end(); ++it) if (it->first == index) {
      auto value = *it; cache_.erase(it); cache_.push_back(value); return Ok(value.second);
    }
    // At most four shared read-only source chunks. Actual D6 byte/handle limits
    // remain enforced and can refuse an inadequate caller-selected budget.
    if (cache_.size() == 4) cache_.erase(cache_.begin());
    ATX_TRY(auto block, store_.open_chunk(index)); cache_.emplace_back(index, block); return Ok(std::move(block));
  }
  const data::PanelStore& store_; std::vector<usize> fields_;
  std::vector<std::pair<usize, data::PanelStoreChunk>> cache_;
};
} // namespace

Result<PanelDatasetBuildResult> build_panel_dataset_from_panel(const alpha::Panel& panel,
    const combine::AlphaStore& store, const FeatureSpec& spec, const PanelDatasetConfig& cfg,
    std::span<const u8> member, std::span<const i64> clocks, const std::string& directory) {
  ATX_TRY(auto sizing, preflight_panel_dataset(cfg)); (void)sizing;
  if (panel.dates() != cfg.session_keys.size() || panel.instruments() != cfg.instrument_ids.size() ||
      member.size() != panel.dates() * panel.instruments() || clocks.size() != panel.dates() ||
      spec.horizons != cfg.holding_horizons || spec.max_lookback != cfg.feature_max_lookback)
    return Err(ErrorCode::InvalidArgument, "dataset: panel/member/clock/horizon shape mismatch");
  std::vector<std::string> names = spec.raw_fields;
  for (auto id : spec.pool_alphas) names.push_back("alpha:" + std::to_string(id.value));
  if (names != cfg.feature_names) return Err(ErrorCode::InvalidArgument, "dataset: feature order/identity mismatch");
  if (!spec.pool_alphas.empty() && (store.n_periods() != panel.dates() || store.n_instruments() != panel.instruments()))
    return Err(ErrorCode::InvalidArgument, "dataset: pool feature axes mismatch");
  for (auto id : spec.pool_alphas) if (id.value >= store.n_alphas()) return Err(ErrorCode::InvalidArgument, "dataset: missing pool feature");
  ATX_TRY(auto fields, detail::resolve_raw_fields(panel, spec)); ATX_TRY(auto close, panel.field_id("close"));
  PanelSource source(panel, store, spec, std::move(fields), close, member, clocks);
  return build_panel_dataset(source, cfg, directory);
}

Result<PanelDatasetBuildResult> build_panel_dataset_from_store(const data::PanelStore& store,
    const PanelDatasetConfig& cfg, const std::string& directory) {
  ATX_TRY(auto sizing, preflight_panel_dataset(cfg)); (void)sizing;
  if (cfg.session_keys != store.config().session_keys || cfg.instrument_ids != store.config().instrument_ids ||
      cfg.instrument_namespace != store.config().instrument_namespace || cfg.source_sha256 != store.manifest_sha256())
    return Err(ErrorCode::InvalidArgument, "dataset: panel-store source identity/axes mismatch");
  std::vector<usize> fields;
  for (const auto& name : cfg.feature_names) {
    const auto& available = store.config().fields;
    const auto found = std::find_if(available.begin(), available.end(), [&](const auto& f) { return f.name == name; });
    if (found == available.end()) return Err(ErrorCode::NotFound, "dataset: raw source field absent");
    fields.push_back(static_cast<usize>(found - available.begin()));
  }
  // Account our four cached chunks and source metadata, regardless of the
  // reader's externally configured byte limit. Other caller-owned mappings are
  // not owned by this synchronous adapter; the shared store also bounds them.
  ATX_TRY(auto source_size, data::preflight_panel_store(store.config()));
  const auto source_bytes = source_size.largest_chunk_bytes * 4 + source_size.metadata_bound_bytes * 4;
  if (source_bytes > cfg.max_working_bytes || sizing.working_bytes > cfg.max_working_bytes - source_bytes)
    return Err(ErrorCode::InvalidArgument, "dataset: combined source mapping and build budget exceeded");
  StoreSource source(store, std::move(fields)); return build_panel_dataset(source, cfg, directory);
}

Result<FeatureMatrix> read_dataset_features(const PanelDataset& dataset, usize begin, usize end, usize asof, u64 budget) {
  const auto& c = dataset.config(); const auto n = c.instrument_ids.size(), features = c.feature_names.size() * 2;
  if (begin >= end || end > c.session_keys.size() || asof >= c.session_keys.size() || end - 1 > asof)
    return Err(ErrorCode::InvalidArgument, "dataset: explicit bounded window/asof required");
  const u64 rows = u64{end - begin} * n;
  const u64 per_row = u64{features + c.holding_horizons.size()} * 8 + 192; // row vectors + unordered lookup/bucket slack
  constexpr u64 fixed = 32ULL * 1024 * 1024;
  if (budget < fixed || c.max_mapped_bytes > budget - fixed || rows > (budget - fixed - c.max_mapped_bytes) / per_row)
    return Err(ErrorCode::InvalidArgument, "dataset: legacy materialization working budget exceeded");
  FeatureMatrix fm; fm.n_dates = c.session_keys.size(); fm.n_instruments = n; fm.n_features = features;
  fm.label_horizons.reserve(c.holding_horizons.size());
  for (auto h : c.holding_horizons) fm.label_horizons.push_back(static_cast<u16>(h + c.execution_delay));
  fm.Y.resize(c.holding_horizons.size()); fm.dataset_manifest_sha256 = dataset.manifest_sha256();
  fm.dataset_recipe = "rank-residual-v2;features-f32-widened;labels-f64;holding-plus-delay-maturity;inclusive-asof=" + std::to_string(asof) +
      ";begin=" + std::to_string(begin) + ";end=" + std::to_string(end);
  const auto bound = static_cast<usize>(rows);
  fm.X.reserve(bound * features); fm.row_date.reserve(bound); fm.row_inst.reserve(bound);
  fm.row_valid.reserve(bound); fm.row_present.reserve(bound); for (auto& labels : fm.Y) labels.reserve(bound);
  for (usize block_index = begin / c.block_dates; block_index <= (end - 1) / c.block_dates; ++block_index) {
    ATX_TRY(auto block, dataset.open_block(block_index));
    std::vector<std::span<const f32>> x; x.reserve(features);
    for (usize f = 0; f < features; ++f) { ATX_TRY(auto column, block.feature(f)); x.push_back(column); }
    std::vector<std::span<const f64>> y; y.reserve(fm.Y.size());
    for (usize h = 0; h < fm.Y.size(); ++h) { ATX_TRY(auto column, block.label(h)); y.push_back(column); }
    const auto member = block.member(), present = block.present();
    for (usize date = std::max(begin, block.begin_date()); date < std::min(end, block.begin_date() + block.dates()); ++date)
      for (usize i = 0; i < n; ++i) {
        const auto cell = (date - block.begin_date()) * n + i; if (!member[cell]) continue;
        fm.push_row(date, i); fm.row_valid.push_back(1); fm.row_present.push_back(present[cell]);
        for (usize f = 0; f < features; ++f) fm.X.push_back(static_cast<f64>(x[f][cell]));
        for (usize h = 0; h < fm.Y.size(); ++h) fm.Y[h].push_back(label_matured(date, fm.label_horizons[h], asof, 0) ? y[h][cell] : std::numeric_limits<f64>::quiet_NaN());
      }
  }
  return Ok(std::move(fm));
}

} // namespace atx::engine::learn
