// atx::engine::data — catalog_report (S6.9) implementation.
//
// Builds the report text in memory (deterministic string concatenation over the
// catalog's ascending-name dataset list) then writes it once. No clock / RNG /
// absolute path / hash-order iteration enters the bytes — the only inputs are the
// catalog's deterministically-ordered content. See catalog_report.hpp.

#include "atx/engine/data/catalog_report.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace atx::engine::data {

namespace {

[[nodiscard]] std::string_view role_name(Role role) noexcept {
  switch (role) {
  case Role::Price:
    return "Price";
  case Role::Feature:
    return "Feature";
  case Role::Signal:
    return "Signal";
  case Role::Reference:
    return "Reference";
  }
  return "Unknown";
}

[[nodiscard]] std::string_view dtype_name(ColumnDType dt) noexcept {
  switch (dt) {
  case ColumnDType::F64:
    return "F64";
  case ColumnDType::I64:
    return "I64";
  case ColumnDType::Category:
    return "Category";
  }
  return "Unknown";
}

// Serialize ONE dataset's name/role/schema/provenance/lineage block to `out`.
// All fields are emitted in a fixed order; column names + dtypes are emitted in
// schema-storage order (deterministic). Lineage parents arrive ascending +
// de-duplicated from catalog.lineage() — no re-sorting needed here.
void append_dataset(std::string &out, const std::string &name, const Dataset &ds,
                    const std::vector<std::string> &parents) {
  const DatasetSchema &s = ds.schema();
  out += "dataset: ";
  out += name;
  out += "\n  role: ";
  out += role_name(s.role);
  out += "\n  columns:";
  for (atx::usize c = 0; c < s.columns.size(); ++c) {
    out += ' ';
    out += s.columns[c];
    out += '(';
    out += (c < s.dtypes.size() ? dtype_name(s.dtypes[c]) : std::string_view{"?"});
    out += ')';
  }
  out += "\n  pit_delay: ";
  out += std::to_string(s.pit_delay);
  out += "\n  region: ";
  out += s.region;
  out += "\n  universe_tag: ";
  out += s.universe_tag;
  out += "\n  effective_dated: ";
  out += (s.as_of.effective_dated ? "true" : "false");
  out += "\n  provenance_source: ";
  out += ds.provenance().source;
  out += "\n  lineage_parents:";
  for (const std::string &p : parents) {
    out += ' ';
    out += p;
  }
  out += "\n\n";
}

} // namespace

atx::core::Status write_catalog_report(const DatasetCatalog &catalog, const std::string &out_dir) {
  std::string body = "catalog_report v1\n\n";
  // names() is ascending (std::map key order) — the deterministic visitation order.
  for (const std::string &name : catalog.names()) {
    ATX_TRY(const auto ds_ref, catalog.resolve(name));
    ATX_TRY(const std::vector<std::string> parents, catalog.lineage(name));
    append_dataset(body, name, ds_ref.get(), parents);
  }

  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path{out_dir}, ec);
  if (ec) {
    return atx::core::Err(atx::core::ErrorCode::IoError,
                          "write_catalog_report: cannot create out_dir: " + ec.message());
  }
  const std::filesystem::path path = std::filesystem::path{out_dir} / "catalog_report.txt";
  std::ofstream os{path, std::ios::binary | std::ios::trunc};
  if (!os) {
    return atx::core::Err(atx::core::ErrorCode::IoError,
                          "write_catalog_report: cannot open " + path.string());
  }
  os.write(body.data(), static_cast<std::streamsize>(body.size()));
  if (!os) {
    return atx::core::Err(atx::core::ErrorCode::IoError,
                          "write_catalog_report: write failed for " + path.string());
  }
  return atx::core::Ok();
}

} // namespace atx::engine::data
