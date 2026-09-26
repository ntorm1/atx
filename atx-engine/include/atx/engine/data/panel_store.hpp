#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::data {

enum class LevelBasis : atx::u8;
enum class PanelStorePrecision : atx::u8 { Float32V2 = 1, ExactFloat64V2 = 2 };

struct PanelStoreField {
  std::string name;
  LevelBasis basis{};
  PanelStorePrecision precision{PanelStorePrecision::Float32V2};
};
struct PanelStoreParent { std::string role, sha256; };

// ATPSTR2: fixed complete-universe axes, never per-year or per-window compaction.
// Session keys are labels, NOT assertions of market-data publication times.
// Missing numeric cells are canonical quiet NaNs; infinities are rejected.
struct PanelStoreConfig {
  std::vector<atx::i64> session_keys;    // positive, strictly increasing, below seal
  std::vector<atx::i64> instrument_ids;  // positive, strictly increasing numeric IDs
  std::vector<atx::u64> original_indices; // unique source-axis index per output column
  std::vector<PanelStoreField> fields;
  std::string instrument_namespace;
  std::string recipe; // exact bytes; includes source-availability qualifications
  std::string membership_sha256;
  std::vector<PanelStoreParent> parents;
  atx::usize chunk_dates{32};
  atx::i64 sealed_end_exclusive{1'577'836'800'000'000'000LL};
  atx::u64 max_working_bytes{64ULL * 1024 * 1024};
  atx::u64 max_mapped_bytes{128ULL * 1024 * 1024};
  atx::u32 max_open_chunks{8};
};

struct PanelStoreSizing {
  atx::u64 payload_bytes{}, largest_chunk_bytes{}, writer_working_bytes{}, metadata_bound_bytes{};
};
[[nodiscard]] atx::core::Result<PanelStoreSizing> preflight_panel_store(const PanelStoreConfig&);

class PanelStoreWriter {
public:
  // Exclusive fresh directory; failed writes leave evidence, never replace files.
  [[nodiscard]] static atx::core::Result<PanelStoreWriter> create(
      const std::string& directory, const PanelStoreConfig&);
  PanelStoreWriter(PanelStoreWriter&&) noexcept;
  PanelStoreWriter& operator=(PanelStoreWriter&&) noexcept;
  ~PanelStoreWriter();
  PanelStoreWriter(const PanelStoreWriter&) = delete;
  PanelStoreWriter& operator=(const PanelStoreWriter&) = delete;

  // Append each date exactly once in axis order. One N-cell span per field.
  // present = observed source row, independent of tradable = dated membership.
  // A tradable cell must be present; its membership decision must be strictly
  // before session_keys[date]. An all-zero tradable row may use decision_key=0.
  // Values of absent cells are stored missing. No warm-up values are masked by
  // tradability. exact_close stores original f64 adjusted close, NOT a widened
  // f32 value; finite entries must be positive. The writer cannot authenticate
  // caller clocks/source identities and does not repair corporate-action data.
  [[nodiscard]] atx::core::Status append_date(atx::usize date,
      std::span<const std::span<const atx::f64>> fields,
      std::span<const atx::f64> exact_close, std::span<const atx::u8> present,
      std::span<const atx::u8> tradable, atx::i64 membership_decision_key);
  [[nodiscard]] atx::core::Result<std::string> finish(); // manifest.bin SHA; published last
private:
  struct Impl;
  explicit PanelStoreWriter(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};

class PanelStoreChunk {
public:
  [[nodiscard]] atx::usize begin_date() const noexcept;
  [[nodiscard]] atx::usize dates() const noexcept;
  [[nodiscard]] atx::usize instruments() const noexcept;
  // All numeric access widens before consumer arithmetic; no f32 accumulation.
  [[nodiscard]] atx::core::Status read_field_row(atx::usize field, atx::usize local_date,
                                               std::span<atx::f64> output) const;
  [[nodiscard]] atx::core::Status read_exact_close_row(atx::usize local_date,
                                                     std::span<atx::f64> output) const;
  // Returned spans borrow this chunk's shared read-only mapping, not the store.
  [[nodiscard]] atx::core::Result<std::span<const atx::u8>> present(atx::usize local_date) const;
  [[nodiscard]] atx::core::Result<std::span<const atx::u8>> tradable(atx::usize local_date) const;
  [[nodiscard]] atx::core::Result<atx::i64> membership_decision_key(atx::usize local_date) const;
private:
  struct Impl;
  explicit PanelStoreChunk(std::shared_ptr<const Impl>);
  std::shared_ptr<const Impl> impl_;
  friend class PanelStore;
};

class PanelStore {
public:
  // Opens bounded metadata and checks chunk extents without reading the union's
  // numeric cells. Each open_chunk verifies the captured mapping's SHA and every
  // numeric/mask/clock invariant BEFORE exposing it. No unvalidated chunk view.
  [[nodiscard]] static atx::core::Result<PanelStore> open(const std::string& directory,
      std::string_view expected_manifest_sha256 = {},
      atx::u64 max_mapped_bytes = 128ULL * 1024 * 1024, atx::u32 max_open_chunks = 8);
  [[nodiscard]] const PanelStoreConfig& config() const noexcept;
  [[nodiscard]] std::string_view manifest_sha256() const noexcept;
  [[nodiscard]] atx::usize chunks() const noexcept;
  // Live chunks share a checked byte/handle budget even across copied stores.
  // No prefetch; holding a chunk keeps its mapping alive independently of store.
  [[nodiscard]] atx::core::Result<PanelStoreChunk> open_chunk(atx::usize index) const;

  // Exact original-f64 ratio endpoint/entry-1, never f32-price reconstruction.
  // Caller specifies entry/endpoint explicitly; endpoint must be < maturity_end.
  // Missing/nonpositive endpoints yield NaN. No terminal-return imputation or
  // return guard is silently applied. Equal inputs/formula give bit-identical
  // finite results; year-boundary cold-start or alternate guard rules do not.
  [[nodiscard]] atx::core::Status forward_returns(atx::usize entry, atx::usize endpoint,
      atx::usize maturity_end, std::span<atx::f64> output) const;
private:
  struct Impl;
  explicit PanelStore(std::shared_ptr<Impl>);
  std::shared_ptr<Impl> impl_;
};

} // namespace atx::engine::data
