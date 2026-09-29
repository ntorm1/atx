// The `risk` verb of atx-equity-strategy-risk: pinned role + fields -> atx-risk-v1.1 outputs
// (schema atx.risk-model/v1) and the bias harness (strategy_risk_model.hpp). Loads, checks and
// derives the descriptors, streams per-session outputs, writes manifest.json LAST; a
// specific-variance invariant refusal leaves no manifest.

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "build_provenance.hpp"
#include "stage_data_provenance.hpp"
#include "strategy_risk_model.hpp"

namespace atx::impl::strategy::risk {
namespace {
using namespace atx;
namespace co = atx::core;
using Json = nlohmann::json;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr u64 max_manifest_bytes = 16ULL << 20;
constexpr usize max_dates = 4096, max_names = 20000;
constexpr const char* role_schema = "atx.recent-research-role/v1";
// 2023-01-01 00:00 UTC: the sealed VAL/holdout begins (R1 m-5). A role reaching it needs an owner.
constexpr i64 seal_begin_ns = 1'672'531'200LL * 1'000'000'000LL;
constexpr const char* fields_schema = "atx.research-role-fields/v1";
// Bytes per cell the run holds: role prices (24) and masks (2), cap (8), industry (1), seven
// f32 descriptors (28), three transient f64 fields while deriving (24), the residual history
// (1), rounded up.
constexpr u64 bytes_per_cell = 96;
constexpr u64 fixed_bytes = 64ULL << 20;

bool hash_valid(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
co::Result<Json> pinned_json(const std::string& path, const std::string& pin, const char* what) {
  const std::string label = std::string("risk: ") + what;
  if (!hash_valid(pin)) return co::Err(co::ErrorCode::InvalidArgument, label + " pin");
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() <= 0 || static_cast<u64>(file.tellg()) > max_manifest_bytes)
    return co::Err(co::ErrorCode::InvalidArgument, label + " missing/oversized");
  std::string text(static_cast<usize>(file.tellg()), '\0');
  file.seekg(0);
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file) return co::Err(co::ErrorCode::IoError, label + " read");
  ATX_TRY(auto digest, co::sha256_hex(text));
  if (digest != pin) return co::Err(co::ErrorCode::InvalidArgument, label + " SHA-256");
  return co::Ok(Json::parse(text));
}
// Exactly count values of T whose bytes hash to the manifest receipt files[name].
template <class T>
co::Status read_payload(const std::filesystem::path& base, const Json& files, const std::string& name,
                        usize count, std::vector<T>& out) {
  if (!files.contains(name)) return co::Err(co::ErrorCode::InvalidArgument, "risk: no receipt " + name);
  const auto& receipt = files.at(name);
  const auto sha = receipt.at("sha256").get<std::string>();
  if (!hash_valid(sha) || receipt.at("bytes").get<u64>() != u64{count} * sizeof(T))
    return co::Err(co::ErrorCode::InvalidArgument, "risk: receipt " + name);
  std::ifstream file(base / name, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() < 0 || static_cast<u64>(file.tellg()) != u64{count} * sizeof(T))
    return co::Err(co::ErrorCode::IoError, "risk: payload extent " + name);
  file.seekg(0);
  out.resize(count);
  const auto bytes = std::as_writable_bytes(std::span(out));
  // SAFETY: char accesses the object representation of a trivially copyable array.
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!file) return co::Err(co::ErrorCode::IoError, "risk: payload read " + name);
  ATX_TRY(auto digest, co::sha256_hex(std::span<const std::byte>(bytes)));
  if (digest != sha) return co::Err(co::ErrorCode::InvalidArgument, "risk: payload SHA " + name);
  return co::Ok();
}

struct Inputs {
  usize dates{}, names{};
  std::vector<i64> sessions;
  std::vector<u64> ids;
  std::vector<f64> close, raw, volume, cap;
  std::vector<u8> present, member, industry;
  std::array<std::vector<f32>, descriptor_count> descriptors;
  Json fields_used = Json::object();
  std::vector<std::string> unavailable;
  std::string cap_rule;
  u64 member_cells{}, member_absent_cells{};
};

co::Status load_role(const std::string& path, const std::string& sha, u64 budget, Inputs& in) {
  ATX_TRY(auto j, pinned_json(path, sha, "role manifest"));
  if (j.at("schema") != role_schema || j.at("status") != "complete" ||
      j.at("volume_basis") != "raw-share-volume")
    return co::Err(co::ErrorCode::InvalidArgument, "risk: role schema/status/volume basis");
  in.dates = j.at("dates").get<usize>(); in.names = j.at("instruments").get<usize>();
  if (!in.dates || in.dates > max_dates || !in.names || in.names > max_names)
    return co::Err(co::ErrorCode::InvalidArgument, "risk: role geometry");
  const u64 cells = u64{in.dates} * in.names;
  if (budget <= fixed_bytes || cells > (budget - fixed_bytes) / bytes_per_cell)
    return co::Err(co::ErrorCode::OutOfRange, "risk: --max-bytes below the run's working set");
  const auto base = std::filesystem::path(path).parent_path();
  const auto& files = j.at("files");
  const usize c = in.dates * in.names;
  ATX_TRY_VOID(read_payload(base, files, "sessions.i64", in.dates, in.sessions));
  ATX_TRY_VOID(read_payload(base, files, "ids.u64", in.names, in.ids));
  ATX_TRY_VOID(read_payload(base, files, "close.f64", c, in.close));
  ATX_TRY_VOID(read_payload(base, files, "raw_close.f64", c, in.raw));
  ATX_TRY_VOID(read_payload(base, files, "volume.f64", c, in.volume));
  ATX_TRY_VOID(read_payload(base, files, "present.u8", c, in.present));
  ATX_TRY_VOID(read_payload(base, files, "member.u8", c, in.member));
  if (!std::is_sorted(in.sessions.begin(), in.sessions.end()) ||
      std::adjacent_find(in.sessions.begin(), in.sessions.end()) != in.sessions.end() ||
      !std::is_sorted(in.ids.begin(), in.ids.end()) ||
      std::adjacent_find(in.ids.begin(), in.ids.end()) != in.ids.end())
    return co::Err(co::ErrorCode::InvalidArgument, "risk: role axes must strictly increase");
  // The engine role contract (atx-engine strategy_data.cpp read_strategy_role, and the
  // replay's load_prices): binary masks; a present cell has finite prices (close, raw > 0,
  // volume >= 0), an absent one NaN. Membership is the decision membership (prior window,
  // e.g. the linked-operating restriction of a -lo role) and is independent of presence: a
  // member may be absent at a session. The model fits members present at t-1 and t
  // (strategy_risk_model.cpp: eligibility = member && present && cap; returns need both ends).
  for (usize k = 0; k < c; ++k) {
    if (in.present[k] > 1 || in.member[k] > 1)
      return co::Err(co::ErrorCode::InvalidArgument, "risk: role presence/membership masks not binary");
    const std::array<f64, 3> prices{in.close[k], in.raw[k], in.volume[k]};
    for (usize f = 0; f < prices.size(); ++f) {
      const f64 x = prices[f];
      if (in.present[k] ? (!std::isfinite(x) || (f == 2 ? x < 0 : x <= 0)) : !std::isnan(x))
        return co::Err(co::ErrorCode::InvalidArgument,
                       "risk: role price contract (present: finite, close/raw > 0, volume >= 0; "
                       "absent: NaN)");
    }
    in.member_cells += in.member[k];
    in.member_absent_cells += in.member[k] && !in.present[k] ? 1U : 0U;
  }
  return co::Ok();
}

// The fields manifest, pinned and bound to the role (manifest, sessions, ids, member SHA).
struct Fields {
  Json manifest;
  std::filesystem::path base;
  usize dates{}, names{};
  // A used field: declared point in time, <f8 date-major role geometry, payload SHA. Absent
  // from the manifest: nullopt (reported unavailable).
  co::Result<std::optional<std::vector<f64>>> load(const std::string& name, Json& used) const {
    const Json* found = nullptr;
    for (const auto& e : manifest.at("fields"))
      if (e.at("name").get<std::string>() == name) {
        if (found) return co::Err(co::ErrorCode::InvalidArgument, "risk: duplicate field " + name);
        found = &e;
      }
    if (!found) return co::Ok(std::optional<std::vector<f64>>{});
    const auto& e = *found;
    if (!e.contains("point_in_time") || !e.at("point_in_time").is_boolean() ||
        !e.at("point_in_time").get<bool>() || e.at("file").get<std::string>() != name + ".f64" ||
        e.at("dtype") != "<f8" || e.at("layout") != "date-major" ||
        e.at("shape") != Json::array({dates, names}) ||
        manifest.at("files").at(name + ".f64").at("sha256") != e.at("sha256"))
      return co::Err(co::ErrorCode::InvalidArgument, "risk: field contract " + name);
    std::vector<f64> values;
    ATX_TRY_VOID(read_payload(base, manifest.at("files"), name + ".f64", dates * names, values));
    used[name] = Json{{"sha256", e.at("sha256")}, {"point_in_time", true},
        {"clock", e.contains("clock") ? e.at("clock") : Json(nullptr)}};
    return co::Ok(std::optional<std::vector<f64>>(std::move(values)));
  }
};
co::Result<Fields> open_fields(const std::string& path, const std::string& sha,
                               const std::string& role_path, const std::string& role_sha,
                               const Inputs& in) {
  ATX_TRY(auto m, pinned_json(path, sha, "fields manifest"));
  if (m.at("schema") != fields_schema || m.at("status") != "complete")
    return co::Err(co::ErrorCode::InvalidArgument, "risk: fields schema/status");
  ATX_TRY(auto role, pinned_json(role_path, role_sha, "role manifest"));
  const auto& pins = m.at("role");
  const auto& receipts = role.at("files");
  if (pins.at("manifest_sha256").get<std::string>() != role_sha ||
      pins.at("sessions_sha256") != receipts.at("sessions.i64").at("sha256") ||
      pins.at("ids_sha256") != receipts.at("ids.u64").at("sha256") ||
      pins.at("member_sha256") != receipts.at("member.u8").at("sha256"))
    return co::Err(co::ErrorCode::InvalidArgument, "risk: fields role pins do not match --role");
  return co::Ok(Fields{std::move(m), std::filesystem::path(path).parent_path(), in.dates, in.names});
}

// Cap = me_company (issuer market equity), else shares_out x raw_close; descriptors as f32.
co::Status derive(const Fields& f, Inputs& in) {
  const usize c = in.dates * in.names;
  ATX_TRY(auto me, f.load("me_company", in.fields_used));
  ATX_TRY(auto shares, f.load("shares_out", in.fields_used));
  if (!me && !shares)
    return co::Err(co::ErrorCode::InvalidArgument, "risk: neither me_company nor shares_out");
  in.cap.assign(c, nan);
  for (usize k = 0; k < c; ++k) {
    const f64 a = me ? (*me)[k] : nan;
    const f64 b = shares && in.present[k] ? (*shares)[k] * in.raw[k] : nan;
    in.cap[k] = std::isfinite(a) && a > 0 ? a : (std::isfinite(b) && b > 0 ? b : nan);
  }
  in.cap_rule = "me_company (issuer market equity), else shares_out x raw_close";
  me.reset();
  const auto ratio = [&](usize d, const std::vector<f64>& num, const std::vector<f64>* den,
                         bool minus_one) {
    auto& out = in.descriptors[d];
    out.assign(c, std::numeric_limits<f32>::quiet_NaN());
    for (usize k = 0; k < c; ++k) {
      const f64 bottom = den ? (*den)[k] : 1.0;
      if (!std::isfinite(num[k]) || !std::isfinite(bottom) || !(bottom > 0)) continue;
      out[k] = static_cast<f32>(num[k] / bottom - (minus_one ? 1.0 : 0.0));
    }
  };
  const auto missing = [&](const char* name) { in.unavailable.emplace_back(name); };
  { // SI ratio (needs shares_out), days to cover
    ATX_TRY(auto si, f.load("si_shares", in.fields_used));
    if (si && shares) ratio(5, *si, &*shares, false); else missing("si_ratio (si_shares/shares_out)");
    ATX_TRY(auto dtc, f.load("si_dtc", in.fields_used));
    if (dtc) ratio(6, *dtc, nullptr, false); else missing("days_to_cover (si_dtc)");
  }
  shares.reset();
  { // value and earnings yield over cap
    ATX_TRY(auto be, f.load("be", in.fields_used));
    if (be) ratio(0, *be, &in.cap, false); else missing("book_to_price (be)");
    ATX_TRY(auto ni, f.load("ni_ttm", in.fields_used));
    if (ni) ratio(1, *ni, &in.cap, false); else missing("earnings_yield (ni_ttm)");
  }
  { // balance-sheet ratios over total assets
    ATX_TRY(auto at, f.load("at", in.fields_used));
    ATX_TRY(auto gp, f.load("gp_ttm", in.fields_used));
    if (at && gp) ratio(2, *gp, &*at, false); else missing("gross_profitability (gp_ttm/at)");
    gp.reset();
    ATX_TRY(auto lt, f.load("lt", in.fields_used));
    if (at && lt) ratio(4, *lt, &*at, false); else missing("leverage (lt/at)");
    lt.reset();
    ATX_TRY(auto at4, f.load("at_lag4", in.fields_used));
    if (at && at4) ratio(3, *at, &*at4, true); else missing("asset_growth (at/at_lag4-1)");
  }
  ATX_TRY(auto ff49, f.load("grp_ff49", in.fields_used));
  if (ff49) {
    in.industry.assign(c, 0);
    for (usize k = 0; k < c; ++k) {
      const f64 id = (*ff49)[k];
      if (std::isfinite(id) && id >= 1 && id <= 49 && id == std::floor(id))
        in.industry[k] = static_cast<u8>(id);
    }
  } else {
    missing("industry (grp_ff49): every name in the residual slot");
  }
  return co::Ok();
}

co::Result<std::vector<BookWeight>> load_book(const std::string& path, const std::string& sha) {
  if (!hash_valid(sha)) return co::Err(co::ErrorCode::InvalidArgument, "risk: book weights pin");
  ATX_TRY(auto digest, co::sha256_file(path));
  if (digest != sha) return co::Err(co::ErrorCode::InvalidArgument, "risk: book weights SHA-256");
  std::ifstream file(path, std::ios::binary);
  std::string line;
  if (!std::getline(file, line)) return co::Err(co::ErrorCode::InvalidArgument, "risk: empty book file");
  const auto split = [](const std::string& text) {
    std::vector<std::string> cells;
    std::string cell;
    std::istringstream stream(text);
    while (std::getline(stream, cell, ',')) {
      if (!cell.empty() && cell.back() == '\r') cell.pop_back();
      cells.push_back(cell);
    }
    return cells;
  };
  const auto header = split(line);
  const auto column = [&](const char* name) -> usize {
    const auto it = std::find(header.begin(), header.end(), name);
    return it == header.end() ? header.size() : static_cast<usize>(it - header.begin());
  };
  // Plain (session, instrument_id, weight) or lane L3's holdings.csv (session_ns, held_weight:
  // the weight held at the session's close, after its fills), read as is.
  const auto either = [&](const char* a, const char* b) {
    const usize k = column(a);
    return k != header.size() ? k : column(b);
  };
  const usize cs = either("session", "session_ns"), ci = column("instrument_id"),
              cw = either("weight", "held_weight");
  if (cs == header.size() || ci == header.size() || cw == header.size())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "risk: book weights need columns session|session_ns (UTC-midnight ns), "
                   "instrument_id, weight|held_weight");
  std::vector<BookWeight> out;
  while (std::getline(file, line)) {
    if (line.empty() || line == "\r") continue;
    const auto cells = split(line);
    if (cells.size() < header.size() || out.size() >= 50'000'000)
      return co::Err(co::ErrorCode::InvalidArgument, "risk: book weights row");
    BookWeight w;
    const auto& s = cells[cs]; const auto& id = cells[ci]; const auto& v = cells[cw];
    const auto a = std::from_chars(s.data(), s.data() + s.size(), w.session);
    const auto b = std::from_chars(id.data(), id.data() + id.size(), w.instrument_id);
    const auto c = std::from_chars(v.data(), v.data() + v.size(), w.weight);
    if (a.ec != std::errc{} || b.ec != std::errc{} || c.ec != std::errc{} ||
        a.ptr != s.data() + s.size() || b.ptr != id.data() + id.size() || c.ptr != v.data() + v.size())
      return co::Err(co::ErrorCode::InvalidArgument, "risk: book weights value");
    out.push_back(w);
  }
  return co::Ok(std::move(out));
}

std::string number(f64 x) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << x;
  return out.str();
}
template <class T>
void write_binary(std::ofstream& file, std::span<const T> values) {
  const auto bytes = std::as_bytes(values);
  // SAFETY: char accesses the object representation of a trivially copyable array.
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

enum class ExposureEmit : u8 { None, Last, All };

// Session t's factor-regression style audit (F3): styles dropped as degenerate, and the smallest
// effective names and z dispersion (with the style) among the styles regressed.
struct StyleAudit {
  usize dropped{}, min_effective_style{}, min_dispersion_style{};
  f64 min_effective{nan}, min_dispersion{nan};
};
StyleAudit style_audit(const RiskDay& d) {
  StyleAudit a;
  for (usize s = 0; s < style_count; ++s) {
    if (d.style_dropped[s]) { ++a.dropped; continue; }
    const f64 e = d.style_effective_names[s], v = d.style_dispersion[s];
    if (std::isfinite(e) && (std::isnan(a.min_effective) || e < a.min_effective)) {
      a.min_effective = e; a.min_effective_style = s;
    }
    if (std::isfinite(v) && (std::isnan(a.min_dispersion) || v < a.min_dispersion)) {
      a.min_dispersion = v; a.min_dispersion_style = s;
    }
  }
  return a;
}

// Streams every session's outputs; the last session's exposures are kept for the CSV.
class FileSink final : public RiskSink {
public:
  FileSink(const std::filesystem::path& dir, const RiskPanel& panel, ExposureEmit emit)
      : dir_(dir), panel_(panel), emit_(emit),
        returns_(dir / "factor_returns.csv", std::ios::binary),
        diagnostics_(dir / "diagnostics.csv", std::ios::binary),
        covariance_(dir / "factor_covariance.f64", std::ios::binary),
        specific_(dir / "specific_variance.f64", std::ios::binary),
        structural_(dir / "factor_structural.u8", std::ios::binary) {
    for (auto* s : {&returns_, &diagnostics_}) s->imbue(std::locale::classic());
    returns_ << "session,date_index,fitted,regression_rows,active_industries,r2";
    for (usize k = 0; k < factor_count; ++k) returns_ << ',' << factor_name(k);
    returns_ << '\n';
    // Columns after the first two are appended only (spo-v1 reads session and forecast).
    diagnostics_ << "session,forecast,lambda2_factor,lambda2_specific,bias_factor,bias_specific,"
                    "structural_names,specific_names,structural_factors,"
                    "structural_correlation_scale,unforecast_exposed_factors,residual_names,"
                    "fenced_values,unscaled_descriptors,styles_dropped,min_style_effective_names,"
                    "min_style_dispersion,max_specific_variance,structural_sigma_lower,"
                    "structural_sigma_upper,structural_bounded_names,structural_clamped_names\n";
    if (emit_ == ExposureEmit::All) {
      styles_.open(dir / "style_exposures.f32", std::ios::binary);
      slots_.open(dir / "industry_slot.u8", std::ios::binary);
    }
  }
  co::Status on_day(const RiskDay& d) override {
    returns_ << d.session << ',' << d.date << ',' << (d.fitted ? 1 : 0) << ',' << d.regression_rows
             << ',' << d.active_industries << ',' << number(d.r2);
    for (const f64 f : d.factor_return) returns_ << ',' << number(f);
    returns_ << '\n';
    diagnostics_ << d.session << ',' << (d.forecast ? 1 : 0) << ',' << number(d.lambda2_factor)
                 << ',' << number(d.lambda2_specific) << ',' << number(d.bias_factor) << ','
                 << number(d.bias_specific) << ',' << d.structural_names << ','
                 << d.specific_names << ',' << d.structural_factors << ','
                 << number(d.structural_scale) << ',' << d.unforecast_exposed_factors << ','
                 << d.residual_names;
    const StyleAudit audit = style_audit(d);
    diagnostics_ << ',' << d.fenced_values << ',' << d.unscaled_descriptors << ',' << audit.dropped
                 << ','
                 << number(audit.min_effective) << ',' << number(audit.min_dispersion) << ','
                 << number(d.max_specific_variance) << ',' << number(d.structural_sigma_lower)
                 << ',' << number(d.structural_sigma_upper) << ',' << d.structural_bounded_names
                 << ',' << d.structural_clamped_names << '\n';
    write_binary(covariance_, d.covariance);
    write_binary(specific_, d.specific_variance);
    write_binary(structural_, d.factor_structural);
    structural_stats(d);
    forecasts_ += d.forecast ? 1U : 0U;
    if (d.forecast && first_forecast_ < 0) first_forecast_ = d.session;
    if (d.fitted) { r2_sum_ += d.r2; ++fitted_; }
    if (emit_ == ExposureEmit::All) {
      std::vector<f32> narrow(d.styles.size());
      for (usize k = 0; k < narrow.size(); ++k) narrow[k] = static_cast<f32>(d.styles[k]);
      write_binary(styles_, std::span<const f32>(narrow));
      write_binary(slots_, d.industry_slot);
    }
    if (d.date + 1 == panel_.dates && emit_ != ExposureEmit::None) {
      last_slot_.assign(d.industry_slot.begin(), d.industry_slot.end());
      last_eligible_.assign(d.eligible.begin(), d.eligible.end());
      last_styles_.assign(d.styles.begin(), d.styles.end());
    }
    if (!returns_ || !diagnostics_ || !covariance_ || !specific_ || !structural_)
      return co::Err(co::ErrorCode::IoError, "risk: output stream");
    return co::Ok();
  }
  co::Status close(Json& files) {
    for (auto* s : {&returns_, &diagnostics_, &covariance_, &specific_, &structural_, &styles_,
                    &slots_})
      if (s->is_open()) s->close();
    if (!returns_ || !diagnostics_ || !covariance_ || !specific_ || !structural_ ||
        (emit_ == ExposureEmit::All && (!styles_ || !slots_)))
      return co::Err(co::ErrorCode::IoError, "risk: output close");
    std::vector<std::string> names{"factor_returns.csv", "diagnostics.csv", "factor_covariance.f64",
                                   "specific_variance.f64", "factor_structural.u8"};
    if (emit_ == ExposureEmit::All) { names.emplace_back("style_exposures.f32"); names.emplace_back("industry_slot.u8"); }
    if (emit_ != ExposureEmit::None) {
      std::ofstream last(dir_ / "exposures_last.csv", std::ios::binary);
      last.imbue(std::locale::classic());
      last << "instrument_id,industry_slot,eligible";
      for (const char* s : style_names) last << ',' << s;
      last << '\n';
      for (usize i = 0; i < last_slot_.size(); ++i) {
        if (last_slot_[i] == no_exposure) continue;
        last << panel_.ids[i] << ',' << static_cast<unsigned>(last_slot_[i]) << ','
             << static_cast<unsigned>(last_eligible_[i]);
        for (usize s = 0; s < style_count; ++s) last << ',' << number(last_styles_[i * style_count + s]);
        last << '\n';
      }
      last.close();
      if (!last) return co::Err(co::ErrorCode::IoError, "risk: exposures_last.csv");
      names.emplace_back("exposures_last.csv");
    }
    for (const auto& name : names) {
      ATX_TRY(auto sha, co::sha256_file((dir_ / name).string()));
      files[name] = Json{{"sha256", sha}, {"bytes", std::filesystem::file_size(dir_ / name)}};
    }
    return co::Ok();
  }
  [[nodiscard]] Json run_stats() const {
    return Json{{"forecast_sessions", forecasts_},
        {"first_forecast_session_ns", first_forecast_ >= 0 ? Json(first_forecast_) : Json(nullptr)},
        {"fitted_sessions", fitted_},
        {"mean_r2", fitted_ ? Json(r2_sum_ / static_cast<f64>(fitted_)) : Json(nullptr)}};
  }
  // The structural factor audit (F2): every factor ever forecast structurally with its count of
  // such sessions and the first and last; sessions with a structural factor, those whose
  // structural terms needed a scale s < 1 (and the smallest s); forecast sessions where a
  // present name was exposed to a factor without a forecast; sessions with residual-slot names.
  [[nodiscard]] Json structural_json() const {
    Json factors = Json::array();
    for (usize k = 0; k < factor_count; ++k) {
      const auto& f = per_factor_[k];
      if (!f.sessions) continue;
      factors.push_back(Json{{"factor", factor_name(k)}, {"structural_sessions", f.sessions},
          {"first_session_ns", f.first}, {"last_session_ns", f.last}});
    }
    return Json{{"side_file", "factor_structural.u8"}, {"factors", std::move(factors)},
        {"factor_sessions", factor_sessions_}, {"sessions_with_structural", structural_sessions_},
        {"sessions_scaled", scaled_sessions_},
        {"min_correlation_scale", scaled_sessions_ ? Json(min_scale_) : Json(1.0)},
        {"forecast_sessions_with_unforecast_exposure", unforecast_sessions_},
        {"sessions_with_residual_names", residual_sessions_}};
  }

private:
  struct StructuralFactor {
    usize sessions{};
    i64 first{-1}, last{-1};
  };
  void structural_stats(const RiskDay& d) {
    for (usize k = 0; k < d.factor_structural.size() && k < factor_count; ++k) {
      if (!d.factor_structural[k]) continue;
      auto& f = per_factor_[k];
      if (!f.sessions) f.first = d.session;
      f.last = d.session;
      ++f.sessions; ++factor_sessions_;
    }
    if (d.structural_factors) {
      ++structural_sessions_;
      if (d.structural_scale < 1.0) {
        ++scaled_sessions_;
        min_scale_ = std::min(min_scale_, d.structural_scale);
      }
    }
    if (d.forecast && d.unforecast_exposed_factors) ++unforecast_sessions_;
    if (d.residual_names) ++residual_sessions_;
  }
  std::filesystem::path dir_;
  const RiskPanel& panel_;
  ExposureEmit emit_{};
  std::ofstream returns_, diagnostics_, covariance_, specific_, structural_, styles_, slots_;
  std::array<StructuralFactor, factor_count> per_factor_{};
  usize factor_sessions_{}, structural_sessions_{}, scaled_sessions_{}, unforecast_sessions_{};
  usize residual_sessions_{};
  f64 min_scale_{1.0};
  usize forecasts_{}, fitted_{};
  i64 first_forecast_{-1};
  f64 r2_sum_{};
  std::vector<u8> last_slot_, last_eligible_;
  std::vector<f64> last_styles_;
};

f64 median_of(std::vector<f64> v) {
  v.erase(std::remove_if(v.begin(), v.end(), [](f64 x) { return !std::isfinite(x); }), v.end());
  if (v.empty()) return nan;
  std::sort(v.begin(), v.end());
  const usize h = v.size() / 2;
  return v.size() % 2 ? v[h] : 0.5 * (v[h - 1] + v[h]);
}
Json finite_or_null(f64 x) { return std::isfinite(x) ? Json(x) : Json(nullptr); }
Json band_json(Band b) { return Json::array({finite_or_null(b.lo), finite_or_null(b.hi)}); }
bool inside(f64 b, Band band) { return std::isfinite(b) && b >= band.lo && b <= band.hi; }

// The v1.1 robustness audit over the run (F3; R2 finding I-2): the largest daily specific
// variance with its instrument and session, the smallest effective names and z dispersion of a
// regressed style column with the style and session, style-sessions dropped as degenerate (per
// style), structural names (history < 252) at a sigma bound or with a clamped exposure
// (name-sessions), and the descriptor values the robust fence moved.
class RobustnessSink final : public RiskSink {
public:
  RobustnessSink(const RiskPanel& panel, const RiskModelConfig& cfg)
      : panel_(panel), cfg_(cfg) {}
  co::Status on_day(const RiskDay& d) override {
    if (std::isfinite(d.max_specific_variance) &&
        (std::isnan(max_d_.value) || d.max_specific_variance > max_d_.value) &&
        d.max_specific_instrument < panel_.instruments)
      max_d_ = {d.max_specific_variance, d.session, panel_.ids[d.max_specific_instrument]};
    const StyleAudit audit = style_audit(d);
    if (std::isfinite(audit.min_effective) &&
        (std::isnan(min_effective_.value) || audit.min_effective < min_effective_.value))
      min_effective_ = {audit.min_effective, d.session, audit.min_effective_style};
    if (std::isfinite(audit.min_dispersion) &&
        (std::isnan(min_dispersion_.value) || audit.min_dispersion < min_dispersion_.value))
      min_dispersion_ = {audit.min_dispersion, d.session, audit.min_dispersion_style};
    for (usize s = 0; s < style_count; ++s) dropped_[s] += d.style_dropped[s];
    bounded_ += d.structural_bounded_names;
    clamped_ += d.structural_clamped_names;
    sessions_at_bounds_ += (d.structural_bounded_names || d.structural_clamped_names) ? 1U : 0U;
    fenced_ += d.fenced_values;
    unscaled_ += d.unscaled_descriptors;
    return co::Ok();
  }
  [[nodiscard]] Json json() const {
    Json by_style = Json::object();
    usize dropped = 0;
    for (usize s = 0; s < style_count; ++s) {
      by_style[style_names[s]] = dropped_[s];
      dropped += dropped_[s];
    }
    const auto style_extreme = [](const StyleExtreme& e) {
      return Json{{"value", finite_or_null(e.value)},
          {"style", e.session >= 0 ? Json(style_names[e.style]) : Json(nullptr)},
          {"session_ns", e.session >= 0 ? Json(e.session) : Json(nullptr)}};
    };
    return Json{{"model", risk_model_id},
        {"specific_variance_bound", cfg_.max_specific_variance},
        {"invariant", "every forecast daily specific variance lies in (0, "
                      "specific_variance_bound) or the run is refused naming the instrument and "
                      "session (nothing is clamped); a complete output therefore has 0 refusals"},
        {"invariant_refusals", 0},
        {"max_daily_specific_variance",
         {{"value", finite_or_null(max_d_.value)},
          {"session_ns", max_d_.session >= 0 ? Json(max_d_.session) : Json(nullptr)},
          {"instrument_id", max_d_.session >= 0 ? Json(max_d_.id) : Json(nullptr)}}},
        {"min_style_effective_names", style_extreme(min_effective_)},
        {"min_style_dispersion", style_extreme(min_dispersion_)},
        {"style_floor_effective_names", cfg_.min_style_effective_names},
        {"style_dates_dropped", dropped}, {"style_dates_dropped_by_style", std::move(by_style)},
        {"structural_names_at_sigma_bound", bounded_},
        {"structural_names_with_clamped_exposure", clamped_},
        {"sessions_with_structural_names_at_bounds", sessions_at_bounds_},
        {"fenced_descriptor_values", fenced_},
        {"unscaled_descriptor_dates", unscaled_},
        {"definitions",
         {{"style_dispersion", "equal-weighted SD of a regressed style's z over the factor "
                               "regression's rows (session t regresses on X_{t-1})"},
          {"style_effective_names", "(sum w z^2)^2 / sum (w z^2)^2 over those rows, w = "
                                    "sqrt(cap); a style below the floor is not regressed"},
          {"structural_names", "names with fewer than structural_history residuals in the "
                               "last structural_history sessions (their sigma uses the "
                               "structural model)"},
          {"unscaled_descriptor_dates", "descriptor-sessions (11 styles + 2 SI components) with "
                                        ">= 2 eligible values but a zero MAD (over half tied) "
                                        "or zero dispersion: that style (component) is 0 then"}}}};
  }

private:
  struct DailyExtreme {
    f64 value{nan};
    i64 session{-1};
    u64 id{};
  };
  struct StyleExtreme {
    f64 value{nan};
    i64 session{-1};
    usize style{};
  };
  const RiskPanel& panel_;
  const RiskModelConfig& cfg_;
  DailyExtreme max_d_{};
  StyleExtreme min_effective_{}, min_dispersion_{};
  std::array<usize, style_count> dropped_{};
  usize bounded_{}, clamped_{}, sessions_at_bounds_{}, fenced_{}, unscaled_{};
};
void print_robustness(std::ostream& progress, const Json& r) {
  const auto& d = r.at("max_daily_specific_variance");
  progress << "risk robustness (" << r.at("model").get<std::string>() << "): max daily D "
           << d.at("value") << " (instrument " << d.at("instrument_id") << ", session "
           << d.at("session_ns") << ", bound " << r.at("specific_variance_bound")
           << "); min style dispersion " << r.at("min_style_dispersion").at("value") << " ("
           << r.at("min_style_dispersion").at("style") << "); min style effective names "
           << r.at("min_style_effective_names").at("value") << "; style-dates dropped "
           << r.at("style_dates_dropped") << "; structural names at sigma bound "
           << r.at("structural_names_at_sigma_bound") << ", with clamped exposure "
           << r.at("structural_names_with_clamped_exposure") << "; fenced descriptor values "
           << r.at("fenced_descriptor_values") << ", unscaled descriptor-dates "
           << r.at("unscaled_descriptor_dates") << '\n';
}

// Exclusion counts and status of one family (R1 M-5): series with no kept observation are
// empty, series whose dropped share exceeds max_dropped_share are refused; only the rest (ok)
// are summarised. Family status: ok (none refused), partial, refused (none ok) or empty.
Json family_exclusions(const std::vector<const BiasSeries*>& family,
                       std::vector<const BiasSeries*>& ok) {
  Json refused = Json::array();
  usize empty = 0, kept = 0, warmup = 0, factor_drops = 0, name_drops = 0;
  for (const auto* s : family) {
    kept += s->z.size(); warmup += s->warmup_excluded;
    factor_drops += s->dropped_factor_exposures; name_drops += s->uncovered_name_returns;
    switch (series_status(*s)) {
    case SeriesStatus::Ok: ok.push_back(s); break;
    case SeriesStatus::Refused: refused.push_back(s->name); break;
    case SeriesStatus::Empty: ++empty; break;
    }
  }
  const usize dropped = factor_drops + name_drops;
  const char* status = refused.empty() ? (ok.empty() ? "empty" : "ok")
                                       : (ok.empty() ? "refused" : "partial");
  const usize total = dropped + kept;
  const f64 share = total > 0 ? static_cast<f64>(dropped) / static_cast<f64>(total) : 0.0;
  const usize refused_count = refused.size();
  return Json{{"status", status}, {"series_ok", ok.size()}, {"series_refused", refused_count},
      {"series_empty", empty}, {"refused_series", std::move(refused)}, {"observations", kept},
      {"warmup_excluded", warmup}, {"dropped_factor_exposures", factor_drops},
      {"uncovered_name_returns", name_drops}, {"dropped_share", share},
      {"max_dropped_share", max_dropped_share}};
}

// USE4 Appendix A summary of one family over its ok series: full-sample b per series against
// 1 +- sqrt(2/T) and the kurtosis-widened band (pooled kurtosis of those series' z), and
// rolling 63/252 windows; plus the family's exclusion counts and status.
Json family_summary(const std::vector<const BiasSeries*>& all) {
  std::vector<const BiasSeries*> family;
  Json out = family_exclusions(all, family);
  usize missing = 0, uncovered = 0;
  for (const auto* s : all) { missing += s->missing_returns; uncovered += s->uncovered_names; }
  std::vector<f64> pooled;
  for (const auto* s : family) pooled.insert(pooled.end(), s->z.begin(), s->z.end());
  const f64 kurtosis = sample_kurtosis(pooled);
  std::vector<f64> full;
  usize in_band = 0, in_kband = 0;
  for (const auto* s : family) {
    const f64 b = bias_statistic(s->z);
    full.push_back(b);
    in_band += inside(b, bias_band(s->z.size())) ? 1U : 0U;
    in_kband += inside(b, kurtosis_band(s->z.size(), kurtosis)) ? 1U : 0U;
  }
  Json rolling = Json::object();
  for (const usize window : {usize{63}, usize{252}}) {
    std::vector<f64> values;
    usize windows = 0, in = 0, in_k = 0;
    const Band band = bias_band(window), kband = kurtosis_band(window, kurtosis);
    for (const auto* s : family)
      for (const f64 b : rolling_bias(s->z, window)) {
        if (!std::isfinite(b)) continue;
        ++windows; in += inside(b, band) ? 1U : 0U; in_k += inside(b, kband) ? 1U : 0U;
        values.push_back(b);
      }
    f64 sum = 0;
    for (const f64 v : values) sum += v;
    rolling[std::to_string(window)] = Json{{"band", band_json(band)},
        {"kurtosis_band", band_json(kband)}, {"windows", windows},
        {"share_in_band", windows ? Json(static_cast<f64>(in) / static_cast<f64>(windows)) : Json(nullptr)},
        {"share_in_kurtosis_band",
         windows ? Json(static_cast<f64>(in_k) / static_cast<f64>(windows)) : Json(nullptr)},
        {"b_mean", windows ? Json(sum / static_cast<f64>(windows)) : Json(nullptr)},
        {"b_median", finite_or_null(median_of(values))}};
  }
  f64 sum = 0; usize count = 0;
  for (const f64 b : full)
    if (std::isfinite(b)) { sum += b; ++count; }
  out["series"] = all.size();
  out["pooled_kurtosis"] = finite_or_null(kurtosis);
  out["full_sample"] = Json{{"b_mean", count ? Json(sum / static_cast<f64>(count)) : Json(nullptr)},
                            {"b_median", finite_or_null(median_of(full))},
                            {"series_in_band", in_band}, {"series_in_kurtosis_band", in_kband}};
  out["rolling"] = std::move(rolling);
  out["missing_returns"] = missing;
  out["uncovered_book_names"] = uncovered;
  return out;
}

co::Status write_bias(const std::filesystem::path& dir, const BiasHarness& harness,
                      const Json& robustness, Json& files, Json& summary) {
  std::ofstream csv(dir / "bias.csv", std::ios::binary);
  csv.imbue(std::locale::classic());
  csv << "family,name,session,forecast_vol,realized_return,z,b63,b252\n";
  std::map<std::string, std::vector<const BiasSeries*>> families;
  for (const auto& s : harness.series()) {
    families[s.family].push_back(&s);
    const auto b63 = rolling_bias(s.z, 63), b252 = rolling_bias(s.z, 252);
    for (usize j = 0; j < s.z.size(); ++j)
      csv << s.family << ',' << s.name << ',' << s.sessions[j] << ',' << number(s.forecast_vol[j])
          << ',' << number(s.realized[j]) << ',' << number(s.z[j]) << ',' << number(b63[j]) << ','
          << number(b252[j]) << '\n';
  }
  csv.close();
  if (!csv) return co::Err(co::ErrorCode::IoError, "risk: bias.csv");
  Json fam = Json::object();
  for (const auto& [name, family] : families) fam[name] = family_summary(family);
  summary = Json{{"schema", "atx.risk-bias/v2"},
      {"definition", "z_t = realized return over (t-1, t] / forecast vol at t-1 (x'Fx + sum "
                     "w^2 D); bias statistic b = sample SD of z over a window of T; band 1 +- "
                     "sqrt(2/T); kurtosis-widened band 1 +- 1.96 sqrt((k-1)/(4T)) with k the "
                     "family's pooled kurtosis (USE4 Appendix A, asymptotic)"},
      {"exclusion_rule", "complete forecasts only: an observation whose portfolio holds a name "
                         "without a forecast (no exposure row or specific variance) or has a "
                         "nonzero exposure to a factor without a forecast (a short-history factor "
                         "is forecast structurally once its class has a fully observed factor) "
                         "is excluded whole, never priced by a partial x'Fx or realized "
                         "on a sub-book; exclusions before a series' first kept observation are "
                         "warmup_excluded, later ones dropped_factor_exposures / "
                         "uncovered_name_returns; a series whose dropped share (dropped / "
                         "(dropped + kept)) exceeds max_dropped_share is refused and, like an "
                         "empty series, left out of every b statistic (bias.csv keeps its rows)"},
      {"max_dropped_share", max_dropped_share},
      {"families", std::move(fam)},
      {"risk_model_robustness", robustness}};
  const std::string text = summary.dump(2) + "\n";
  std::ofstream js(dir / "bias_summary.json", std::ios::binary);
  js << text;
  js.close();
  if (!js) return co::Err(co::ErrorCode::IoError, "risk: bias_summary.json");
  for (const char* name : {"bias.csv", "bias_summary.json"}) {
    ATX_TRY(auto sha, co::sha256_file((dir / name).string()));
    files[name] = Json{{"sha256", sha}, {"bytes", std::filesystem::file_size(dir / name)}};
  }
  return co::Ok();
}

Json recipe_json(const RiskModelConfig& c) {
  Json factors = Json::array();
  for (usize k = 0; k < factor_count; ++k) factors.push_back(factor_name(k));
  return Json{{"model", risk_model_id},
      {"factors", std::move(factors)},
      {"factor_covariance", {{"vol_halflife", c.vol_halflife},
          {"variance_nw_lags", c.variance_nw_lags}, {"correlation_halflife", c.correlation_halflife},
          {"correlation_nw_lags", c.correlation_nw_lags}, {"nw_halflife", c.nw_halflife},
          {"vra_halflife", c.vra_halflife}, {"vra_min_dates", c.vra_min_dates},
          {"min_factor_history", c.min_factor_history},
          {"source", "USE4S (Menchero-Orr-Wang 2011): 84/504, NW 5/2, VRA 42; estimator = atx-engine "
                     "risk V2 cov_ewma (recursive twin); PSD eigenvalue floor"},
          {"vra_factors", "fully observed factors only (>= min_factor_history returns)"},
          {"structural", {{"enabled", c.structural_factor_forecast},
              {"applies_to", "a non-market factor with n < min_factor_history returns that has a "
                             "history (n >= 1) or a present name exposed to it at t"},
              {"weight", "w = n / min_factor_history (own), 1 - w (class prior)"},
              {"variance", "w own + (1 - w) prior; own = the factor's EWMA + NW variance (the "
                           "fully observed estimator, clamped at 0; w = 0 without one)"},
              {"prior", "industries: mean fully observed industry variance at t weighted by the "
                        "industry's eligible cap at t (equal weights when none holds cap); "
                        "styles: equal-weighted mean fully observed style variance; pre-VRA, then "
                        "x lambda^2 like every entry"},
              {"correlation", "w rho with a fully observed factor, w w' rho with a structural one "
                              "(rho = the correlation estimator's value, 0 without joint history)"},
              {"psd", "the fully observed block is never altered; every structural off-diagonal "
                      "term is scaled by the largest s = 2^-j (j = 0..30, else 0) that keeps F "
                      "positive definite (diagnostics.csv structural_correlation_scale)"},
              {"source", "USE4 structural fallback for short-history factors (R3.1; platform "
                         "v7 F2); weights declared, not fitted"}}}}},
      {"specific_risk", {{"halflife", c.specific_halflife}, {"nw_lags", c.specific_nw_lags},
          {"structural_history", c.structural_history}, {"bayesian_q", c.bayesian_q},
          {"structural_sigma_quantile", c.structural_sigma_quantile},
          {"max_specific_variance", c.max_specific_variance},
          {"rule", "sigma = gamma sigma_TS + (1-gamma) sigma_STR, gamma = min(1, h/252), "
                   "sigma_STR = E0 exp(x b) from the constrained WLS of ln sigma_TS on X_t over "
                   "full-history eligible names (sqrt-cap weights; styles under the effective-"
                   "names floor left out), each style exposure of x clamped to the fitted names' "
                   "[min, max] and sigma_STR bounded to the fitted names' type-7 [q, 1-q] sigma_TS "
                   "quantiles (fallback without a fit: their cap-weighted median, same bounds), "
                   "then Bayesian shrinkage to the lower cap-weighted size-decile median (engine "
                   "specific_risk_v2 formula with a robust target), then VRA"},
          {"invariant", "every forecast daily specific variance in (0, max_specific_variance) or "
                        "the run is refused naming the instrument and session; nothing clamped"},
          {"source", "USE4 structural + Bayesian specific risk (Menchero-Orr-Wang 2011); F3: no "
                     "extrapolation outside the fit (R2 I-2), [p1, p99] = the conventional 1%/99% "
                     "cross-sectional winsorisation, quantiles Hyndman-Fan (1996) type 7"}}},
      {"exposures", {{"min_industry_names", c.min_industry_names}, {"beta_window", c.beta_window},
          {"min_beta_pairs", c.min_beta_pairs}, {"momentum_window", c.momentum_window},
          {"momentum_skip", c.momentum_skip}, {"adv_window", c.adv_window},
          {"winsor_k", c.winsor_k}, {"mad_consistency", mad_consistency}, {"clip_z", c.clip_z},
          {"standardization", "fence at median +- winsor_k x 1.4826 MAD of the eligible names' "
                              "values (member, present, cap > 0), then cap-weighted mean 0 and "
                              "equal-weighted SD 1 of the fenced values over them (USE4), clipped "
                              "at clip_z, missing 0; a MAD of 0 (over half tied) leaves the style "
                              "0 that date"},
          {"standardization_source", "k = 3.5: Iglewicz-Hoaglin (1993) modified z-score cutoff; "
                                     "1.4826 = 1/Phi^-1(3/4), the MAD's normal consistency "
                                     "constant (Hampel 1974; Rousseeuw-Croux 1993); one outlier "
                                     "moves the others' z by O(cap share + 1/n), not O(1)"},
          {"style_validity", {{"min_effective_names", c.min_style_effective_names},
              {"rule", "a style column enters a cross-sectional fit only when its effective names "
                       "(sum w z^2)^2 / sum (w z^2)^2 over the fit's rows reach the floor; "
                       "otherwise its factor return is missing that session (the F2 structural "
                       "forecast covers a factor short of history)"},
              {"source", "numbers-equivalent of the Herfindahl index (Adelman 1969); floor = "
                         "min_industry_names, the model's thin-industry floor"}}},
          {"styles", "size ln cap; beta and residual_vol from the rolling 252-interval OLS on the "
                     "equal-weight member market (>= 126 pairs); momentum ln(close[t-21] / "
                     "close[t-252]); value be/cap; earnings_yield ni_ttm/cap; profitability "
                     "gp_ttm/at; asset_growth at/at_lag4 - 1; leverage lt/at; liquidity ln mean "
                     "raw$ volume over 63 sessions; short_interest = mean of z(si/shares_out) and "
                     "z(si_dtc), re-standardized"}}},
      {"regression", {{"min_regression_names", c.min_regression_names},
          {"rule", "session-t simple returns on X_{t-1}, WLS weights sqrt(cap_{t-1}), market + "
                   "active industries + styles with at least min_effective_names effective names "
                   "(exposures.style_validity), sum_k cap_k f_k = 0 over active industries"}}}};
}

struct RiskArgs {
  std::string role, role_sha, fields, fields_sha, output, book, book_sha;
  std::string unseal; // --unseal OWNER: the owner who admits a role reaching the seal
  usize random{64};
  u64 seed{7}, max_bytes{1'400'000'000ULL};
  ExposureEmit emit{ExposureEmit::Last};
};

co::Status check_args(const RiskArgs& a) {
  if (a.role.empty() || a.fields.empty() || a.output.empty())
    return co::Err(co::ErrorCode::InvalidArgument, "risk: --role, --fields and --output are required");
  if (a.book.empty() != a.book_sha.empty())
    return co::Err(co::ErrorCode::InvalidArgument, "risk: --book-weights and its SHA go together");
  if (a.random > 1024) return co::Err(co::ErrorCode::InvalidArgument, "risk: at most 1024 random portfolios");
  return co::Ok();
}
// The TRAIN/seal guard (R1 m-5): a role whose sessions reach 2023-01-01 is refused unless an
// owner admits it by name (--unseal OWNER, recorded in the manifest). Book weights must lie in
// the role's sessions (BiasHarness), so the role bounds them too.
co::Status check_seal(std::span<const i64> sessions, const std::string& owner) {
  if (sessions.empty() || sessions.back() < seal_begin_ns || !owner.empty()) return co::Ok();
  return co::Err(co::ErrorCode::InvalidArgument, "risk: the role reaches 2023-01-01 (sealed "
                                                 "VAL/holdout); refused without --unseal OWNER");
}
RiskPanel panel_of(const Inputs& in) {
  RiskPanel panel;
  panel.dates = in.dates; panel.instruments = in.names;
  panel.sessions = in.sessions; panel.ids = in.ids;
  panel.close = in.close; panel.raw_close = in.raw; panel.volume = in.volume;
  panel.present = in.present; panel.member = in.member; panel.cap = in.cap;
  panel.industry = in.industry;
  for (usize d = 0; d < descriptor_count; ++d) panel.descriptors[d] = in.descriptors[d];
  return panel;
}
// The executable and build that wrote the outputs (R1 m-16): the running image's SHA-256
// (null when it cannot be hashed) and the configure-time engine git SHA.
Json producer_json() {
  const auto exe = current_executable_sha256();
  return Json{{"executable_sha256", exe && !exe->empty() ? Json(*exe) : Json(nullptr)},
              {"engine_git_sha", std::string(build_engine_git_sha())}};
}
void print_bias(std::ostream& progress, const Json& bias) {
  for (const auto& item : bias.at("families").items()) {
    const Json& f = item.value();
    progress << "risk bias " << item.key() << ": " << f.at("status").get<std::string>()
             << " (series ok " << f.at("series_ok") << ", refused " << f.at("series_refused")
             << ", empty " << f.at("series_empty") << "); observations " << f.at("observations")
             << ", dropped_factor_exposures " << f.at("dropped_factor_exposures")
             << ", uncovered_name_returns " << f.at("uncovered_name_returns")
             << ", warmup_excluded " << f.at("warmup_excluded") << ", dropped share "
             << f.at("dropped_share") << " (max " << max_dropped_share << ")\n";
  }
}
void print_structural(std::ostream& progress, const Json& s) {
  progress << "risk structural: " << s.at("factors").size() << " factors structural on "
           << s.at("sessions_with_structural") << " sessions (" << s.at("factor_sessions")
           << " factor-sessions), scaled " << s.at("sessions_scaled") << " (min scale "
           << s.at("min_correlation_scale") << "); forecast sessions with an unforecast exposed "
           << "factor " << s.at("forecast_sessions_with_unforecast_exposure") << '\n';
}
Json risk_manifest(const RiskArgs& a, const Inputs& in, usize book_rows, const RiskModelConfig& cfg,
                   const Json& bias, Json run_stats, Json structural, Json robustness,
                   Json files) {
  Json descriptors_used = Json::array();
  for (usize d = 0; d < descriptor_count; ++d)
    if (!in.descriptors[d].empty()) descriptors_used.push_back(descriptor_names[d]);
  // The directory schema is unchanged (readers such as spo-v1 RiskStore accept it); the model
  // that filled it is `model` (v1 pins stay valid as v1 data directories).
  return Json{{"schema", "atx.risk-model/v1"}, {"status", "complete"}, {"model", risk_model_id},
      {"role", {{"path", a.role}, {"manifest_sha256", a.role_sha}}},
      {"fields", {{"path", a.fields}, {"manifest_sha256", a.fields_sha},
                  {"fields_used", in.fields_used}}},
      {"book_weights", a.book.empty() ? Json(nullptr)
                                      : Json{{"path", a.book}, {"sha256", a.book_sha},
                                             {"rows", book_rows}}},
      {"seal", {{"begin", "2023-01-01"}, {"role_last_session_ns", in.sessions.back()},
                {"unseal_owner", a.unseal.empty() ? Json(nullptr) : Json(a.unseal)}}},
      {"producer", producer_json()},
      {"geometry", {{"dates", in.dates}, {"instruments", in.names}, {"factors", factor_count},
                    {"styles", style_count}}},
      {"role_masks", {{"member_cells", in.member_cells},
                      {"member_absent_cells", in.member_absent_cells},
                      {"rule", "binary present/member masks; membership independent of "
                               "presence; the WLS and the standardization universe are members "
                               "present with cap (returns need both ends present); present "
                               "non-members get exposures and specific risk, never enter a fit"}}},
      {"cap_rule", in.cap_rule}, {"descriptors_used", std::move(descriptors_used)},
      {"unavailable", in.unavailable}, {"recipe", recipe_json(cfg)},
      {"bias_harness", {{"schema", bias.at("schema")}, {"random_portfolios", a.random},
                        {"seed", a.seed}, {"max_dropped_share", max_dropped_share},
                        {"families", bias.at("families")}}},
      {"run", std::move(run_stats)},
      {"structural_factors", std::move(structural)},
      {"robustness", std::move(robustness)},
      {"layouts", {{"factor_covariance.f64", "little-endian f64, dates x factors x factors "
                                            "row-major, forecast at the session's close for the "
                                            "next session, NaN where not forecast"},
                   {"factor_structural.u8", "dates x factors, 1 = the factor's forecast in "
                                           "factor_covariance.f64 at the session is structural "
                                           "(recipe.factor_covariance.structural), else 0"},
                   {"specific_variance.f64", "little-endian f64, dates x instruments, daily "
                                             "variance forecast, NaN where none"},
                   {"style_exposures.f32", "(--emit-exposures all) little-endian f32, dates x "
                                           "instruments x styles, 0 = missing"},
                   {"industry_slot.u8", "(--emit-exposures all) dates x instruments, FF49 id - 1, "
                                        "49 residual, 255 no exposure row"}}},
      {"files", std::move(files)}};
}

co::Status run(const RiskArgs& a, std::ostream& progress) {
  ATX_TRY_VOID(check_args(a));
  const RiskModelConfig cfg{};
  ATX_TRY_VOID(validate_config(cfg));
  Inputs in;
  ATX_TRY_VOID(load_role(a.role, a.role_sha, a.max_bytes, in));
  ATX_TRY_VOID(check_seal(in.sessions, a.unseal));
  ATX_TRY(auto fields, open_fields(a.fields, a.fields_sha, a.role, a.role_sha, in));
  ATX_TRY_VOID(derive(fields, in));
  std::vector<BookWeight> book;
  if (!a.book.empty()) { ATX_TRY(book, load_book(a.book, a.book_sha)); }
  const auto dir = std::filesystem::path(a.output);
  if (!std::filesystem::create_directory(dir))
    return co::Err(co::ErrorCode::AlreadyExists, "risk: output must not exist");
  const RiskPanel panel = panel_of(in);
  FileSink files_sink(dir, panel, a.emit);
  RobustnessSink robust_sink(panel, cfg);
  BiasHarness harness(panel, a.random, a.seed, book);
  std::array<RiskSink*, 3> sinks{&files_sink, &robust_sink, &harness};
  // An invariant refusal (F3) returns here: no manifest.json is written, the output is void.
  ATX_TRY_VOID(run_risk_model(panel, cfg, sinks));
  Json files = Json::object();
  ATX_TRY_VOID(files_sink.close(files));
  const Json robustness = robust_sink.json();
  Json bias;
  ATX_TRY_VOID(write_bias(dir, harness, robustness, files, bias));
  const Json structural = files_sink.structural_json();
  const Json manifest = risk_manifest(a, in, book.size(), cfg, bias, files_sink.run_stats(),
                                      structural, robustness, std::move(files));
  std::ofstream out(dir / "manifest.json", std::ios::binary);
  out << manifest.dump(2) << '\n';
  out.close();
  if (!out) return co::Err(co::ErrorCode::IoError, "risk: manifest.json");
  progress << "risk: " << in.dates << " sessions x " << in.names << " names, "
           << files_sink.run_stats().dump() << '\n';
  print_structural(progress, structural);
  print_robustness(progress, robustness);
  print_bias(progress, bias);
  return co::Ok();
}
} // namespace

int dispatch_risk_model(int argc, char** argv, std::ostream& out, std::ostream& err) {
  try {
    RiskArgs a;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--help") {
        out << "risk --role PATH/manifest.json --role-sha256 SHA --fields PATH/manifest.json "
               "--fields-sha256 SHA --output NEWDIR [--book-weights CSV (session|session_ns,"
               "instrument_id,weight|held_weight; L3 holdings.csv as is) --book-weights-sha256 SHA] [--random-portfolios 64] [--seed 7] "
               "[--emit-exposures none|last|all] [--max-bytes 1400000000] [--unseal OWNER (a "
               "role reaching 2023-01-01, the sealed VAL/holdout, is refused without it)]\n";
        return 0;
      }
      if (!seen.insert(key).second || i + 1 >= argc)
        throw std::invalid_argument("duplicate/missing flag " + key);
      const std::string value = argv[++i];
      const auto integer = [&]() {
        u64 x = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), x);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
          throw std::invalid_argument("invalid integer " + value);
        return x;
      };
      if (key == "--role") a.role = value;
      else if (key == "--role-sha256") a.role_sha = value;
      else if (key == "--fields") a.fields = value;
      else if (key == "--fields-sha256") a.fields_sha = value;
      else if (key == "--output") a.output = value;
      else if (key == "--book-weights") a.book = value;
      else if (key == "--book-weights-sha256") a.book_sha = value;
      else if (key == "--random-portfolios") a.random = static_cast<usize>(integer());
      else if (key == "--seed") a.seed = integer();
      else if (key == "--max-bytes") a.max_bytes = integer();
      else if (key == "--unseal") {
        if (value.empty()) throw std::invalid_argument("--unseal needs an owner name");
        a.unseal = value;
      } else if (key == "--emit-exposures") {
        if (value == "none") a.emit = ExposureEmit::None;
        else if (value == "last") a.emit = ExposureEmit::Last;
        else if (value == "all") a.emit = ExposureEmit::All;
        else throw std::invalid_argument("--emit-exposures none|last|all");
      } else throw std::invalid_argument("unknown flag " + key);
    }
    const auto status = run(a, out);
    if (!status) { err << status.error().to_string() << '\n'; return 1; }
    return 0;
  } catch (const std::exception& e) {
    err << "risk: " << e.what() << '\n';
    return 2;
  }
}
} // namespace atx::impl::strategy::risk
