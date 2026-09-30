#include "strategy_ic_detail.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "build_provenance.hpp"

namespace atx::impl::strategy::ic_detail {
namespace {
// ---- IC-result cache identity (T15) -----------------------------------------
// BUMP ic_result_semantics_version with any change that can alter one bit of a
// candidate's IC result or daily IC series for the same signal bytes: the engine
// IC scoring sources pinned below, or how score_role configures IC. Every IC
// configuration field, the decision-membership span and the return guard are
// also keyed by value/content, so the bump covers only code, never inputs.
constexpr int ic_result_semantics_version=1;
constexpr const char* ic_cache_schema="atx.dsl-candidate-ic/v1";
// Tripwire for the bump above (test StrategyIcRunner.IcSourcesPinnedToSemanticsVersion,
// same digest recipe as dsl_vm_sources): the IC scoring TU and its atx/engine include
// closure, plus the TUs those headers declare.
constexpr std::array<std::string_view,10> ic_result_sources{
    "atx-engine/include/atx/engine/alpha/fwd.hpp",
    "atx-engine/include/atx/engine/alpha/panel.hpp",
    "atx-engine/include/atx/engine/eval/hac.hpp",
    "atx-engine/include/atx/engine/factory/ic_research.hpp",
    "atx-engine/include/atx/engine/factory/ic_screen.hpp",
    "atx-engine/include/atx/engine/factory/ic_screen_config.hpp",
    "atx-engine/include/atx/engine/parallel/det_pool.hpp",
    "atx-engine/include/atx/engine/parallel/fwd.hpp",
    "atx-engine/src/alpha/panel.cpp",
    "atx-engine/src/factory/ic_screen.cpp"};
constexpr std::string_view ic_result_sources_sha256=
    "e7a40331a3f2f1a4268feece00d354961ae7ab8215a379733d5855f40f61579a";
} // namespace
// FP build flavor as for the VM, plus the IC kernel's SIMD width (its reduction
// order): ic_screen.cpp reports its own compiled xsimd batch size.
std::string ic_identity() {
  return "dslic"+std::to_string(ic_result_semantics_version)+"_"+std::string(vm_compiler)+
      std::string(vm_fp_flavor)+"_simd"+std::to_string(ex::ic_screen_simd_width());
}
IcSeries scratch_series(const ex::ResearchIcScratch& scratch) {
  IcSeries out;
  for (usize k=0;k<out.rank.size();++k) {
    out.pearson[k]=scratch.pearson_series(k); out.rank[k]=scratch.rank_series(k);
  }
  return out;
}
// ---- IC-result cache: <signal entry dir>/ic<v>_<key16>/<id>.json (T15) ------
// One verified evaluate_research_ic result per candidate signal, so a warm pass
// skips IC scoring. An entry binds the exact signal bytes (the signal cache's
// payload SHA256), the candidate id and DSL SHA, and the scope key: the role
// manifest pin (panel close and presence), the content SHA256 of the exact
// membership and return-guard spans given to prepare_research_ic, every
// IcScreenConfig/option field that can change a bit, and ic_identity().
// max_cache_bytes and workers are not keyed: they only admit or schedule (the
// row kernels are per-date independent; HAC and classification run after the
// join), as the serial/parallel research IC fixtures pin. The directory carries
// the first 16 hex of SHA256(key.dump()) and the record the full key, so a
// prefix collision refuses instead of serving. Every f64 is stored as its IEEE
// bit pattern in hex (NaNs included) and the record is self-hashed, so a hit
// reproduces the evaluated struct and series exactly. Like the signal cache it is
// never a method input: the recipe and every output byte are unchanged by it.
// Memory: one entry at a time, at most 1 MiB of text (metadata_text) plus its
// parsed/decoded form, inside admission's fixed 32 MiB slack.
IcSeries cached_series(const CachedIc& cached) {
  IcSeries out;
  for (usize k=0;k<out.rank.size();++k) { out.pearson[k]=cached.pearson[k]; out.rank[k]=cached.rank[k]; }
  return out;
}
namespace {
std::string bits_hex(f64 value) {
  constexpr char digits[]="0123456789abcdef"; const auto bits=std::bit_cast<u64>(value);
  std::string out(16,'0');
  for (usize i=0;i<out.size();++i) out[i]=digits[(bits>>(60U-4U*i))&15U];
  return out;
}
// Exactly 16 lowercase hex digits, most significant first; anything else fails.
std::optional<u64> hex_bits(std::string_view text) {
  if (text.size()!=16) return std::nullopt;
  u64 bits=0;
  for (const char c:text) {
    u64 nibble=0;
    if (c>='0' && c<='9') nibble=static_cast<u64>(c-'0');
    else if (c>='a' && c<='f') nibble=static_cast<u64>(c-'a')+10U;
    else return std::nullopt;
    bits=(bits<<4U)|nibble;
  }
  return bits;
}
std::string series_hex(std::span<const f64> values) {
  std::string out; out.reserve(values.size()*16U);
  for (const auto v:values) out+=bits_hex(v);
  return out;
}
} // namespace
// Hashes the membership (6.5 MB) and guard (26 MB) spans once per scored role.
co::Result<IcCacheScope> ic_cache_scope(const Role& spec,const engine::data::StrategyRoleData& role,
    const ex::IcScreenConfig& ic,const ex::ResearchIcOptions& options,std::span<const u32> guard,
    HashMeter& meter) {
  const auto hashing=std::chrono::steady_clock::now();
  ATX_TRY(auto member_sha,co::sha256_hex(std::as_bytes(std::span<const u8>(role.decision_member))));
  ATX_TRY(auto guard_sha,co::sha256_hex(std::as_bytes(guard)));
  meter.seconds+=std::chrono::duration<f64>(std::chrono::steady_clock::now()-hashing).count();
  Json horizons=Json::array();
  for (const auto h:ic.horizons) horizons.push_back(h);
  Json key{{"semantics_version",static_cast<u64>(ic_result_semantics_version)},{"ic_identity",ic_identity()},
      {"role_manifest_sha256",spec.sha},{"dates",role.panel.dates()},{"instruments",role.panel.instruments()},
      {"rule",std::string(ex::ic_screen_rule_name(ic.rule))},{"horizons",std::move(horizons)},
      {"execution_delay",ic.execution_delay},{"window_begin",ic.window_begin},{"window_end",ic.window_end},
      {"maturity_end",ic.maturity_end},{"min_names",ic.min_names},{"min_dates",ic.min_dates},
      {"practical_abs_ic_bits",bits_hex(ic.practical_abs_ic)},
      {"confidence_multiplier_bits",bits_hex(ic.confidence_multiplier)},
      {"active_horizons",options.active_horizons},
      {"require_endpoint_presence",options.require_endpoint_presence},
      {"price_field",std::string(ic_price_field)},{"decision_member_sha256",std::move(member_sha)},
      {"return_guard_sha256",std::move(guard_sha)}};
  ATX_TRY(auto key_sha,co::sha256_hex(key.dump()));
  auto directory="ic"+std::to_string(ic_result_semantics_version)+"_"+key_sha.substr(0,16);
  return co::Ok(IcCacheScope{std::move(key),std::move(directory)});
}
namespace {
Json estimate_record(const ex::IcScreenEstimate& e) {
  return {{"valid_dates",e.valid_dates},{"calendar_dates",e.calendar_dates},{"hac_lag",e.hac_lag},
      {"mean",bits_hex(e.mean)},{"standard_error",bits_hex(e.standard_error)},
      {"upper_abs_ic",bits_hex(e.upper_abs_ic)},{"max_segment_abs_ic",bits_hex(e.max_segment_abs_ic)},
      {"defined",e.defined},{"suggestive_direction",e.suggestive_direction}};
}
// Every field of the result (all four horizon/coverage slots) plus the active
// horizons' daily series; integers stay JSON integers, so dump() round-trips.
Json ic_result_record(const ex::ResearchIcResult& result,const IcSeries& daily) {
  Json horizons=Json::array(),coverage=Json::array(),pearson=Json::array(),rank=Json::array();
  for (const auto& h:result.screen.horizons)
    horizons.push_back({{"horizon",h.horizon},{"enough_evidence",h.enough_evidence},
        {"pearson",estimate_record(h.pearson)},{"rank",estimate_record(h.rank)}});
  for (const auto& c:result.coverage)
    coverage.push_back({{"mature_dates",c.mature_dates},{"structural_tail_dates",c.structural_tail_dates},
        {"decision_eligible_pairs",c.decision_eligible_pairs},{"finite_label_pairs",c.finite_label_pairs},
        {"paired_signal_pairs",c.paired_signal_pairs},{"missing_entry_pairs",c.missing_entry_pairs},
        {"missing_exit_pairs",c.missing_exit_pairs},{"guard_excluded_pairs",c.guard_excluded_pairs},
        {"invalid_price_pairs",c.invalid_price_pairs},{"nonfinite_return_pairs",c.nonfinite_return_pairs}});
  for (usize k=0;k<result.active_horizons && k<daily.rank.size();++k) {
    pearson.push_back(series_hex(daily.pearson[k])); rank.push_back(series_hex(daily.rank[k]));
  }
  return {{"active_horizons",result.active_horizons},{"reject",result.screen.reject},
      {"enough_evidence",result.screen.enough_evidence},{"reason",static_cast<u64>(result.screen.reason)},
      {"horizons",std::move(horizons)},{"coverage",std::move(coverage)},
      {"pearson_series",std::move(pearson)},{"rank_series",std::move(rank)}};
}
// Strict readers for a cached record: any absent or mistyped field clears `ok`.
struct IcRecordReader {
  bool ok{true};
  u64 count(const Json& j,const char* key) {
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_number_unsigned()) { ok=false; return 0; }
    return j.at(key).get<u64>();
  }
  bool flag(const Json& j,const char* key) {
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_boolean()) { ok=false; return false; }
    return j.at(key).get<bool>();
  }
  f64 real(const Json& j,const char* key) {
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_string()) { ok=false; return 0; }
    const auto bits=hex_bits(j.at(key).get_ref<const std::string&>());
    if (!bits) { ok=false; return 0; }
    return std::bit_cast<f64>(*bits);
  }
  void estimate(const Json& j,ex::IcScreenEstimate& e) {
    e.valid_dates=static_cast<usize>(count(j,"valid_dates"));
    e.calendar_dates=static_cast<usize>(count(j,"calendar_dates"));
    e.hac_lag=static_cast<usize>(count(j,"hac_lag"));
    e.mean=real(j,"mean"); e.standard_error=real(j,"standard_error"); e.upper_abs_ic=real(j,"upper_abs_ic");
    e.max_segment_abs_ic=real(j,"max_segment_abs_ic");
    e.defined=flag(j,"defined"); e.suggestive_direction=flag(j,"suggestive_direction");
  }
  // Exactly `length` hex-encoded f64 words into `out`.
  void values(const Json& j,usize length,std::vector<f64>& out) {
    if (!j.is_string() || j.get_ref<const std::string&>().size()!=length*16U) { ok=false; return; }
    const std::string_view text=j.get_ref<const std::string&>();
    out.resize(length);
    for (usize i=0;i<length;++i) {
      const auto bits=hex_bits(text.substr(i*16U,16U));
      if (!bits) { ok=false; return; }
      out[i]=std::bit_cast<f64>(*bits);
    }
  }
};
// `scratch` supplies this role's series lengths; `active` the configured horizons.
std::optional<CachedIc> decode_ic_record(const Json& r,const ex::ResearchIcScratch& scratch,usize active) {
  IcRecordReader in; CachedIc out; auto& result=out.result;
  const auto sized=[&](const char* key,usize n) {
    return r.is_object() && r.contains(key) && r.at(key).is_array() && r.at(key).size()==n;
  };
  const auto recorded=in.count(r,"active_horizons"),reason=in.count(r,"reason");
  if (!in.ok || recorded!=active || active>out.rank.size() ||
      reason>static_cast<u64>(ex::IcScreenReason::PracticalNull) || !sized("horizons",4) ||
      !sized("coverage",4) || !sized("pearson_series",active) || !sized("rank_series",active))
    return std::nullopt;
  result.active_horizons=active;
  result.screen.reject=in.flag(r,"reject"); result.screen.enough_evidence=in.flag(r,"enough_evidence");
  result.screen.reason=static_cast<ex::IcScreenReason>(static_cast<u8>(reason));
  for (usize h=0;h<result.screen.horizons.size();++h) {
    const auto& row=r.at("horizons").at(h); auto& dst=result.screen.horizons[h];
    if (!row.is_object() || !row.contains("pearson") || !row.contains("rank")) return std::nullopt;
    dst.horizon=static_cast<usize>(in.count(row,"horizon"));
    dst.enough_evidence=in.flag(row,"enough_evidence");
    in.estimate(row.at("pearson"),dst.pearson); in.estimate(row.at("rank"),dst.rank);
    const auto& c=r.at("coverage").at(h); auto& cov=result.coverage[h];
    cov.mature_dates=static_cast<usize>(in.count(c,"mature_dates"));
    cov.structural_tail_dates=static_cast<usize>(in.count(c,"structural_tail_dates"));
    cov.decision_eligible_pairs=in.count(c,"decision_eligible_pairs");
    cov.finite_label_pairs=in.count(c,"finite_label_pairs");
    cov.paired_signal_pairs=in.count(c,"paired_signal_pairs");
    cov.missing_entry_pairs=in.count(c,"missing_entry_pairs");
    cov.missing_exit_pairs=in.count(c,"missing_exit_pairs");
    cov.guard_excluded_pairs=in.count(c,"guard_excluded_pairs");
    cov.invalid_price_pairs=in.count(c,"invalid_price_pairs");
    cov.nonfinite_return_pairs=in.count(c,"nonfinite_return_pairs");
  }
  for (usize k=0;k<active;++k) {
    in.values(r.at("pearson_series").at(k),scratch.pearson_series(k).size(),out.pearson[k]);
    in.values(r.at("rank_series").at(k),scratch.rank_series(k).size(),out.rank[k]);
  }
  if (!in.ok) return std::nullopt;
  return out;
}
} // namespace
// nullopt: no entry (score, then store). A present entry must be intact and name
// exactly this candidate, signal bytes and scope key; anything else is a loud
// refusal, never a silent rescore over foreign bytes (the signal cache's policy).
co::Result<std::optional<CachedIc>> ic_cache_lookup(const std::filesystem::path& path,
    const IcCacheScope& scope,const Candidate& c,const std::string& signal_sha,
    const ex::ResearchIcScratch& scratch,usize active) {
  std::error_code ec; const bool present=std::filesystem::exists(path,ec);
  if (ec) return co::Err(co::ErrorCode::IoError,"IC runner: candidate IC cache probe: "+c.id);
  if (!present) return co::Ok(std::optional<CachedIc>{});
  ATX_TRY(auto text,metadata_text(path.string()));
  const auto j=Json::parse(text,nullptr,false);
  if (j.is_discarded() || !j.is_object() || !j.contains("schema") || !j.at("schema").is_string() ||
      j.at("schema").get<std::string>()!=ic_cache_schema || !j.contains("record") ||
      !j.at("record").is_object() || !j.contains("record_sha256") || !j.at("record_sha256").is_string())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate IC cache entry malformed: "+c.id);
  const auto& record=j.at("record");
  ATX_TRY(auto digest,co::sha256_hex(record.dump()));
  if (digest!=j.at("record_sha256").get<std::string>())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate IC cache entry integrity: "+c.id);
  const auto text_of=[&](const char* key) {
    return record.contains(key) && record.at(key).is_string()?record.at(key).get<std::string>():std::string{};
  };
  if (text_of("candidate_id")!=c.id || text_of("dsl_sha256")!=c.dsl_sha ||
      text_of("signal_payload_sha256")!=signal_sha || !record.contains("key") ||
      record.at("key")!=scope.key || !record.contains("result"))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate IC cache entry mismatch: "+c.id);
  auto cached=decode_ic_record(record.at("result"),scratch,active);
  if (!cached)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate IC cache entry malformed: "+c.id);
  return co::Ok(std::move(cached));
}
// No-replace publication; an entry that raced in first is accepted only if its
// record is identical. `recorded` values are provenance only, outside the hash.
co::Status ic_cache_store(const std::filesystem::path& path,const IcCacheScope& scope,const Candidate& c,
    const std::string& signal_sha,const ex::ResearchIcResult& result,const IcSeries& daily,usize workers) {
  std::error_code ec; std::filesystem::create_directories(path.parent_path(),ec);
  if (ec) return co::Err(co::ErrorCode::IoError,"IC runner: candidate IC cache directory: "+ec.message());
  const Json record{{"candidate_id",c.id},{"dsl_sha256",c.dsl_sha},{"signal_payload_sha256",signal_sha},
      {"key",scope.key},{"result",ic_result_record(result,daily)}};
  ATX_TRY(auto digest,co::sha256_hex(record.dump()));
  const Json entry{{"schema",ic_cache_schema},{"record_sha256",std::move(digest)},{"record",record},
      {"recorded",{{"engine_git_sha",std::string(build_engine_git_sha())},{"ic_workers",workers}}}};
  const auto text=entry.dump()+"\n";
  const PartialFile partial(path);
  ATX_TRY_VOID(write_partial(partial,std::as_bytes(std::span(text.data(),text.size()))));
  ATX_TRY(auto existed,publish_new(partial.path,path));
  if (existed) {
    ATX_TRY(auto committed,metadata_text(path.string()));
    const auto other=Json::parse(committed,nullptr,false);
    if (other.is_discarded() || !other.is_object() || !other.contains("record") || other.at("record")!=record)
      return co::Err(co::ErrorCode::AlreadyExists,
          "IC runner: candidate IC cache entry raced with different bytes: "+c.id);
  }
  return co::Ok();
}
} // namespace atx::impl::strategy::ic_detail
namespace atx::impl::strategy {
IcCacheVmIdentity ic_result_cache_identity() {
  using namespace ic_detail;
  IcCacheVmIdentity out{ic_result_semantics_version,ic_identity(),{},std::string(ic_result_sources_sha256)};
  for (const auto path:ic_result_sources) out.sources.emplace_back(path);
  return out;
}
} // namespace atx::impl::strategy
