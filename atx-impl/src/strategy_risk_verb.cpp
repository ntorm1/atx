// The `risk` verb of atx-equity-strategy-risk: pinned role + fields -> atx-risk-v1 outputs and
// the bias harness (strategy_risk_model.hpp). Loads, checks and derives the descriptors,
// streams per-session outputs, writes manifest.json LAST.

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
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
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
  for (usize k = 0; k < c; ++k)
    if (in.present[k] > 1 || in.member[k] > 1 || (in.member[k] && !in.present[k]))
      return co::Err(co::ErrorCode::InvalidArgument, "risk: role presence/membership masks");
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

// Streams every session's outputs; the last session's exposures are kept for the CSV.
class FileSink final : public RiskSink {
public:
  FileSink(const std::filesystem::path& dir, const RiskPanel& panel, ExposureEmit emit)
      : dir_(dir), panel_(panel), emit_(emit),
        returns_(dir / "factor_returns.csv", std::ios::binary),
        diagnostics_(dir / "diagnostics.csv", std::ios::binary),
        covariance_(dir / "factor_covariance.f64", std::ios::binary),
        specific_(dir / "specific_variance.f64", std::ios::binary) {
    for (auto* s : {&returns_, &diagnostics_}) s->imbue(std::locale::classic());
    returns_ << "session,date_index,fitted,regression_rows,active_industries,r2";
    for (usize k = 0; k < factor_count; ++k) returns_ << ',' << factor_name(k);
    returns_ << '\n';
    diagnostics_ << "session,forecast,lambda2_factor,lambda2_specific,bias_factor,bias_specific,"
                    "structural_names,specific_names\n";
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
                 << d.specific_names << '\n';
    write_binary(covariance_, d.covariance);
    write_binary(specific_, d.specific_variance);
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
    if (!returns_ || !diagnostics_ || !covariance_ || !specific_)
      return co::Err(co::ErrorCode::IoError, "risk: output stream");
    return co::Ok();
  }
  co::Status close(Json& files) {
    for (auto* s : {&returns_, &diagnostics_, &covariance_, &specific_, &styles_, &slots_})
      if (s->is_open()) s->close();
    if (!returns_ || !diagnostics_ || !covariance_ || !specific_ || (emit_ == ExposureEmit::All && (!styles_ || !slots_)))
      return co::Err(co::ErrorCode::IoError, "risk: output close");
    std::vector<std::string> names{"factor_returns.csv", "diagnostics.csv", "factor_covariance.f64",
                                   "specific_variance.f64"};
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

private:
  std::filesystem::path dir_;
  const RiskPanel& panel_;
  ExposureEmit emit_{};
  std::ofstream returns_, diagnostics_, covariance_, specific_, styles_, slots_;
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

// USE4 Appendix A summary of one family: full-sample b per series against 1 +- sqrt(2/T) and
// the kurtosis-widened band (pooled kurtosis of the family's z), and rolling 63/252 windows.
Json family_summary(const std::vector<const BiasSeries*>& family) {
  std::vector<f64> pooled;
  for (const auto* s : family) pooled.insert(pooled.end(), s->z.begin(), s->z.end());
  const f64 kurtosis = sample_kurtosis(pooled);
  std::vector<f64> full;
  usize in_band = 0, in_kband = 0, missing = 0, uncovered = 0;
  for (const auto* s : family) {
    const f64 b = bias_statistic(s->z);
    full.push_back(b);
    in_band += inside(b, bias_band(s->z.size())) ? 1U : 0U;
    in_kband += inside(b, kurtosis_band(s->z.size(), kurtosis)) ? 1U : 0U;
    missing += s->missing_returns; uncovered += s->uncovered_names;
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
  return Json{{"series", family.size()}, {"pooled_kurtosis", finite_or_null(kurtosis)},
      {"full_sample", {{"b_mean", count ? Json(sum / static_cast<f64>(count)) : Json(nullptr)},
                       {"b_median", finite_or_null(median_of(full))},
                       {"series_in_band", in_band}, {"series_in_kurtosis_band", in_kband}}},
      {"rolling", std::move(rolling)}, {"missing_returns", missing},
      {"uncovered_book_names", uncovered}};
}

co::Status write_bias(const std::filesystem::path& dir, const BiasHarness& harness, Json& files,
                      Json& summary) {
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
  summary = Json{{"schema", "atx.risk-bias/v1"},
      {"definition", "z_t = realized return over (t-1, t] / forecast vol at t-1 (x'Fx + sum "
                     "w^2 D); bias statistic b = sample SD of z over a window of T; band 1 +- "
                     "sqrt(2/T); kurtosis-widened band 1 +- 1.96 sqrt((k-1)/(4T)) with k the "
                     "family's pooled kurtosis (USE4 Appendix A, asymptotic)"},
      {"families", std::move(fam)}};
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
  return Json{{"model", "atx-risk-v1"},
      {"factors", std::move(factors)},
      {"factor_covariance", {{"vol_halflife", c.vol_halflife},
          {"variance_nw_lags", c.variance_nw_lags}, {"correlation_halflife", c.correlation_halflife},
          {"correlation_nw_lags", c.correlation_nw_lags}, {"nw_halflife", c.nw_halflife},
          {"vra_halflife", c.vra_halflife}, {"vra_min_dates", c.vra_min_dates},
          {"min_factor_history", c.min_factor_history},
          {"source", "USE4S (Menchero-Orr-Wang 2011): 84/504, NW 5/2, VRA 42; estimator = atx-engine "
                     "risk V2 cov_ewma (recursive twin); PSD eigenvalue floor"}}},
      {"specific_risk", {{"halflife", c.specific_halflife}, {"nw_lags", c.specific_nw_lags},
          {"structural_history", c.structural_history}, {"bayesian_q", c.bayesian_q},
          {"rule", "sigma = gamma sigma_TS + (1-gamma) sigma_STR, gamma = min(1, h/252), "
                   "sigma_STR = E0 exp(x b) from the constrained WLS of ln sigma_TS on X_t over "
                   "full-history eligible names (sqrt-cap weights), then Bayesian shrinkage to the "
                   "cap-weighted size-decile mean (engine specific_risk_v2 formula), then VRA"}}},
      {"exposures", {{"min_industry_names", c.min_industry_names}, {"beta_window", c.beta_window},
          {"min_beta_pairs", c.min_beta_pairs}, {"momentum_window", c.momentum_window},
          {"momentum_skip", c.momentum_skip}, {"adv_window", c.adv_window},
          {"winsor_sd", c.winsor_sd}, {"clip_z", c.clip_z},
          {"standardization", "cap-weighted mean 0, equal-weighted SD 1 over eligible names "
                              "(member, present, cap > 0), winsorized then clipped, missing 0"},
          {"styles", "size ln cap; beta and residual_vol from the rolling 252-interval OLS on the "
                     "equal-weight member market (>= 126 pairs); momentum ln(close[t-21] / "
                     "close[t-252]); value be/cap; earnings_yield ni_ttm/cap; profitability "
                     "gp_ttm/at; asset_growth at/at_lag4 - 1; leverage lt/at; liquidity ln mean "
                     "raw$ volume over 63 sessions; short_interest = mean of z(si/shares_out) and "
                     "z(si_dtc), re-standardized"}}},
      {"regression", {{"min_regression_names", c.min_regression_names},
          {"rule", "session-t simple returns on X_{t-1}, WLS weights sqrt(cap_{t-1}), market + "
                   "active industries + styles, sum_k cap_k f_k = 0 over active industries"}}}};
}

struct RiskArgs {
  std::string role, role_sha, fields, fields_sha, output, book, book_sha;
  usize random{64};
  u64 seed{7}, max_bytes{1'400'000'000ULL};
  ExposureEmit emit{ExposureEmit::Last};
};

co::Status run(const RiskArgs& a, std::ostream& progress) {
  if (a.role.empty() || a.fields.empty() || a.output.empty())
    return co::Err(co::ErrorCode::InvalidArgument, "risk: --role, --fields and --output are required");
  if (a.book.empty() != a.book_sha.empty())
    return co::Err(co::ErrorCode::InvalidArgument, "risk: --book-weights and its SHA go together");
  if (a.random > 1024) return co::Err(co::ErrorCode::InvalidArgument, "risk: at most 1024 random portfolios");
  const RiskModelConfig cfg{};
  ATX_TRY_VOID(validate_config(cfg));
  Inputs in;
  ATX_TRY_VOID(load_role(a.role, a.role_sha, a.max_bytes, in));
  ATX_TRY(auto fields, open_fields(a.fields, a.fields_sha, a.role, a.role_sha, in));
  ATX_TRY_VOID(derive(fields, in));
  std::vector<BookWeight> book;
  if (!a.book.empty()) { ATX_TRY(book, load_book(a.book, a.book_sha)); }
  const auto dir = std::filesystem::path(a.output);
  if (!std::filesystem::create_directory(dir))
    return co::Err(co::ErrorCode::AlreadyExists, "risk: output must not exist");
  RiskPanel panel;
  panel.dates = in.dates; panel.instruments = in.names;
  panel.sessions = in.sessions; panel.ids = in.ids;
  panel.close = in.close; panel.raw_close = in.raw; panel.volume = in.volume;
  panel.present = in.present; panel.member = in.member; panel.cap = in.cap;
  panel.industry = in.industry;
  for (usize d = 0; d < descriptor_count; ++d) panel.descriptors[d] = in.descriptors[d];
  FileSink files_sink(dir, panel, a.emit);
  BiasHarness harness(panel, a.random, a.seed, book);
  std::array<RiskSink*, 2> sinks{&files_sink, &harness};
  ATX_TRY_VOID(run_risk_model(panel, cfg, sinks));
  Json files = Json::object();
  ATX_TRY_VOID(files_sink.close(files));
  Json bias;
  ATX_TRY_VOID(write_bias(dir, harness, files, bias));
  Json descriptors_used = Json::array();
  for (usize d = 0; d < descriptor_count; ++d)
    if (!in.descriptors[d].empty()) descriptors_used.push_back(descriptor_names[d]);
  Json manifest{{"schema", "atx.risk-model/v1"}, {"status", "complete"},
      {"role", {{"path", a.role}, {"manifest_sha256", a.role_sha}}},
      {"fields", {{"path", a.fields}, {"manifest_sha256", a.fields_sha},
                  {"fields_used", in.fields_used}}},
      {"book_weights", a.book.empty() ? Json(nullptr)
                                      : Json{{"path", a.book}, {"sha256", a.book_sha},
                                             {"rows", book.size()}}},
      {"geometry", {{"dates", in.dates}, {"instruments", in.names}, {"factors", factor_count},
                    {"styles", style_count}}},
      {"cap_rule", in.cap_rule}, {"descriptors_used", std::move(descriptors_used)},
      {"unavailable", in.unavailable}, {"recipe", recipe_json(cfg)},
      {"bias_harness", {{"random_portfolios", a.random}, {"seed", a.seed},
                        {"families", bias.at("families")}}},
      {"run", files_sink.run_stats()},
      {"layouts", {{"factor_covariance.f64", "little-endian f64, dates x factors x factors "
                                            "row-major, forecast at the session's close for the "
                                            "next session, NaN where not forecast"},
                   {"specific_variance.f64", "little-endian f64, dates x instruments, daily "
                                             "variance forecast, NaN where none"},
                   {"style_exposures.f32", "(--emit-exposures all) little-endian f32, dates x "
                                           "instruments x styles, 0 = missing"},
                   {"industry_slot.u8", "(--emit-exposures all) dates x instruments, FF49 id - 1, "
                                        "49 residual, 255 no exposure row"}}},
      {"files", std::move(files)}};
  std::ofstream out(dir / "manifest.json", std::ios::binary);
  out << manifest.dump(2) << '\n';
  out.close();
  if (!out) return co::Err(co::ErrorCode::IoError, "risk: manifest.json");
  progress << "risk: " << in.dates << " sessions x " << in.names << " names, "
           << files_sink.run_stats().dump() << '\n';
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
               "[--emit-exposures none|last|all] [--max-bytes 1400000000]\n";
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
      else if (key == "--emit-exposures") {
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
