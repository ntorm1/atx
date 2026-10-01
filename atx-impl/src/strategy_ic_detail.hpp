#pragma once
// Internal to the IC runner translation units (strategy_ic_*.cpp): the types,
// constants and helpers they share. Platform v8 B-3 split strategy_ic_runner.cpp
// at its seams and moved the code verbatim, so every comment still describes the
// same code. Not a public interface: callers use strategy_ic_runner.hpp.
//   strategy_ic_library.cpp       pinned inputs, library + field plan, field residency
//   strategy_ic_admission.cpp     method recipe, frozen TRAIN, memory admission,
//                                 fields binding, composition weights
//   strategy_ic_signal_cache.cpp  VM identity + source pin, candidate signal cache, report
//   strategy_ic_result_cache.cpp  IC identity + source pin, IC-result cache
//   strategy_ic_runner.cpp        score_role, outputs, run_ic, dispatch_ic
#include <array>
#include <bitset>
#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/strategy_data.hpp"
#include "atx/engine/factory/ic_research.hpp"
#include "strategy_ic_composition.hpp"
#include "strategy_ic_runner.hpp"

namespace atx::impl::strategy::ic_detail {
namespace co=atx::core;
namespace al=atx::engine::alpha;
namespace ex=atx::engine::factory;
using Json=nlohmann::json;
// ---- Shared constants ------------------------------------------------------
inline constexpr f64 quiet_nan=std::numeric_limits<f64>::quiet_NaN();
inline constexpr const char* vm_eval_mode="ResearchFast;full-historical-asof-member-mask";
// ew-theme-v6 (v4-prereg v6 revision V6-W): the only admitted theme_redistribution rule.
inline constexpr const char* theme_redistribution_rule="within-theme-v1";
// ew-theme-std-v1 (platform v8 R-1): the theme_standardise rule whose per-date standardisation
// every row of the rule table runs (strategy_ic_admission.cpp; platform v8 R-10 adds ic-shrink-v1).
inline constexpr const char* theme_standardise_rule="ew-theme-std-v1";
inline constexpr const char* fields_schema="atx.research-role-fields/v1";
inline constexpr const char* cache_schema_v2="atx.dsl-candidate-signal/v2";
inline constexpr usize io_chunk=1U<<20;
inline constexpr const char* ic_price_field="close";
// Field caps (platform v8 B-2). A pinned fields manifest may list up to 1,024 rows
// (only declared extras are bound, only referenced ones loaded); a library may
// reference up to 256 distinct extra fields, one FieldMask bit each.
inline constexpr usize max_field_manifest_rows=1024;
inline constexpr usize max_extra_fields=256;
// Metadata files (library, role manifests, recipes, sidecars, weights): at most 1 MiB. A
// fields manifest is read under ic_fields_manifest_max_bytes instead (strategy_ic_runner.hpp).
inline constexpr u64 max_metadata_bytes=1ULL<<20;
// Bit f = Library::extra_fields[f]. In-memory only: no mask is ever written to a
// manifest, cache sidecar or summary, so no on-disk format depends on its width.
using FieldMask=std::bitset<max_extra_fields>;
// FP-relevant build flavor of the IC runner TUs (strategy_ic_runner.cpp instantiates
// the header-only VM; every strategy_ic_*.cpp shares one flag set in
// atx-impl/CMakeLists.txt, so these inline variables have one definition):
// compiler major.minor and FMA/AVX2/fast-math. Patch-level compiler updates are
// assumed not to change strict-FP results. clang-cl defines both __clang__ and
// _MSC_VER; the clang branch wins.
#define ATX_IC_STRINGIZE2(x) #x
#define ATX_IC_STRINGIZE(x) ATX_IC_STRINGIZE2(x)
#if defined(__clang__)
inline constexpr std::string_view vm_compiler=
    "clang" ATX_IC_STRINGIZE(__clang_major__) "." ATX_IC_STRINGIZE(__clang_minor__);
#elif defined(_MSC_VER)
inline constexpr std::string_view vm_compiler="msvc" ATX_IC_STRINGIZE(_MSC_VER);
#else
inline constexpr std::string_view vm_compiler{}; // unknown: --candidate-cache refuses
#endif
#undef ATX_IC_STRINGIZE
#undef ATX_IC_STRINGIZE2
inline constexpr std::string_view vm_fp_flavor=""
#if defined(__FMA__)
    "_fma"
#endif
#if defined(__AVX2__)
    "_avx2"
#endif
#if defined(__FAST_MATH__)
    "_fastmath"
#endif
    ;
// ---- Library, roles, pinned inputs ------------------------------------------
// extra_fields: the sorted non-base fields this compiled program loads.
struct Candidate { std::string id,family,dsl_sha; al::Program program; std::vector<std::string> extra_fields; };
// Extra-field residency schedule (FieldMask bit f = Library::extra_fields[f]).
// Candidates run in library order because the blend accumulates in that order, so
// at most `capacity` columns are resident: the most extras any one candidate reads.
// needs[k] = candidate k's fields; planned[k] = the resident set while k runs, by
// Belady's MIN over library order (a field no later candidate reads is dropped;
// a load at capacity evicts the resident field read farthest ahead, lowest index
// on ties). It depends on the library alone, so admission counts `capacity`
// columns rather than the union of referenced fields.
struct FieldPlan { usize capacity{},loads{}; std::vector<FieldMask> needs,planned; };
// declared_extra: every declared non-base field; extra_fields: the sorted union
// the compiled programs reference -- the only extra columns ever loaded.
struct Library {
  std::string id; std::vector<Candidate> candidates; usize max_slots{},lookback{};
  std::vector<std::string> declared_extra,extra_fields; FieldPlan field_plan;
};
// non_pit_aspects: the producer's list, comma-joined; empty iff point in time.
// definition: the producer's spec-level description, compared across roles.
struct FieldFile {
  std::string name; std::filesystem::path path; std::string sha; u64 bytes{}; std::string non_pit_aspects;
  Json definition;
};
// A role's pinned fields manifest (empty sha: none pinned), the referenced subset
// to load (in Library::extra_fields order), each with its manifest-pinned SHA256
// and exact extent, and every declared extra's definition.
struct RoleFields { std::string directory,sha; std::vector<FieldFile> load; std::map<std::string,Json> declared; };
struct Role { std::string path,sha,name; Json metadata; u64 bytes{}; RoleFields fields; };
// train_fields_sha: the frozen TRAIN fields manifest pin (empty: none).
struct FrozenTrain {
  Json artifact,recipe; std::vector<int> signs; std::string recipe_sha; std::string train_fields_sha;
};
// Pinned per-candidate weights in library order (empty = default equal family/
// within-family weights) and, when the file carries `signs`, the blend sign per
// candidate (+1/-1; 0 only for an unsigned zero-weight candidate). Empty signs =
// the runner's TRAIN IC orientation.
// `provenance`: the file's provenance object (null when absent), bound to a frozen
// TRAIN artifact by frozen_weights_binding in validation-only mode.
// `themes`: per-candidate theme index of an ew-theme-v6 theme_redistribution block
// (empty: none; see composition_themes), `theme_count` its number of themes.
// `std_themes`/`std_theme_count`: the same for an ew-theme-std-v1 theme_standardise block
// with rerank true (empty: no block, or rerank false -- then the composition is the plain
// pinned-weights path, the ew-theme-v1 blend bit for bit); `standardise` records a present
// block for the summary ("" absent; else the rule, suffixed ";rerank-off" when off).
struct PinnedWeights {
  std::vector<f64> values; std::vector<int> signs; Json provenance; std::vector<usize> themes; usize theme_count{};
  std::vector<usize> std_themes; usize std_theme_count{}; std::string standardise;
  // What the composition and admission receive: the standardised themes under their rule,
  // else the (possibly empty) within-theme-v1 themes under redistribute.
  [[nodiscard]] IcThemeRule theme_rule() const noexcept {
    return std_themes.empty()?IcThemeRule::redistribute:IcThemeRule::standardise;
  }
  [[nodiscard]] std::span<const usize> composition_themes() const noexcept {
    return std_themes.empty()?std::span<const usize>(themes):std::span<const usize>(std_themes);
  }
  [[nodiscard]] usize composition_theme_count() const noexcept {
    return std_themes.empty()?theme_count:std_theme_count;
  }
  // The theme_standardise rule the composition runs under ("" unless standardised, i.e. a block
  // with rerank true; then `standardise` is exactly the block's rule), recorded by the recipe and
  // the combined manifest. A view of `standardise`: valid while this object lives unchanged.
  [[nodiscard]] std::string_view standardise_rule() const noexcept {
    return std_themes.empty()?std::string_view{}:std::string_view(standardise);
  }
};
// ---- Candidate signal cache (layout: strategy_ic_signal_cache.cpp) ----------
// field name -> payload SHA256, ordered by name (the key's field order).
using FieldShas=std::map<std::string,std::string>;
struct CacheKey {
  std::filesystem::path dir; std::string stem; // v2 location: dir/stem.{f64,json}
  std::string role_sha; u64 dates{},instruments{}; std::string vm_identity;
  FieldShas fields;                 // every extra field the DSL reads (empty: base)
  std::string fields_manifest_sha;  // pinned manifest, provenance only (field candidates)
  std::string signal_key;           // SHA256(signal_key_text)
};
// A committed entry serving a candidate: dir/stem.{f64,json}; legacy = v1 layout.
struct CacheHit { std::filesystem::path dir; std::string stem,payload_sha; bool legacy{}; };
// Fields manifests known to have keyed v1 field entries, by manifest SHA256.
struct KnownManifest { std::string role_sha; FieldShas fields; };
using KnownManifests=std::map<std::string,KnownManifest>;
// SHA-256 time and verified bytes of one scored role (summary hash_seconds and
// verify_bytes). Null meter: not accounted.
struct HashMeter { f64 seconds{}; u64 verify_bytes{}; };
// Unique per attempt so a concurrent or killed writer never shares a partial.
// The name is ".<16-hex nonce>.partial" (25 characters) beside `final`, never
// derived from the final name: a partial path must not be the deepest path of an
// entry. Windows without long-path opt-in caps a path at 259 characters, and the
// v2 layout already spends ROOT/<64-hex role>/fp_<16>/ic1_<16>/<id>.<dsl16>.json;
// a ".<final name>.<nonce>.partial" name added ~26 characters on top of that and
// failed to open under deep roots while every final path still fit.
struct PartialFile {
  std::filesystem::path path;
  explicit PartialFile(const std::filesystem::path& final);
  PartialFile(const PartialFile&)=delete;
  PartialFile& operator=(const PartialFile&)=delete;
  PartialFile(PartialFile&&)=delete;
  PartialFile& operator=(PartialFile&&)=delete;
  // Published bytes live on under the final hard link. A partial that cannot be
  // removed is an inert dot-file no lookup ever reads, so cleanup is best effort.
  ~PartialFile();
};
// Every candidate's key and committed entry (nullopt: a miss), each entry
// identity-checked and its payload extent stat'ed; payload hashes are verified
// only when loaded. Metadata only: no role payload is opened.
struct CacheResolution {
  std::vector<CacheKey> keys; std::vector<std::optional<CacheHit>> hits; usize ready{},legacy{};
};
// ---- Field residency (strategy_ic_library.cpp) ------------------------------
// Size and modification time of a file: a reload of a field payload this process
// already verified skips the hash only while both are unchanged since then.
struct FileStamp {
  u64 size{}; std::filesystem::file_time_type modified{};
  bool operator==(const FileStamp&) const=default;
};
// Runtime side of FieldPlan for one scored role. The resident set never leaves
// planned[k]: enter(k) drops what planned[k] excludes; panel_for(k) loads only the
// fields candidate k reads, and only when k needs the VM (a cache miss), so a warm
// run loads nothing. Any resident-set change destroys the VM first (it borrows
// the panel, which borrows the columns), so a live VM always matches panel_for's
// result. Each field file is hashed once per process: a load whose file stamp
// is unchanged since its verification (verify_fields, or an earlier load of this
// residency) trusts it; any other load hashes as the bytes land. Borrows `lib`,
// `spec` and `meter`; non-copyable because the panel aliases columns_.
class FieldResidency {
public:
  FieldResidency(const Library& lib,const Role& spec,usize cells,HashMeter& meter,
                 std::vector<std::optional<FileStamp>> verified)
      : lib_{lib},spec_{spec},cells_{cells},meter_{meter},columns_(lib.extra_fields.size()),
        verified_(std::move(verified)) {
    verified_.resize(lib.extra_fields.size());
  }
  FieldResidency(const FieldResidency&)=delete;
  FieldResidency& operator=(const FieldResidency&)=delete;
  FieldResidency(FieldResidency&&)=delete;
  FieldResidency& operator=(FieldResidency&&)=delete;
  void enter(usize k,std::unique_ptr<al::Engine>& vm,std::ostream& progress);
  [[nodiscard]] co::Result<const al::Panel*> panel_for(usize k,const al::Panel& base,const std::string& candidate,
                                                       std::unique_ptr<al::Engine>& vm,std::ostream& progress);
  void drop_all(std::unique_ptr<al::Engine>& vm) noexcept;
  [[nodiscard]] usize loads() const noexcept { return loads_; }
  [[nodiscard]] usize peak() const noexcept { return peak_; }
  [[nodiscard]] f64 seconds() const noexcept { return seconds_; }
private:
  const Library& lib_; const Role& spec_; usize cells_; HashMeter& meter_;
  std::vector<std::vector<f64>> columns_; // index = Library::extra_fields index; empty = not resident
  std::vector<std::optional<FileStamp>> verified_; // stamp at the verified load; nullopt = never hashed
  FieldMask resident_{};
  std::optional<al::Panel> panel_;
  usize loads_{},peak_{}; f64 seconds_{};
};
// ---- IC-result cache (layout: strategy_ic_result_cache.cpp) ----------------
// The daily IC series of one evaluation per active horizon (5, 21, 63): borrowed
// from the IC scratch right after evaluate_research_ic, or from a verified
// cached record (CachedIc). Valid until the next evaluation or record reuse.
struct IcSeries { std::array<std::span<const f64>,3> pearson,rank; };
struct IcCacheScope { Json key; std::string directory; };
struct CachedIc { ex::ResearchIcResult result; std::array<std::vector<f64>,3> pearson,rank; };
// ---- strategy_ic_library.cpp -------------------------------------------------
bool hash_valid(std::string_view value);
bool base_field(std::string_view name);
bool field_identifier(std::string_view s);
// `limit`: the file's byte bound (max_metadata_bytes, or ic_fields_manifest_max_bytes for a
// fields manifest); the refusal names it.
co::Result<std::string> metadata_text(const std::string& path,u64 limit=max_metadata_bytes);
co::Result<std::string> pinned_text(const std::string& path,const std::string& pin,u64 limit=max_metadata_bytes);
co::Result<Json> pinned_json(const std::string& path,const std::string& pin,u64 limit=max_metadata_bytes);
co::Result<Library> library(const IcRunnerConfig& cfg);
Json candidate_plan_rows(const Library& lib);
void release(std::vector<f64>& buffer) noexcept;
co::Status check_field_extents(const Role& spec);
co::Status verify_fields(const Role& spec,const FieldMask& needed,HashMeter& meter,
                         std::vector<std::optional<FileStamp>>& verified);
// ---- strategy_ic_admission.cpp -----------------------------------------------
// `standardised`: the theme_standardise rule of a standardised composition ("" none).
Json method_recipe(const IcRunnerConfig& cfg,bool parallel_ic=true,bool pinned_signs=false,bool themed=false,
                   std::string_view standardised={});
Json fields_recipe(Json pins,const Library& lib);
Json fields_pins(const IcRunnerConfig& cfg);
bool fields_pinned(const IcRunnerConfig& cfg);
co::Result<FrozenTrain> frozen_train(const IcRunnerConfig& cfg,const Library& lib,const Role& train);
co::Result<Role> admit(const IcRunnerConfig& cfg,const Library& lib,std::string path,
                      std::string pin,std::string name,bool enforce_budget=true,usize themes=0,
                      IcThemeRule rule=IcThemeRule::redistribute);
co::Status same_field_definitions(const Library& lib,const Role& train,const Role& validation);
co::Status bind_fields(const Library& lib,Role& role,const std::string& directory,const std::string& pin,
                       bool scored);
co::Result<PinnedWeights> composition_weights(const IcRunnerConfig& cfg,const Library& lib);
Json weights_summary(const IcRunnerConfig& cfg,const PinnedWeights& pinned,const char* binding);
co::Result<Json> frozen_weights_binding(const IcRunnerConfig& cfg,const PinnedWeights& pinned,
                                        const FrozenTrain& frozen);
co::Result<std::string> frozen_field_definitions(const Library& lib,const FrozenTrain& frozen,const Role& train,
                                                 const Role& validation);
// ---- strategy_ic_signal_cache.cpp --------------------------------------------
std::string vm_identity();
co::Status metered_update(co::Sha256& digest,std::span<const std::byte> bytes,HashMeter* meter);
std::filesystem::path cache_root(const IcRunnerConfig& cfg);
Json fields_json(const FieldShas& fields);
co::Status fields_bound(const Library& lib,const Role& role);
std::string hex(const std::array<std::byte,32>& bytes);
co::Status cache_preflight(const IcRunnerConfig& cfg,const Library& lib);
co::Result<KnownManifests> legacy_manifests(const IcRunnerConfig& cfg);
co::Status load_pinned_f64(const std::filesystem::path& path,const std::string& expected,usize cells,
                           std::vector<f64>& out,std::string_view what,HashMeter* meter,bool verify=true);
co::Status cache_load(const CacheHit& hit,usize cells,std::vector<f64>& out,HashMeter& meter);
co::Result<bool> publish_new(const std::filesystem::path& partial,const std::filesystem::path& final);
co::Result<std::string> write_partial(const PartialFile& partial,std::span<const std::byte> bytes,
                                      HashMeter* meter=nullptr);
co::Result<CacheHit> cache_store(const CacheKey& key,const IcRunnerConfig& cfg,const Role& spec,
    const engine::data::StrategyRoleData& role,const Candidate& c,std::span<f64> signal,HashMeter& meter);
co::Result<CacheResolution> cache_resolve(const IcRunnerConfig& cfg,const Library& lib,const Role& role,
                                          const KnownManifests& known);
co::Result<Json> cache_plan(const IcRunnerConfig& cfg,const Library& lib,const Role& role,
                            const KnownManifests& known);
co::Result<Json> cache_report(const IcRunnerConfig& cfg,const Library& lib,const std::vector<Role>& roles,
                              const KnownManifests& known);
// ---- strategy_ic_result_cache.cpp --------------------------------------------
std::string ic_identity();
IcSeries scratch_series(const ex::ResearchIcScratch& scratch);
IcSeries cached_series(const CachedIc& cached);
co::Result<IcCacheScope> ic_cache_scope(const Role& spec,const engine::data::StrategyRoleData& role,
    const ex::IcScreenConfig& ic,const ex::ResearchIcOptions& options,std::span<const u32> guard,
    HashMeter& meter);
co::Result<std::optional<CachedIc>> ic_cache_lookup(const std::filesystem::path& path,
    const IcCacheScope& scope,const Candidate& c,const std::string& signal_sha,
    const ex::ResearchIcScratch& scratch,usize active);
co::Status ic_cache_store(const std::filesystem::path& path,const IcCacheScope& scope,const Candidate& c,
    const std::string& signal_sha,const ex::ResearchIcResult& result,const IcSeries& daily,usize workers);
} // namespace atx::impl::strategy::ic_detail
