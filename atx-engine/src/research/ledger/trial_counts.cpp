#include "atx/engine/research/ledger/trial_counts.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <system_error>

#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/ledger/era_pool.hpp"
#include "atx/engine/research/numpy_reduce.hpp"

namespace atx::engine::research::ledger {
namespace {

using Json = nlohmann::json;

// rec.get(key): the value, or null when absent.
[[nodiscard]] const Json &get(const Json &rec, const char *key) {
  static const Json null_value;
  if (!rec.is_object()) {
    return null_value;
  }
  const auto it = rec.find(key);
  return it == rec.end() ? null_value : *it;
}

// `value == text` for a JSON value and a Python str.
[[nodiscard]] bool is_text(const Json &value, std::string_view text) {
  return value.is_string() && value.get_ref<const std::string &>() == text;
}

// Python truthiness of a JSON value.
[[nodiscard]] bool truthy(const Json &v) {
  if (v.is_null()) {
    return false;
  }
  if (v.is_boolean()) {
    return v.get<bool>();
  }
  if (v.is_number_unsigned()) {
    return v.get<u64>() != 0U;
  }
  if (v.is_number_integer()) {
    return v.get<i64>() != 0;
  }
  if (v.is_number_float()) {
    return v.get<f64>() != 0.0; // NaN is truthy in Python, and NaN != 0
  }
  if (v.is_string() || v.is_array() || v.is_object()) {
    return !v.empty() && !(v.is_string() && v.get_ref<const std::string &>().empty());
  }
  return true;
}

// The identity of a JSON value in a Python set (None reads "null").
[[nodiscard]] std::string key_of(const Json &v) { return v.dump(); }

// Python int() of a JSON value.
[[nodiscard]] core::Result<i64> python_int(const Json &v, const char *what) {
  if (v.is_boolean()) {
    return core::Ok(static_cast<i64>(v.get<bool>() ? 1 : 0));
  }
  if (v.is_number_unsigned()) {
    const u64 u = v.get<u64>();
    if (u > static_cast<u64>(std::numeric_limits<i64>::max())) {
      return core::Err(core::ErrorCode::InvalidArgument,
                       std::string("trial ledger: ") + what + " out of range");
    }
    return core::Ok(static_cast<i64>(u));
  }
  if (v.is_number_integer()) {
    return core::Ok(v.get<i64>());
  }
  if (v.is_number_float()) {
    const f64 x = std::trunc(v.get<f64>());
    if (!std::isfinite(x) || std::abs(x) > 9.0e18) {
      return core::Err(core::ErrorCode::InvalidArgument,
                       std::string("trial ledger: ") + what + " is not an integer");
    }
    return core::Ok(static_cast<i64>(x));
  }
  if (v.is_string()) {
    std::string_view s = v.get_ref<const std::string &>();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
      s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
      s.remove_suffix(1);
    }
    if (!s.empty() && s.front() == '+') {
      s.remove_prefix(1);
    }
    i64 out = 0;
    const auto parsed = std::from_chars(s.data(), s.data() + s.size(), out);
    if (!s.empty() && parsed.ec == std::errc{} && parsed.ptr == s.data() + s.size()) {
      return core::Ok(out);
    }
  }
  return core::Err(core::ErrorCode::InvalidArgument,
                   std::string("trial ledger: ") + what + " is not an integer: " + v.dump());
}

[[nodiscard]] const Json &window_label(const Json &rec) {
  return get(get(rec, "window"), "label");
}

[[nodiscard]] bool basis_is(const Json &rec, std::string_view basis) {
  return is_text(get(rec, "rerun_basis"), basis);
}

// The research window's TRAIN begin as YYYY-MM-DD (the window module's own date).
[[nodiscard]] std::string train_begin_date() {
  constexpr i64 kDayNs = 86'400'000'000'000LL;
  return iso_date_from_days(data::kTrainBeginNs / kDayNs);
}

} // namespace

bool is_event(const Record &rec) {
  const Json &kind = get(rec, "kind");
  return is_text(kind, kProtocol) || is_text(kind, kDefect) || is_text(kind, kValidation);
}

bool is_campaign(const Record &rec) { return is_text(get(rec, "kind"), kMiningCampaign); }

bool is_era_line(const Record &rec) { return rec.is_object() && rec.contains("era_of"); }

bool is_pool_line(const Record &rec) { return is_text(window_label(rec), kPoolWindow); }

bool is_history_line(const Record &rec) {
  const Json &label = window_label(rec);
  const bool era_window =
      label.is_string() && label.get_ref<const std::string &>().starts_with("ERA ");
  return (rec.is_object() && rec.contains("era")) || is_era_line(rec) || era_window ||
         is_pool_line(rec);
}

bool begins_before_train(const Record &rec) {
  const Json &first = get(get(rec, "window"), "first_session");
  return first.is_string() && first.get_ref<const std::string &>() < train_begin_date();
}

std::vector<usize> history_read_lines(Records records) {
  std::set<std::string> pools;
  for (const Record &r : records) {
    if (is_pool_line(r)) {
      pools.insert(key_of(get(r, "trial_id")));
    }
  }
  std::vector<usize> out;
  for (usize i = 0; i < records.size(); ++i) {
    const Record &r = records[i];
    const bool pool_read = is_pool_line(r) && begins_before_train(r);
    const bool era_read = r.is_object() && r.contains("era") && !is_pool_line(r) &&
                          !pools.contains(key_of(get(r, "era_of")));
    if (pool_read || era_read) {
      out.push_back(i);
    }
  }
  return out;
}

std::set<std::string> invalid_ids(Records records) {
  std::set<std::string> out;
  for (const Record &r : records) {
    const Json &defect = get(r, "defect");
    if (defect.is_object() && truthy(get(defect, "invalid")) && !is_event(r)) {
      out.insert(key_of(get(r, "trial_id")));
    }
  }
  for (const Record &r : records) {
    if (is_text(get(r, "kind"), kDefect)) {
      out.insert(key_of(get(r, "defect_of")));
    }
  }
  return out;
}

core::Result<std::vector<i64>> trial_counts(Records records) {
  std::set<std::string> rerun;
  for (const Record &r : records) {
    if (basis_is(r, "blind") || basis_is(r, "returns")) {
      rerun.insert(key_of(get(r, "rerun_of")));
    }
  }
  const std::set<std::string> invalid = invalid_ids(records);
  const std::vector<usize> reads_list = history_read_lines(records);
  const std::set<usize> reads(reads_list.begin(), reads_list.end());
  std::vector<i64> out;
  out.reserve(records.size());
  for (usize i = 0; i < records.size(); ++i) {
    const Record &rec = records[i];
    const std::string tid = key_of(get(rec, "trial_id"));
    if (is_event(rec) || basis_is(rec, "window") || basis_is(rec, "blind") ||
        is_era_line(rec) || is_campaign(rec) || reads.contains(i)) {
      out.push_back(0);
    } else if (invalid.contains(tid) && !rerun.contains(tid)) {
      out.push_back(0);
    } else if (!rec.contains("count")) {
      out.push_back(1);
    } else {
      ATX_TRY(const i64 count, python_int(get(rec, "count"), "count"));
      out.push_back(count);
    }
  }
  return core::Ok(std::move(out));
}

core::Result<i64> ledger_n(Records records, bool scored_in_ledger, std::string_view kind) {
  ATX_TRY(const std::vector<i64> counts, trial_counts(records));
  i64 n = 0;
  for (usize i = 0; i < records.size(); ++i) {
    if (!records[i].contains("kind") || is_text(get(records[i], "kind"), kind)) {
      n += counts[i];
    }
  }
  return core::Ok(n + (scored_in_ledger ? 0 : 1));
}

std::vector<usize> excluded_lines(Records records) {
  std::set<std::string> seen;
  std::set<std::string> replaced;
  for (const Record &r : records) {
    if (basis_is(r, "returns")) {
      seen.insert(key_of(get(r, "rerun_of")));
    }
    if (basis_is(r, "blind")) {
      replaced.insert(key_of(get(r, "rerun_of")));
    }
  }
  const std::set<std::string> invalid = invalid_ids(records);
  std::vector<usize> out;
  for (usize i = 0; i < records.size(); ++i) {
    const std::string tid = key_of(get(records[i], "trial_id"));
    if (!is_event(records[i]) && !seen.contains(tid) &&
        (invalid.contains(tid) || replaced.contains(tid))) {
      out.push_back(i);
    }
  }
  return out;
}

core::Result<i64> campaign_registry_count(Records records) {
  i64 total = 0;
  for (const Record &r : records) {
    if (!is_campaign(r)) {
      continue;
    }
    const Json &registry = get(r, "registry");
    if (!truthy(registry)) {
      continue;
    }
    if (!registry.is_object()) {
      return core::Err(core::ErrorCode::InvalidArgument,
                       "trial ledger: a campaign line's registry is not an object");
    }
    if (!registry.contains("count")) {
      continue;
    }
    ATX_TRY(const i64 count, python_int(registry.at("count"), "registry.count"));
    total += count;
  }
  return core::Ok(total);
}

core::Result<TrialBreakdown> trial_breakdown(Records records) {
  ATX_TRY(const std::vector<i64> counts, trial_counts(records));
  std::map<std::string, i64> by_kind;
  TrialBreakdown out;
  for (usize i = 0; i < records.size(); ++i) {
    out.ledger_trials += counts[i];
    if (counts[i] == 0) {
      continue;
    }
    std::string kind(kConstruction);
    if (records[i].contains("kind")) {
      const Json &k = get(records[i], "kind");
      kind = k.is_string() ? k.get<std::string>() : k.dump();
    }
    by_kind[kind] += counts[i];
  }
  out.by_kind.assign(by_kind.begin(), by_kind.end());
  ATX_TRY(out.campaign_registry, campaign_registry_count(records));
  out.n_tot = out.ledger_trials + out.campaign_registry;
  return core::Ok(std::move(out));
}

core::Result<DsrVariance> dsr_variance(Records records, std::string_view window_id,
                                       std::string_view kind) {
  ATX_TRY(const std::vector<i64> counts, trial_counts(records));
  std::set<std::string> out_ids;
  for (const usize i : excluded_lines(records)) {
    out_ids.insert(key_of(get(records[i], "trial_id")));
  }
  const f64 root = std::sqrt(kAnnualization);
  std::vector<f64> cur;
  std::vector<f64> old;
  DsrVariance out;
  out.window_id = std::string(window_id);
  for (usize i = 0; i < records.size(); ++i) {
    const Record &r = records[i];
    if (!is_text(get(r, "kind"), kind)) {
      continue;
    }
    out.n += counts[i];
    if (out_ids.contains(key_of(get(r, "trial_id"))) || is_history_line(r)) {
      continue;
    }
    const Json &s2 = get(r, "s2_net_sr");
    if (!s2.is_number() && !s2.is_boolean()) {
      continue;
    }
    const f64 sr = s2.is_boolean() ? (s2.get<bool>() ? 1.0 : 0.0) : s2.get<f64>();
    if (!std::isfinite(sr)) {
      continue;
    }
    if (is_text(get(r, "window_id"), window_id)) {
      cur.push_back(sr / root);
    }
    if (!r.contains("window_id")) {
      old.push_back(sr / root);
    }
  }
  out.cells = cur.size();
  out.legacy_cells = old.size();
  if (cur.size() >= 2U) {
    out.variance_sr = numpy::var(cur, 1U);
  }
  if (old.size() >= 2U) {
    out.legacy_variance_sr = numpy::var(old, 1U);
  }
  out.variance_source = "sample variance (ddof 1) of " + std::to_string(cur.size()) +
                        " per-session S2 net SRs ledgered on " + std::string(window_id) +
                        " (re-runs and new cells; defect-rule exclusions left out)";
  return core::Ok(std::move(out));
}

std::set<std::string> construction_ids(Records records) {
  std::set<std::string> out;
  for (const Record &r : records) {
    const Json &tid = get(r, "trial_id");
    if (is_text(get(r, "kind"), kConstruction) && tid.is_string()) {
      out.insert(tid.get<std::string>());
    }
  }
  return out;
}

std::optional<usize> construction_prefix_length(Records records, std::string_view trial_id) {
  for (usize k = 0; k < records.size(); ++k) {
    if (is_text(get(records[k], "kind"), kConstruction) &&
        is_text(get(records[k], "trial_id"), trial_id)) {
      return k + 1U;
    }
  }
  return std::nullopt;
}

} // namespace atx::engine::research::ledger
