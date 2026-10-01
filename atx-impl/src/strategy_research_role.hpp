#pragma once
// atx::impl::strategy — a pinned research role and its pinned research fields as one DSL panel
// (platform v8 H-3). The loader the research verbs share: the role manifest by its SHA-256
// (ic_detail::pinned_json), the fields manifest bound to the role exactly as the IC runner binds
// it (ic_detail::bind_fields: schema, role binding, point-in-time flags, extents), the role
// payload (engine read_strategy_role), each requested field by its manifest SHA-256
// (ic_detail::load_pinned_f64), then the engine's role adapters (data/role_panel.hpp): the DSL
// overlay panel and the research IC return guard.
//
// Research window (research_window.hpp): a manifest whose score end lies after the seal is
// refused from its metadata, before any payload is opened; every session of a loaded role is
// checked against the seal again.
//
// Ruling E-10 (review B-3; review MINE-8): a role built with --delisting-returns (manifest
// universe.delisting.returns_applied true) is refused from its metadata by geometry() and
// load(), before any payload (engine refuse_delisting_returns_signal_role): the research verbs
// take signals and IC labels from the same role, and its imputed terminal returns are
// classified after their session.
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/strategy_data.hpp"

namespace atx::impl::strategy {

// What to load. The manifest pin is required; the fields pin is required with a directory.
// `fields` names the DSL fields the caller evaluates: the role's base fields (close, raw_close,
// volume) are always on the panel, every other name must be a point-in-time row of the pinned
// fields manifest. `max_bytes` bounds the whole load (research_role_bytes).
struct ResearchRoleSpec {
  std::string manifest;
  std::string manifest_sha256;
  std::string fields_directory;
  std::string fields_sha256;
  std::vector<std::string> fields;
  atx::u64 max_bytes{1ULL << 30};
};

// One loaded extra field: its name and the payload SHA-256 the fields manifest pins.
struct ResearchFieldReceipt {
  std::string name;
  std::string sha256;
};

// Conservative bytes of a loaded role: the role reader's own admission (16 MiB, 26 B per cell,
// the axes), 8 B per cell per extra field, the 4 B per cell return guard and the 1 B per cell
// overlay mask. Err on a geometry the role reader refuses anyway.
[[nodiscard]] atx::core::Result<atx::u64> research_role_bytes(atx::usize dates, atx::usize names,
                                                              atx::usize extras);

class ResearchRole {
public:
  struct Geometry {
    atx::usize dates{};
    atx::usize instruments{};
  };
  // The pinned manifest's axes, read from its metadata only (the seal and delisting-returns
  // refusals apply).
  [[nodiscard]] static atx::core::Result<Geometry> geometry(const ResearchRoleSpec &spec);
  // Loads the role; refuses before any payload a pin mismatch, a score end after the seal, a
  // --delisting-returns role, an unknown or non-point-in-time field and a load above
  // spec.max_bytes.
  [[nodiscard]] static atx::core::Result<std::unique_ptr<ResearchRole>>
  load(const ResearchRoleSpec &spec);

  // Not copyable or movable: the panel borrows the role's and the fields' columns.
  ResearchRole(const ResearchRole &) = delete;
  ResearchRole &operator=(const ResearchRole &) = delete;
  ResearchRole(ResearchRole &&) = delete;
  ResearchRole &operator=(ResearchRole &&) = delete;
  ~ResearchRole() = default;

  [[nodiscard]] const engine::data::StrategyRoleData &data() const noexcept { return data_; }
  // The DSL panel: the role's base fields and every loaded extra, the role's presence mask.
  [[nodiscard]] const engine::alpha::Panel &panel() const noexcept { return *panel_; }
  // The decision membership (panel cells, {0, 1}) and the research IC return guard (cells).
  [[nodiscard]] std::span<const atx::u8> member() const noexcept { return data_.decision_member; }
  [[nodiscard]] std::span<const atx::u32> guard() const noexcept { return guard_; }
  [[nodiscard]] const std::vector<ResearchFieldReceipt> &extras() const noexcept {
    return extras_;
  }
  // The fields manifest pin (empty: no fields directory).
  [[nodiscard]] const std::string &fields_sha256() const noexcept { return fields_sha256_; }

private:
  explicit ResearchRole(engine::data::StrategyRoleData data);

  engine::data::StrategyRoleData data_;
  std::vector<std::vector<atx::f64>> columns_; // extra fields, in extras_ order
  std::vector<ResearchFieldReceipt> extras_;
  std::optional<engine::alpha::Panel> panel_;
  std::vector<atx::u32> guard_;
  std::string fields_sha256_;
};

} // namespace atx::impl::strategy
