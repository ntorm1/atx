#include "strategy_ic_detail.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "build_provenance.hpp"

namespace atx::impl::strategy::ic_detail {
namespace {
// ---- Candidate-cache VM identity -------------------------------------------
// BUMP dsl_vm_semantics_version with any change that can alter one evaluated bit
// of a DSL signal: parse/analyze/compile (atx-engine alpha parser/typecheck/dag/
// bytecode/fusion), the VM or its kernels (alpha/vm.hpp, cs_ops, ts_ops, ts_*,
// state_ops), or the member-mask/eval-mode contract used here. The value keys the
// cache directory, so a bump is a clean miss + recompute, never a stale hit.
constexpr int dsl_vm_semantics_version=1;
// Tripwire for the bump above (test StrategyIcRunner.VmSourcesPinnedToSemanticsVersion):
// every engine source that parses, compiles or evaluates a DSL signal here -- the
// IC runner TUs' (strategy_ic_*.cpp) include closure under atx/engine minus IC
// scoring (factory/), plus the TUs those headers declare -- is hashed and compared
// with the pin below.
// Digest: SHA-256 over, per listed path in order, "<path>\n<bytes>\n<text>", the
// text CRLF->LF normalized (bytes = its normalized length). On a mismatch, decide:
// a semantic change bumps dsl_vm_semantics_version (clean cache miss); either way
// the pin is re-set. The test also fails if a listed file includes an unlisted
// atx/engine header, so the list cannot silently fall behind the closure.
constexpr std::array<std::string_view,30> dsl_vm_sources{
    "atx-engine/include/atx/engine/alpha/bytecode.hpp",
    "atx-engine/include/atx/engine/alpha/cs_ops.hpp",
    "atx-engine/include/atx/engine/alpha/cs_radix.hpp",
    "atx-engine/include/atx/engine/alpha/dag.hpp",
    "atx-engine/include/atx/engine/alpha/fusion.hpp",
    "atx-engine/include/atx/engine/alpha/fwd.hpp",
    "atx-engine/include/atx/engine/alpha/lexer.hpp",
    "atx-engine/include/atx/engine/alpha/lit_ops.hpp",
    "atx-engine/include/atx/engine/alpha/panel.hpp",
    "atx-engine/include/atx/engine/alpha/parser.hpp",
    "atx-engine/include/atx/engine/alpha/registry.hpp",
    "atx-engine/include/atx/engine/alpha/state_ops.hpp",
    "atx-engine/include/atx/engine/alpha/subtree_cache.hpp",
    "atx-engine/include/atx/engine/alpha/ts_ops.hpp",
    "atx-engine/include/atx/engine/alpha/ts_order_stat.hpp",
    "atx-engine/include/atx/engine/alpha/ts_sliding.hpp",
    "atx-engine/include/atx/engine/alpha/typecheck.hpp",
    "atx-engine/include/atx/engine/alpha/vm.hpp",
    "atx-engine/include/atx/engine/parallel/det_pool.hpp",
    "atx-engine/include/atx/engine/parallel/fwd.hpp",
    "atx-engine/include/atx/engine/data/strategy_data.hpp",
    "atx-engine/src/alpha/bytecode.cpp",
    "atx-engine/src/alpha/dag.cpp",
    "atx-engine/src/alpha/lexer.cpp",
    "atx-engine/src/alpha/panel.cpp",
    "atx-engine/src/alpha/parser.cpp",
    "atx-engine/src/alpha/registry.cpp",
    "atx-engine/src/alpha/subtree_cache.cpp",
    "atx-engine/src/alpha/typecheck.cpp",
    "atx-engine/src/data/strategy_data.cpp"};
constexpr std::string_view dsl_vm_sources_sha256=
    "fa1e9d0fd6b4234fa61dc9c884159e7565b545a7f365f3c416cb38cf68980aab";
// Entries written before this identity existed sit directly under DIR/<sha>/ with
// no vm_identity key. They came from engine builds 429cbe43/6d85ac2a (clang-cl
// 18.1.8, dev preset, no /arch) and no alpha, parallel or core source changed
// between those commits and this key, so exactly this identity keeps that layout
// and accepts keyless sidecars recorded by exactly those builds. Every other
// identity lives under DIR/<identity>/ and must match its sidecar.
constexpr std::string_view legacy_vm_identity="dslvm1_clang18.1";
constexpr std::array<std::string_view,2> legacy_engine_shas{
    "429cbe43d275a49ad3cae89dfa8aa591846a2e4f","6d85ac2a8b7aca6f28cea0e651cdcfc55d77aa29"};
constexpr const char* cache_schema="atx.dsl-candidate-signal/v1";
constexpr const char* cache_layout="date-major-little-endian-f64;non-finite-stored-as-quiet-NaN";
} // namespace
std::string vm_identity() {
  return "dslvm"+std::to_string(dsl_vm_semantics_version)+"_"+std::string(vm_compiler)+std::string(vm_fp_flavor);
}
// ---- Candidate signal cache -------------------------------------------------
// ROOT is DIR/<vm identity>/ (DIR itself for the legacy identity), so a VM or
// build-flavor change is a clean miss; a sidecar's vm_identity must match too.
//
// v2, content-keyed (atx.dsl-candidate-signal/v2, the only layout written). An
// entry's key is SHA256(signal_key_text): the VM identity (semantics version,
// compiler, FP flavor), eval mode, payload layout, role manifest SHA256 and
// geometry, DSL SHA256, and one "field=<name>:<payload sha256>" line per extra
// field the DSL reads, by name. Paths: ROOT/<role sha>/<id>.<dsl16>.{f64,json}
// for a base candidate, ROOT/<role sha>/fp_<fk16>/<id>.<dsl16>.{f64,json} for a
// field candidate (fk16: SHA256 of its field lines). The sidecar records every
// key part and is matched on all of them, so a prefix collision refuses rather
// than serves. One changed field payload therefore misses only the candidates
// that read it; a new fields manifest with the same payloads misses nothing; a
// changed DSL under an old id is a new entry. Orientation and IC labels never
// touch a raw signal (the IC-result cache keys those).
//
// v1 (read in place, never written): ROOT/<role sha>/<id>.{f64,json} for a base
// candidate, ROOT/<fields manifest sha>/<id>.{f64,json} for a field candidate.
// A v1 field entry is a hit when a known manifest -- the pinned one, or one
// named by --cache-legacy-fields -- gives every field the candidate reads the
// payload SHA256 the pinned manifest gives it (the entry names that manifest,
// whose own SHA256 authenticates it). A v1 entry of another DSL is ignored;
// any other mismatch refuses loudly, as before.
//
// Library, role name, source, engine sha and the pinned fields manifest are
// recorded only, so a grown library reuses unchanged candidates. vm_workers is
// recorded, not matched: DetPool column/row parallelism is bit-identical to the
// serial VM (vm.hpp S3-3 contract), pinned on raw payload bytes by the
// worker-parity fixture.
namespace {
constexpr const char* cache_schema_v2="atx.dsl-candidate-signal/v2";
constexpr const char* signal_key_schema="atx.dsl-candidate-signal-key/v2";
} // namespace
co::Status metered_update(co::Sha256& digest,std::span<const std::byte> bytes,HashMeter* meter) {
  if (!meter) return digest.update(bytes);
  const auto started=std::chrono::steady_clock::now();
  auto status=digest.update(bytes);
  meter->seconds+=std::chrono::duration<f64>(std::chrono::steady_clock::now()-started).count();
  return status;
}
std::filesystem::path cache_root(const IcRunnerConfig& cfg) {
  const auto root=std::filesystem::path(cfg.candidate_cache_directory); const auto identity=vm_identity();
  return identity==legacy_vm_identity?root:root/identity;
}
namespace {
std::string field_lines(const FieldShas& fields) {
  std::string out;
  for (const auto& [name,sha]:fields) out+="field="+name+":"+sha+"\n";
  return out;
}
// The exact bytes the signal key hashes (see the layout comment above).
std::string signal_key_text(const CacheKey& key,const std::string& dsl_sha) {
  return std::string(signal_key_schema)+"\nvm_identity="+key.vm_identity+"\neval_mode="+vm_eval_mode+
      "\nlayout="+cache_layout+"\nrole_manifest_sha256="+key.role_sha+"\ndates="+std::to_string(key.dates)+
      "\ninstruments="+std::to_string(key.instruments)+"\ndsl_sha256="+dsl_sha+"\n"+field_lines(key.fields);
}
} // namespace
Json fields_json(const FieldShas& fields) {
  Json out=Json::object();
  for (const auto& [name,sha]:fields) out[name]=sha;
  return out;
}
namespace {
co::Result<CacheKey> cache_key(const IcRunnerConfig& cfg,const Role& role,const Candidate& c) {
  CacheKey key;
  key.stem=c.id+"."+c.dsl_sha.substr(0,16); key.role_sha=role.sha;
  key.dates=role.metadata.at("dates").get<u64>(); key.instruments=role.metadata.at("instruments").get<u64>();
  key.vm_identity=vm_identity(); key.dir=cache_root(cfg)/role.sha;
  for (const auto& name:c.extra_fields) {
    const auto bound=std::find_if(role.fields.load.begin(),role.fields.load.end(),
        [&](const FieldFile& field) { return field.name==name; });
    if (bound==role.fields.load.end() || !hash_valid(bound->sha))
      return co::Err(co::ErrorCode::Internal,"IC runner: cache key field not bound: "+name);
    key.fields.emplace(name,bound->sha);
  }
  if (!key.fields.empty()) {
    ATX_TRY(auto field_key,co::sha256_hex(field_lines(key.fields)));
    key.dir/="fp_"+field_key.substr(0,16); key.fields_manifest_sha=role.fields.sha;
  }
  ATX_TRY(key.signal_key,co::sha256_hex(signal_key_text(key,c.dsl_sha)));
  return co::Ok(std::move(key));
}
// Keyless sidecars predate vm_identity: only base entries recorded by the
// verified legacy builds, read under the legacy identity, are accepted.
bool legacy_entry(const Json& j,const std::string& identity,const std::string& fields_sha) {
  if (identity!=legacy_vm_identity || !fields_sha.empty() || !j.contains("engine_git_sha") ||
      !j.at("engine_git_sha").is_string())
    return false;
  const auto engine=j.at("engine_git_sha").get<std::string>();
  return std::find(legacy_engine_shas.begin(),legacy_engine_shas.end(),engine)!=legacy_engine_shas.end();
}
} // namespace
// Every referenced extra field is bound for a scored role (bind_fields refuses
// otherwise); re-asserted before any key or panel is derived from it.
co::Status fields_bound(const Library& lib,const Role& role) {
  if (!lib.extra_fields.empty() && (role.fields.sha.empty() || role.fields.load.size()!=lib.extra_fields.size()))
    return co::Err(co::ErrorCode::Internal,"IC runner: research fields not bound for role "+role.name);
  return co::Ok();
}
std::string hex(const std::array<std::byte,32>& bytes) {
  constexpr char digits[]="0123456789abcdef"; std::string out(64,'0');
  for (usize i=0;i<bytes.size();++i) {
    const auto b=std::to_integer<unsigned>(bytes[i]);
    out[2*i]=digits[b>>4]; out[2*i+1]=digits[b&15];
  }
  return out;
}
namespace {
// Candidate IDs are [a-z0-9_]{1,64}; only these basenames are Windows devices.
bool device_name(std::string_view id) {
  for (const std::string_view reserved:{"con","prn","aux","nul"}) if (id==reserved) return true;
  return id.size()==4 && (id.starts_with("com") || id.starts_with("lpt")) && id[3]>='0' && id[3]<='9';
}
} // namespace
co::Status cache_preflight(const IcRunnerConfig& cfg,const Library& lib) {
  if (cfg.candidate_cache_directory.empty()) return co::Ok();
  if constexpr (std::endian::native!=std::endian::little)
    return co::Err(co::ErrorCode::Unavailable,"IC runner: candidate cache requires little-endian host");
  if constexpr (vm_compiler.empty())
    return co::Err(co::ErrorCode::Unavailable,"IC runner: candidate cache requires a known VM build identity");
  for (const auto& c:lib.candidates) if (device_name(c.id))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate id unsafe as cache file name: "+c.id);
  std::error_code ec; const auto root=std::filesystem::path(cfg.candidate_cache_directory);
  const bool present=std::filesystem::exists(root,ec);
  if (ec || (present && !std::filesystem::is_directory(root,ec)) || ec)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate cache path must be a directory");
  return co::Ok();
}
// --cache-legacy-fields: each DIR/manifest.json, authenticated by its own SHA256
// (the v1 sidecars it keyed name it), reduced to field -> payload SHA256.
co::Result<KnownManifests> legacy_manifests(const IcRunnerConfig& cfg) {
  KnownManifests out;
  if (cfg.candidate_cache_legacy_fields.size()>64)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: at most 64 --cache-legacy-fields");
  for (const auto& directory:cfg.candidate_cache_legacy_fields) {
    const auto where="IC runner: --cache-legacy-fields "+directory;
    ATX_TRY(auto text,metadata_text((std::filesystem::path(directory)/"manifest.json").string()));
    ATX_TRY(auto sha,co::sha256_hex(text));
    const auto j=Json::parse(text,nullptr,false);
    if (j.is_discarded() || !j.is_object() || j.value("schema",std::string{})!=fields_schema ||
        j.value("status",std::string{})!="complete" || !j.contains("role") || !j.at("role").is_object() ||
        !j.contains("fields") || !j.at("fields").is_array() || !j.contains("files") || !j.at("files").is_object())
      return co::Err(co::ErrorCode::InvalidArgument,where+": not a complete "+fields_schema+" manifest");
    KnownManifest known{j.at("role").value("manifest_sha256",std::string{}),{}};
    if (!hash_valid(known.role_sha))
      return co::Err(co::ErrorCode::InvalidArgument,where+": role manifest binding");
    const auto& files=j.at("files");
    for (const auto& row:j.at("fields")) {
      const auto name=row.is_object()?row.value("name",std::string{}):std::string{};
      const auto file=name+".f64";
      const auto payload=row.is_object()?row.value("sha256",std::string{}):std::string{};
      if (!field_identifier(name) || row.value("file",std::string{})!=file || !hash_valid(payload) ||
          !files.contains(file) || !files.at(file).is_object() ||
          files.at(file).value("sha256",std::string{})!=payload || !known.fields.emplace(name,payload).second)
        return co::Err(co::ErrorCode::InvalidArgument,where+": field entry "+name);
    }
    out.emplace(std::move(sha),std::move(known));
  }
  return co::Ok(std::move(out));
}
namespace {
// This role's v1 field-entry manifests: the pinned one first (from its bound
// payloads), then every known manifest bound to the same role manifest.
std::vector<std::pair<std::string,FieldShas>> role_manifests(const Role& role,const KnownManifests& known) {
  std::vector<std::pair<std::string,FieldShas>> out;
  if (!role.fields.sha.empty()) {
    FieldShas pinned;
    for (const auto& field:role.fields.load) pinned.emplace(field.name,field.sha);
    out.emplace_back(role.fields.sha,std::move(pinned));
  }
  for (const auto& [sha,manifest]:known)
    if (manifest.role_sha==role.sha && sha!=role.fields.sha) out.emplace_back(sha,manifest.fields);
  return out;
}
co::Result<std::optional<Json>> read_sidecar(const std::filesystem::path& path,const Candidate& c) {
  std::error_code ec; const bool present=std::filesystem::exists(path,ec);
  if (ec) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache probe: "+c.id);
  if (!present) return co::Ok(std::optional<Json>{});
  ATX_TRY(auto text,metadata_text(path.string()));
  auto j=Json::parse(text,nullptr,false);
  if (j.is_discarded())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate cache sidecar JSON: "+c.id);
  return co::Ok(std::optional<Json>(std::move(j)));
}
std::string text_field(const Json& j,const char* key) {
  return j.is_object() && j.contains(key) && j.at(key).is_string()?j.at(key).get<std::string>():std::string{};
}
u64 count_field(const Json& j,const char* key) {
  return j.is_object() && j.contains(key) && j.at(key).is_number_unsigned()?j.at(key).get<u64>():~u64{0};
}
// Checks shared by both layouts: this candidate on this role geometry and eval mode.
bool same_geometry(const Json& j,const CacheKey& key,const Candidate& c) {
  return j.is_object() && text_field(j,"candidate_id")==c.id && text_field(j,"dsl_sha256")==c.dsl_sha &&
      text_field(j,"role_manifest_sha256")==key.role_sha && text_field(j,"eval_mode")==vm_eval_mode &&
      text_field(j,"layout")==cache_layout && count_field(j,"dates")==key.dates &&
      count_field(j,"instruments")==key.instruments &&
      count_field(j,"bytes")==key.dates*key.instruments*sizeof(f64);
}
// A present v2 sidecar at the key's path must record exactly this key.
co::Result<std::string> v2_payload_sha(const Json& j,const CacheKey& key,const Candidate& c) {
  auto sha=text_field(j,"payload_sha256");
  if (!same_geometry(j,key,c) || text_field(j,"schema")!=cache_schema_v2 ||
      text_field(j,"payload")!=key.stem+".f64" || text_field(j,"vm_identity")!=key.vm_identity ||
      !j.contains("field_payload_sha256") || j.at("field_payload_sha256")!=fields_json(key.fields) ||
      text_field(j,"signal_key_sha256")!=key.signal_key || !hash_valid(sha))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate cache entry mismatch: "+c.id);
  return co::Ok(std::move(sha));
}
// A v1 sidecar at a probed path: nullopt when it records another DSL (a changed
// candidate, ignored); otherwise it must describe this candidate under
// `fields_sha` (empty: a base entry), as the v1 runner's lookup required.
co::Result<std::optional<std::string>> v1_payload_sha(const Json& j,const CacheKey& key,const Candidate& c,
                                                      const std::string& fields_sha) {
  if (text_field(j,"candidate_id")==c.id && text_field(j,"dsl_sha256")!=c.dsl_sha)
    return co::Ok(std::optional<std::string>{});
  auto sha=text_field(j,"payload_sha256");
  const bool fields_match=fields_sha.empty()?!j.contains("fields_manifest_sha256")
      :(text_field(j,"fields_manifest_sha256")==fields_sha && j.contains("research_fields") &&
        j.at("research_fields")==Json(c.extra_fields));
  const bool vm_match=j.contains("vm_identity")?text_field(j,"vm_identity")==key.vm_identity
                                               :legacy_entry(j,key.vm_identity,fields_sha);
  if (!same_geometry(j,key,c) || text_field(j,"schema")!=cache_schema || !fields_match || !vm_match ||
      !hash_valid(sha))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate cache entry mismatch: "+c.id);
  return co::Ok(std::optional<std::string>(std::move(sha)));
}
co::Result<std::optional<CacheHit>> v1_probe(const std::filesystem::path& dir,const std::string& fields_sha,
                                             const CacheKey& key,const Candidate& c) {
  ATX_TRY(auto sidecar,read_sidecar(dir/(c.id+".json"),c));
  if (!sidecar) return co::Ok(std::optional<CacheHit>{});
  ATX_TRY(auto sha,v1_payload_sha(*sidecar,key,c,fields_sha));
  if (!sha) return co::Ok(std::optional<CacheHit>{});
  return co::Ok(std::optional<CacheHit>(CacheHit{dir,c.id,std::move(*sha),true}));
}
// nullopt: no entry (evaluate and write v2). Order: the v2 entry, then v1 in
// place -- the base directory for a base candidate, else each of `manifests`
// (pinned first) whose payload SHA256s agree with the key on every field read.
co::Result<std::optional<CacheHit>> cache_lookup(const std::filesystem::path& root,const CacheKey& key,
    const Candidate& c,const std::vector<std::pair<std::string,FieldShas>>& manifests) {
  ATX_TRY(auto sidecar,read_sidecar(key.dir/(key.stem+".json"),c));
  if (sidecar) {
    ATX_TRY(auto sha,v2_payload_sha(*sidecar,key,c));
    return co::Ok(std::optional<CacheHit>(CacheHit{key.dir,key.stem,std::move(sha),false}));
  }
  if (key.fields.empty()) return v1_probe(root/key.role_sha,{},key,c);
  for (const auto& manifest:manifests) {
    const auto& known=manifest.second;
    const bool agrees=std::all_of(key.fields.begin(),key.fields.end(),[&known](const auto& field) {
      const auto at=known.find(field.first);
      return at!=known.end() && at->second==field.second;
    });
    if (!agrees) continue;
    ATX_TRY(auto hit,v1_probe(root/manifest.first,manifest.first,key,c));
    if (hit) return co::Ok(std::move(hit));
  }
  return co::Ok(std::optional<CacheHit>{});
}
} // namespace
// Reads a pinned date-major f64 payload into `out` (reused; no second copy),
// hashing each chunk as it lands unless `verify` is false (a reload the caller
// has shown unchanged since this process verified it). `what` names the payload
// kind in refusals.
co::Status load_pinned_f64(const std::filesystem::path& path,const std::string& expected,usize cells,
                           std::vector<f64>& out,std::string_view what,HashMeter* meter,bool verify) {
  const auto bytes=static_cast<u64>(cells)*sizeof(f64);
  const auto label=std::string("IC runner: ")+std::string(what); const auto name=path.filename().string();
  std::ifstream in(path,std::ios::binary|std::ios::ate);
  if (!in || in.tellg()<0 || static_cast<u64>(in.tellg())!=bytes)
    return co::Err(co::ErrorCode::InvalidArgument,label+" extent: "+name);
  in.seekg(0); out.resize(cells);
  const auto destination=std::as_writable_bytes(std::span(out)); co::Sha256 digest;
  for (usize offset=0;offset<destination.size();) {
    const auto chunk=destination.subspan(offset,std::min(io_chunk,destination.size()-offset));
    // SAFETY: char accesses the object representation of trivially copyable f64 storage.
    in.read(reinterpret_cast<char*>(chunk.data()),static_cast<std::streamsize>(chunk.size()));
    if (!in) return co::Err(co::ErrorCode::IoError,label+" truncated: "+name);
    if (verify) ATX_TRY_VOID(metered_update(digest,chunk,meter));
    offset+=chunk.size();
  }
  if (in.peek()!=std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError,label+" changed extent: "+name);
  if (!verify) return co::Ok();
  ATX_TRY(auto actual,digest.finalize());
  if (hex(actual)!=expected)
    return co::Err(co::ErrorCode::InvalidArgument,label+" SHA256 mismatch: "+name);
  if (meter) meter->verify_bytes+=bytes;
  return co::Ok();
}
co::Status cache_load(const CacheHit& hit,usize cells,std::vector<f64>& out,HashMeter& meter) {
  return load_pinned_f64(hit.dir/(hit.stem+".f64"),hit.payload_sha,cells,out,"candidate cache payload",&meter);
}
PartialFile::PartialFile(const std::filesystem::path& final) {
  std::random_device entropy;
  const auto nonce=((static_cast<u64>(entropy())<<32)^static_cast<u64>(entropy()))^
      static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
  constexpr char digits[]="0123456789abcdef"; std::string tag(16,'0');
  for (usize i=0;i<tag.size();++i) tag[i]=digits[(nonce>>(4*i))&15U];
  path=final.parent_path()/("."+tag+".partial");
}
PartialFile::~PartialFile() { std::error_code ec; std::filesystem::remove(path,ec); }
// Atomic no-replace publication in one directory; true iff `final` already existed.
co::Result<bool> publish_new(const std::filesystem::path& partial,const std::filesystem::path& final) {
  std::error_code ec; std::filesystem::create_hard_link(partial,final,ec);
  if (!ec) return co::Ok(false);
  std::error_code probe;
  if (std::filesystem::exists(final,probe) && !probe) return co::Ok(true);
  return co::Err(co::ErrorCode::IoError,
      "IC runner: candidate cache publish "+final.filename().string()+": "+ec.message());
}
co::Result<std::string> write_partial(const PartialFile& partial,std::span<const std::byte> bytes,
                                      HashMeter* meter) {
  std::ofstream out(partial.path,std::ios::binary); co::Sha256 digest;
  if (!out)
    return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache partial output: "+partial.path.string());
  for (usize offset=0;offset<bytes.size();) {
    const auto chunk=bytes.subspan(offset,std::min(io_chunk,bytes.size()-offset));
    // SAFETY: char reads the object representation of the caller's byte span.
    out.write(reinterpret_cast<const char*>(chunk.data()),static_cast<std::streamsize>(chunk.size()));
    if (!out) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache partial write");
    ATX_TRY_VOID(metered_update(digest,chunk,meter)); offset+=chunk.size();
  }
  out.close(); if (!out) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache partial close");
  ATX_TRY(auto sha,digest.finalize());
  return co::Ok(hex(sha));
}
namespace {
// Never overwrites: an existing payload (a run stopped between the payload and
// sidecar publications, or a concurrent writer) is adopted only if byte-identical.
co::Result<std::string> cache_store_payload(const std::filesystem::path& final,
                                            std::span<const std::byte> bytes,HashMeter& meter) {
  std::error_code ec; const bool present=std::filesystem::exists(final,ec);
  if (ec) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache probe: "+final.filename().string());
  std::string sha;
  if (!present) {
    const PartialFile partial(final);
    ATX_TRY(sha,write_partial(partial,bytes,&meter));
    ATX_TRY(auto existed,publish_new(partial.path,final));
    if (!existed) return co::Ok(std::move(sha));
  } else {
    ATX_TRY(sha,co::sha256_hex(bytes));
  }
  ATX_TRY(auto existing,co::sha256_file(final.string()));
  if (existing!=sha)
    return co::Err(co::ErrorCode::AlreadyExists,"IC runner: candidate cache payload exists with "
        "different bytes; refusing overwrite: "+final.filename().string());
  return co::Ok(std::move(sha));
}
} // namespace
// Canonicalizes non-finite cells to quiet NaN IN PLACE, so this run consumes
// exactly the bytes it stores (cold == warm by construction). Downstream IC rows
// and composition admit a cell only via std::isfinite, so no-cache results are
// unchanged. Writes the v2 entry; the sidecar is published last: it is the
// entry's commit record. Returns the committed entry (its payload SHA256 keys
// the IC-result cache).
co::Result<CacheHit> cache_store(const CacheKey& key,const IcRunnerConfig& cfg,const Role& spec,
    const engine::data::StrategyRoleData& role,const Candidate& c,std::span<f64> signal,HashMeter& meter) {
  if (signal.size()!=key.dates*key.instruments)
    return co::Err(co::ErrorCode::Internal,"IC runner: candidate cache geometry");
  for (auto& v:signal) if (!std::isfinite(v)) v=quiet_nan;
  std::error_code ec; std::filesystem::create_directories(key.dir,ec);
  if (ec) return co::Err(co::ErrorCode::IoError,"IC runner: candidate cache directory: "+ec.message());
  ATX_TRY(auto sha,cache_store_payload(key.dir/(key.stem+".f64"),std::as_bytes(signal),meter));
  Json sidecar{{"schema",cache_schema_v2},{"candidate_id",c.id},{"family",c.family},
      {"dsl_sha256",c.dsl_sha},{"library_sha256",cfg.library_sha256},
      {"role_manifest_sha256",key.role_sha},{"role",spec.name},{"source_sha256",role.source_sha256},
      {"dates",key.dates},{"instruments",key.instruments},{"bytes",signal.size()*sizeof(f64)},
      {"layout",cache_layout},{"payload",key.stem+".f64"},{"payload_sha256",sha},
      {"semantics","raw-unoriented-pre-composition-single-VM-root"},{"eval_mode",vm_eval_mode},
      {"vm_workers",cfg.workers},{"engine_git_sha",std::string(build_engine_git_sha())},
      {"vm_identity",key.vm_identity},{"field_payload_sha256",fields_json(key.fields)},
      {"signal_key_sha256",key.signal_key}};
  // Provenance only: the manifest this run pinned (never matched on lookup).
  if (!key.fields_manifest_sha.empty()) sidecar["fields_manifest_sha256"]=key.fields_manifest_sha;
  const auto text=sidecar.dump(2)+"\n";
  const auto final=key.dir/(key.stem+".json");
  const PartialFile partial(final);
  ATX_TRY_VOID(write_partial(partial,std::as_bytes(std::span(text.data(),text.size()))));
  ATX_TRY(auto existed,publish_new(partial.path,final));
  if (existed) {
    // A concurrent writer committed first: accept only the same key and bytes.
    ATX_TRY(auto committed,read_sidecar(final,c));
    if (!committed) return co::Err(co::ErrorCode::Internal,"IC runner: candidate cache sidecar vanished: "+c.id);
    ATX_TRY(auto committed_sha,v2_payload_sha(*committed,key,c));
    if (committed_sha!=sha)
      return co::Err(co::ErrorCode::AlreadyExists,
          "IC runner: candidate cache sidecar raced with different bytes: "+c.id);
  }
  return co::Ok(CacheHit{key.dir,key.stem,std::move(sha),false});
}
co::Result<CacheResolution> cache_resolve(const IcRunnerConfig& cfg,const Library& lib,const Role& role,
                                          const KnownManifests& known) {
  ATX_TRY_VOID(fields_bound(lib,role));
  const auto manifests=role_manifests(role,known); const auto root=cache_root(cfg);
  CacheResolution out;
  for (const auto& c:lib.candidates) {
    ATX_TRY(auto key,cache_key(cfg,role,c));
    ATX_TRY(auto hit,cache_lookup(root,key,c,manifests));
    if (hit) {
      const auto payload=hit->dir/(hit->stem+".f64");
      std::error_code ec; const auto size=std::filesystem::file_size(payload,ec);
      if (ec || size!=key.dates*key.instruments*sizeof(f64))
        return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate cache payload extent: "+
            payload.filename().string());
      ++out.ready; out.legacy+=hit->legacy?1U:0U;
    }
    out.keys.push_back(std::move(key)); out.hits.push_back(std::move(hit));
  }
  return co::Ok(std::move(out));
}
// Metadata-only readiness for --plan-only.
co::Result<Json> cache_plan(const IcRunnerConfig& cfg,const Library& lib,const Role& role,
                            const KnownManifests& known) {
  ATX_TRY(auto resolved,cache_resolve(cfg,lib,role,known));
  Json plan{{"role",role.name},{"directory",(cache_root(cfg)/role.sha).string()},{"layout",cache_schema_v2},
      {"ready_entries",resolved.ready},{"legacy_ready_entries",resolved.legacy},
      {"candidates",lib.candidates.size()},{"vm_identity",vm_identity()}};
  if (!lib.extra_fields.empty()) plan["fields_directory"]=(cache_root(cfg)/role.fields.sha).string();
  return co::Ok(std::move(plan));
}
namespace {
// ---- --cache-report ----------------------------------------------------------
// Bounded, error-coded directory listing (the report never throws on a race).
co::Result<std::vector<std::filesystem::directory_entry>> list_directory(const std::filesystem::path& dir) {
  std::vector<std::filesystem::directory_entry> out; std::error_code ec;
  std::filesystem::directory_iterator it(dir,ec);
  for (;!ec && it!=std::filesystem::directory_iterator();it.increment(ec)) {
    if (out.size()>=(usize{1}<<20)) return co::Err(co::ErrorCode::OutOfRange,"IC runner: cache report listing");
    out.push_back(*it);
  }
  if (ec) return co::Err(co::ErrorCode::IoError,"IC runner: cache report listing "+dir.string()+": "+ec.message());
  return co::Ok(std::move(out));
}
u64 size_or_zero(const std::filesystem::path& path) {
  std::error_code ec; const auto size=std::filesystem::file_size(path,ec);
  return ec?0:static_cast<u64>(size);
}
// An entry's bytes: sidecar, payload and its IC results (<dir>/ic*/<stem>.json).
co::Result<u64> entry_bytes(const std::filesystem::path& dir,const std::string& stem) {
  u64 total=size_or_zero(dir/(stem+".json"))+size_or_zero(dir/(stem+".f64"));
  ATX_TRY(auto listing,list_directory(dir));
  for (const auto& sub:listing) {
    std::error_code ec;
    if (sub.is_directory(ec) && sub.path().filename().string().starts_with("ic"))
      total+=size_or_zero(sub.path()/(stem+".json"));
  }
  return co::Ok(total);
}
std::string path_key(const std::filesystem::path& path) { return path.lexically_normal().string(); }
struct CacheScan { u64 entries{},bytes{},orphan_files{},orphan_bytes{}; Json list=Json::array(); };
// One entry directory: sidecars not in `referenced` are unreferenced entries;
// partial files and payloads without a sidecar are orphans.
co::Status scan_entries(const std::filesystem::path& dir,const std::set<std::string>& referenced,CacheScan& scan) {
  ATX_TRY(auto listing,list_directory(dir));
  for (const auto& file:listing) {
    std::error_code ec;
    if (!file.is_regular_file(ec)) continue;
    const auto name=file.path().filename().string(); const auto extension=file.path().extension().string();
    const auto stem=file.path().stem().string();
    const bool partial=name.ends_with(".partial");
    if (partial || (extension==".f64" && !std::filesystem::exists(dir/(stem+".json"),ec))) {
      ++scan.orphan_files; scan.orphan_bytes+=size_or_zero(file.path()); continue;
    }
    if (extension!=".json" || referenced.contains(path_key(file.path()))) continue;
    ATX_TRY(auto bytes,entry_bytes(dir,stem));
    ++scan.entries; scan.bytes+=bytes;
    scan.list.push_back({{"sidecar",file.path().string()},{"candidate_id",stem.substr(0,stem.find('.'))},
        {"bytes",bytes}});
  }
  return co::Ok();
}
} // namespace
// Resolves every candidate of every scored role (as a run would) and lists what
// else lies under ROOT: <64-hex>/ entry directories and their fp_* children.
co::Result<Json> cache_report(const IcRunnerConfig& cfg,const Library& lib,const std::vector<Role>& roles,
                              const KnownManifests& known) {
  const auto root=cache_root(cfg);
  Json report{{"mode","cache-report"},{"root",root.string()},{"vm_identity",vm_identity()},
      {"layout",cache_schema_v2},{"roles",Json::array()}};
  std::set<std::string> referenced;
  for (const auto& role:roles) {
    ATX_TRY(auto resolved,cache_resolve(cfg,lib,role,known));
    Json rows=Json::array(); u64 hit_bytes=0;
    for (usize k=0;k<lib.candidates.size();++k) {
      const auto& hit=resolved.hits[k]; const auto& key=resolved.keys[k];
      if (!hit) {
        rows.push_back({{"id",lib.candidates[k].id},{"status","miss"},
            {"sidecar",(key.dir/(key.stem+".json")).string()}});
        continue;
      }
      const auto sidecar=hit->dir/(hit->stem+".json"); referenced.insert(path_key(sidecar));
      ATX_TRY(auto bytes,entry_bytes(hit->dir,hit->stem)); hit_bytes+=bytes;
      rows.push_back({{"id",lib.candidates[k].id},{"status","hit"},{"layout",hit->legacy?"v1":"v2"},
          {"sidecar",sidecar.string()},{"bytes",bytes}});
    }
    report["roles"].push_back({{"role",role.name},{"manifest_sha256",role.sha},
        {"candidates",lib.candidates.size()},{"hits",resolved.ready},{"legacy_hits",resolved.legacy},
        {"misses",lib.candidates.size()-resolved.ready},{"hit_bytes",hit_bytes},{"entries",std::move(rows)}});
  }
  CacheScan scan; std::error_code ec;
  if (std::filesystem::is_directory(root,ec)) {
    ATX_TRY(auto top,list_directory(root));
    for (const auto& dir:top) {
      if (!dir.is_directory(ec) || !hash_valid(dir.path().filename().string())) continue;
      ATX_TRY_VOID(scan_entries(dir.path(),referenced,scan));
      ATX_TRY(auto children,list_directory(dir.path()));
      for (const auto& child:children)
        if (child.is_directory(ec) && child.path().filename().string().starts_with("fp_"))
          ATX_TRY_VOID(scan_entries(child.path(),referenced,scan));
    }
  }
  report["unreferenced"]={{"entries",scan.entries},{"bytes",scan.bytes},{"list",std::move(scan.list)}};
  report["orphans"]={{"files",scan.orphan_files},{"bytes",scan.orphan_bytes}};
  return co::Ok(std::move(report));
}
} // namespace atx::impl::strategy::ic_detail
namespace atx::impl::strategy {
IcCacheVmIdentity ic_cache_vm_identity() {
  using namespace ic_detail;
  IcCacheVmIdentity out{dsl_vm_semantics_version,vm_identity(),{},std::string(dsl_vm_sources_sha256)};
  for (const auto path:dsl_vm_sources) out.sources.emplace_back(path);
  return out;
}
} // namespace atx::impl::strategy
