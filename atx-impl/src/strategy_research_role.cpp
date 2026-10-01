#include "strategy_research_role.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/engine/data/research_window.hpp"
#include "atx/engine/data/role_panel.hpp"
#include "atx/engine/data/strategy_data.hpp"
#include "strategy_ic_detail.hpp"

namespace atx::impl::strategy {
namespace {
namespace co = atx::core;
namespace dt = atx::engine::data;
namespace icd = ic_detail;
using Json = nlohmann::json;

// The role reader's own admission terms (strategy_data.cpp): overhead, bytes per cell, per date
// and per instrument.
constexpr u64 kRoleOverhead = 16ULL << 20;
constexpr u64 kRoleCellBytes = 26U;
// The role's name in the shared field-binding refusals ("--role-fields").
constexpr const char *kRoleName = "role";

co::Error fail(co::ErrorCode code, const std::string &message) {
  return co::Error{code, "research role: " + message};
}

std::string seal_refusal(const std::string &what) {
  return what + " at or after the research seal " + std::string(dt::kSealBeginDate) + " (" +
         std::string(dt::kResearchWindowId) + ")";
}

struct Pinned {
  Json metadata;
  ResearchRole::Geometry geometry;
};

// The pinned manifest. A score end after the seal is refused here, from metadata alone, so
// nothing a sealed role holds is ever opened. So is a --delisting-returns role (Ruling E-10,
// review B-3; review MINE-8): its imputed terminal returns are classified after their session,
// so it may mark NAV books only and never feeds a research verb's signals or IC labels.
co::Result<Pinned> pinned_manifest(const ResearchRoleSpec &spec) {
  ATX_TRY(const std::string text, icd::pinned_text(spec.manifest, spec.manifest_sha256));
  ATX_TRY_VOID(dt::refuse_delisting_returns_signal_role(text, spec.manifest));
  Json j = Json::parse(text);
  if (!j.is_object() || !j.contains("score_end_ns") || !j.at("score_end_ns").is_number_integer() ||
      !j.contains("dates") || !j.at("dates").is_number_unsigned() || !j.contains("instruments") ||
      !j.at("instruments").is_number_unsigned())
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "manifest lacks score_end_ns, dates or instruments: " + spec.manifest));
  if (j.at("score_end_ns").get<i64>() > dt::kSealBeginNs)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        seal_refusal("score end") + ": " + spec.manifest));
  Pinned out;
  out.geometry.dates = static_cast<usize>(j.at("dates").get<u64>());
  out.geometry.instruments = static_cast<usize>(j.at("instruments").get<u64>());
  out.metadata = std::move(j);
  return co::Ok(std::move(out));
}

// The extra fields to load: every requested name that is not a role base field, sorted, unique.
co::Result<std::vector<std::string>> extra_fields(const ResearchRoleSpec &spec) {
  std::vector<std::string> out;
  for (const auto &name : spec.fields) {
    if (!icd::field_identifier(name))
      return co::Err(fail(co::ErrorCode::InvalidArgument, "field name: " + name));
    if (!icd::base_field(name)) out.push_back(name);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  if (out.size() > icd::max_extra_fields)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "at most " +
                        std::to_string(icd::max_extra_fields) + " extra fields"));
  return co::Ok(std::move(out));
}
} // namespace

co::Result<u64> research_role_bytes(usize dates, usize names, usize extras) {
  if (dates == 0U || dates > 4096U || names == 0U || names > 20000U || extras > 256U)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "working-bytes geometry"));
  // Every factor is bounded above, so no product below can overflow u64.
  const u64 cells = static_cast<u64>(dates) * names;
  u64 total = kRoleOverhead + cells * kRoleCellBytes + static_cast<u64>(dates) * 24U +
              static_cast<u64>(names) * 8U;
  total += cells * sizeof(f64) * extras; // extra field columns
  total += cells * sizeof(u32);          // return guard
  total += cells;                        // overlay presence mask
  return co::Ok(total);
}

ResearchRole::ResearchRole(dt::StrategyRoleData data) : data_{std::move(data)} {}

co::Result<ResearchRole::Geometry> ResearchRole::geometry(const ResearchRoleSpec &spec) {
  try {
    ATX_TRY(const auto pinned, pinned_manifest(spec));
    return co::Ok(pinned.geometry);
  } catch (const std::exception &e) {
    return co::Err(fail(co::ErrorCode::InvalidArgument, e.what()));
  }
}

co::Result<std::unique_ptr<ResearchRole>> ResearchRole::load(const ResearchRoleSpec &spec) {
  try {
    ATX_TRY(auto pinned, pinned_manifest(spec));
    ATX_TRY(const auto extras, extra_fields(spec));
    const usize d = pinned.geometry.dates;
    const usize n = pinned.geometry.instruments;
    ATX_TRY(const u64 required, research_role_bytes(d, n, extras.size()));
    if (required > spec.max_bytes)
      return co::Err(fail(co::ErrorCode::Unavailable, "required_bytes=" + std::to_string(required) +
                          " exceeds the budget before any payload load"));
    // The fields manifest bound as the IC runner binds it; `load` keeps the requested order.
    icd::Role role{spec.manifest, spec.manifest_sha256, kRoleName, std::move(pinned.metadata),
                   0U, {}};
    icd::Library lib;
    lib.declared_extra = extras;
    lib.extra_fields = extras;
    ATX_TRY_VOID(icd::bind_fields(lib, role, spec.fields_directory, spec.fields_sha256, true));
    const u64 cells = static_cast<u64>(d) * n;
    const u64 role_budget = kRoleOverhead + cells * kRoleCellBytes + static_cast<u64>(d) * 24U +
                            static_cast<u64>(n) * 8U;
    ATX_TRY(auto data, dt::read_strategy_role(spec.manifest, role_budget));
    if (data.manifest_sha256 != spec.manifest_sha256)
      return co::Err(fail(co::ErrorCode::InvalidArgument,
                          "manifest changed after it was pinned: " + spec.manifest));
    if (std::any_of(data.session_keys.begin(), data.session_keys.end(),
                    [](i64 session) { return dt::is_sealed(session); }))
      return co::Err(fail(co::ErrorCode::InvalidArgument, seal_refusal("a session")));
    std::unique_ptr<ResearchRole> out{new ResearchRole{std::move(data)}};
    const usize panel_cells = out->data_.panel.cells();
    out->columns_.resize(role.fields.load.size());
    for (usize k = 0; k < role.fields.load.size(); ++k) {
      const icd::FieldFile &field = role.fields.load[k];
      ATX_TRY_VOID(icd::load_pinned_f64(field.path, field.sha, panel_cells, out->columns_[k],
                                        "research field " + field.name, nullptr, true));
      const auto &column = out->columns_[k];
      if (std::any_of(column.begin(), column.end(), [](f64 v) { return std::isinf(v); }))
        return co::Err(fail(co::ErrorCode::InvalidArgument,
                            "research field value is infinite: " + field.name));
      out->extras_.push_back(ResearchFieldReceipt{field.name, field.sha});
    }
    // Spans are taken once every column is loaded: no column is resized after this.
    std::vector<std::string> names;
    std::vector<std::span<const f64>> columns;
    for (usize k = 0; k < out->columns_.size(); ++k) {
      names.push_back(out->extras_[k].name);
      columns.emplace_back(out->columns_[k]);
    }
    ATX_TRY(auto panel, dt::overlay_panel(out->data_.panel, std::move(names), std::move(columns)));
    out->panel_.emplace(std::move(panel));
    ATX_TRY(out->guard_, dt::research_return_guard(out->data_));
    out->fields_sha256_ = role.fields.sha;
    return co::Ok(std::move(out));
  } catch (const std::bad_alloc &) {
    return co::Err(fail(co::ErrorCode::Unavailable, "allocation within the budget failed"));
  } catch (const std::exception &e) {
    return co::Err(fail(co::ErrorCode::InvalidArgument, e.what()));
  }
}

} // namespace atx::impl::strategy
