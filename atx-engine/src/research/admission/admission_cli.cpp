#include "atx/engine/research/admission/admission_cli.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <new>
#include <set>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/admission/traded_horizon.hpp"

namespace atx::engine::research::admission {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr u64 kManifestLimit = 16ULL << 20;
constexpr u64 kCandidatesLimit = 64ULL << 20; // the fitter's admission.json may be passed whole
constexpr usize kMaxCandidates = 20000U;
constexpr usize kMaxDecisions = 4096U;

[[nodiscard]] core::Error invalid(const std::string &message) {
  return core::Error(core::ErrorCode::InvalidArgument, "atx-research-admission: " + message);
}

// Every byte of `path`, refused above `limit`.
[[nodiscard]] core::Result<std::string> read_file(const fs::path &path, u64 limit) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    return core::Err(core::ErrorCode::IoError, "atx-research-admission: cannot open " +
                                                   path.string());
  }
  const std::streamoff size = in.tellg();
  if (size < 0 || static_cast<u64>(size) > limit) {
    return core::Err(invalid("missing or oversized " + path.string()));
  }
  std::string out(static_cast<usize>(size), '\0');
  in.seekg(0);
  in.read(out.data(), static_cast<std::streamsize>(out.size()));
  if (!in) {
    return core::Err(core::ErrorCode::IoError, "atx-research-admission: cannot read " +
                                                   path.string());
  }
  return core::Ok(std::move(out));
}

// A little-endian f64 payload named in the manifest's files block, verified by extent and SHA.
[[nodiscard]] core::Result<std::vector<f64>> read_payload(const fs::path &dir, const Json &files,
                                                          const char *name, usize count) {
  if (!files.contains(name) || !files.at(name).is_object() ||
      !files.at(name).contains("sha256") || !files.at(name).at("sha256").is_string()) {
    return core::Err(invalid(std::string("factor manifest lacks files.") + name));
  }
  ATX_TRY(const std::string bytes, read_file(dir / name, static_cast<u64>(count) * sizeof(f64)));
  if (bytes.size() != count * sizeof(f64)) {
    return core::Err(invalid(std::string(name) + " extent differs from the manifest geometry"));
  }
  ATX_TRY(const std::string digest, core::sha256_hex(std::string_view(bytes)));
  if (digest != files.at(name).at("sha256").get<std::string>()) {
    return core::Err(invalid(std::string(name) + " SHA-256 differs from the manifest"));
  }
  std::vector<f64> out(count);
  if (count > 0U) {
    std::memcpy(out.data(), bytes.data(), bytes.size()); // little-endian hosts only (role reader)
  }
  return core::Ok(std::move(out));
}

[[nodiscard]] bool safe_text(const std::string &s) {
  return !s.empty() && s.find_first_of(",;\n\r") == std::string::npos;
}

[[nodiscard]] core::Result<std::string> required_string(const Json &row, const char *key) {
  if (!row.contains(key) || !row.at(key).is_string()) {
    return core::Err(invalid(std::string("candidate needs a string ") + key));
  }
  return core::Ok(row.at(key).get<std::string>());
}

[[nodiscard]] core::Result<i32> required_sign(const Json &row, const char *key, i32 lowest) {
  if (!row.contains(key) || !row.at(key).is_number_integer()) {
    return core::Err(invalid(std::string("candidate needs an integer ") + key));
  }
  const i64 value = row.at(key).get<i64>();
  if (value < lowest || value > 1) {
    return core::Err(invalid(std::string(key) + " out of range: " + std::to_string(value)));
  }
  return core::Ok(static_cast<i32>(value));
}

[[nodiscard]] core::Result<CandidateMeta> parse_candidate(const Json &row) {
  if (!row.is_object()) {
    return core::Err(invalid("a candidate is not an object"));
  }
  CandidateMeta m;
  ATX_TRY(m.id, required_string(row, "id"));
  ATX_TRY(m.family, required_string(row, "family"));
  ATX_TRY(m.theme, required_string(row, "theme"));
  ATX_TRY(m.cache_entry, required_string(row, "cache_entry"));
  ATX_TRY(m.cache_payload_sha256, required_string(row, "cache_payload_sha256"));
  if (!safe_text(m.id)) {
    return core::Err(invalid("candidate id empty or holding , ; or a newline: " + m.id));
  }
  // fit:577: v4 embeds the prior sign in the DSL, so -1 is refused.
  ATX_TRY(m.prior_sign, required_sign(row, "prior_sign", 0));
  ATX_TRY(m.runner_sign, required_sign(row, "runner_sign", -1));
  if (!row.contains("tier")) {
    return core::Err(invalid("candidate " + m.id + " lacks tier"));
  }
  const Json &tier = row.at("tier");
  if (tier.is_string()) {
    ATX_TRY(m.tier, tier_from_grade(tier.get<std::string>()));
  } else if (tier.is_number_integer() && tier.get<i64>() >= 0) {
    m.tier = tier_from_integer(tier.get<u64>());
  } else {
    return core::Err(invalid("tier of " + m.id + " must be a grade or an integer >= 0"));
  }
  return core::Ok(std::move(m));
}

// The output directory's files, written in order; the caller writes the manifest last.
[[nodiscard]] core::Status write_file(const fs::path &path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) {
    return core::Err(core::ErrorCode::IoError, "atx-research-admission: cannot write " +
                                                   path.string());
  }
  return core::Ok();
}

[[nodiscard]] core::Result<Json> file_entry(std::string_view bytes) {
  ATX_TRY(const std::string digest, core::sha256_hex(bytes));
  return core::Ok(Json{{"bytes", bytes.size()}, {"sha256", digest}});
}

// The statuses a screen can give, in the fitter's order (V4_STATUSES / V42_STATUSES).
[[nodiscard]] std::vector<Status> screen_statuses(ScreenId screen) {
  std::vector<Status> out;
  for (const Status s : {Status::Admitted, Status::RejectNoPrior, Status::RejectInsufficient,
                         Status::RejectTurnover, Status::RejectTurnoverCost, Status::RejectVeto,
                         Status::RejectRedundant}) {
    if (s != Status::RejectTurnoverCost || screen == ScreenId::V4PriorV2) {
      out.push_back(s);
    }
  }
  return out;
}

[[nodiscard]] bool is_grade(const Tier &tier) {
  return std::find(kTierGrades.begin(), kTierGrades.end(), tier.text) != kTierGrades.end();
}

[[nodiscard]] Json rules_json(const ScreenRules &rules, ScreenId screen, bool grades) {
  Json precedence = Json::array();
  for (const Status s : screen_statuses(screen)) {
    if (s != Status::Admitted) {
      precedence.push_back(std::string(status_name(s)));
    }
  }
  Json tier_order = Json("integer-ascending");
  if (grades) {
    tier_order = Json::array();
    for (const std::string_view g : kTierGrades) {
      tier_order.push_back(std::string(g));
    }
  }
  return Json{{"min_train_days", rules.min_train_days},
              {"tau_limit", rules.tau_limit},
              {"cost_tau_limit", rules.cost_tau_limit ? Json(*rules.cost_tau_limit) : Json()},
              {"veto_t", rules.veto_t},
              {"hac",
               Json{{"method", std::string(hac_method_name(rules.hac_method))},
                    {"kernel", "bartlett"},
                    {"lag", rules.hac_lag},
                    {"autocovariance_divisor", "n"},
                    {"small_sample_correction", false},
                    {"t", "mean/sqrt(LRV/n)"},
                    {"undefined", "n<2, constant series or LRV<=0 -> no veto"}}},
              {"rho_limit", rules.rho_limit},
              {"min_common_days", rules.min_common_days},
              {"redundancy", "survivors by (tier, roster order); |pearson rho| over TRAIN days "
                             "both live > rho_limit vs an admitted candidate -> reject_redundant "
                             "(largest |rho|); < min_common_days or undefined rho -> uncorrelated, "
                             "noted"},
              {"sharpe_annualization", rules.sharpe_annualization},
              {"status_precedence", std::move(precedence)},
              {"tier_order", std::move(tier_order)}};
}

[[nodiscard]] Json sign_rule_json(SignRule rule, std::span<const CandidateMeta> meta,
                                  std::span<const ScreenRow> rows,
                                  std::span<const usize> admitted) {
  Json stands = Json::array();
  Json fails = Json::array();
  for (const usize k : admitted) {
    const bool ok = admitted_sign_stands(rule, rows[k].s_k, meta[k].runner_sign);
    (ok ? stands : fails).push_back(meta[k].id);
  }
  return Json{{"name", std::string(sign_rule_name(rule))},
              {"predicate", "admitted_sign_stands(rule, prior_sign, runner_sign)"},
              {"admitted_sign_stands", std::move(stands)},
              {"admitted_sign_fails", std::move(fails)}};
}

struct Published {
  std::string admission_csv;
  std::string horizon_csv;
  Json manifest;
  std::vector<std::string> admitted;
};

[[nodiscard]] core::Result<std::vector<u8>> train_mask(const FactorSeriesInput &in) {
  std::vector<u8> mask(in.decisions, 0U);
  for (usize d = 0; d < in.decisions; ++d) {
    const i64 session = in.decision_sessions_ns[d];
    if (data::is_sealed(session)) {
      return core::Err(invalid("a decision session at or after the " +
                               std::string(data::kResearchWindowId) + " seal (" +
                               std::string(data::kSealBeginDate) + ")"));
    }
    mask[d] = static_cast<u8>(session >= data::kTrainBeginNs &&
                              session < data::kTrainEndExclusiveNs);
  }
  return core::Ok(std::move(mask));
}

[[nodiscard]] core::Result<Published> screen_outputs(const ScreenSpec &spec,
                                                     const FactorSeriesInput &in,
                                                     std::span<const CandidateMeta> meta,
                                                     std::string_view candidates_sha256) {
  if (meta.size() != in.ids.size()) {
    return core::Err(invalid("candidates and the factor series differ in count"));
  }
  std::vector<usize> tier_rank;
  std::vector<i32> prior_signs;
  for (usize k = 0; k < meta.size(); ++k) {
    if (meta[k].id != in.ids[k]) {
      return core::Err(invalid("candidate " + std::to_string(k) + " is " + meta[k].id +
                               " but the factor series lists " + in.ids[k]));
    }
    tier_rank.push_back(meta[k].tier.rank);
    prior_signs.push_back(meta[k].prior_sign);
  }
  ATX_TRY(const std::vector<u8> mask, train_mask(in));
  const ScreenInput input{meta.size(), in.decisions, in.factors, in.taus, mask, tier_rank,
                          prior_signs};
  const ScreenRules rules = screen_rules(spec.screen);
  ATX_TRY(const std::vector<ScreenRow> rows, screen_v4(input, rules));
  ATX_TRY(const std::vector<TradedHorizonRow> horizon,
          traded_horizon(meta.size(), in.decisions, in.factors_h21, mask, prior_signs));
  Published out;
  ATX_TRY(out.admission_csv, admission_csv(meta, rows));
  ATX_TRY(out.horizon_csv, traded_horizon_csv(meta, rows, horizon));
  const std::vector<usize> admitted = admitted_order(rows);
  Json counts = Json::object();
  for (const Status s : screen_statuses(spec.screen)) {
    counts[std::string(status_name(s))] =
        std::count_if(rows.begin(), rows.end(), [&](const ScreenRow &r) { return r.status == s; });
  }
  Json admitted_ids = Json::array();
  for (const usize k : admitted) {
    admitted_ids.push_back(meta[k].id);
    out.admitted.push_back(meta[k].id);
  }
  Json conflicts = Json::array();
  for (usize k = 0; k < meta.size(); ++k) {
    if (meta[k].runner_sign != rows[k].s_k) {
      conflicts.push_back(meta[k].id);
    }
  }
  const usize train_decisions =
      static_cast<usize>(std::count(mask.begin(), mask.end(), static_cast<u8>(1U)));
  ATX_TRY(Json csv_entry, file_entry(out.admission_csv));
  ATX_TRY(Json horizon_entry, file_entry(out.horizon_csv));
  out.manifest = Json{
      {"schema", std::string(kAdmissionManifestSchema)},
      {"status", "complete"},
      {"screen", std::string(screen_name(spec.screen))},
      {"rules", rules_json(rules, spec.screen, is_grade(meta.front().tier))},
      {"research_window", std::string(data::kResearchWindowId)},
      {"train_window_ns", Json::array({data::kTrainBeginNs, data::kTrainEndExclusiveNs})},
      {"train_decisions", train_decisions},
      {"inputs", Json{{"factors_manifest_sha256", in.manifest_sha256},
                      {"role_manifest_sha256", in.role_manifest_sha256},
                      {"candidates_sha256", std::string(candidates_sha256)}}},
      {"counts", std::move(counts)},
      {"admitted", std::move(admitted_ids)},
      {"sign_conflicts", std::move(conflicts)},
      {"sign_rule", sign_rule_json(spec.sign_rule, meta, rows, admitted)},
      {"traded_horizon",
       Json{{"sessions", kTradedHorizonSessions},
            {"series", "factor_h21.f64: the decision book held over the 21 sessions after its "
                       "fill session"},
            {"ic_h21", "mean of s_k*h over live TRAIN decisions"},
            {"ic_h21_hac_t", "eval::hac::mean_tstat(HorizonAwareV3, label horizon 21)"},
            {"use", "report only: gates, orders and weights nothing (F-3)"}}},
      {"files", Json{{"admission.csv", std::move(csv_entry)},
                     {"traded_horizon.csv", std::move(horizon_entry)}}}};
  return core::Ok(std::move(out));
}

[[nodiscard]] std::string usage() {
  return "usage: atx-research-admission screen --factors DIR --candidates FILE --output NEWDIR "
         "[--screen v4-prior-v1|v4-prior-v2] [--sign-rule gate-prior-v1|wave-zero-kept-v1]\n";
}

} // namespace

core::Result<FactorSeriesInput> read_factor_series(const fs::path &dir) {
  try {
    ATX_TRY(const std::string text, read_file(dir / "manifest.json", kManifestLimit));
    const Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("schema", std::string{}) !=
                                                  kFactorSeriesSchema ||
        j.value("status", std::string{}) != "complete") {
      return core::Err(invalid("not a complete atx.factor-series/v1 manifest: " + dir.string()));
    }
    FactorSeriesInput in;
    ATX_TRY(in.manifest_sha256, core::sha256_hex(std::string_view(text)));
    in.role_manifest_sha256 = j.value("role_manifest_sha256", std::string{});
    const Json &candidates = j.at("candidates");
    const Json &sessions = j.at("decision_sessions_ns");
    in.decisions = j.at("decisions").get<usize>();
    if (!candidates.is_array() || candidates.empty() || candidates.size() > kMaxCandidates ||
        in.decisions == 0U || in.decisions > kMaxDecisions || !sessions.is_array() ||
        sessions.size() != in.decisions) {
      return core::Err(invalid("factor manifest geometry"));
    }
    for (const Json &c : candidates) {
      in.ids.push_back(c.at("id").get<std::string>());
    }
    for (const Json &s : sessions) {
      in.decision_sessions_ns.push_back(s.get<i64>());
    }
    const usize cells = in.decisions * in.ids.size();
    const Json &files = j.at("files");
    ATX_TRY(in.factors, read_payload(dir, files, "factor.f64", cells));
    ATX_TRY(in.taus, read_payload(dir, files, "tau.f64", in.ids.size()));
    ATX_TRY(in.factors_h21, read_payload(dir, files, "factor_h21.f64", cells));
    return core::Ok(std::move(in));
  } catch (const std::bad_alloc &) {
    return core::Err(core::ErrorCode::OutOfRange, "atx-research-admission: allocation failed");
  } catch (const std::exception &e) {
    return core::Err(invalid(std::string("factor manifest: ") + e.what()));
  }
}

core::Result<std::vector<CandidateMeta>> parse_candidates(std::string_view text) {
  try {
    const Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("candidates") ||
        !j.at("candidates").is_array() || j.at("candidates").empty()) {
      return core::Err(invalid("candidates: needs an object with a non-empty candidates array"));
    }
    std::vector<CandidateMeta> out;
    std::set<std::string> seen;
    for (const Json &row : j.at("candidates")) {
      ATX_TRY(CandidateMeta m, parse_candidate(row));
      if (!seen.insert(m.id).second) {
        return core::Err(invalid("duplicate candidate " + m.id));
      }
      out.push_back(std::move(m));
    }
    const auto graded = [](const CandidateMeta &m) { return is_grade(m.tier); };
    if (!std::all_of(out.begin(), out.end(), graded) &&
        !std::none_of(out.begin(), out.end(), graded)) {
      return core::Err(invalid("tiers mix grades and integers"));
    }
    return core::Ok(std::move(out));
  } catch (const std::exception &e) {
    return core::Err(invalid(std::string("candidates: ") + e.what()));
  }
}

core::Result<std::vector<std::string>> run_screen(const ScreenSpec &spec) {
  try {
    std::error_code ec;
    if (fs::exists(spec.output_dir, ec) || ec) {
      return core::Err(core::ErrorCode::AlreadyExists,
                       "atx-research-admission: output must not exist: " +
                           spec.output_dir.string());
    }
    ATX_TRY(const FactorSeriesInput in, read_factor_series(spec.factors_dir));
    ATX_TRY(const std::string text, read_file(spec.candidates_file, kCandidatesLimit));
    ATX_TRY(const std::string candidates_sha256, core::sha256_hex(std::string_view(text)));
    ATX_TRY(const std::vector<CandidateMeta> meta, parse_candidates(text));
    ATX_TRY(Published out, screen_outputs(spec, in, meta, candidates_sha256));
    if (!fs::create_directory(spec.output_dir, ec) || ec) {
      return core::Err(core::ErrorCode::AlreadyExists,
                       "atx-research-admission: output must not exist: " +
                           spec.output_dir.string());
    }
    ATX_TRY_VOID(write_file(spec.output_dir / "admission.csv", out.admission_csv));
    ATX_TRY_VOID(write_file(spec.output_dir / "traded_horizon.csv", out.horizon_csv));
    ATX_TRY_VOID(write_file(spec.output_dir / "manifest.json", out.manifest.dump(2) + "\n"));
    return core::Ok(std::move(out.admitted));
  } catch (const std::bad_alloc &) {
    return core::Err(core::ErrorCode::OutOfRange, "atx-research-admission: allocation failed");
  } catch (const std::exception &e) {
    return core::Err(core::ErrorCode::Internal, std::string("atx-research-admission: ") + e.what());
  }
}

int research_admission_main(std::span<const std::string_view> args, std::ostream &out,
                            std::ostream &err) {
  if (args.size() < 2 || args[1] != "screen") {
    err << usage();
    return 2;
  }
  ScreenSpec spec;
  std::set<std::string_view> seen;
  for (usize i = 2; i < args.size(); i += 2) {
    const std::string_view key = args[i];
    if (i + 1 >= args.size() || !seen.insert(key).second) {
      err << "atx-research-admission: missing value or repeated flag " << key << "\n" << usage();
      return 2;
    }
    const std::string_view value = args[i + 1];
    if (key == "--factors") {
      spec.factors_dir = fs::path(value);
    } else if (key == "--candidates") {
      spec.candidates_file = fs::path(value);
    } else if (key == "--output") {
      spec.output_dir = fs::path(value);
    } else if (key == "--screen" && screen_from_name(value)) {
      spec.screen = *screen_from_name(value);
    } else if (key == "--sign-rule" && sign_rule_from_name(value)) {
      spec.sign_rule = *sign_rule_from_name(value);
    } else {
      err << "atx-research-admission: unknown argument " << key << " " << value << "\n"
          << usage();
      return 2;
    }
  }
  if (spec.factors_dir.empty() || spec.candidates_file.empty() || spec.output_dir.empty()) {
    err << usage();
    return 2;
  }
  const auto admitted = run_screen(spec);
  if (!admitted) {
    err << admitted.error().to_string() << "\n";
    return 1;
  }
  out << "atx-research-admission: " << screen_name(spec.screen) << " admitted "
      << admitted->size() << " -> " << spec.output_dir.string() << "\n";
  return 0;
}

} // namespace atx::engine::research::admission
