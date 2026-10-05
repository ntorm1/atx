// atx-research-composition fit (composition_cli.hpp; P9 lane D2).
#include "atx/engine/research/composition/composition_cli.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <ios>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/admission/admission_cli.hpp"

namespace atx::engine::research::composition {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr u64 kSpecLimit = 16ULL << 20;

[[nodiscard]] core::Error invalid(const std::string& message) {
  return core::Error(core::ErrorCode::InvalidArgument, "atx-research-composition: " + message);
}

// Every byte of `path`, refused above `limit`.
[[nodiscard]] core::Result<std::string> read_file(const fs::path& path, u64 limit) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    return core::Err(core::ErrorCode::IoError,
                     "atx-research-composition: cannot open " + path.string());
  }
  const std::streamoff size = in.tellg();
  if (size < 0 || static_cast<u64>(size) > limit) {
    return core::Err(invalid("missing or oversized " + path.string()));
  }
  std::string out(static_cast<usize>(size), '\0');
  in.seekg(0);
  in.read(out.data(), static_cast<std::streamsize>(out.size()));
  if (!in) {
    return core::Err(core::ErrorCode::IoError,
                     "atx-research-composition: cannot read " + path.string());
  }
  return core::Ok(std::move(out));
}

// The TRAIN decisions of the series (admission_cli's rule): a sealed decision is refused.
[[nodiscard]] core::Result<std::vector<u8>> train_mask(const admission::FactorSeriesInput& in) {
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

[[nodiscard]] Json passes_json(std::span<const combine::GroupCapPass> passes,
                               std::span<const std::string> ids,
                               std::span<const std::string> themes) {
  Json out = Json::array();
  for (const auto& pass : passes) {
    Json capped = Json::array();
    for (const usize k : pass.capped) capped.push_back(ids[k]);
    Json excess = Json::object();
    for (usize g = 0; g < pass.excess.size() && g < themes.size(); ++g) {
      if (pass.excess[g] > 0.0) excess[themes[g]] = pass.excess[g];
    }
    out.push_back(Json{{"capped", std::move(capped)}, {"excess_by_theme", std::move(excess)}});
  }
  return out;
}

[[nodiscard]] Json theme_names(std::span<const usize> index, std::span<const std::string> themes) {
  Json out = Json::array();
  for (const usize t : index) out.push_back(themes[t]);
  return out;
}

[[nodiscard]] Json fit_json(const CompositionFit& fit, const FitSpec& spec,
                            const admission::FactorSeriesInput& in,
                            const std::string& spec_sha256) {
  std::vector<std::string> ids;
  for (const auto& m : spec.members) ids.push_back(m.id);
  const std::span<const std::string> themes(fit.themes);
  Json parent{{"rule", spec.composition},
              {"weights", fit.parent_weights},
              {"weights_before_cap", fit.parent_uncapped},
              {"cap", fit.parent_cap},
              {"cap_iterations", passes_json(fit.parent_passes, ids, themes)}};
  if (!fit.tiers.empty()) {
    Json regrades = Json::array();
    for (const auto& r : fit.regrades) {
      Json row{{"id", r.id}, {"from", r.from}, {"to", r.to}, {"status", r.status}};
      if (r.member) row["source_tier"] = r.source_tier;
      regrades.push_back(std::move(row));
    }
    parent["member_tiers"] = fit.tiers;
    parent["member_scores"] = fit.scores;
    parent["tier_regrades"] = std::move(regrades);
  } else {
    Json means = Json::object();
    Json equal = Json::array();
    for (usize t = 0; t < themes.size(); ++t) {
      means[themes[t]] = fit.theme_mean[t];
      if (fit.equal_theme[t] != 0U) equal.push_back(themes[t]);
    }
    parent["ic"] = fit.ic;
    parent["shrunk_ic"] = fit.shrunk;
    parent["within_theme_shares"] = fit.share;
    parent["theme_mean_ic"] = std::move(means);
    parent["equal_share_themes"] = std::move(equal);
  }
  Json member_themes = Json::array();
  for (const usize t : fit.member_theme) member_themes.push_back(themes[t]);
  Json doc{{"schema", std::string(kFitSchema)},
           {"status", "complete"},
           {"composition", spec.composition},
           {"research_window", std::string(data::kResearchWindowId)},
           {"inputs",
            Json{{"factors_manifest_sha256", in.manifest_sha256},
                 {"role_manifest_sha256", in.role_manifest_sha256},
                 {"spec_sha256", spec_sha256}}},
           {"decisions", in.decisions},
           {"train_decisions", fit.train_decisions},
           {"members", ids},
           {"themes", fit.themes},
           {"member_themes", std::move(member_themes)},
           {"parent", std::move(parent)},
           {"weights", fit.weights}};
  if (fit.erc) {
    Json covariance = Json::array();
    for (usize i = 0; i < themes.size(); ++i) {
      Json row = Json::array();
      for (usize j = 0; j < themes.size(); ++j) {
        row.push_back(fit.covariance[i * themes.size() + j]);
      }
      covariance.push_back(std::move(row));
    }
    doc["theme_erc"] = Json{{"within_theme_shares", fit.erc_share},
                            {"theme_order", fit.themes},
                            {"covariance", std::move(covariance)},
                            {"decisions", fit.train_decisions},
                            {"theme_shares", fit.erc_fit.theme_share},
                            {"risk_contributions", fit.erc_fit.contribution},
                            {"dispersion", fit.erc_fit.dispersion},
                            {"cap", fit.erc_fit.cap},
                            {"weights_before_cap", fit.erc_fit.uncapped},
                            {"weights", fit.erc_fit.weights},
                            {"cap_iterations", passes_json(fit.erc_fit.passes, ids, themes)}};
  }
  if (fit.tsmom) {
    Json blocks = Json::array();
    for (const auto& b : fit.schedule.blocks) {
      blocks.push_back(Json{{"decision_index", b.decision_index},
                            {"from_session", b.from_session},
                            {"trailing", b.trailing},
                            {"masses", b.masses},
                            {"themes_off", b.off}});
    }
    doc["theme_tsmom"] = Json{{"theme_order", theme_names(fit.schedule.themes, themes)},
                              {"parent_masses", fit.schedule.parent_mass},
                              {"blocks", std::move(blocks)},
                              {"theme_blocks_off", fit.schedule.off},
                              {"theme_on_count", fit.schedule.on_count}};
  }
  if (fit.resid) doc["theme_resid"] = Json{{"order", fit.resid_order}};
  return doc;
}

// P9 lane D3W: the walk-forward rows, each row's final weights in member order.
[[nodiscard]] Json walk_forward_json(const WalkForwardSpec& wf,
                                     std::span<const WalkForwardRow> rows) {
  Json out = Json::array();
  for (const auto& row : rows) {
    out.push_back(Json{{"decision_index", row.decision_index},
                       {"from_session", row.from_session},
                       {"fit_decisions", row.fit_decisions},
                       {"last_fit_index", row.last_fit_index},
                       {"last_fit_session", row.last_fit_session},
                       {"weights", row.fit.weights}});
  }
  return Json{{"rule", std::string(walk_forward_rule)},
              {"window", wf.window},
              {"lag", wf.lag},
              {"step", wf.step},
              {"min_decisions", wf.min_decisions},
              {"rows", std::move(out)}};
}

// A walk-forward count: decimal digits only, at most 9 of them (no sign, no blank).
[[nodiscard]] std::optional<usize> count_value(std::string_view text) {
  if (text.empty() || text.size() > 9U ||
      !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  usize value = 0;
  for (const char c : text) value = value * 10U + static_cast<usize>(c - '0');
  return value;
}

[[nodiscard]] core::Result<bool> optional_bool(const Json& j, const char* key) {
  if (!j.contains(key)) return core::Ok(false);
  if (!j.at(key).is_boolean()) {
    return core::Err(invalid(std::string("spec ") + key + " must be a boolean"));
  }
  return core::Ok(j.at(key).get<bool>());
}

// A spec member row: {id: string, theme: string, sign: integer, tier?: string, gain?: number}.
[[nodiscard]] bool member_row_shape(const Json& row) {
  return row.is_object() && row.contains("id") && row.at("id").is_string() &&
         row.contains("theme") && row.at("theme").is_string() && row.contains("sign") &&
         row.at("sign").is_number_integer() &&
         (!row.contains("tier") || row.at("tier").is_string()) &&
         (!row.contains("gain") || row.at("gain").is_number());
}

[[nodiscard]] std::string usage() {
  return "usage: atx-research-composition fit --factors DIR --spec FILE --output NEWDIR "
         "[--walk-forward expanding --lag N --step N [--min-decisions N]]\n";
}

} // namespace

core::Result<FitSpec> parse_fit_spec(std::string_view text) {
  try {
    const Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("schema") ||
        !j.at("schema").is_string() || j.at("schema").get<std::string>() != kFitSpecSchema ||
        !j.contains("composition") || !j.at("composition").is_string() ||
        !j.contains("members") || !j.at("members").is_array() || j.at("members").empty()) {
      return core::Err(invalid("spec must be an atx.composition-fit-spec/v1 object with a "
                               "composition and a non-empty members array"));
    }
    FitSpec spec;
    spec.composition = j.at("composition").get<std::string>();
    for (const Json& row : j.at("members")) {
      if (!member_row_shape(row)) {
        return core::Err(invalid("spec member needs {id: string, theme: string, sign: integer, "
                                 "tier?: string, gain?: number}"));
      }
      FitSpecMember m;
      m.id = row.at("id").get<std::string>();
      m.theme = row.at("theme").get<std::string>();
      const i64 sign = row.at("sign").get<i64>();
      if (sign < -1 || sign > 1) {
        return core::Err(invalid("spec member sign out of range: " + m.id));
      }
      m.sign = static_cast<i32>(sign);
      if (row.contains("tier")) m.tier = row.at("tier").get<std::string>();
      if (row.contains("gain")) m.gain = row.at("gain").get<f64>();
      spec.members.push_back(std::move(m));
    }
    if (j.contains("tier_scores")) {
      const Json& scores = j.at("tier_scores");
      if (!scores.is_object()) return core::Err(invalid("spec tier_scores must be an object"));
      for (auto it = scores.begin(); it != scores.end(); ++it) {
        if (!it->is_number()) {
          return core::Err(invalid("spec tier score must be a number: " + it.key()));
        }
        spec.tier_scores.emplace_back(it.key(), it->get<f64>());
      }
    }
    ATX_TRY(spec.theme_erc, optional_bool(j, "theme_erc"));
    ATX_TRY(spec.theme_tsmom, optional_bool(j, "theme_tsmom"));
    ATX_TRY(spec.theme_resid, optional_bool(j, "theme_resid"));
    if (j.contains("registered_themes")) {
      const Json& registered = j.at("registered_themes");
      if (!registered.is_array()) {
        return core::Err(invalid("spec registered_themes must be an array"));
      }
      for (const Json& theme : registered) {
        if (!theme.is_string()) {
          return core::Err(invalid("spec registered_themes must hold strings"));
        }
        spec.registered_themes.push_back(theme.get<std::string>());
      }
    }
    return core::Ok(std::move(spec));
  } catch (const std::exception& e) {
    return core::Err(invalid(std::string("spec: ") + e.what()));
  }
}

core::Result<std::string> run_fit(const fs::path& factors_dir, const fs::path& spec_file,
                                  const fs::path& output_dir,
                                  const std::optional<WalkForwardSpec>& walk_forward) {
  try {
    std::error_code ec;
    if (fs::exists(output_dir, ec) || ec) {
      return core::Err(core::ErrorCode::AlreadyExists,
                       "atx-research-composition: output must not exist: " + output_dir.string());
    }
    ATX_TRY(const admission::FactorSeriesInput in, admission::read_factor_series(factors_dir));
    ATX_TRY(const std::string text, read_file(spec_file, kSpecLimit));
    ATX_TRY(const std::string spec_sha256, core::sha256_hex(std::string_view(text)));
    ATX_TRY(const FitSpec spec, parse_fit_spec(text));
    ATX_TRY(const std::vector<u8> mask, train_mask(in));
    const FactorSeriesView view{in.ids.size(), in.decisions, in.factors,
                                in.decision_sessions_ns, mask};
    ATX_TRY(const CompositionFit fit, fit_composition(view, in.ids, spec));
    Json doc = fit_json(fit, spec, in, spec_sha256);
    if (walk_forward) {
      ATX_TRY(const auto rows, fit_walk_forward(view, in.ids, spec, *walk_forward));
      doc["walk_forward"] = walk_forward_json(*walk_forward, rows);
    }
    const std::string document = doc.dump(2) + "\n";
    if (!fs::create_directory(output_dir, ec) || ec) {
      return core::Err(core::ErrorCode::AlreadyExists,
                       "atx-research-composition: output must not exist: " + output_dir.string());
    }
    std::ofstream file(output_dir / std::string(kFitFile), std::ios::binary);
    file.write(document.data(), static_cast<std::streamsize>(document.size()));
    file.close();
    if (!file) {
      return core::Err(core::ErrorCode::IoError,
                       "atx-research-composition: cannot write " +
                           (output_dir / std::string(kFitFile)).string());
    }
    return core::Ok(document);
  } catch (const std::bad_alloc&) {
    return core::Err(core::ErrorCode::OutOfRange, "atx-research-composition: allocation failed");
  } catch (const std::exception& e) {
    return core::Err(core::ErrorCode::Internal,
                     std::string("atx-research-composition: ") + e.what());
  }
}

int research_composition_main(std::span<const std::string_view> args, std::ostream& out,
                              std::ostream& err) {
  if (args.size() < 2 || args[1] != "fit") {
    err << usage();
    return 2;
  }
  fs::path factors, spec, output;
  std::set<std::string_view> seen;
  std::optional<std::string> window;
  std::optional<usize> lag, step, min_decisions;
  for (usize i = 2; i < args.size(); i += 2) {
    const std::string_view key = args[i];
    if (i + 1 >= args.size() || !seen.insert(key).second) {
      err << "atx-research-composition: missing value or repeated flag " << key << "\n" << usage();
      return 2;
    }
    const std::string_view value = args[i + 1];
    const bool count = key == "--lag" || key == "--step" || key == "--min-decisions";
    if (count && !count_value(value)) {
      err << "atx-research-composition: " << key << " takes a count (decimal digits): " << value
          << "\n"
          << usage();
      return 2;
    }
    if (key == "--factors") {
      factors = fs::path(value);
    } else if (key == "--spec") {
      spec = fs::path(value);
    } else if (key == "--output") {
      output = fs::path(value);
    } else if (key == "--walk-forward") {
      window = std::string(value);
    } else if (key == "--lag") {
      lag = count_value(value);
    } else if (key == "--step") {
      step = count_value(value);
    } else if (key == "--min-decisions") {
      min_decisions = count_value(value);
    } else {
      err << "atx-research-composition: unknown argument " << key << " " << value << "\n"
          << usage();
      return 2;
    }
  }
  // P9 lane D3W: --lag / --step / --min-decisions only with --walk-forward, which needs the first
  // two (the values themselves are fit_walk_forward's to refuse).
  if (factors.empty() || spec.empty() || output.empty() ||
      (window.has_value() != lag.has_value()) || (window.has_value() != step.has_value()) ||
      (!window && min_decisions)) {
    err << usage();
    return 2;
  }
  std::optional<WalkForwardSpec> walk_forward;
  if (window) {
    walk_forward = WalkForwardSpec{*window, *lag, *step,
                                   min_decisions.value_or(walk_forward_min_decisions)};
  }
  const auto published = run_fit(factors, spec, output, walk_forward);
  if (!published) {
    err << published.error().to_string() << "\n";
    return 1;
  }
  out << "atx-research-composition: fit -> " << (output / std::string(kFitFile)).string() << "\n";
  return 0;
}

} // namespace atx::engine::research::composition
