#include "atx/engine/research/fields/finra_asof_field.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/research/fields/asof_series.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/file_io.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr u64 kMaxAsofBytes = 4ULL << 30; // the C++ panel loader's bound (asof_field.cpp)
constexpr u64 kMaxReceiptBytes = 64ULL << 20;

// Verbatim from prepare_research_fields.py FIELDS["si_shares"] / FIELDS["si_dtc"]; the formula
// fingerprint test pins the text against the Python's formula_sha256.
[[nodiscard]] FieldSpec finra_base(std::string name, std::string units) {
  FieldSpec s;
  s.name = std::move(name);
  s.group = "finra";
  s.revision = 1;
  FieldDefinition &d = s.definition;
  d.units = std::move(units);
  d.clock = "finra-asof:latest-row-with-available_at<date(session)-strict;available_at=official-"
            "dissemination-date;"
            "replicates-atx-impl-build_asof_column";
  d.staleness =
      "age=date(session)-available_at calendar days; age>45 -> NaN; before first visible row ->"
      " NaN; visible NaN stays NaN (no skip-back)";
  d.source_columns = {"value"};
  d.point_in_time = true;
  return s;
}

[[nodiscard]] FieldSpec make_si_shares_spec() {
  FieldSpec s =
      finra_base("si_shares", "shares short (FINRA consolidated currentShortPositionQuantity)");
  s.caveats = {
      "settlements before 2021-06 come from FINRA's later consolidated republication, not the"
      " bytes the exchanges disseminated (vintage risk; see vintage_risk counts)",
      "securityID mapping by ORATS ticker_tk on the last trading day <= settlement (producer"
      " mapping_report.json)"};
  return s;
}

[[nodiscard]] FieldSpec make_si_dtc_spec() {
  FieldSpec s = finra_base(
      "si_dtc", "days to cover (FINRA daysToCoverQuantity; FINRA floors the ratio at 1.00)");
  s.caveats = {
      "FINRA reports days-to-cover floored at 1.00 (about 40% of producer rows equal 1);"
      " si_shares / volume is the unfloored alternative",
      "settlements before 2021-06 come from FINRA's later consolidated republication (vintage"
      " risk)"};
  return s;
}

// outputs.<name>.sha256 of the producer receipt, empty when the receipt names none.
[[nodiscard]] core::Result<std::string> pinned_sha256(const std::string &receipt,
                                                      std::string_view name) {
  try {
    const Json j = Json::parse(receipt, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      return core::Err(core::ErrorCode::InvalidArgument, "asof receipt: not a JSON object");
    }
    const auto outputs = j.find("outputs");
    if (outputs == j.end() || !outputs->is_object()) {
      return core::Ok(std::string{});
    }
    const auto entry = outputs->find(std::string(name));
    if (entry == outputs->end() || !entry->is_object()) {
      return core::Ok(std::string{});
    }
    const auto sha = entry->find("sha256");
    return core::Ok(sha != entry->end() && sha->is_string() ? sha->get<std::string>()
                                                            : std::string{});
  } catch (const std::exception &e) {
    return core::Err(core::ErrorCode::InvalidArgument, std::string("asof receipt: ") + e.what());
  }
}

struct Observed {
  std::vector<AsofObservation> observations;
  FinraStats stats;
};

// The rows on the role axis that the seal leaves (every available_at already checked against the
// schedule).
[[nodiscard]] Observed observe(const std::vector<AsofCsvRow> &rows, const RoleAxes &role) {
  Observed out;
  out.stats.rows_total = static_cast<u64>(rows.size());
  out.observations.reserve(rows.size());
  for (const AsofCsvRow &r : rows) {
    if (is_sealed_day(r.available_day)) {
      ++out.stats.rows_sealed; // reader-side seal: dropped before use, counted
      continue;
    }
    const auto column = role.column_of(r.security_id);
    if (!column) {
      continue;
    }
    ++out.stats.rows_matched_axis;
    out.observations.push_back(AsofObservation{*column, r.available_day, r.value});
  }
  out.stats.rows_ignored_unknown_id =
      out.stats.rows_total - out.stats.rows_sealed - out.stats.rows_matched_axis;
  return out;
}

[[nodiscard]] VintageRisk vintage_risk(const RoleAxes &role, const std::optional<i64> &cutoff,
                                       u64 cells, const std::optional<usize> &last) {
  VintageRisk v;
  v.rule = "visible row disseminated on or before " +
           (cutoff ? iso_date(*cutoff) : std::string("None")) + " (settlement before " +
           std::string(kFinraRepublicationSettlementBefore) + "): later FINRA republication";
  v.finite_member_cells = cells;
  if (last) {
    v.last_session_with_republished_visible_cell = iso_date(role.days()[*last]);
  }
  const usize safe = last ? *last + 1 : 0;
  if (safe < role.dates()) {
    v.first_session_vintage_safe = iso_date(role.days()[safe]);
  }
  return v;
}

} // namespace

const FieldSpec *finra_spec(std::string_view name) {
  static const FieldSpec shares = make_si_shares_spec();
  static const FieldSpec dtc = make_si_dtc_spec();
  if (name == shares.name) {
    return &shares;
  }
  if (name == dtc.name) {
    return &dtc;
  }
  return nullptr;
}

core::Result<FinraField> build_finra_field(std::string_view name,
                                           const std::filesystem::path &finra_root,
                                           const DisseminationSchedule &schedule,
                                           const RoleAxes &role,
                                           const std::filesystem::path &output_dir) {
  const FieldSpec *spec = finra_spec(name);
  if (spec == nullptr) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "finra field: unknown field " + std::string(name));
  }
  const auto asof = finra_root / "asof";
  const auto csv_path = asof / (spec->name + ".csv");
  const auto receipt_path = asof / "manifest.json";
  ATX_TRY(const auto stamp, file_stamp(csv_path));
  ATX_TRY(const auto bytes, read_bounded(csv_path, kMaxAsofBytes));
  ATX_TRY(const auto digest, core::sha256_hex(std::string_view(bytes)));
  ATX_TRY(const auto receipt, read_bounded(receipt_path, kMaxReceiptBytes));
  ATX_TRY(const auto receipt_sha, core::sha256_hex(std::string_view(receipt)));
  ATX_TRY(const auto pinned, pinned_sha256(receipt, spec->name));
  if (pinned != digest) {
    return core::Err(
        core::ErrorCode::InvalidArgument,
        spec->name + ": CSV bytes do not match the as-of producer receipt (asof/manifest.json)");
  }
  ATX_TRY(const auto rows, parse_asof_csv(bytes));
  for (const AsofCsvRow &r : rows) {
    if (!std::binary_search(schedule.dissemination_days.begin(), schedule.dissemination_days.end(),
                            r.available_day)) {
      return core::Err(core::ErrorCode::InvalidArgument,
                       spec->name +
                           ": an available_at is not an official FINRA dissemination date");
    }
  }
  Observed observed = observe(rows, role);
  ATX_TRY(const auto series,
          AsofSeries::create(observed.observations, role.instruments(), kFinraMaxStaleDays));
  ATX_TRY(auto writer, FieldWriter::create(output_dir, spec->name, role));
  const usize n = role.instruments();
  std::vector<f64> values(n);
  std::vector<i64> sources(n);
  const std::optional<i64> &cutoff = schedule.vintage_cutoff_day;
  u64 vintage_cells = 0;
  std::optional<usize> last_republished;
  for (usize t = 0; t < role.dates(); ++t) {
    series.row(role.days()[t], values, sources);
    ATX_TRY_VOID(writer.write(values));
    if (!cutoff) {
      continue;
    }
    const auto member = role.member_row(t);
    bool republished_row = false;
    for (usize j = 0; j < n; ++j) {
      if (sources[j] == AsofSeries::kNoSource || sources[j] > *cutoff) {
        continue;
      }
      republished_row = true;
      vintage_cells += (std::isfinite(values[j]) && member[j] != 0) ? 1U : 0U;
    }
    if (republished_row) {
      last_republished = t;
    }
  }
  ATX_TRY(auto written, writer.close());
  ATX_TRY(const auto after, file_stamp(csv_path));
  if (after != stamp) {
    return core::Err(core::ErrorCode::IoError, spec->name + ": source changed while reading");
  }
  FinraField out;
  out.field = std::move(written);
  out.sources = {
      SourceRecord{record_path(csv_path), static_cast<u64>(bytes.size()), digest},
      SourceRecord{record_path(receipt_path), static_cast<u64>(receipt.size()), receipt_sha},
      schedule.source};
  out.stats = observed.stats;
  out.vintage = vintage_risk(role, cutoff, vintage_cells, last_republished);
  if (cutoff) {
    out.vintage_safe_from = iso_date(*cutoff + 1);
  }
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::fields
