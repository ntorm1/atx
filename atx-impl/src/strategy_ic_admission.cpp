#include "strategy_ic_detail.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "strategy_ic_composition.hpp"

namespace atx::impl::strategy::ic_detail {
namespace {
constexpr const char* weights_schema="atx.dsl-composition-weights/v1";
// ew-theme-v6 files (V6-W fix round 1 I1): v2 iff a theme_redistribution block is
// present, v1 iff absent, so a binary predating within-theme-v1 refuses them loudly.
constexpr const char* weights_schema_v2="atx.dsl-composition-weights/v2";
// ew-theme-v6 (v4-prereg v6 revision V6-W): the only admitted theme_redistribution
// composition (its rule is theme_redistribution_rule, strategy_ic_detail.hpp).
constexpr const char* theme_redistribution_composition="ew-theme-v6";
constexpr const char* fields_semantics="extra-date-major-f64-columns-resolved-by-name;NaN-where-not-visible;"
    "role-presence-mask;decision-member-mask-unchanged";
} // namespace
Json method_recipe(const IcRunnerConfig& cfg,bool parallel_ic,bool pinned_signs,bool themed) {
  Json recipe{{"schema","atx.dsl-fast-ic/v1"},{"library_sha256",cfg.library_sha256},
      {"horizons",{5,21,63}},{"active_horizons",3},{"require_endpoint_presence",true},
      {"execution_delay",1},{"min_names",cfg.min_names},{"min_dates",cfg.min_dates},
      {"screen_rule","equivalence-v3"},{"practical_abs_ic",.002},{"confidence_multiplier",3.5},
      {"max_working_bytes",cfg.max_working_bytes},{"vm",vm_eval_mode},
      {"labels","close[d+1+h]/close[d+1]-1;strict-positive-observed-endpoints;role-maturity"},
      {"guard","observed-adjacent-log1.5;adjusted-log-vs-raw+.10;no-missing-zero-fill"},
      {"orientation","TRAIN21h-nonzero-sample-rank-mean;undefined=0;screen-diagnostic-only;freeze-before-validation"},
      {"composition","fixed-equal-family/equal-within;centered-tied-rank;missing-or-unoriented-neutral;no-redistribution"},
      {"planned_targets","final-rank-neutral-gross1;cadence5;fraction.25;no-drift;offcycle-membership-exit-zero;deployment-included"},
      {"scope","IC-and-planned-weight-change-only;no-costs-trades-NAV-Sharpe-or-holdout"}};
  if (cfg.workers!=1) recipe["vm_workers"]=cfg.workers;
  if (parallel_ic && cfg.workers!=1) recipe["research_ic_workers"]=cfg.workers;
  if (cfg.save_combined) recipe["saved_combined"]="date-major-f64-with-explicit-support-v1";
  // Absent weights leave the recipe byte-identical; pinned weights change both
  // the method statement and the canonical hash. The candidate cache is not a
  // method input: a verified hit is the exact VM output it replaces.
  if (!cfg.composition_weights_sha256.empty()) {
    // Signs pinned in the (TRAIN-bound, hashed) weights file replace the IC
    // orientation in the blend; IC diagnostics keep the TRAIN orientation.
    recipe["composition"]=pinned_signs
        ?"pinned-candidate-weights;pinned-candidate-signs;centered-tied-rank;"
         "missing-or-unoriented-neutral;no-redistribution"
        :"pinned-candidate-weights;TRAIN-orientation-signs;centered-tied-rank;"
         "missing-or-unoriented-neutral;no-redistribution";
    // A pinned theme_redistribution block (ew-theme-v6) replaces no-redistribution;
    // absent, the recipe bytes above are unchanged.
    if (themed) {
      recipe["composition"]=std::string(pinned_signs?"pinned-candidate-weights;pinned-candidate-signs;"
                                                    :"pinned-candidate-weights;TRAIN-orientation-signs;")+
          "centered-tied-rank;missing-or-unoriented-mass-stays-in-theme;within-theme-v1;"
          "theme-without-present-member-neutral";
      recipe["composition_redistribution"]=theme_redistribution_rule;
    }
    recipe["composition_weights_sha256"]=cfg.composition_weights_sha256;
  }
  return recipe;
}
// Recorded as recipe["research_fields"] exactly when a fields manifest is pinned
// (absent: recipe bytes unchanged). `loaded` is library-derived, identical per role.
Json fields_recipe(Json pins,const Library& lib) {
  return Json{{"schema",fields_schema},{"manifest_sha256",std::move(pins)},{"loaded",lib.extra_fields},
      {"semantics",fields_semantics}};
}
Json fields_pins(const IcRunnerConfig& cfg) {
  Json pins=Json::object();
  if (!cfg.train_fields_sha256.empty()) pins["train"]=cfg.train_fields_sha256;
  if (!cfg.validation_fields_sha256.empty()) pins["validation"]=cfg.validation_fields_sha256;
  return pins;
}
bool fields_pinned(const IcRunnerConfig& cfg) {
  return !cfg.train_fields_sha256.empty() || !cfg.validation_fields_sha256.empty();
}
namespace {
// A frozen TRAIN recipe records its pinned fields exactly as run_ic writes them.
// TRAIN's pin is taken from the source (its payload is never opened here) and, if
// re-supplied, must match; a validation pin must equal this run's, like role pins.
co::Status expect_frozen_fields(const IcRunnerConfig& cfg,const Library& lib,const Json& recipe,Json& expected) {
  if (!recipe.contains("research_fields")) {
    // A library declaring extras cannot have been scored on TRAIN without a pin.
    if (!lib.declared_extra.empty() || !cfg.train_fields_sha256.empty())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN research fields pin differs");
    return co::Ok();
  }
  const auto& source=recipe.at("research_fields");
  if (!source.is_object() || !source.contains("manifest_sha256") || !source.at("manifest_sha256").is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN research fields record");
  const auto& source_pins=source.at("manifest_sha256");
  Json pins=Json::object();
  if (source_pins.contains("train")) {
    const auto& train=source_pins.at("train");
    if (!train.is_string() || !hash_valid(train.get<std::string>()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN research fields record");
    pins["train"]=train;
  }
  if (!cfg.train_fields_sha256.empty() && (!pins.contains("train") || pins.at("train")!=cfg.train_fields_sha256))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN research fields pin differs");
  if (source_pins.contains("validation")) pins["validation"]=cfg.validation_fields_sha256;
  expected["research_fields"]=fields_recipe(std::move(pins),lib);
  return co::Ok();
}
} // namespace
co::Result<FrozenTrain> frozen_train(const IcRunnerConfig& cfg,const Library& lib,const Role& train) {
  ATX_TRY(auto artifact,pinned_json(cfg.orientations_path,cfg.orientations_sha256));
  if (artifact.at("schema")!="atx.dsl-ic-orientations/v1" ||
      artifact.at("library_sha256")!=cfg.library_sha256 || artifact.at("train_manifest_sha256")!=cfg.train_sha256)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN artifact identity");
  const auto recipe_sha=artifact.at("recipe_sha256").get<std::string>();
  if (!hash_valid(recipe_sha)) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN recipe SHA");
  // The externally pinned artifact binds the adjacent recipe by canonical JSON
  // hash, not by trusting a second self-reported filename/hash pair.
  ATX_TRY(auto text,metadata_text((std::filesystem::path(cfg.orientations_path).parent_path()/"recipe.json").string()));
  auto recipe=Json::parse(text); ATX_TRY(auto actual,co::sha256_hex(recipe.dump()));
  if (actual!=recipe_sha) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN recipe hash differs");
  if (!recipe.at("max_working_bytes").is_number_integer() ||
      (recipe.contains("vm_workers") && !recipe.at("vm_workers").is_number_integer()))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN resource types");
  const auto source_bytes=recipe.at("max_working_bytes").get<i64>();
  const auto source_workers=recipe.value("vm_workers",i64{1});
  if (source_bytes<(32LL<<20) || source_bytes>(16LL<<30) || source_workers<1 ||
      source_workers>static_cast<i64>(ex::max_research_ic_workers))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN resource bounds");
  // T14 ruling (strict M1): a blend frozen WITH pinned weights is never a validation
  // source. Its artifact hashes a recipe that pins the weights, whose provenance
  // names the orientations they were fit on, so that provenance can never name
  // this artifact. The shipped flow is: unweighted TRAIN run (orientations O) ->
  // fitter -> weights W (provenance names O) -> validation-only run with O + W,
  // which reproduces a weighted run's validation bytes exactly.
  if (recipe.contains("composition_weights_sha256"))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN artifact is a weighted run; validate "
        "from the unweighted TRAIN run whose orientations the weights were fit on (the weights' "
        "provenance.orientations_sha256)");
  auto source_cfg=cfg; source_cfg.max_working_bytes=static_cast<u64>(source_bytes);
  source_cfg.workers=static_cast<usize>(source_workers);
  source_cfg.save_combined=recipe.contains("saved_combined");
  source_cfg.composition_weights_sha256.clear();
  // Earlier completed TRAIN artifacts used parallel VM but serial IC. Absence
  // means precisely that original execution path, not unknown numerical policy.
  auto expected=method_recipe(source_cfg,recipe.contains("research_ic_workers"),false);
  expected["role_manifest_sha256"]["train"]=cfg.train_sha256;
  if (recipe.at("role_manifest_sha256").contains("validation"))
    expected["role_manifest_sha256"]["validation"]=cfg.validation_sha256;
  ATX_TRY_VOID(expect_frozen_fields(cfg,lib,recipe,expected));
  if (recipe!=expected)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN method/statistical settings/role pins differ");
  const auto& rows=artifact.at("candidates");
  if (!rows.is_array() || rows.size()!=lib.candidates.size())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN candidate count");
  const auto score_dates=train.metadata.at("score_end").get<u64>()-train.metadata.at("score_begin").get<u64>();
  const auto max_dates=score_dates>22?score_dates-22:0;
  std::vector<int> signs; signs.reserve(rows.size());
  for (usize k=0;k<rows.size();++k) {
    const auto& row=rows[k]; const auto& c=lib.candidates[k];
    if (row.at("id")!=c.id || row.at("family")!=c.family || row.at("dsl_sha256")!=c.dsl_sha ||
        row.at("orientation_horizon")!=21 || row.at("composition_selection")!="all-fixed-candidates-no-screen-selection" ||
        row.at("fit_status")!="noisy-TRAIN-sample-orientation-not-significance" ||
        !row.at("sign").is_number_integer() || !row.at("sample_orientation_sign").is_number_integer())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN candidate recipe");
    const auto& horizons=row.at("ic").at("horizons");
    if (!horizons.is_array() || horizons.size()!=3 || horizons[0].at("horizon")!=5 ||
        horizons[1].at("horizon")!=21 || horizons[2].at("horizon")!=63)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN horizons");
    const auto& estimate=horizons[1].at("rank");
    if (!estimate.at("valid_dates").is_number_integer())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN date count type");
    const auto dates=estimate.at("valid_dates").get<i64>();
    if (dates<0 || static_cast<u64>(dates)>max_dates || row.at("orientation_dates")!=dates ||
        (dates==0 && !estimate.at("mean").is_null()) || (dates>0 && !estimate.at("mean").is_number()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN mature observations");
    const auto mean=dates>0?estimate.at("mean").get<f64>():0;
    if (!std::isfinite(mean) || std::abs(mean)>1.000000000001)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN rank mean");
    const int sign=mean>0?1:(mean<0?-1:0);
    if (row.at("sign")!=sign || row.at("sample_orientation_sign")!=sign ||
        row.at("orientation_defined")!=(sign!=0))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN sign differs from recorded sample");
    signs.push_back(sign);
  }
  // expect_frozen_fields validated the record's shape and hash above.
  std::string train_fields_sha;
  if (recipe.contains("research_fields") && recipe.at("research_fields").at("manifest_sha256").contains("train"))
    train_fields_sha=recipe.at("research_fields").at("manifest_sha256").at("train").get<std::string>();
  return co::Ok(FrozenTrain{std::move(artifact),std::move(recipe),std::move(signs),recipe_sha,
                            std::move(train_fields_sha)});
}
namespace {
struct Budget {
  u64 limit{},used{};
  bool add(u64 n,u64 width) { if (width && n>(limit-used)/width) return false; used+=n*width; return true; }
};
} // namespace
// `themes`: pinned within-theme redistribution themes (0: none, admission unchanged).
co::Result<Role> admit(const IcRunnerConfig& cfg,const Library& lib,std::string path,
                      std::string pin,std::string name,bool enforce_budget,usize themes) {
  ATX_TRY(auto j,pinned_json(path,pin));
  const auto d=j.at("dates").get<u64>(),n=j.at("instruments").get<u64>();
  const auto begin=j.at("score_begin").get<u64>(),end=j.at("score_end").get<u64>();
  if (!d || d>4096 || !n || n>20000 || end!=d || begin>=end || begin<383 ||
      lib.lookback>begin-63)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: role shape/warmup/maturity");
  const auto cells=d*n,score_dates=end-begin;
  ATX_TRY(auto composition,ic_composition_working_bytes(static_cast<usize>(d),
      static_cast<usize>(n),lib.candidates.size(),themes));
  Budget b{std::numeric_limits<u64>::max(),0};
  // One role26, guard4, effective+VM masks2, returned signal8, VM scratch32;
  // One maximum compiled slot payload: the runner destroys an undersized Engine
  // before creating its replacement. Output SignalSet has its own8B/cell above.
  // That returned-signal8 is also the single reused candidate buffer: a cache
  // hit loads into it, and it is released before any VM evaluation allocates.
  // Cache I/O and hashing stream it in place in 1MiB slices: no second copy.
  // The fresh Engine's initial1x1 pool is covered by the fixed slack. No surfaces,
  // execution context, per-candidate retained signals, or book position arrays.
  // --no-composition builds no blend, so its composition plane is not admitted.
  const u64 blend=cfg.no_composition?0:composition;
  if (!b.add(1,32ULL<<20) || !b.add(cells,72+8*lib.max_slots) || !b.add(blend,1) ||
      !b.add(d,512) || !b.add(n,512))
    return co::Err(co::ErrorCode::Unavailable,"IC runner: combined role/VM/composition memory budget");
  // Extra fields: at most field_plan.capacity columns are ever resident (8B/cell
  // each; FieldResidency enforces it), plus the 1B/cell owned presence mask of the
  // borrowed DSL panel (base columns are borrowed, never copied). Nothing is added
  // without extras, so default admission is unchanged.
  if (!lib.extra_fields.empty() && !b.add(cells,8*static_cast<u64>(lib.field_plan.capacity)+1))
    return co::Err(co::ErrorCode::Unavailable,"IC runner: research field memory budget");
  for (u64 h:{5ULL,21ULL,63ULL}) {
    const auto mature=score_dates>h+1?score_dates-h-1:0;
    if (!b.add(mature*n,16) || !b.add(mature,32))
      return co::Err(co::ErrorCode::Unavailable,"IC runner: combined IC label/rank/scratch budget");
  }
  // No second VM or label cache: only worker-local Cs/TS scratch. Allow 2x
  // vector growth within1024B/name and64B/date; add64B/name for IC row buffers;
  // separately reserve8MiB stack
  // address/commit envelope plus64KiB runtime slack per explicit worker. This
  // conservative admission is not a measured thread-stack/RSS guarantee.
  // Pooled composition adds one 16B/name ranked row per worker (T15).
  if (cfg.workers>1 && (!b.add(cfg.workers,(8ULL<<20)+(64ULL<<10)) ||
      !b.add(cfg.workers*n,1024+64+16) || !b.add(cfg.workers*d,64)))
    return co::Err(co::ErrorCode::OutOfRange,"IC runner: worker scratch/stack envelope overflow");
  if (enforce_budget && b.used>cfg.max_working_bytes)
    return co::Err(co::ErrorCode::Unavailable,"IC runner: required_bytes="+std::to_string(b.used)+
        " max_compiled_slots="+std::to_string(lib.max_slots)+" exceeds configured memory budget before payload load");
  return co::Ok(Role{std::move(path),std::move(pin),std::move(name),std::move(j),b.used,RoleFields{}});
}
namespace {
// The producer flags every field entry with point_in_time (bool) and
// non_pit_aspects (strings, empty iff point in time). An entry without both, or
// with a contradictory pair, refuses the whole manifest: absence cannot be shown
// safe (pre-flag manifests listed look-ahead fields unmarked). Returns the aspects
// comma-joined, empty for a point-in-time field.
co::Result<std::string> non_pit_aspects(const Json& row,const std::string& where) {
  if (!row.contains("point_in_time") || !row.at("point_in_time").is_boolean() ||
      !row.contains("non_pit_aspects") || !row.at("non_pit_aspects").is_array())
    return co::Err(co::ErrorCode::InvalidArgument,where+" lacks point_in_time/non_pit_aspects flags");
  std::string aspects;
  for (const auto& aspect:row.at("non_pit_aspects")) {
    if (!aspect.is_string() || aspect.get<std::string>().empty())
      return co::Err(co::ErrorCode::InvalidArgument,where+" has a malformed non_pit_aspects entry");
    aspects+=(aspects.empty()?"":",")+aspect.get<std::string>();
  }
  if (row.at("point_in_time").get<bool>()==!aspects.empty())
    return co::Err(co::ErrorCode::InvalidArgument,where+" point_in_time contradicts non_pit_aspects");
  return co::Ok(std::move(aspects));
}
// The producer's spec-level meaning of a field, identical across roles written by
// one producer version: not its role-specific sources, coverage or counts.
Json field_definition(const Json& row) {
  Json out=Json::object();
  for (const auto* key:{"units","clock","staleness","source_columns","definition","point_in_time","non_pit_aspects"})
    out[key]=row.contains(key)?row.at(key):Json(nullptr);
  Json domain=nullptr;
  if (row.contains("plausibility") && row.at("plausibility").is_object()) {
    const auto& p=row.at("plausibility"); domain=Json::object();
    for (const auto* key:{"min","max","inclusive","rule"}) domain[key]=p.contains(key)?p.at(key):Json(nullptr);
  }
  out["plausibility"]=std::move(domain);
  return out;
}
} // namespace
// Review M5: with both manifests pinned, every declared extra must mean the same
// thing in TRAIN and validation (a different producer version would otherwise pass).
co::Status same_field_definitions(const Library& lib,const Role& train,const Role& validation) {
  if (train.fields.sha.empty() || validation.fields.sha.empty()) return co::Ok();
  for (const auto& name:lib.declared_extra)
    if (train.fields.declared.at(name)!=validation.fields.declared.at(name))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: research field '"+name+
          "' definition differs between the train and validation fields manifests");
  return co::Ok();
}
// Binds a pinned fields manifest to an admitted role before any payload: schema,
// role manifest pin, sessions/ids receipts and shape. Every declared extra must be
// present; referenced extras are selected for load and, for a scored role, their
// extents stat'ed. A declared extra with no pinned manifest refuses for a scored
// role; an unscored (frozen-TRAIN) role needs none.
co::Status bind_fields(const Library& lib,Role& role,const std::string& directory,const std::string& pin,
                       bool scored) {
  const auto option="--"+role.name+"-fields";
  if (directory.empty()) {
    if (scored && !lib.declared_extra.empty())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: library field '"+lib.declared_extra.front()+
          "' is not a role price field and no "+option+" manifest is pinned");
    return co::Ok();
  }
  const auto dir=std::filesystem::path(directory);
  // The option names the fields DIRECTORY (its manifest.json is what the SHA pins);
  // a file path here would otherwise surface only as a missing DIR/manifest.json.
  if (std::error_code ec; !std::filesystem::is_directory(dir,ec))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+option+" must name the fields directory "
        "(the one holding manifest.json), not a file or missing path: "+directory);
  ATX_TRY(auto j,pinned_json((dir/"manifest.json").string(),pin));
  const auto d=role.metadata.at("dates").get<u64>(),n=role.metadata.at("instruments").get<u64>();
  const auto& receipts=role.metadata.at("files");
  if (!j.is_object() || j.value("schema",std::string{})!=fields_schema ||
      j.value("status",std::string{})!="complete" || !j.contains("role") || !j.at("role").is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+role.name+" fields manifest schema");
  const auto& bound=j.at("role");
  if (bound.value("manifest_sha256",std::string{})!=role.sha ||
      receipts.at("sessions.i64").at("sha256")!=bound.value("sessions_sha256",std::string{}) ||
      receipts.at("ids.u64").at("sha256")!=bound.value("ids_sha256",std::string{}) ||
      !bound.contains("dates") || bound.at("dates")!=d || !bound.contains("instruments") ||
      bound.at("instruments")!=n)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+role.name+
        " fields manifest role binding differs from the pinned role manifest/axes");
  const auto& rows=j.at("fields"); const auto& files=j.at("files");
  if (!rows.is_array() || rows.empty() || rows.size()>max_field_manifest_rows || !files.is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+role.name+" fields manifest field list (1.."+
        std::to_string(max_field_manifest_rows)+" rows)");
  const auto bytes=d*n*sizeof(f64);
  std::map<std::string,FieldFile> available;
  for (const auto& row:rows) {
    const auto name=row.at("name").get<std::string>(),file=row.at("file").get<std::string>();
    const auto sha=row.at("sha256").get<std::string>();
    ATX_TRY(auto aspects,non_pit_aspects(row,"IC runner: "+role.name+" fields manifest entry "+name));
    if (!field_identifier(name) || base_field(name) || file!=name+".f64" || row.at("dtype")!="<f8" ||
        row.at("layout")!="date-major" || row.at("shape")!=Json::array({d,n}) || !hash_valid(sha) ||
        !files.contains(file) || files.at(file).at("sha256")!=sha || files.at(file).at("bytes")!=bytes ||
        !available.emplace(name,FieldFile{name,dir/file,sha,bytes,std::move(aspects),field_definition(row)}).second)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: "+role.name+" fields manifest entry: "+name);
  }
  for (const auto& name:lib.declared_extra) {
    const auto it=available.find(name);
    if (it==available.end())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: library field '"+name+
          "' is neither a role price field nor in the pinned "+role.name+" fields manifest");
    // Look-ahead guard with no override: a non-point-in-time field is never
    // declared, so never loaded (referenced fields are a subset of declared).
    if (!it->second.non_pit_aspects.empty())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: library field '"+name+
          "' is not point-in-time in the pinned "+role.name+" fields manifest (non_pit_aspects: "+
          it->second.non_pit_aspects+"); refusing look-ahead");
  }
  RoleFields bound_fields{directory,pin,{},{}};
  for (const auto& name:lib.declared_extra) bound_fields.declared[name]=available.at(name).definition;
  for (const auto& name:lib.extra_fields) {
    const auto& field=available.at(name);
    std::error_code ec; const auto size=std::filesystem::file_size(field.path,ec);
    if (scored && (ec || size!=field.bytes))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: research field payload extent: "+
          field.path.filename().string());
    bound_fields.load.push_back(field);
  }
  role.fields=std::move(bound_fields);
  return co::Ok();
}
namespace {
// Duplicate object keys would silently keep the last weight; refuse them instead.
// Malformed text and numeric overflow (e.g. 1e999) are parse refusals here.
co::Result<Json> unique_key_json(const std::string& text) {
  std::vector<std::set<std::string>> open; bool duplicate=false;
  auto j=Json::parse(text,[&](int,Json::parse_event_t event,Json& value) {
    if (event==Json::parse_event_t::object_start) open.emplace_back();
    else if (event==Json::parse_event_t::object_end && !open.empty()) open.pop_back();
    else if (event==Json::parse_event_t::key && !open.empty() &&
             !open.back().insert(value.get<std::string>()).second) duplicate=true;
    return true;
  },false);
  if (j.is_discarded()) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights JSON parse");
  if (duplicate) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights duplicate key");
  return co::Ok(std::move(j));
}
// Optional `signs`: id -> integer +1/-1, known ids only; every candidate with a
// positive weight must carry one (the weights were fit on so-oriented returns).
co::Result<std::vector<int>> composition_signs(const Json& j,const Library& lib,std::span<const f64> weights) {
  std::vector<int> signs;
  if (!j.contains("signs")) return co::Ok(std::move(signs));
  const auto& rows=j.at("signs");
  if (!rows.is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition signs must be an object");
  std::set<std::string> ids;
  for (const auto& c:lib.candidates) ids.insert(c.id);
  for (auto it=rows.begin();it!=rows.end();++it) {
    if (!ids.contains(it.key()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition sign for unknown candidate: "+it.key());
    if (!it->is_number_integer() || (it->get<i64>()!=1 && it->get<i64>()!=-1))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition sign must be +1 or -1: "+it.key());
  }
  signs.reserve(lib.candidates.size());
  for (usize k=0;k<lib.candidates.size();++k) {
    const auto it=rows.find(lib.candidates[k].id);
    if (it==rows.end() && weights[k]>0)
      return co::Err(co::ErrorCode::InvalidArgument,
          "IC runner: composition sign missing for weighted candidate: "+lib.candidates[k].id);
    signs.push_back(it==rows.end()?0:static_cast<int>(it->get<i64>()));
  }
  return co::Ok(std::move(signs));
}
// Optional top-level `theme_redistribution` (fitter ew-theme-v6, v4-prereg v6 revision
// V6-W): exactly {"rule":"within-theme-v1","composition":"ew-theme-v6","themes":{id:
// theme}} with known ids, names [a-z0-9_]{1,64}, a theme for every positive-weight
// candidate and 1..32 themes. Indices follow first appearance in library order; a
// zero-weight candidate keeps 0 (ignored by the composition). Absent: pinned.themes
// stays empty and nothing downstream changes. The block requires schema v2 and v2
// requires the block (checked by composition_weights).
co::Status composition_themes(const Json& j,const Library& lib,PinnedWeights& pinned) {
  if (!j.contains("theme_redistribution")) return co::Ok();
  const auto& block=j.at("theme_redistribution");
  if (!block.is_object() || !block.contains("rule") || block.at("rule")!=theme_redistribution_rule ||
      !block.contains("composition") || block.at("composition")!=theme_redistribution_composition ||
      !block.contains("themes") || !block.at("themes").is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_redistribution must be {rule: within-theme-v1, "
        "composition: ew-theme-v6, themes: {id: theme}}");
  const auto& rows=block.at("themes");
  std::set<std::string> ids;
  for (const auto& c:lib.candidates) ids.insert(c.id);
  const auto theme_name=[](const std::string& s) {
    return !s.empty() && s.size()<=64 && std::all_of(s.begin(),s.end(),[](char c) {
      return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_';
    });
  };
  for (auto it=rows.begin();it!=rows.end();++it) {
    if (!ids.contains(it.key()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme for unknown candidate: "+it.key());
    if (!it->is_string() || !theme_name(it->get<std::string>()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme name must match [a-z0-9_]{1,64}: "+it.key());
  }
  std::vector<std::string> names;
  std::vector<usize> index(lib.candidates.size(),0);
  for (usize k=0;k<lib.candidates.size();++k) {
    if (!(pinned.values[k]>0)) continue;
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
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_redistribution needs 1..32 weighted themes");
  pinned.themes=std::move(index); pinned.theme_count=names.size();
  return co::Ok();
}
} // namespace
// Runs before any role payload load (including under --plan-only); every refusal
// is loud. Selection hygiene: the file must name the TRAIN role manifest it was
// fitted on (top-level train_manifest_sha256 == --train-sha256; in validation-only
// mode --train-sha256 is also the frozen TRAIN artifact's). Unknown keys are allowed.
co::Result<PinnedWeights> composition_weights(const IcRunnerConfig& cfg,const Library& lib) {
  PinnedWeights pinned;
  if (cfg.composition_weights_path.empty()) return co::Ok(std::move(pinned));
  auto& weights=pinned.values;
  ATX_TRY(auto text,pinned_text(cfg.composition_weights_path,cfg.composition_weights_sha256));
  ATX_TRY(auto j,unique_key_json(text));
  if (!j.is_object() || !j.contains("schema") ||
      (j.at("schema")!=weights_schema && j.at("schema")!=weights_schema_v2) ||
      !j.contains("library_sha256") || j.at("library_sha256")!=cfg.library_sha256 ||
      !j.contains("weights") || !j.at("weights").is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights schema/library identity");
  const auto& rows=j.at("weights");
  std::set<std::string> ids;
  for (const auto& c:lib.candidates) ids.insert(c.id);
  for (auto it=rows.begin();it!=rows.end();++it)
    if (!ids.contains(it.key()))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weight for unknown candidate: "+it.key());
  weights.reserve(lib.candidates.size());
  for (const auto& c:lib.candidates) {
    const auto it=rows.find(c.id);
    if (it==rows.end())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weight missing: "+c.id);
    const f64 w=it->is_number()?it->get<f64>():quiet_nan;
    if (!std::isfinite(w) || w<0)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weight must be finite and >= 0: "+c.id);
    weights.push_back(w);
  }
  if (!j.contains("train_manifest_sha256") || !j.at("train_manifest_sha256").is_string() ||
      j.at("train_manifest_sha256")!=cfg.train_sha256)
    return co::Err(co::ErrorCode::InvalidArgument,
        "IC runner: composition weights TRAIN binding: train_manifest_sha256 must equal --train-sha256");
  ATX_TRY(pinned.signs,composition_signs(j,lib,weights));
  ATX_TRY_VOID(composition_themes(j,lib,pinned));
  const bool v2=j.at("schema")==weights_schema_v2;
  if (v2 && pinned.themes.empty())
    return co::Err(co::ErrorCode::InvalidArgument,
        "IC runner: composition weights schema atx.dsl-composition-weights/v2 requires a theme_redistribution block");
  if (!v2 && !pinned.themes.empty())
    return co::Err(co::ErrorCode::InvalidArgument,
        "IC runner: theme_redistribution requires composition weights schema atx.dsl-composition-weights/v2");
  if (j.contains("provenance")) pinned.provenance=j.at("provenance");
  return co::Ok(std::move(pinned));
}
// The plan/summary record of pinned weights: SHA, TRAIN binding, signs mode, the
// file's provenance pins (null when absent) and how they are tied to TRAIN.
Json weights_summary(const IcRunnerConfig& cfg,const PinnedWeights& pinned,const char* binding) {
  const auto& p=pinned.provenance;
  const auto pin=[&](const char* key) { return p.is_object() && p.contains(key)?p.at(key):Json(nullptr); };
  Json out{{"sha256",cfg.composition_weights_sha256},{"train_manifest_sha256",cfg.train_sha256},
      {"signs",pinned.signs.empty()?"TRAIN-orientation-signs":"pinned-candidate-signs"},
      {"provenance_orientations_sha256",pin("orientations_sha256")},
      {"provenance_fields_manifest_sha256",pin("fields_manifest_sha256")},{"binding",binding}};
  // ew-theme-v6 only (absent otherwise): the pinned within-theme redistribution.
  if (!pinned.themes.empty()) out["redistribution"]=theme_redistribution_rule;
  return out;
}
// Review M1 (root ruling, strict): weights applied against a frozen TRAIN artifact
// must name it as the orientations they were fit on. The T11 fitter records
// provenance.orientations_sha256 (the orientations.json file SHA it consumed) and
// provenance.fields_manifest_sha256 (the TRAIN fields pin, or null). The first must
// equal --orientations-sha256, full stop (frozen_train already refused a weighted
// frozen source, whose artifact no provenance can name); the second must equal the
// frozen TRAIN fields pin (absent or null when TRAIN pinned none).
co::Result<Json> frozen_weights_binding(const IcRunnerConfig& cfg,const PinnedWeights& pinned,
                                        const FrozenTrain& frozen) {
  const auto& p=pinned.provenance;
  if (!p.is_object() || !p.contains("orientations_sha256") || !p.at("orientations_sha256").is_string() ||
      !hash_valid(p.at("orientations_sha256").get<std::string>()))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: validation-only composition weights need "
        "provenance.orientations_sha256 naming the frozen TRAIN orientations they were fit on");
  if (p.at("orientations_sha256")!=cfg.orientations_sha256)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights provenance.orientations_sha256 "
        "must equal the frozen TRAIN --orientations-sha256 (weights were fit on other orientations)");
  const Json expected_fields=frozen.train_fields_sha.empty()?Json(nullptr):Json(frozen.train_fields_sha);
  const Json recorded_fields=p.contains("fields_manifest_sha256")?p.at("fields_manifest_sha256"):Json(nullptr);
  if (recorded_fields!=expected_fields)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: composition weights provenance.fields_manifest_sha256 "
        "must equal the frozen TRAIN fields manifest pin (null when TRAIN pinned none)");
  return co::Ok(weights_summary(cfg,pinned,"provenance-orientations-equal-frozen-TRAIN-orientations-artifact"));
}
// Review N3: validation-only runs compare the validation fields manifest with the
// frozen TRAIN definitions even without --train-fields. The TRAIN artifact records
// them (orientations.json research_fields.definitions); an older artifact without
// that record needs --train-fields, whose manifest same_field_definitions compared.
// Returns how the check was satisfied (empty: no declared extras).
co::Result<std::string> frozen_field_definitions(const Library& lib,const FrozenTrain& frozen,const Role& train,
                                                 const Role& validation) {
  if (lib.declared_extra.empty()) return co::Ok(std::string{});
  const auto& artifact=frozen.artifact;
  if (!artifact.contains("research_fields")) {
    if (train.fields.sha.empty())
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN artifact records no research field "
          "definitions; pass --train-fields/--train-fields-sha256 (the frozen TRAIN pin) so the validation "
          "field definitions can be checked");
    return co::Ok(std::string("train-fields-manifest"));
  }
  const auto& record=artifact.at("research_fields");
  if (!record.is_object() || !record.contains("manifest_sha256") || record.at("manifest_sha256")!=frozen.train_fields_sha ||
      !record.contains("definitions") || !record.at("definitions").is_object())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: frozen TRAIN artifact research field record differs "
        "from its recipe pin");
  const auto& definitions=record.at("definitions");
  for (const auto& name:lib.declared_extra)
    if (!definitions.contains(name) || definitions.at(name)!=validation.fields.declared.at(name))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: research field '"+name+
          "' definition differs between the frozen TRAIN artifact and the validation fields manifest");
  return co::Ok(std::string("frozen-TRAIN-artifact"));
}
} // namespace atx::impl::strategy::ic_detail
