#include "strategy_ic_detail.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "atx/engine/alpha/vm.hpp"

namespace atx::impl::strategy::ic_detail {
namespace {
constexpr std::array<std::string_view,3> base_fields{"close","raw_close","volume"};
} // namespace
bool hash_valid(std::string_view value) {
  return value.size()==64 && std::all_of(value.begin(),value.end(),[](char c) {
    return (c>='0' && c<='9') || (c>='a' && c<='f');
  });
}
bool base_field(std::string_view name) {
  return std::find(base_fields.begin(),base_fields.end(),name)!=base_fields.end();
}
// Plain DSL identifier: no dots or separators, so `<name>.f64` is a safe basename.
bool field_identifier(std::string_view s) {
  return !s.empty() && s.size()<=64 && ((s.front()>='a' && s.front()<='z') || s.front()=='_') &&
      std::all_of(s.begin(),s.end(),[](char c) { return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_'; });
}
co::Result<std::string> metadata_text(const std::string& path) {
  std::ifstream in(path,std::ios::binary|std::ios::ate);
  if (!in || in.tellg()<0)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: metadata file missing or unreadable: "+path);
  if (in.tellg()==0 || static_cast<u64>(in.tellg())>(1ULL<<20))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: metadata file empty or over 1 MiB ("+
        std::to_string(static_cast<u64>(in.tellg()))+" B): "+path);
  std::string text(static_cast<usize>(in.tellg()),'\0');
  in.seekg(0); in.read(text.data(),static_cast<std::streamsize>(text.size()));
  if (!in || in.peek()!=std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError,"IC runner: metadata extent changed");
  return co::Ok(std::move(text));
}
co::Result<std::string> pinned_text(const std::string& path,const std::string& pin) {
  if (!hash_valid(pin)) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: external SHA256 required");
  ATX_TRY(auto text,metadata_text(path));
  ATX_TRY(auto actual,co::sha256_hex(text));
  if (actual!=pin) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: external metadata pin differs");
  return co::Ok(std::move(text));
}
co::Result<Json> pinned_json(const std::string& path,const std::string& pin) {
  ATX_TRY(auto text,pinned_text(path,pin));
  return co::Ok(Json::parse(text));
}
namespace {
// See FieldPlan. At most 64 candidates' worth of scans per field: O(n^2 f) with
// n <= 256 candidates and f <= 64 fields.
co::Result<FieldPlan> field_plan(const std::vector<Candidate>& candidates,const std::vector<std::string>& extras) {
  FieldPlan plan;
  if (extras.size()>64) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: at most 64 extra fields");
  const usize n=candidates.size(),f=extras.size();
  for (const auto& c:candidates) {
    u64 mask=0;
    for (const auto& name:c.extra_fields) {
      const auto at=std::lower_bound(extras.begin(),extras.end(),name);
      if (at==extras.end() || *at!=name) return co::Err(co::ErrorCode::Internal,"IC runner: field plan index");
      mask|=u64{1}<<static_cast<unsigned>(at-extras.begin());
    }
    plan.needs.push_back(mask); plan.capacity=std::max(plan.capacity,static_cast<usize>(std::popcount(mask)));
  }
  const auto next_use=[&](usize from,usize field) {
    for (usize t=from;t<n;++t) if ((plan.needs[t]>>field)&1U) return t;
    return n;
  };
  u64 resident=0;
  for (usize k=0;k<n;++k) {
    for (usize g=0;g<f;++g) if (((resident>>g)&1U) && next_use(k,g)==n) resident&=~(u64{1}<<g);
    for (usize g=0;g<f;++g) {
      if (!((plan.needs[k]>>g)&1U) || ((resident>>g)&1U)) continue;
      if (static_cast<usize>(std::popcount(resident))>=plan.capacity) {
        // A victim exists: fewer than `capacity` needed fields are resident yet.
        usize victim=f,farthest=0;
        for (usize h=0;h<f;++h) {
          if (!((resident>>h)&1U) || ((plan.needs[k]>>h)&1U)) continue;
          const auto t=next_use(k,h);
          if (victim==f || t>farthest) { victim=h; farthest=t; }
        }
        if (victim==f) return co::Err(co::ErrorCode::Internal,"IC runner: field plan eviction");
        resident&=~(u64{1}<<victim);
      }
      resident|=u64{1}<<g; ++plan.loads;
    }
    plan.planned.push_back(resident);
  }
  return co::Ok(std::move(plan));
}
bool composition_id(std::string_view s) {
  return !s.empty() && s.size()<=64 && std::all_of(s.begin(),s.end(),[](char c) {
    return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_';
  });
}
} // namespace
co::Result<Library> library(const IcRunnerConfig& cfg) {
  ATX_TRY(auto j,pinned_json(cfg.library_path,cfg.library_sha256));
  if (j.at("schema")!="atx.dsl-ic-library/v1")
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: library schema");
  Library out; out.id=j.at("id").get<std::string>();
  std::set<std::string> fields,families,ids,expressions,used_families,referenced;
  // The role's three price/volume fields plus any plain-identifier extras; each
  // extra must be present in every scored role's pinned fields manifest.
  for (const auto& field:j.at("fields")) {
    const auto name=field.at("name").get<std::string>();
    if (!field_identifier(name) || !fields.insert(name).second)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: declared field contract: "+name);
    if (!base_field(name)) out.declared_extra.push_back(name);
  }
  for (const auto base:base_fields) if (!fields.contains(std::string(base)))
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: declared field contract: missing "+std::string(base));
  std::sort(out.declared_extra.begin(),out.declared_extra.end());
  for (const auto& family:j.at("families"))
    if (!families.insert(family.at("id").get<std::string>()).second)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: duplicate family");
  if (out.id.empty() || out.id.size()>128 || families.empty() || families.size()>32 ||
      j.at("candidates").empty() || j.at("candidates").size()>256)
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: bounded library size");
  const al::Library operators;
  for (const auto& row:j.at("candidates")) {
    Candidate c; c.id=row.at("id").get<std::string>(); c.family=row.at("family").get<std::string>();
    const auto dsl=row.at("dsl").get<std::string>();
    if (!composition_id(c.id) || !composition_id(c.family) ||
        !ids.insert(c.id).second || !expressions.insert(dsl).second || !families.contains(c.family) ||
        row.at("sign_policy")!="train-rank-ic21" || row.at("horizons")!=Json::array({5,21,63}) ||
        dsl.empty() || dsl.size()>4096)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: candidate recipe/identity");
    // IDs are also CSV fields; reject separators rather than publish ambiguous rows.
    if (c.id.find_first_of(",\r\n\"")!=std::string::npos)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: unsafe candidate CSV identifier");
    ATX_TRY(auto ast,al::parse_expr(dsl,operators)); ATX_TRY(auto analysis,al::analyze(ast));
    ATX_TRY(c.program,al::compile(ast,analysis)); ATX_TRY(c.dsl_sha,co::sha256_hex(dsl));
    if (c.program.roots.size()!=1 || c.program.num_slots>64)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: one bounded DSL root required");
    // The compiled field dictionary is exactly what the VM resolves by name.
    for (const auto& field:c.program.fields) {
      if (!fields.contains(field))
        return co::Err(co::ErrorCode::InvalidArgument,"IC runner: undeclared DSL field: "+field);
      if (!base_field(field)) c.extra_fields.push_back(field);
    }
    std::sort(c.extra_fields.begin(),c.extra_fields.end());
    c.extra_fields.erase(std::unique(c.extra_fields.begin(),c.extra_fields.end()),c.extra_fields.end());
    referenced.insert(c.extra_fields.begin(),c.extra_fields.end());
    out.max_slots=std::max(out.max_slots,static_cast<usize>(c.program.num_slots));
    out.lookback=std::max(out.lookback,static_cast<usize>(c.program.required_lookback));
    used_families.insert(c.family); out.candidates.push_back(std::move(c));
  }
  if (families!=used_families) return co::Err(co::ErrorCode::InvalidArgument,"IC runner: empty declared family");
  out.extra_fields.assign(referenced.begin(),referenced.end());
  ATX_TRY(out.field_plan,field_plan(out.candidates,out.extra_fields));
  return co::Ok(std::move(out));
}
// Contract K1 (platform v8): the --plan-only `candidates` rows, read by the alpha
// registry as its static validation. One row per candidate in library order, each
// value taken from the compiled program the VM runs: num_slots (peak live slots,
// the <= 64 cap above), required_lookback (prior bars), extra_fields (the sorted
// non-base fields it reads) and node_count (unique DAG nodes after CSE,
// Program::unique_nodes; the generators' dag_nodes).
Json candidate_plan_rows(const Library& lib) {
  Json rows=Json::array();
  for (const auto& c:lib.candidates)
    rows.push_back({{"id",c.id},{"dsl_sha256",c.dsl_sha},{"num_slots",c.program.num_slots},
        {"required_lookback",c.program.required_lookback},{"extra_fields",c.extra_fields},
        {"node_count",c.program.unique_nodes}});
  return rows;
}
void release(std::vector<f64>& buffer) noexcept { std::vector<f64>().swap(buffer); }
// Every referenced field file is stat'ed before the role payload is opened, so a
// truncated field refuses first (verify_fields hashes only what misses load).
co::Status check_field_extents(const Role& spec) {
  for (const auto& field:spec.fields.load) {
    std::error_code ec; const auto size=std::filesystem::file_size(field.path,ec);
    if (ec || size!=field.bytes)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: research field payload extent: "+
          field.path.filename().string());
  }
  return co::Ok();
}
namespace {
// The DSL panel: the role's base columns and the resident extras, all BORROWED
// (no copy of any column), resolved by name. Only the 1B/cell presence mask is
// owned, copied from the role panel, so LoadField NaNs extras exactly where it
// NaNs the base fields. Must not outlive `base` or the extra columns.
co::Result<al::Panel> dsl_panel(const al::Panel& base,std::vector<std::string> extra_names,
                                std::vector<std::span<const f64>> extra_columns) {
  const auto d=base.dates(),n=base.instruments();
  std::vector<std::string> names; std::vector<std::span<const f64>> columns;
  names.reserve(base.num_fields()+extra_names.size()); columns.reserve(base.num_fields()+extra_names.size());
  for (usize f=0;f<base.num_fields();++f) {
    names.push_back(base.field_name(f)); columns.push_back(base.field_all(static_cast<al::FieldId>(f)));
  }
  for (usize k=0;k<extra_names.size();++k) {
    names.push_back(std::move(extra_names[k])); columns.push_back(extra_columns[k]);
  }
  std::vector<u8> presence(d*n);
  for (usize t=0;t<d;++t) for (usize i=0;i<n;++i) presence[t*n+i]=static_cast<u8>(base.in_universe(t,i));
  return al::Panel::create_borrowed(d,n,std::move(names),std::move(columns),std::move(presence));
}
std::optional<FileStamp> file_stamp(const std::filesystem::path& path) {
  std::error_code ec; const auto size=std::filesystem::file_size(path,ec);
  if (ec) return std::nullopt;
  const auto modified=std::filesystem::last_write_time(path,ec);
  if (ec) return std::nullopt;
  return FileStamp{static_cast<u64>(size),modified};
}
// Streams a pinned payload through SHA-256 without retaining it (one 1 MiB
// buffer); refuses a wrong extent and returns the lowercase hex digest.
co::Result<std::string> hash_payload(const std::filesystem::path& path,u64 bytes,HashMeter& meter,
                                     std::string_view what) {
  const auto label=std::string("IC runner: ")+std::string(what); const auto name=path.filename().string();
  std::ifstream in(path,std::ios::binary|std::ios::ate);
  if (!in || in.tellg()<0 || static_cast<u64>(in.tellg())!=bytes)
    return co::Err(co::ErrorCode::InvalidArgument,label+" extent: "+name);
  in.seekg(0); std::vector<char> buffer(io_chunk); co::Sha256 digest;
  for (u64 offset=0;offset<bytes;) {
    const auto chunk=static_cast<usize>(std::min<u64>(io_chunk,bytes-offset));
    in.read(buffer.data(),static_cast<std::streamsize>(chunk));
    if (!in) return co::Err(co::ErrorCode::IoError,label+" truncated: "+name);
    ATX_TRY_VOID(metered_update(digest,std::as_bytes(std::span(buffer.data(),chunk)),&meter));
    offset+=chunk;
  }
  if (in.peek()!=std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError,label+" changed extent: "+name);
  ATX_TRY(auto actual,digest.finalize());
  meter.verify_bytes+=bytes;
  return co::Ok(hex(actual));
}
} // namespace
// Fail fast, hash once: every field in `needed` (bit f = Library::extra_fields[f],
// the fields some cache miss will load) is hashed before the role payload opens,
// so a tampered field refuses first. `verified[f]` records the file stamp it was
// verified under (unchanged across the read), which its loads then trust.
co::Status verify_fields(const Role& spec,u64 needed,HashMeter& meter,
                         std::vector<std::optional<FileStamp>>& verified) {
  for (usize f=0;f<spec.fields.load.size();++f) {
    if (!((needed>>f)&1U)) continue;
    const auto& field=spec.fields.load[f];
    const auto before=file_stamp(field.path);
    ATX_TRY(auto sha,hash_payload(field.path,field.bytes,meter,"research field payload"));
    if (sha!=field.sha)
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: research field payload SHA256 mismatch: "+
          field.path.filename().string());
    const auto after=file_stamp(field.path);
    if (before && after && *before==*after) verified[f]=after;
  }
  return co::Ok();
}
void FieldResidency::enter(usize k,std::unique_ptr<al::Engine>& vm,std::ostream& progress) {
  const u64 drop=resident_&~lib_.field_plan.planned[k];
  if (!drop) return;
  vm.reset(); panel_.reset();
  for (usize f=0;f<columns_.size();++f) if ((drop>>f)&1U) {
    release(columns_[f]);
    progress<<"IC field-release role="<<spec_.name<<" field="<<lib_.extra_fields[f]<<'\n';
  }
  resident_&=~drop; progress<<std::flush;
}
co::Result<const al::Panel*> FieldResidency::panel_for(usize k,const al::Panel& base,const std::string& candidate,
    std::unique_ptr<al::Engine>& vm,std::ostream& progress) {
  const u64 missing=lib_.field_plan.needs[k]&~resident_;
  if (missing) {
    vm.reset(); panel_.reset();
    for (usize f=0;f<columns_.size();++f) {
      if (!((missing>>f)&1U)) continue;
      const auto& field=spec_.fields.load[f]; const auto started=std::chrono::steady_clock::now();
      const auto before=file_stamp(field.path);
      const bool trusted=before && verified_[f] && *verified_[f]==*before;
      ATX_TRY_VOID(load_pinned_f64(field.path,field.sha,cells_,columns_[f],"research field payload",&meter_,
          !trusted));
      if (!trusted) {
        // Recorded only if the file did not change while it was read and hashed.
        const auto after=file_stamp(field.path);
        verified_[f]=before && after && *before==*after?after:std::nullopt;
      }
      if (std::any_of(columns_[f].begin(),columns_[f].end(),[](f64 v) { return std::isinf(v); }))
        return co::Err(co::ErrorCode::InvalidArgument,"IC runner: research field value is infinite: "+field.name);
      const auto seconds=std::chrono::duration<f64>(std::chrono::steady_clock::now()-started).count();
      seconds_+=seconds; ++loads_; resident_|=u64{1}<<f;
      progress<<"IC field-load role="<<spec_.name<<" field="<<field.name<<" candidate="<<candidate
              <<" seconds="<<seconds<<" hashed="<<(trusted?0:1)<<'\n'<<std::flush;
    }
    peak_=std::max(peak_,static_cast<usize>(std::popcount(resident_)));
  }
  const al::Panel* out=&base;
  if (resident_) {
    if (!panel_) {
      std::vector<std::string> names; std::vector<std::span<const f64>> columns;
      for (usize f=0;f<columns_.size();++f) if ((resident_>>f)&1U) {
        names.push_back(lib_.extra_fields[f]); columns.emplace_back(columns_[f]);
      }
      ATX_TRY(auto panel,dsl_panel(base,std::move(names),std::move(columns)));
      panel_.emplace(std::move(panel));
    }
    out=&*panel_;
  }
  return co::Ok(out);
}
void FieldResidency::drop_all(std::unique_ptr<al::Engine>& vm) noexcept {
  vm.reset(); panel_.reset();
  for (auto& column:columns_) release(column);
  resident_=0;
}
} // namespace atx::impl::strategy::ic_detail
