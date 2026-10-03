#include "atx/engine/research/fields/volume_mean_field.hpp"

#include <utility>

#include "atx/engine/research/fields/role_rows.hpp"
#include "atx/engine/research/fields/trailing_mean.hpp"

namespace atx::engine::research::fields {
namespace {

// Verbatim from research_fields_price.py (_spec("vol_126", ...), ROLE_CLOCK, LINE_NOTE); the
// formula fingerprint test pins it against the Python's formula_sha256.
[[nodiscard]] FieldSpec make_vol_126_spec() {
  FieldSpec s;
  s.name = "vol_126";
  s.group = "price_volume";
  s.formula_id = "price-vol-126-lag1-v1";
  s.revision = 1;
  s.min_history = "126 role sessions";
  FieldDefinition &d = s.definition;
  d.units = "shares per session: mean raw daily share volume";
  d.clock = "role-close-lag1-v1: row t reads the role's volume.f64 and present.u8 rows of sessions"
            " t-126..t-1 only (each known at its 22:00 UTC close mark)";
  d.staleness = "no fill: fewer than 63 present sessions in the window -> NaN";
  d.source_columns = {"volume.f64", "present.u8"};
  d.definition =
      "row t >= 126: mean of the role's volume.f64 over the sessions s in t-126..t-1 with"
      " present[s] == 1 and a finite volume >= 0; NaN when fewer than 63 such sessions or t <"
      " 126. Raw share volume in each session's own share units, as the DSL's volume (a split"
      " inside the window mixes share bases)";
  d.point_in_time = true;
  s.caveats = {
      "the role's volume.f64 is raw share volume (volume_basis raw-share-volume), not restated"
      " across splits",
      "one price line's values (the vendor securityID), not an issuer total"};
  return s;
}

} // namespace

const FieldSpec &vol_126_spec() {
  static const FieldSpec spec = make_vol_126_spec();
  return spec;
}

core::Result<BuiltField> build_vol_126(const RoleAxes &role,
                                       const std::filesystem::path &output_dir) {
  const usize n = role.instruments();
  ATX_TRY(auto volume, RoleRowReader::open(role, "volume.f64", RowKind::F64));
  ATX_TRY(auto present, RoleRowReader::open(role, "present.u8", RowKind::U8));
  ATX_TRY(auto mean, TrailingMean::create(n, kVolumeMeanWindow, kVolumeMeanMinSessions));
  ATX_TRY(auto writer, FieldWriter::create(output_dir, vol_126_spec().name, role));
  std::vector<f64> row(n);
  std::vector<f64> volume_row(n);
  std::vector<u8> present_row(n);
  for (usize t = 0; t < role.dates(); ++t) {
    mean.value(row); // row t from sessions t-126..t-1: written before session t is read
    ATX_TRY_VOID(writer.write(row));
    ATX_TRY_VOID(volume.next(std::span<f64>(volume_row)));
    ATX_TRY_VOID(present.next(std::span<u8>(present_row)));
    mean.push(volume_row, present_row);
  }
  ATX_TRY(auto volume_source, volume.finish());
  ATX_TRY(auto present_source, present.finish());
  ATX_TRY(auto written, writer.close());
  BuiltField out;
  out.field = std::move(written);
  out.sources = {std::move(volume_source), std::move(present_source)};
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::fields
