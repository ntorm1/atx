#include "strategy_ic_rules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/research/composition/fit.hpp" // P9 lane D3W: walk-forward-v1's constants
#include "strategy_ic_mom_volman.hpp"
#include "strategy_ic_shrink.hpp"
#include "strategy_ic_theme_erc.hpp"
#include "strategy_ic_theme_hedge.hpp"
#include "strategy_ic_theme_tsmom.hpp"
#include "strategy_two_speed.hpp"

namespace atx::impl::strategy::ic_detail {
namespace rc=atx::engine::research::composition;
// ---- Theme table ---------------------------------------------------------------
bool theme_name(const std::string& s) {
  return !s.empty() && s.size()<=64 && std::all_of(s.begin(),s.end(),[](char c) {
    return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_';
  });
}
namespace {
// The registry document in its own key order (ordered_json), refusing duplicate object keys as
// the fitter's unique_json does (a duplicate theme would otherwise collapse silently).
co::Result<nlohmann::ordered_json> unique_key_ordered_json(const std::string& text) {
  using Ordered=nlohmann::ordered_json;
  std::vector<std::set<std::string>> open; bool duplicate=false;
  auto j=Ordered::parse(text,[&](int,Ordered::parse_event_t event,Ordered& value) {
    if (event==Ordered::parse_event_t::object_start) open.emplace_back();
    else if (event==Ordered::parse_event_t::object_end && !open.empty()) open.pop_back();
    else if (event==Ordered::parse_event_t::key && !open.empty() &&
             !open.back().insert(value.get<std::string>()).second) duplicate=true;
    return true;
  },false);
  if (j.is_discarded() || duplicate)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "IC runner: theme registry JSON parse or duplicate key");
  return co::Ok(std::move(j));
}
} // namespace
co::Result<ThemeTable> theme_table_from_registry(const std::string& text,
                                                 std::string registry_sha256) {
  ATX_TRY(const auto j,unique_key_ordered_json(text));
  if (!j.is_object() || !j.contains("schema") || j.at("schema")!=theme_registry_schema ||
      !j.contains("themes") || !j.at("themes").is_object() || j.at("themes").empty() ||
      j.at("themes").size()>max_registered_themes)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme registry must be an "
        "atx.alpha-registry/v1 document with a themes table of 1.."+
        std::to_string(max_registered_themes)+" themes");
  ThemeTable out;
  out.registry_sha256=std::move(registry_sha256);
  const auto& themes=j.at("themes");
  for (auto it=themes.begin();it!=themes.end();++it) {
    if (!theme_name(it.key()))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "IC runner: theme registry theme name must match [a-z0-9_]{1,64}: "+it.key());
    out.names.push_back(it.key());
  }
  return co::Ok(std::move(out));
}
ThemeTable builtin_theme_table() {
  ThemeTable out;
  out.names.assign(builtin_registry_themes.begin(),builtin_registry_themes.end());
  return out;
}
co::Result<ThemeTable> theme_table(const IcRunnerConfig& cfg) {
  if (cfg.theme_registry_path.empty()) return co::Ok(builtin_theme_table());
  ATX_TRY(auto text,pinned_text(cfg.theme_registry_path,cfg.theme_registry_sha256,
                                theme_registry_max_bytes));
  return theme_table_from_registry(text,cfg.theme_registry_sha256);
}
bool registered_theme(const ThemeTable& table,std::string_view theme) noexcept {
  return std::any_of(table.names.begin(),table.names.end(),
                     [theme](const std::string& name) { return std::string_view(name)==theme; });
}

// ---- The grouping blocks (moved from strategy_ic_admission.cpp; checks and messages unchanged) --
namespace {
// A block's `themes` object {id: theme} (theme_redistribution and theme_standardise
// alike): known ids, names [a-z0-9_]{1,64}, a theme for every positive-weight candidate
// and 1..32 themes, `block` naming the block in that last refusal. Indices follow first
// appearance in library order; a zero-weight candidate keeps 0 (ignored by the composition).
co::Status theme_indices(const Json& rows,const Library& lib,const std::vector<f64>& weights,const char* block,
                         std::vector<usize>& index,usize& count) {
  std::set<std::string> ids;
  for (const auto& c:lib.candidates) ids.insert(c.id);
  for (auto it=rows.begin();it!=rows.end();++it) {
    if (!ids.contains(it.key()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme for unknown candidate: "+it.key());
    if (!it->is_string() || !theme_name(it->get<std::string>()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme name must match [a-z0-9_]{1,64}: "+it.key());
  }
  std::vector<std::string> names;
  index.assign(lib.candidates.size(),0);
  for (usize k=0;k<lib.candidates.size();++k) {
    if (!(weights[k]>0)) continue;
    const auto it=rows.find(lib.candidates[k].id);
    if (it==rows.end())
      return co::Err(co::ErrorCode::InvalidArgument,
          "IC runner: theme missing for weighted candidate: "+lib.candidates[k].id);
    const auto name=it->get<std::string>();
    const auto at=std::find(names.begin(),names.end(),name);
    index[k]=static_cast<usize>(at-names.begin());
    if (at==names.end()) names.push_back(name);
  }
  if (names.empty() || names.size()>32)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+std::string(block)+" needs 1..32 weighted themes");
  count=names.size();
  return co::Ok();
}
} // namespace
// ic-shrink-v1 (strategy_ic_shrink.hpp): the block's ic_shrink {intensity, floor, members: {id:
// {theme, ic}}} records the fitter's inputs, the registered constants and every member that took
// part (a floored member at weight 0 too) with its theme and IC estimate. The rule runs on the
// members in library order; each pinned weight must equal its rule weight within
// ic_shrink_weight_tolerance (0 for a candidate that is not a member), a weighted candidate must
// be a member, and its `themes` entry must name its member theme.
// ic-shrink-aim-v1 (fix round 1, Ruling E-44): the same, each member also recording the parent's
// aim gain (members: {id: {theme, ic, gain}}, gain finite > 0), and the rule takes the gains.
namespace {
co::Status verify_shrink(const Json& block,const Library& lib,const std::vector<f64>& weights,bool aim) {
  const std::string rule_id(aim?ic_shrink_aim_rule:ic_shrink_rule);
  const std::string shape(aim?"{theme, ic, gain}":"{theme, ic}");
  const auto refuse=[&rule_id](const std::string& what) {
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_standardise rule "+rule_id+": "+what);
  };
  const auto number=[](const Json& j,const char* key) {
    return j.is_object() && j.contains(key) && j.at(key).is_number()?j.at(key).get<f64>():quiet_nan;
  };
  if (!block.contains("ic_shrink") || !block.at("ic_shrink").is_object())
    return refuse("needs ic_shrink {intensity, floor, members: {id: "+shape+"}}");
  const auto& shrink=block.at("ic_shrink");
  if (!(number(shrink,"intensity")==ic_shrink_intensity) || !(number(shrink,"floor")==ic_shrink_floor))
    return refuse("ic_shrink intensity and floor must be the registered 0.5 and 0");
  if (!shrink.contains("members") || !shrink.at("members").is_object() || shrink.at("members").empty())
    return refuse("ic_shrink.members must be a non-empty object {id: "+shape+"}");
  const auto& members=shrink.at("members");
  std::set<std::string> ids;
  for (const auto& c:lib.candidates) ids.insert(c.id);
  for (auto it=members.begin();it!=members.end();++it) {
    if (!ids.contains(it.key())) return refuse("member of unknown candidate: "+it.key());
    const auto& m=*it;
    const f64 gain=number(m,"gain");
    if (!m.is_object() || !m.contains("theme") || !m.at("theme").is_string() ||
        !theme_name(m.at("theme").get<std::string>()) || !std::isfinite(number(m,"ic")) ||
        (aim && !(std::isfinite(gain) && gain>0)))
      return refuse("member "+it.key()+" needs {theme: [a-z0-9_]{1,64}, ic: finite number"+
                    (aim?std::string(", gain: finite number > 0}"):std::string("}")));
  }
  // The members in library order, theme indices by first appearance.
  std::vector<f64> ic,gains; std::vector<usize> theme,position; std::vector<std::string> names;
  for (usize k=0;k<lib.candidates.size();++k) {
    const auto it=members.find(lib.candidates[k].id);
    if (it==members.end()) continue;
    const auto name=it->at("theme").get<std::string>();
    const auto found=std::find(names.begin(),names.end(),name);
    theme.push_back(static_cast<usize>(found-names.begin()));
    if (found==names.end()) names.push_back(name);
    ic.push_back(it->at("ic").get<f64>()); position.push_back(k);
    if (aim) gains.push_back(it->at("gain").get<f64>());
  }
  const auto fit=ic_shrink_weights(ic,theme,names.size(),gains);
  if (!fit) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+fit.error().message());
  std::vector<f64> rule(lib.candidates.size(),0.0);
  for (usize m=0;m<position.size();++m) rule[position[m]]=fit->weights[m];
  const auto& themes=block.at("themes");
  for (usize k=0;k<lib.candidates.size();++k) {
    const auto& id=lib.candidates[k].id;
    const auto member=members.find(id);
    if (weights[k]>0 && member==members.end()) return refuse("weighted candidate "+id+" is not an ic_shrink member");
    if (!(std::abs(weights[k]-rule[k])<=ic_shrink_weight_tolerance))
      return refuse("composition weight of "+id+" is "+Json(weights[k]).dump()+", the rule on ic_shrink.members "
                    "gives "+Json(rule[k]).dump());
    if (weights[k]>0 && (!themes.contains(id) || themes.at(id)!=member->at("theme")))
      return refuse("themes."+id+" is not its ic_shrink member theme");
  }
  return co::Ok();
}
} // namespace
co::Status verify_ic_shrink(const Json& block,const Library& lib,const std::vector<f64>& weights) {
  return verify_shrink(block,lib,weights,false);
}
co::Status verify_ic_shrink_aim(const Json& block,const Library& lib,const std::vector<f64>& weights) {
  return verify_shrink(block,lib,weights,true);
}
// theme-erc-v1 (strategy_ic_theme_erc.hpp): the block's theme_erc {sweeps, dispersion, members: {id:
// {theme, share}}, covariance: {themes: [theme, ...], matrix: [[...], ...]}} records the registered
// constants, every member with its theme and the parent's pre-cap within-theme share (a zero share
// too), and the themes' sleeve covariance in the order the fitter solved it. The rule runs on the
// members in library order with theme indices in the covariance's order; each pinned weight must
// equal its rule weight within theme_erc_weight_tolerance (0 for a candidate that is not a
// member), a weighted candidate must be a member, and its `themes` entry must name its member theme.
co::Status verify_theme_erc(const Json& block,const Library& lib,const std::vector<f64>& weights) {
  const auto refuse=[](const std::string& what) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "IC runner: theme_standardise rule "+std::string(theme_erc_rule)+": "+what);
  };
  const auto number=[](const Json& j,const char* key) {
    return j.is_object() && j.contains(key) && j.at(key).is_number()?j.at(key).get<f64>():quiet_nan;
  };
  if (!block.contains("theme_erc") || !block.at("theme_erc").is_object())
    return refuse("needs theme_erc {sweeps, dispersion, members: {id: {theme, share}}, covariance: {themes, "
                  "matrix}}");
  const auto& erc=block.at("theme_erc");
  if (!erc.contains("sweeps") || !erc.at("sweeps").is_number_integer() ||
      erc.at("sweeps").get<i64>()!=static_cast<i64>(theme_erc_sweeps) ||
      !(number(erc,"dispersion")==theme_erc_dispersion))
    return refuse("theme_erc sweeps and dispersion must be the registered 10000 and 1e-10");
  if (!erc.contains("covariance") || !erc.at("covariance").is_object() ||
      !erc.at("covariance").contains("themes") || !erc.at("covariance").at("themes").is_array() ||
      !erc.at("covariance").contains("matrix") || !erc.at("covariance").at("matrix").is_array())
    return refuse("theme_erc.covariance must be {themes: [theme, ...], matrix: [[number, ...], ...]}");
  const auto& order=erc.at("covariance").at("themes");
  const auto& rows=erc.at("covariance").at("matrix");
  std::vector<std::string> names;
  for (const auto& name:order) {
    if (!name.is_string() || !theme_name(name.get<std::string>()) ||
        std::find(names.begin(),names.end(),name.get<std::string>())!=names.end())
      return refuse("theme_erc.covariance.themes must name distinct themes [a-z0-9_]{1,64}");
    names.push_back(name.get<std::string>());
  }
  if (names.empty() || names.size()>32 || rows.size()!=names.size())
    return refuse("theme_erc.covariance needs 1..32 themes and one matrix row per theme");
  std::vector<f64> covariance;
  covariance.reserve(names.size()*names.size());
  for (const auto& row:rows) {
    if (!row.is_array() || row.size()!=names.size())
      return refuse("theme_erc.covariance.matrix must be square, one entry per theme in each row");
    for (const auto& value:row) {
      if (!value.is_number()) return refuse("theme_erc.covariance.matrix entries must be numbers");
      covariance.push_back(value.get<f64>());
    }
  }
  if (!erc.contains("members") || !erc.at("members").is_object() || erc.at("members").empty())
    return refuse("theme_erc.members must be a non-empty object {id: {theme, share}}");
  const auto& members=erc.at("members");
  std::set<std::string> ids;
  for (const auto& c:lib.candidates) ids.insert(c.id);
  for (auto it=members.begin();it!=members.end();++it) {
    if (!ids.contains(it.key())) return refuse("member of unknown candidate: "+it.key());
    const auto& m=*it;
    if (!m.is_object() || !m.contains("theme") || !m.at("theme").is_string() ||
        std::find(names.begin(),names.end(),m.at("theme").get<std::string>())==names.end() ||
        !std::isfinite(number(m,"share")))
      return refuse("member "+it.key()+" needs {theme: a theme of theme_erc.covariance.themes, share: finite "
                    "number}");
  }
  // The members in library order, theme indices in the covariance's order.
  std::vector<f64> share; std::vector<usize> theme,position;
  for (usize k=0;k<lib.candidates.size();++k) {
    const auto it=members.find(lib.candidates[k].id);
    if (it==members.end()) continue;
    const auto name=it->at("theme").get<std::string>();
    theme.push_back(static_cast<usize>(std::find(names.begin(),names.end(),name)-names.begin()));
    share.push_back(it->at("share").get<f64>()); position.push_back(k);
  }
  const auto fit=theme_erc_weights(share,theme,names.size(),covariance);
  if (!fit) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+fit.error().message());
  std::vector<f64> rule(lib.candidates.size(),0.0);
  for (usize m=0;m<position.size();++m) rule[position[m]]=fit->weights[m];
  const auto& themes=block.at("themes");
  for (usize k=0;k<lib.candidates.size();++k) {
    const auto& id=lib.candidates[k].id;
    const auto member=members.find(id);
    if (weights[k]>0 && member==members.end()) return refuse("weighted candidate "+id+" is not a theme_erc member");
    if (!(std::abs(weights[k]-rule[k])<=theme_erc_weight_tolerance))
      return refuse("composition weight of "+id+" is "+Json(weights[k]).dump()+", the rule on theme_erc gives "+
                    Json(rule[k]).dump());
    if (weights[k]>0 && (!themes.contains(id) || themes.at(id)!=member->at("theme")))
      return refuse("themes."+id+" is not its theme_erc member theme");
  }
  return co::Ok();
}
co::Status redistribution_block(const Json& block) {
  if (!block.is_object() || !block.contains("rule") || block.at("rule")!=theme_redistribution_rule ||
      !block.contains("composition") || block.at("composition")!=theme_redistribution_composition ||
      !block.contains("themes") || !block.at("themes").is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_redistribution must be {rule: within-theme-v1, "
        "composition: ew-theme-v6, themes: {id: theme}}");
  return co::Ok();
}
// theme_standardise: a rule of the table, a boolean rerank (true for a row without rerank_off)
// and a themes object; a rule's own keys are checked by its verify.
co::Status standardise_block(const Json& block) {
  const auto* rule=standardise_row(block);
  if (rule==nullptr || !block.contains("rerank") || !block.at("rerank").is_boolean() ||
      !block.contains("themes") || !block.at("themes").is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_standardise must be {rule: ew-theme-std-v1, "
        "rerank: true|false, themes: {id: theme}} or {rule: ic-shrink-v1|ic-shrink-aim-v1, rerank: true, themes: "
        "{id: theme}, ic_shrink: {intensity, floor, members}} or {rule: theme-erc-v1, rerank: true, themes: {id: "
        "theme}, theme_erc: {sweeps, dispersion, members, covariance}}");
  if (!rule->rerank_off && !block.at("rerank").get<bool>())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_standardise rule "+std::string(rule->id)+
        " needs rerank true (its per-date standardisation is ew-theme-std-v1's, unchanged)");
  return co::Ok();
}
// Optional top-level `theme_redistribution` (fitter ew-theme-v6, v4-prereg v6 revision
// V6-W): exactly {"rule":"within-theme-v1","composition":"ew-theme-v6","themes":{id:
// theme}} with themes as theme_indices checks them. Absent: pinned.themes stays empty and
// nothing downstream changes. The block requires schema v2 and v2 requires a block
// (checked by composition_weights).
co::Status composition_themes(const Json& j,const RuleInputs& in,PinnedWeights& pinned) {
  if (!j.contains("theme_redistribution")) return co::Ok();
  const auto& block=j.at("theme_redistribution");
  ATX_TRY_VOID(redistribution_block(block));
  return theme_indices(block.at("themes"),in.lib,pinned.values,"theme_redistribution",pinned.themes,
                       pinned.theme_count);
}
namespace {
// Optional top-level `theme_standardise` (fitter ew-theme-std-v1, platform v8 R-1):
// exactly {"rule":"ew-theme-std-v1","rerank":true|false,"themes":{id: theme}}, themes as
// theme_indices checks them (also when rerank is false). rerank true fills
// pinned.std_themes (IcThemeRule::standardise); rerank false is the rule's identity
// switch: the composition is the plain pinned-weights path, whose blend is the ew-theme-v1
// one bit for bit. Absent: nothing changes. Schema v2 as for theme_redistribution.
// Rules ic-shrink-v1 / ic-shrink-aim-v1 (platform v8 R-10, rerank true only) add `ic_shrink`, which their verify
// checks against the weights; its per-date path is the same (the rule table).
co::Status standardise_from_block(const Json& j,const Library& lib,PinnedWeights& pinned) {
  if (!j.contains("theme_standardise")) return co::Ok();
  const auto& block=j.at("theme_standardise");
  ATX_TRY_VOID(standardise_block(block));
  const auto* rule=standardise_row(block);
  if (rule==nullptr) return co::Err(co::ErrorCode::Internal,"IC runner: theme_standardise rule table");
  std::vector<usize> index; usize count=0;
  ATX_TRY_VOID(theme_indices(block.at("themes"),lib,pinned.values,"theme_standardise",index,count));
  if (rule->verify!=nullptr) ATX_TRY_VOID(rule->verify(block,lib,pinned.values));
  const bool rerank=block.at("rerank").get<bool>();
  pinned.standardise=std::string(rule->id)+(rerank?"":";rerank-off");
  if (rerank) { pinned.std_themes=std::move(index); pinned.std_theme_count=count; }
  return co::Ok();
}
// Finding R6B-C-5 (every theme_standardise row of the table): the fitter rule a weights file
// records (provenance.rule, a string) must write its theme_standardise block. A file recording a
// rule that writes a row carries exactly that row's block; a file carrying a row's block records a
// rule that writes it, or is that row's rerank-off identity device on its identity_source. So
// a file recording ic-shrink-v1 under an ew-theme-std-v1 block (the ic-shrink verify skipped,
// ew-theme-std-v1 recorded) is refused. A file without a string provenance.rule (hand-written
// weights) passes only without a theme_standardise block (finding R6C-7: deleting or nulling
// provenance.rule would otherwise let any row's block through). Runs after the block is
// validated, before any role payload.
co::Status composition_recorded_rule(const Json& j) {
  if (!j.contains("provenance") || !j.at("provenance").is_object() || !j.at("provenance").contains("rule") ||
      !j.at("provenance").at("rule").is_string()) {
    if (!j.contains("theme_standardise")) return co::Ok();
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights carry a theme_standardise block "
        "without a string provenance.rule (finding R6C-7)");
  }
  const std::string& recorded=j.at("provenance").at("rule").get_ref<const std::string&>();
  const CompositionRule* block=
      j.contains("theme_standardise")?standardise_row(j.at("theme_standardise")):nullptr;
  const CompositionRule* writer=nullptr;
  for (const auto& row:composition_rules())
    if (row.block_key=="theme_standardise" &&
        (recorded==row.id || (!row.also_written_by.empty() && recorded==row.also_written_by)))
      writer=&row;
  if (writer==block) return co::Ok();
  const std::string carried=block==nullptr?std::string("no theme_standardise block")
                                          :"theme_standardise rule "+std::string(block->id);
  if (writer!=nullptr)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights record provenance.rule "+recorded+
        ", which writes theme_standardise rule "+std::string(writer->id)+", but carry "+carried);
  const bool identity=!j.at("theme_standardise").at("rerank").get<bool>() && !block->identity_source.empty() &&
      recorded==block->identity_source;
  if (identity) return co::Ok();
  return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights carry "+carried+
      ", which their provenance.rule "+recorded+" does not write");
}
} // namespace
// The theme_standardise rows' shared parse: the block (above), then the recorded-rule check, which
// also runs without the block (a file recording a row's writer must carry its block).
co::Status composition_standardise(const Json& j,const RuleInputs& in,PinnedWeights& pinned) {
  ATX_TRY_VOID(standardise_from_block(j,in.lib,pinned));
  return composition_recorded_rule(j);
}

// ---- Rule table ----------------------------------------------------------------
namespace {
// The recipe composition statements (after the signs) of the two grouping rules, as the v8
// runner wrote them.
constexpr std::string_view redistribute_text=
    "centered-tied-rank;missing-or-unoriented-mass-stays-in-theme;within-theme-v1;"
    "theme-without-present-member-neutral";
constexpr std::string_view standardise_text=
    "centered-tied-rank;theme-weighted-rank-sum-missing-neutral;"
    "theme-rerank-centered-tied-over-names-with-a-present-member;"
    "theme-weight-sum-of-member-weights";
constexpr std::string_view grouping_bytes="one f64 dates x names plane per theme (the theme's "
    "summed member ranks), dropped before the planned-target pass";
// The ids each row refuses beside it, and the rows a rider needs (one of them, with rerank true).
constexpr std::array<std::string_view,4> standardise_ids{theme_standardise_rule,ic_shrink_rule,
    ic_shrink_aim_rule,theme_erc_rule};
constexpr std::array<std::string_view,1> redistribute_ids{theme_redistribution_rule};
// theme-resid-v1 excludes every other rider; the two theme_schedule rules share one block; the
// hedge and the sleeves exclude each other (P9 lane AL-COMB: mom-volman-v1, theme-cov-hedge-v1).
constexpr std::array<std::string_view,4> resid_excludes{theme_tsmom_rule,mom_volman_rule,
    two_speed_rule,theme_hedge_rule};
constexpr std::array<std::string_view,2> tsmom_excludes{theme_residualise_rule,mom_volman_rule};
constexpr std::array<std::string_view,2> volman_excludes{theme_residualise_rule,theme_tsmom_rule};
constexpr std::array<std::string_view,2> sleeves_excludes{theme_residualise_rule,theme_hedge_rule};
constexpr std::array<std::string_view,2> hedge_excludes{theme_residualise_rule,two_speed_rule};
// params_schema (progress.md ruling D1-PSCHEMA, P9 lane D2): the JSON Schema of the `params` a
// K-P9-9 wave-manifest rule entry {name: <id>, params: {...}} may carry for the row. The params are
// the row's own rule key (`rule`, the id: the key every K-P9-6 block names its rule by; lane E2's
// wave_kinds.checked_params fills it in when a manifest leaves it out) and the rule's registered
// constants: each key optional (absent = the registered value) and, when given, equal to it
// (`const`, integers typed); no other key is accepted. Lane E2 validates a manifest's params
// against the schema it reads from `--list-rules --json`. The weights-file blocks the fitter writes
// are checked by each row's parse and verify, not by params_schema.
// ParamsSchemaConstantsAreTheRegisteredOnes pins every const below to the C++ constant it restates.
constexpr std::string_view redistribute_params_schema=
    R"({"type":"object","additionalProperties":false,)"
    R"("properties":{"rule":{"type":"string","const":"within-theme-v1"}}})";
constexpr std::string_view std_params_schema=
    R"({"type":"object","additionalProperties":false,)"
    R"("properties":{"rule":{"type":"string","const":"ew-theme-std-v1"}}})";
constexpr std::string_view shrink_params_schema=
    R"({"type":"object","additionalProperties":false,)"
    R"("properties":{"rule":{"type":"string","const":"ic-shrink-v1"},)"
    R"("intensity":{"type":"number","const":0.5},"floor":{"type":"number","const":0}}})";
constexpr std::string_view shrink_aim_params_schema=
    R"({"type":"object","additionalProperties":false,)"
    R"("properties":{"rule":{"type":"string","const":"ic-shrink-aim-v1"},)"
    R"("intensity":{"type":"number","const":0.5},"floor":{"type":"number","const":0}}})";
constexpr std::string_view erc_params_schema=
    R"({"type":"object","additionalProperties":false,)"
    R"("properties":{"rule":{"type":"string","const":"theme-erc-v1"},)"
    R"("sweeps":{"type":"integer","const":10000},"dispersion":{"type":"number","const":1e-10}}})";
constexpr std::string_view resid_params_schema=
    R"({"type":"object","additionalProperties":false,)"
    R"("properties":{"rule":{"type":"string","const":"theme-resid-v1"}}})";
constexpr std::string_view tsmom_params_schema=
    R"({"type":"object","additionalProperties":false,)"
    R"("properties":{"rule":{"type":"string","const":"theme-tsmom-v1"},)"
    R"("lookback":{"type":"integer","const":252},"lag":{"type":"integer","const":3},)"
    R"("step":{"type":"integer","const":21}}})";
constexpr std::string_view sleeves_params_schema=
    R"({"type":"object","additionalProperties":false,)"
    R"("properties":{"rule":{"type":"string","const":"two-speed-v1"}}})";
// P9 lane AL-COMB: the registered constants of mom-volman-v1 (strategy_ic_mom_volman.hpp) and
// theme-cov-hedge-v1 (strategy_ic_theme_hedge.hpp) in the same form; each block's parse checks
// the block the fitter writes (composition_volman, composition_hedge).
constexpr std::string_view volman_params_schema=
    R"({"type":"object","additionalProperties":false,)"
    R"("properties":{"rule":{"type":"string","const":"mom-volman-v1"},)"
    R"("lookback":{"type":"integer","const":126},"lag":{"type":"integer","const":2},)"
    R"("step":{"type":"integer","const":21}}})";
constexpr std::string_view hedge_params_schema=
    R"({"type":"object","additionalProperties":false,)"
    R"("properties":{"rule":{"type":"string","const":"theme-cov-hedge-v1"},)"
    R"("lookback":{"type":"integer","const":252},"lag":{"type":"integer","const":2},)"
    R"("step":{"type":"integer","const":21},"min_pairs":{"type":"integer","const":126}}})";
// Parse order: theme_redistribution, theme_standardise (four rows; the recorded-rule check runs
// with them), theme_residualise, theme_schedule (two rows, one shared parse), theme_sleeves -- the
// v8 runner's order, so every refusal is the one it gave -- then theme_hedge (P9 lane AL-COMB).
constexpr std::array<CompositionRule,10> rule_table{{
    {.id=theme_redistribution_rule, .version="v1", .block_key="theme_redistribution",
     .stage=IcStageKind::redistribute, .parse=&composition_themes, .verify=nullptr,
     .recipe_key="composition_redistribution", .recipe_text=redistribute_text,
     .working_bytes="two f64 dates x names planes per theme (blend and present weight)",
     .params_schema=redistribute_params_schema, .incompatible=standardise_ids, .requires_any={},
     .rerank_off=false, .also_written_by={}, .identity_source={}},
    // ew-theme-std-v1 (R-1; R-3's ew-theme-std-aim-v1 writes it too; the R-1 identity device,
    // composition_rules.identity_document, grafts a rerank-off block onto ew-theme-v1 weights)
    {.id=theme_standardise_rule, .version="v1", .block_key="theme_standardise",
     .stage=IcStageKind::standardise, .parse=&composition_standardise, .verify=nullptr,
     .recipe_key="composition_standardise", .recipe_text=standardise_text,
     .working_bytes=grouping_bytes, .params_schema=std_params_schema,
     .incompatible=redistribute_ids, .requires_any={}, .rerank_off=true,
     .also_written_by="ew-theme-std-aim-v1", .identity_source="ew-theme-v1"},
    // ic-shrink-v1 (R-10, strategy_ic_shrink.hpp)
    {.id=ic_shrink_rule, .version="v1", .block_key="theme_standardise",
     .stage=IcStageKind::standardise, .parse=&composition_standardise, .verify=&verify_ic_shrink,
     .recipe_key="composition_standardise", .recipe_text=standardise_text,
     .working_bytes=grouping_bytes, .params_schema=shrink_params_schema,
     .incompatible=redistribute_ids,
     .requires_any={}, .rerank_off=false, .also_written_by={}, .identity_source={}},
    // ic-shrink-aim-v1 (R-10 on an aim parent, E-44)
    {.id=ic_shrink_aim_rule, .version="v1", .block_key="theme_standardise",
     .stage=IcStageKind::standardise, .parse=&composition_standardise,
     .verify=&verify_ic_shrink_aim, .recipe_key="composition_standardise",
     .recipe_text=standardise_text, .working_bytes=grouping_bytes,
     .params_schema=shrink_aim_params_schema, .incompatible=redistribute_ids, .requires_any={},
     .rerank_off=false, .also_written_by={}, .identity_source={}},
    // theme-erc-v1 (X XCOMB, strategy_ic_theme_erc.hpp)
    {.id=theme_erc_rule, .version="v1", .block_key="theme_standardise",
     .stage=IcStageKind::standardise, .parse=&composition_standardise, .verify=&verify_theme_erc,
     .recipe_key="composition_standardise", .recipe_text=standardise_text,
     .working_bytes=grouping_bytes, .params_schema=erc_params_schema,
     .incompatible=redistribute_ids,
     .requires_any={}, .rerank_off=false, .also_written_by={}, .identity_source={}},
    // theme-resid-v1 (R-11, strategy_ic_theme_resid.cpp): residualises the standardised planes
    {.id=theme_residualise_rule, .version="v1", .block_key="theme_residualise",
     .stage=IcStageKind::residualise, .parse=&composition_residualise, .verify=nullptr,
     .recipe_key="composition_residualise", .recipe_text={},
     .working_bytes="per name 8 B per theme plus 8 B of regression scratch, on the standardise "
                    "planes",
     .params_schema=resid_params_schema, .incompatible=resid_excludes,
     .requires_any=standardise_ids,
     .rerank_off=false, .also_written_by={}, .identity_source={}},
    // theme-tsmom-v1 (Y-2, strategy_ic_theme_tsmom.cpp): the theme masses in force per date
    {.id=theme_tsmom_rule, .version="v1", .block_key="theme_schedule",
     .stage=IcStageKind::schedule, .parse=&composition_schedule_rules, .verify=nullptr,
     .recipe_key="composition_schedule", .recipe_text={},
     .working_bytes="none beyond the standardise planes (the block masses)",
     .params_schema=tsmom_params_schema, .incompatible=tsmom_excludes,
     .requires_any=standardise_ids,
     .rerank_off=false, .also_written_by={}, .identity_source={}},
    // mom-volman-v1 (P9 AL-COMB, strategy_ic_mom_volman.cpp): the price_momentum mass per block
    {.id=mom_volman_rule, .version="v1", .block_key="theme_schedule",
     .stage=IcStageKind::schedule, .parse=&composition_schedule_rules, .verify=nullptr,
     .recipe_key="composition_schedule", .recipe_text={},
     .working_bytes="none beyond the standardise planes (the block masses)",
     .params_schema=volman_params_schema, .incompatible=volman_excludes,
     .requires_any=standardise_ids,
     .rerank_off=false, .also_written_by={}, .identity_source={}},
    // two-speed-v1 (Y-5, strategy_ic_two_speed.cpp): the fast and slow sleeves beside the blend
    {.id=two_speed_rule, .version="v1", .block_key="theme_sleeves",
     .stage=IcStageKind::sleeves, .parse=&composition_sleeves, .verify=nullptr,
     .recipe_key="composition_sleeves", .recipe_text={},
     .working_bytes="two f64 dates x names sleeve planes and one f64 per date (the fast share)",
     .params_schema=sleeves_params_schema, .incompatible=sleeves_excludes,
     .requires_any=standardise_ids,
     .rerank_off=false, .also_written_by={}, .identity_source={}},
    // theme-cov-hedge-v1 (P9 AL-COMB, strategy_ic_theme_hedge.cpp over engine
    // combine/char_cov_hedge.hpp): each theme's re-rank hedged on its own sleeve's loading
    {.id=theme_hedge_rule, .version="v1", .block_key="theme_hedge",
     .stage=IcStageKind::hedge, .parse=&composition_hedge, .verify=nullptr,
     .recipe_key="composition_hedge", .recipe_text={},
     .working_bytes="the role's session returns and one hedged plane (f64 dates x names each), a "
                    "presence byte per cell and the kernel's per-name window sums",
     .params_schema=hedge_params_schema, .incompatible=hedge_excludes,
     .requires_any=standardise_ids,
     .rerank_off=false, .also_written_by={}, .identity_source={}},
}};
// What the exe offers beyond the v7 CLI (the envelope's capabilities; lane E2 reads them in place
// of the --help probe, research_cycle.py exe_capabilities). Sorted. P9 S2 (ruling S2-2) adds the
// IC method options ic-hac-rule, label-terminal and min-coverage.
constexpr std::array<std::string_view,8> exe_capabilities{"eval-mode-audit-exact","ic-hac-rule",
    "label-terminal","list-rules","marginal","min-coverage","no-composition","theme-registry"};
// A pair of present rows that list each other is refused, naming both blocks (the v8 runner's
// "theme_redistribution and theme_standardise are exclusive").
co::Status refuse_incompatible(std::span<const CompositionRule* const> present) {
  for (usize a=0;a<present.size();++a)
    for (usize b=a+1;b<present.size();++b) {
      const auto listed=present[a]->incompatible;
      if (std::find(listed.begin(),listed.end(),present[b]->id)!=listed.end())
        return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+
            std::string(present[a]->block_key)+" and "+std::string(present[b]->block_key)+
            " are exclusive");
    }
  return co::Ok();
}
Json ids_json(std::span<const std::string_view> ids) {
  Json out=Json::array();
  for (const auto id:ids) out.push_back(std::string(id));
  return out;
}
} // namespace
std::span<const CompositionRule> composition_rules() noexcept { return rule_table; }
const CompositionRule* find_rule(std::string_view id) noexcept {
  for (const auto& rule:rule_table)
    if (rule.id==id) return &rule;
  return nullptr;
}
const CompositionRule* standardise_row(const Json& block) {
  if (!block.is_object() || !block.contains("rule") || !block.at("rule").is_string()) return nullptr;
  const auto& id=block.at("rule").get_ref<const std::string&>();
  for (const auto& rule:rule_table)
    if (rule.block_key=="theme_standardise" && rule.id==id) return &rule;
  return nullptr;
}
co::Status parse_composition_rules(const Json& doc,const RuleInputs& in,PinnedWeights& pinned) {
  std::string_view parsed; // the block key whose shared parse just ran (its rows are adjacent)
  for (const auto& rule:rule_table) {
    if (rule.block_key==parsed) continue;
    parsed=rule.block_key;
    ATX_TRY_VOID(rule.parse(doc,in,pinned));
  }
  // Every present block passed its parse, so it is an object whose string rule names its row.
  pinned.rules.clear();
  for (const auto& rule:rule_table) {
    const auto block=doc.find(std::string(rule.block_key));
    if (block!=doc.end() && block->at("rule").get_ref<const std::string&>()==rule.id)
      pinned.rules.push_back(&rule);
  }
  return refuse_incompatible(pinned.rules);
}
std::vector<const CompositionRule*> running_rules(const PinnedWeights& pinned) {
  std::vector<const CompositionRule*> out;
  for (const auto* rule:pinned.rules)
    if (rule->stage!=IcStageKind::standardise || !pinned.std_themes.empty()) out.push_back(rule);
  return out;
}
std::vector<IcStageKind> running_stage_kinds(const PinnedWeights& pinned) {
  std::vector<IcStageKind> out;
  for (const auto* rule:running_rules(pinned)) out.push_back(rule->stage);
  return out;
}
namespace {
// The first session of the role at or after `from` (its date index; dates when none is).
usize first_at_or_after(std::span<const i64> session_keys,i64 from) {
  return static_cast<usize>(std::lower_bound(session_keys.begin(),session_keys.end(),from)-
                            session_keys.begin());
}
// P9 lane D3W: the walk_forward rows of a v3 file and, on a standardise grouping, the masses in
// force (composition_stages' comment).
co::Status walk_forward_stages(const PinnedWeights& pinned,std::span<const i64> session_keys,
                               IcCompositionStages& out) {
  if (pinned.weight_rows.size()!=pinned.weight_from.size() || pinned.weight_rows.empty())
    return co::Err(co::ErrorCode::Internal,"IC runner: walk_forward geometry");
  out.weight_rows.reserve(pinned.weight_rows.size());
  for (usize r=0;r<pinned.weight_rows.size();++r)
    out.weight_rows.push_back({first_at_or_after(session_keys,pinned.weight_from[r]),
                               pinned.weight_rows[r]});
  if (pinned.std_themes.empty()) return co::Ok(); // the plain pinned path: per-date weights only
  const bool volman=pinned.schedule==mom_volman_rule;
  std::vector<usize> volman_begin;
  if (volman) {
    if (pinned.volman_scale.size()!=pinned.schedule_from.size() ||
        pinned.volman_theme>=pinned.std_theme_count)
      return co::Err(co::ErrorCode::Internal,"IC runner: mom-volman-v1 schedule geometry");
    for (const i64 from:pinned.schedule_from)
      volman_begin.push_back(first_at_or_after(session_keys,from));
  }
  std::vector<usize> bounds{0};
  for (const auto& row:out.weight_rows) bounds.push_back(row.begin);
  bounds.insert(bounds.end(),volman_begin.begin(),volman_begin.end());
  std::sort(bounds.begin(),bounds.end());
  bounds.erase(std::unique(bounds.begin(),bounds.end()),bounds.end());
  IcStage stage; stage.kind=IcStageKind::schedule;
  stage.blocks.reserve(bounds.size());
  usize row=0,block=0; // rows (blocks) [0, row) begin at or before the boundary
  for (const usize at:bounds) {
    while (row<out.weight_rows.size() && out.weight_rows[row].begin<=at) ++row;
    while (block<volman_begin.size() && volman_begin[block]<=at) ++block;
    std::vector<f64> mass(pinned.std_theme_count,0.0);
    if (row>0) {
      const auto& w=out.weight_rows[row-1U].weights;
      for (usize k=0;k<w.size();++k)
        if (w[k]>0) mass[pinned.std_themes[k]]+=w[k];
      if (block>0)
        mass[pinned.volman_theme]=mass[pinned.volman_theme]*pinned.volman_scale[block-1U];
    }
    stage.blocks.push_back({at,std::move(mass)});
  }
  const auto scheduled=std::find_if(out.stages.begin(),out.stages.end(),
      [](const IcStage& s) { return s.kind==IcStageKind::schedule; });
  if (scheduled!=out.stages.end()) {
    scheduled->blocks=std::move(stage.blocks);
    return co::Ok();
  }
  const auto later=std::find_if(out.stages.begin(),out.stages.end(),
      [](const IcStage& s) { return s.kind>IcStageKind::schedule; });
  out.stages.insert(later,std::move(stage));
  return co::Ok();
}
} // namespace
co::Result<IcCompositionStages> composition_stages(const PinnedWeights& pinned,
                                                   std::span<const i64> session_keys) {
  IcCompositionStages out;
  out.weights=pinned.values;
  out.themes=pinned.composition_themes();
  for (const auto* rule:running_rules(pinned)) {
    IcStage stage; stage.kind=rule->stage;
    if (rule->stage==IcStageKind::schedule) {
      if (pinned.schedule_mass.size()!=pinned.schedule_from.size())
        return co::Err(co::ErrorCode::Internal,"IC runner: theme schedule geometry");
      stage.blocks.reserve(pinned.schedule_from.size());
      for (usize b=0;b<pinned.schedule_from.size();++b)
        stage.blocks.push_back(
            {first_at_or_after(session_keys,pinned.schedule_from[b]),pinned.schedule_mass[b]});
    }
    if (rule->stage==IcStageKind::sleeves) stage.fast=pinned.sleeve_fast;
    out.stages.push_back(std::move(stage));
  }
  if (!pinned.walk_forward.empty()) ATX_TRY_VOID(walk_forward_stages(pinned,session_keys,out));
  return co::Ok(std::move(out));
}

co::Status composition_walk_forward(const Json& j,const RuleInputs& in,PinnedWeights& pinned) {
  const auto& lib=in.lib;
  const auto refuse=[](const std::string& what) {
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+what);
  };
  if (!j.contains("walk_forward"))
    return refuse("composition weights schema atx.dsl-composition-weights/v3 requires a "
                  "walk_forward block");
  const auto& block=j.at("walk_forward");
  const auto at_least=[&block](const char* key,i64 least) {
    return block.contains(key) && block.at(key).is_number_integer() &&
           block.at(key).get<i64>()>=least;
  };
  const auto text=[&block](const char* key,std::string_view want) {
    return block.contains(key) && block.at(key).is_string() &&
           block.at(key).get_ref<const std::string&>()==want;
  };
  if (!block.is_object() || block.size()!=6U || !text("rule",rc::walk_forward_rule) ||
      !text("window",rc::walk_forward_expanding) || !at_least("lag",2) || !at_least("step",1) ||
      !at_least("min_decisions",2) || !block.contains("rows") || !block.at("rows").is_array() ||
      block.at("rows").empty() || block.at("rows").size()>rc::walk_forward_max_rows)
    return refuse("walk_forward must be {rule: walk-forward-v1, window: expanding, lag: >= 2, "
                  "step: >= 1, min_decisions: >= 2, rows: [{from_session, weights: {id: "
                  "weight}}, ...] (1..4096)}");
  if (!pinned.themes.empty() || pinned.residualise || pinned.schedule==theme_tsmom_rule)
    return refuse("walk_forward (walk-forward-v1) is refused with theme_redistribution, "
                  "theme_residualise and theme-tsmom-v1 (their grouping, order and schedule are "
                  "fitted on the whole TRAIN window)");
  std::set<std::string> ids;
  for (const auto& c:lib.candidates) ids.insert(c.id);
  std::vector<i64> from;
  std::vector<std::vector<f64>> rows;
  for (const auto& row:block.at("rows")) {
    if (!row.is_object() || row.size()!=2U || !row.contains("from_session") ||
        !row.at("from_session").is_number_integer() || !row.contains("weights") ||
        !row.at("weights").is_object())
      return refuse("walk_forward row must be {from_session: integer, weights: {id: weight}}");
    const i64 session=row.at("from_session").get<i64>();
    if (!from.empty() && !(session>from.back()))
      return refuse("walk_forward from_session must increase strictly");
    const auto& weights=row.at("weights");
    for (auto it=weights.begin();it!=weights.end();++it)
      if (!ids.contains(it.key()))
        return refuse("walk_forward weight for unknown candidate: "+it.key());
    std::vector<f64> w;
    w.reserve(lib.candidates.size());
    for (const auto& c:lib.candidates) {
      const auto it=weights.find(c.id);
      if (it==weights.end()) return refuse("walk_forward weight missing: "+c.id);
      const f64 x=it->is_number()?it->get<f64>():quiet_nan;
      if (!std::isfinite(x) || x<0)
        return refuse("walk_forward weight must be finite and >= 0: "+c.id);
      w.push_back(x);
    }
    if (std::none_of(w.begin(),w.end(),[](f64 x) { return x>0; }))
      return refuse("walk_forward row at from_session "+std::to_string(session)+
                    " weights no candidate");
    from.push_back(session);
    rows.push_back(std::move(w));
  }
  // A candidate a row weights: its sign, and beyond the top-level weights its theme's index.
  const bool grouped=!pinned.std_themes.empty();
  std::vector<std::string> names(grouped?pinned.std_theme_count:0U);
  if (grouped) {
    const auto& listed=j.at("theme_standardise").at("themes");
    for (usize k=0;k<lib.candidates.size();++k)
      if (pinned.values[k]>0)
        names[pinned.std_themes[k]]=listed.at(lib.candidates[k].id).get<std::string>();
  }
  for (usize k=0;k<lib.candidates.size();++k) {
    if (std::none_of(rows.begin(),rows.end(),[k](const std::vector<f64>& w) { return w[k]>0; }))
      continue;
    const auto& id=lib.candidates[k].id;
    if (!pinned.signs.empty() && pinned.signs[k]==0)
      return refuse("composition sign missing for a candidate a walk_forward row weights: "+id);
    if (!grouped || pinned.values[k]>0) continue;
    const auto& listed=j.at("theme_standardise").at("themes");
    const auto it=listed.find(id);
    if (it==listed.end())
      return refuse("theme missing for a candidate a walk_forward row weights: "+id);
    const auto& theme=it->get_ref<const std::string&>();
    const auto at=std::find(names.begin(),names.end(),theme);
    if (at==names.end())
      return refuse("walk_forward row weights "+id+" of theme "+theme+
                    ", which no top-level weighted candidate has");
    pinned.std_themes[k]=static_cast<usize>(at-names.begin());
  }
  pinned.walk_forward=std::string(rc::walk_forward_rule);
  pinned.weight_from=std::move(from);
  pinned.weight_rows=std::move(rows);
  return co::Ok();
}
std::string_view stage_name(IcStageKind kind) noexcept {
  switch (kind) {
    case IcStageKind::redistribute: return "redistribute";
    case IcStageKind::standardise: return "standardise";
    case IcStageKind::residualise: return "residualise";
    case IcStageKind::schedule: return "schedule";
    case IcStageKind::sleeves: return "sleeves";
    case IcStageKind::hedge: return "hedge";
  }
  return {};
}
Json rule_recipe_json(const CompositionRule& rule) {
  return Json{{"composition",std::string(rule.recipe_text)},{"key",std::string(rule.recipe_key)},
              {"value",std::string(rule.id)}};
}
co::Result<Json> list_rules_json() {
  Json rules=Json::array();
  for (const auto& rule:rule_table) {
    ATX_TRY(auto recipe_sha,co::sha256_hex(rule_recipe_json(rule).dump()));
    rules.push_back({{"id",std::string(rule.id)},{"version",std::string(rule.version)},
        {"block_key",std::string(rule.block_key)},{"stage",std::string(stage_name(rule.stage))},
        {"params_schema",Json::parse(rule.params_schema.begin(),rule.params_schema.end())},
        {"incompatible",ids_json(rule.incompatible)},{"requires_any",ids_json(rule.requires_any)},
        {"recipe_key",std::string(rule.recipe_key)},{"recipe_sha256",std::move(recipe_sha)},
        {"verified",rule.verify!=nullptr},{"working_bytes",std::string(rule.working_bytes)}});
  }
  Json capabilities=Json::array();
  for (const auto capability:exe_capabilities) capabilities.push_back(std::string(capability));
  return co::Ok(Json{{"schema",composition_rules_schema},{"capabilities",std::move(capabilities)},
                     {"rules",std::move(rules)}});
}
} // namespace atx::impl::strategy::ic_detail
