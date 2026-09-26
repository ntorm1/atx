#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::learn {

// Explicit V2 data plane. Legacy raw/drop-invalid FeatureMatrix remains V1;
// this artifact never silently reinterprets its feature or target semantics.
struct PanelDatasetConfig {
  std::vector<atx::i64> session_keys;   // complete, uncompressed source sessions
  std::vector<atx::i64> instrument_ids; // complete sorted positive numeric IDs
  std::vector<std::string> feature_names;
  std::string instrument_namespace;
  std::string source_sha256;
  std::string source_recipe; // includes source/membership vintage qualifications
  std::vector<atx::u16> holding_horizons{21, 63, 126};
  atx::u16 feature_max_lookback{};
  atx::u16 execution_delay{1};
  atx::u16 volatility_window{63};
  atx::u16 volatility_min_observations{20};
  atx::f64 daily_volatility_floor{1e-4};
  atx::usize maturity_end{}; // exclusive source ordinal; 0 resolves to dates()
  atx::usize block_dates{8};
  atx::u64 max_working_bytes{512ULL * 1024 * 1024};
  atx::u64 max_mapped_bytes{256ULL * 1024 * 1024};
  atx::u32 max_open_blocks{4};
};

// The source may hold a small mapped-block cache; it must not retain unbounded
// date history. read_features writes F*N values, feature-major within one date.
// read_close supplies ORIGINAL f64 adjusted close, never f32 reconstructed prices.
// Membership is a decision-time assertion, independent of source-row presence.
// These methods can read future labels only when asked by the dataset builder;
// no future row participates in feature ranks or volatility normalization.
class PanelDatasetSource {
public:
  virtual ~PanelDatasetSource() = default;
  [[nodiscard]] virtual atx::core::Status read_features(atx::usize date,
      std::span<atx::f64> columns, std::span<atx::u8> present,
      std::span<atx::u8> member, atx::i64& membership_decision_key) = 0;
  [[nodiscard]] virtual atx::core::Status read_close(atx::usize date,
      std::span<atx::f64> output) = 0;
};

struct PanelDatasetSizing {
  atx::u64 payload_bytes{}, largest_block_bytes{}, working_bytes{}, metadata_bytes{};
};
[[nodiscard]] atx::core::Result<PanelDatasetSizing>
preflight_panel_dataset(const PanelDatasetConfig&);

struct PanelDatasetBuildResult {
  std::string manifest_sha256;
  atx::u64 member_rows{};
  std::vector<atx::u64> finite_labels;
};

// Exclusive fresh directory, immutable blocks and publish-last manifest.bin.
// Features: date-local average-tie ranks/(finite_count-1)-.5; singleton -> 0;
// nonfinite/missing source -> 0 plus a distinct indicator column. Rank cohorts
// use decision membership only. No member row is dropped for feature holes.
// Labels: endpoint/entry-1 divided by max(prior daily sd,floor)*sqrt(H), then
// demeaned within date over finite member labels. Prior sd uses returns ending
// STRICTLY before the feature date. Insufficient history/labels remain NaN.
// Labels are stored f64; features/indicators are column-major f32 per date block.
// No exposure residualization is claimed before the separate I2 contract exists.
[[nodiscard]] atx::core::Result<PanelDatasetBuildResult> build_panel_dataset(
    PanelDatasetSource&, const PanelDatasetConfig&, const std::string& directory);

class PanelDatasetBlock {
public:
  [[nodiscard]] atx::usize begin_date() const noexcept;
  [[nodiscard]] atx::usize dates() const noexcept;
  [[nodiscard]] atx::usize instruments() const noexcept;
  // Each column has dates()*instruments() cells, date-major within the column.
  // Features 0..F-1 are ranks; F..2F-1 are missing indicators. Spans borrow the
  // shared immutable block, whose lifetime is independent of the reader object.
  [[nodiscard]] atx::core::Result<std::span<const atx::f32>> feature(atx::usize column) const;
  [[nodiscard]] atx::core::Result<std::span<const atx::f64>> label(atx::usize horizon) const;
  [[nodiscard]] std::span<const atx::u8> present() const noexcept;
  [[nodiscard]] std::span<const atx::u8> member() const noexcept;
private:
  struct Impl;
  explicit PanelDatasetBlock(std::shared_ptr<const Impl>);
  std::shared_ptr<const Impl> impl_;
  friend class PanelDataset;
};

class PanelDataset {
public:
  [[nodiscard]] static atx::core::Result<PanelDataset> open(const std::string& directory,
      std::string_view expected_manifest_sha256 = {},
      atx::u64 max_mapped_bytes = 256ULL * 1024 * 1024, atx::u32 max_open_blocks = 4);
  [[nodiscard]] const PanelDatasetConfig& config() const noexcept;
  [[nodiscard]] std::string_view manifest_sha256() const noexcept;
  [[nodiscard]] atx::usize blocks() const noexcept;
  [[nodiscard]] atx::core::Result<PanelDatasetBlock> open_block(atx::usize index) const;
private:
  struct Impl;
  explicit PanelDataset(std::shared_ptr<Impl>);
  std::shared_ptr<Impl> impl_;
};

} // namespace atx::engine::learn
