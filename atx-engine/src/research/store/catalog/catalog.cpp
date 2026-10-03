// The catalog walk, single-file ingest and the ingest dispatcher (catalog.hpp, ingest.hpp).

#include "atx/engine/research/store/catalog/catalog.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/db/connection.hpp"
#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/classify.hpp"
#include "atx/engine/research/store/catalog/ingest.hpp"
#include "atx/engine/research/store/catalog/ops_records.hpp"
#include "atx/engine/research/store/catalog/seal_guard.hpp"
#include "atx/engine/research/store/ops_core.hpp"
#include "atx/engine/research/store/rows_core.hpp"
#include "atx/engine/research/store/store.hpp"
#include "research/store/catalog/catalog_detail.hpp"

namespace atx::engine::research::store::catalog {

namespace detail {

core::Status delete_where(core::db::Database &db, std::string_view table, std::string_view column,
                          std::string_view value) {
  ATX_TRY(core::db::Statement * stmt, db.prepare_cached("DELETE FROM " + std::string{table} +
                                                        " WHERE " + std::string{column} +
                                                        " = ?1;"));
  ATX_TRY_VOID(stmt->bind(1, value));
  ATX_TRY(const core::db::Statement::Step step, stmt->step());
  (void)step; // a DELETE returns no rows
  return core::Ok();
}

} // namespace detail

std::span<const GroupOps *const> catalog_groups() noexcept {
  static const GroupOps *const groups[] = {&core_group(), &records_group()};
  return groups;
}

core::Result<core::db::Database> open_catalog(std::string_view path, StoreOpen how) {
  return open_store(path, DbKind::Catalog, catalog_groups(), how);
}

std::vector<std::string> default_spec_globs() {
  return {"scripts/specs/*.json", "scripts/specs/v8/**", "scripts/specs/p9/**"};
}

std::string_view detect_eol(std::string_view bytes) noexcept {
  bool lf = false;
  bool crlf = false;
  for (usize i = 0; i < bytes.size(); ++i) {
    if (bytes[i] != '\n') {
      continue;
    }
    if (i > 0 && bytes[i - 1] == '\r') {
      crlf = true;
    } else {
      lf = true;
    }
  }
  if (!lf && !crlf) {
    return "none";
  }
  if (lf && crlf) {
    return "mixed";
  }
  return crlf ? "crlf" : "lf";
}

core::Result<IngestResult> ingest_file(const IngestContext &ctx, const FileView &file) {
  IngestResult out;
  const ClassInfo &c = *file.cls;
  if (c.artifact_only()) {
    return out;
  }
  if (c.ingest == "trial_line") {
    ATX_TRY_VOID(detail::ingest_trial_ledger(ctx, file, out));
    return out;
  }
  const std::optional<detail::OJson> doc = detail::parse_strict(file.bytes);
  if (!doc || !doc->is_object()) {
    out.unparsed = true;
    return out;
  }
  out.json_schema = detail::schema_of(*doc);
  const detail::FamilyInput in{ctx, file, *doc};
  const std::string &f = c.ingest;
  if (f == "run") {
    detail::ingest_run(in, out);
  } else if (f == "run_start") {
    detail::ingest_run_start(in, out);
  } else if (f == "stage_receipt") {
    detail::ingest_stage_receipt(in, out);
  } else if (f == "cycle_binding") {
    detail::ingest_cycle_binding(in, out);
  } else if (f == "cycle_verdict") {
    detail::ingest_cycle_verdict(in, out);
  } else if (f == "wave_result") {
    detail::ingest_wave_result(in, out);
  } else if (f == "spec_doc") {
    detail::ingest_spec_doc(in, out);
  } else if (f == "candidate") {
    detail::ingest_candidate(in, out);
  } else if (f == "field_manifest") {
    detail::ingest_field_manifest(in, out);
  } else if (f == "build_receipt") {
    detail::ingest_build_receipt(in, out);
  }
  if (out.unparsed) {
    IngestResult bare;
    bare.unparsed = true;
    bare.json_schema = std::move(out.json_schema);
    return bare;
  }
  return out;
}

namespace {

using core::db::Database;

constexpr std::string_view kBuildEquity = "build-equity";

struct Entry {
  std::string name;
  bool dir{};
  bool file{};
};

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// The entries of one directory, by name; symbolic links and junctions are left out (a walk never
// leaves the tree through one). An unreadable directory lists nothing.
[[nodiscard]] std::vector<Entry> list_dir(const std::filesystem::path &dir) {
  std::vector<Entry> out;
  std::error_code ec;
  std::filesystem::directory_iterator it{
      dir, std::filesystem::directory_options::skip_permission_denied, ec};
  const std::filesystem::directory_iterator end;
  // Bounded by the directory's entry count.
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code type_ec;
    if (it->is_symlink(type_ec)) {
      continue;
    }
    Entry e;
    e.name = utf8_path(it->path().filename());
    e.dir = it->is_directory(type_ec);
    e.file = !e.dir && it->is_regular_file(type_ec);
    out.push_back(std::move(e));
  }
  std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
  return out;
}

[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path &path) {
  std::ifstream in{path, std::ios::binary};
  if (!in) {
    return std::nullopt;
  }
  std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  if (in.bad()) {
    return std::nullopt;
  }
  return bytes;
}

[[nodiscard]] std::string two(i64 v) { return (v < 10 ? "0" : "") + std::to_string(v); }

// "YYYY-MM-DDTHH:MM:SS.ffffffZ" (UTC).
[[nodiscard]] std::string utc_now() {
  using namespace std::chrono;
  const auto now = floor<microseconds>(system_clock::now());
  const auto day = floor<days>(now);
  const year_month_day ymd{day};
  const hh_mm_ss<microseconds> t{now - day};
  std::string micro = std::to_string(t.subseconds().count());
  micro.insert(0, 6 - std::min<usize>(6, micro.size()), '0');
  return std::to_string(static_cast<int>(ymd.year())) + "-" +
         two(static_cast<unsigned>(ymd.month())) + "-" + two(static_cast<unsigned>(ymd.day())) +
         "T" + two(t.hours().count()) + ":" + two(t.minutes().count()) + ":" +
         two(t.seconds().count()) + "." + micro + "Z";
}

// The root-relative path's on-disk spelling (NTFS folds case; a pin may name it in another
// case): each segment is looked up in its parent's listing (cached per directory).
class CaseResolver {
public:
  explicit CaseResolver(std::filesystem::path root) : root_{std::move(root)} {}

  [[nodiscard]] std::optional<std::string> resolve(std::string_view rel) {
    std::string actual;
    usize start = 0;
    // Bounded by the path length: each pass consumes one segment.
    for (;;) {
      const usize slash = rel.find('/', start);
      const std::string_view seg = rel.substr(start, slash == std::string_view::npos
                                                         ? std::string_view::npos
                                                         : slash - start);
      const auto &names = listing(actual);
      const auto it = names.find(path_key(seg));
      if (it == names.end()) {
        return std::nullopt;
      }
      actual = join_rel(actual, it->second);
      if (slash == std::string_view::npos) {
        return actual;
      }
      start = slash + 1;
    }
  }

  [[nodiscard]] const std::filesystem::path &root() const noexcept { return root_; }

private:
  const std::map<std::string, std::string> &listing(const std::string &dir) {
    const std::string key = path_key(dir);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
      std::map<std::string, std::string> names;
      for (const Entry &e : list_dir(dir.empty() ? root_ : root_ / fs_path(dir))) {
        names.emplace(path_key(e.name), e.name);
      }
      it = cache_.emplace(key, std::move(names)).first;
    }
    return it->second;
  }

  std::filesystem::path root_;
  std::map<std::string, std::map<std::string, std::string>> cache_;
};

struct Declaration {
  std::string sha256;
  std::string holder;
};

struct BigFile {
  std::string path;
  u64 size{};
};

struct Skip {
  std::string path;
  std::string reason;
};

[[nodiscard]] core::Status flush(Database &db, std::vector<DbWrite> &writes) {
  if (writes.empty()) {
    return core::Ok();
  }
  ATX_TRY_VOID(core::db::with_immediate(db, [&](Database &d) -> core::Status {
    for (const DbWrite &w : writes) {
      ATX_TRY_VOID(w(d));
    }
    return core::Ok();
  }));
  writes.clear();
  return core::Ok();
}

// One file the catalog opened: hash, classify, ingest (no database access).
struct Opened {
  ArtifactRow artifact;
  IngestResult result;
  bool unparsed{};
};

[[nodiscard]] core::Result<Opened> open_file(const IngestContext &ctx,
                                             const ClassRegistry &registry,
                                             const ClassInfo *forced, const std::string &rel,
                                             std::string_view bytes) {
  ATX_TRY(std::string sha, core::sha256_hex(bytes));
  const std::string key = path_key(rel);
  const bool json = ends_with(key, ".json");
  std::optional<detail::OJson> doc;
  if (json) {
    doc = detail::parse_strict(bytes);
  }
  const std::optional<std::string> schema = doc ? detail::schema_of(*doc) : std::nullopt;
  const ClassInfo *cls =
      forced != nullptr ? forced : classify(registry, key, schema, !json || doc.has_value());
  Opened out;
  ATX_TRY(out.result, ingest_file(ctx, FileView{rel, sha, bytes, cls}));
  out.unparsed = (json && !doc) || out.result.unparsed;
  ArtifactRow &a = out.artifact;
  a.path_key = key;
  a.path = rel;
  a.sha256 = std::move(sha);
  a.bytes = static_cast<i64>(bytes.size());
  a.artifact_class = cls->id;
  a.json_schema = out.result.json_schema ? out.result.json_schema : schema;
  a.sha_source = "verified";
  a.eol = std::string{detect_eol(bytes)};
  a.producer_key = out.result.producer_key;
  return out;
}

class Walk {
public:
  Walk(Database &db, const ClassRegistry &registry, const CatalogOptions &opt)
      : db_{db}, registry_{registry}, opt_{opt}, resolver_{normal_root(opt.root)} {
    ctx_.root = resolver_.root();
    ctx_.ledger_head = opt.ledger_head;
    run_id_ = utc_now();
  }

  [[nodiscard]] core::Result<CatalogReport> run() {
    const auto t0 = std::chrono::steady_clock::now();
    const std::string started = run_id_;
    seed();
    const usize segment = std::max<usize>(1, opt_.segment_files);
    // Bounded: every file is opened at most once (done_), and the tree is finite.
    while (!pending_.empty()) {
      std::vector<DbWrite> writes;
      for (usize n = 0; n < segment && !pending_.empty(); ++n) {
        const auto it = opt_.reverse_walk ? std::prev(pending_.end()) : pending_.begin();
        const std::string key = it->first;
        const std::string named = it->second;
        pending_.erase(it);
        ATX_TRY_VOID(visit(key, named, writes));
      }
      ATX_TRY_VOID(flush(db_, writes));
    }
    ATX_TRY_VOID(settle_big_files());
    list_build_equity();
    ATX_TRY_VOID(write_skips());
    CatalogRunRow run;
    run.catalog_run_id = run_id_;
    run.store_exe_sha256 = opt_.store_exe_sha256;
    run.roots = roots_json();
    run.started_utc = started;
    run.files_verified = verified_;
    run.files_declared = declared_;
    run.files_skipped = static_cast<i64>(skips_.size());
    run.files_seen = verified_ + declared_ + run.files_skipped;
    ATX_TRY(std::string digest, catalog_digest(db_, catalog_groups()));
    run.catalog_digest = digest;
    run.seconds =
        std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count();
    ATX_TRY_VOID(core::db::with_immediate(
        db_, [&](Database &d) -> core::Status { return upsert(d, run); }));
    CatalogReport report;
    report.catalog_run_id = run_id_;
    report.catalog_digest = std::move(digest);
    report.files_seen = run.files_seen;
    report.files_verified = verified_;
    report.files_declared = declared_;
    report.files_skipped = run.files_skipped;
    report.seconds = run.seconds;
    report.checkpointed = core::db::checkpoint_truncate(db_).has_value();
    return report;
  }

private:
  void skip(const std::string &path, std::string_view reason) {
    skips_.emplace(path_key(path), Skip{path, std::string{reason}});
  }

  void enqueue(const std::string &named) {
    const std::string key = path_key(named);
    if (done_.count(key) == 0) {
      pending_.emplace(key, named);
    }
  }

  [[nodiscard]] std::filesystem::path full(const std::string &rel) const {
    return rel.empty() ? ctx_.root : ctx_.root / fs_path(rel);
  }

  // A named directory and everything under it (explicit stack; sealed dirs are listed only).
  void walk_dir(const std::string &named) {
    std::vector<std::string> stack;
    if (dirs_done_.insert(path_key(named)).second) {
      stack.push_back(named);
    }
    // Bounded by the tree's directory count: each dir is pushed once (dirs_done_).
    while (!stack.empty()) {
      const std::string dir = std::move(stack.back());
      stack.pop_back();
      if (has_sealed_year(dir)) {
        skip(dir, "seal-name");
        continue;
      }
      const std::optional<std::string> actual = resolver_.resolve(dir);
      if (!actual) {
        continue;
      }
      for (const Entry &e : list_dir(full(*actual))) {
        const std::string child = join_rel(*actual, e.name);
        if (e.file) {
          enqueue(child);
        } else if (e.dir && dirs_done_.insert(path_key(child)).second) {
          stack.push_back(child);
        }
      }
    }
  }

  // "<dir>/<name>": the dirs of <dir> named <name> or <name>-<anything>.
  void walk_prefix(const std::string &named) {
    const std::string parent = parent_of(named);
    const std::string prefix = path_key(leaf_of(named));
    if (has_sealed_year(parent)) {
      skip(parent, "seal-name");
      return;
    }
    const std::optional<std::string> actual =
        parent.empty() ? std::optional<std::string>{std::string{}} : resolver_.resolve(parent);
    if (!actual) {
      return;
    }
    for (const Entry &e : list_dir(full(*actual))) {
      const std::string name = path_key(e.name);
      if (e.dir && (name == prefix || name.rfind(prefix + "-", 0) == 0)) {
        walk_dir(join_rel(*actual, e.name));
      }
    }
  }

  // Files matching one root-relative glob, level by level ("**" lists the rest recursively).
  void expand_glob(const std::string &glob) {
    const std::string pattern = path_key(glob);
    std::vector<std::string> segs;
    for (usize start = 0;;) {
      const usize slash = pattern.find('/', start);
      segs.push_back(pattern.substr(start, slash == std::string::npos ? std::string::npos
                                                                      : slash - start));
      if (slash == std::string::npos) {
        break;
      }
      start = slash + 1;
    }
    std::vector<std::string> current{std::string{}};
    for (usize i = 0; i < segs.size() && !current.empty(); ++i) {
      if (segs[i] == "**") {
        for (const std::string &dir : current) {
          expand_recursive(dir, pattern);
        }
        return;
      }
      const bool last = i + 1 == segs.size();
      std::vector<std::string> next;
      for (const std::string &dir : current) {
        if (has_sealed_year(dir)) {
          skip(dir, "seal-name");
          continue;
        }
        for (const Entry &e : list_dir(full(dir))) {
          if (!glob_match(segs[i], path_key(e.name))) {
            continue;
          }
          if (last && e.file) {
            enqueue(join_rel(dir, e.name));
          } else if (!last && e.dir) {
            next.push_back(join_rel(dir, e.name));
          }
        }
      }
      current = std::move(next);
    }
  }

  void expand_recursive(const std::string &base, const std::string &pattern) {
    std::vector<std::string> stack{base};
    // Bounded by the directory count under `base`.
    while (!stack.empty()) {
      const std::string dir = std::move(stack.back());
      stack.pop_back();
      if (has_sealed_year(dir)) {
        skip(dir, "seal-name");
        continue;
      }
      for (const Entry &e : list_dir(full(dir))) {
        const std::string child = join_rel(dir, e.name);
        if (e.dir) {
          stack.push_back(child);
        } else if (e.file && glob_match(pattern, path_key(child))) {
          enqueue(child);
        }
      }
    }
  }

  void seed() {
    const std::vector<std::string> specs =
        opt_.spec_globs.empty() ? default_spec_globs() : opt_.spec_globs;
    for (const std::string &glob : specs) {
      expand_glob(glob);
    }
    for (const ClassInfo &c : registry_.classes) {
      for (const std::string &glob : c.seed_globs) {
        expand_glob(glob);
      }
    }
    for (const std::string &ledger : opt_.ledgers) {
      if (std::optional<std::string> rel = root_relative(ctx_.root, ledger); rel) {
        enqueue(*rel);
      }
    }
    for (const std::string &include : opt_.includes) {
      const std::optional<std::string> rel = root_relative(ctx_.root, include);
      if (!rel) {
        continue;
      }
      std::error_code ec;
      if (std::filesystem::is_directory(full(*rel), ec)) {
        walk_dir(*rel);
      } else {
        enqueue(*rel);
      }
    }
  }

  [[nodiscard]] std::string roots_json() const {
    detail::OJson roots = detail::OJson::object();
    roots["specs"] = opt_.spec_globs.empty() ? default_spec_globs() : opt_.spec_globs;
    roots["ledgers"] = opt_.ledgers;
    roots["includes"] = opt_.includes;
    return detail::compact(roots);
  }

  // Open one pending file (see catalog.hpp); its rows join `writes`.
  [[nodiscard]] core::Status visit(const std::string &key, const std::string &named,
                                   std::vector<DbWrite> &writes) {
    if (!done_.insert(key).second) {
      return core::Ok();
    }
    if (has_sealed_year(named)) {
      skip(named, "seal-name");
      return core::Ok();
    }
    const std::optional<std::string> rel = resolver_.resolve(named);
    if (!rel) {
      return core::Ok(); // not on disk: a pin naming it reads `missing`
    }
    std::error_code ec;
    const std::filesystem::path file = full(*rel);
    if (!std::filesystem::is_regular_file(file, ec)) {
      return core::Ok();
    }
    const u64 size = std::filesystem::file_size(file, ec);
    if (ec) {
      skip(*rel, "unreadable");
      return core::Ok();
    }
    if (size > kMaxOpenBytes) {
      big_.emplace(key, BigFile{*rel, size});
      return core::Ok();
    }
    const std::optional<std::string> bytes = read_file(file);
    if (!bytes) {
      skip(*rel, "unreadable");
      return core::Ok();
    }
    ATX_TRY(Opened opened, open_file(ctx_, registry_, nullptr, *rel, *bytes));
    if (opened.unparsed) {
      skip(*rel, "unparsed");
    }
    for (const PinRow &pin : opened.result.pins) {
      if (!pin.target_path) {
        continue;
      }
      const auto [it, fresh] =
          declarations_.emplace(*pin.target_path, Declaration{pin.target_sha256, *rel});
      if (!fresh && *rel < it->second.holder) {
        it->second = Declaration{pin.target_sha256, *rel};
      }
    }
    add_artifact(opened.artifact, writes);
    for (DbWrite &w : opened.result.writes) {
      writes.push_back(std::move(w));
    }
    for (const std::string &f : opened.result.named_files) {
      enqueue(f);
    }
    for (const std::string &d : opened.result.named_dirs) {
      walk_dir(d);
    }
    for (const std::string &p : opened.result.named_prefixes) {
      walk_prefix(p);
    }
    ++verified_;
    return core::Ok();
  }

  void add_artifact(const ArtifactRow &row, std::vector<DbWrite> &writes) const {
    writes.emplace_back([row, run = run_id_](Database &d) -> core::Status {
      ATX_TRY_VOID(upsert(d, row));
      return upsert(d, ArtifactSeenRow{row.path_key, row.sha256, run});
    });
  }

  // Files over 16 MiB: declared by their smallest holder, hashed under --verify-payloads, or
  // listed (declared-only).
  [[nodiscard]] core::Status settle_big_files() {
    const std::optional<std::string> verify_dir =
        opt_.verify_payloads_dir ? root_relative(ctx_.root, *opt_.verify_payloads_dir)
                                 : std::nullopt;
    const std::string verify_prefix = verify_dir ? path_key(*verify_dir) + "/" : std::string{};
    std::vector<DbWrite> writes;
    for (const auto &[key, big] : big_) {
      ArtifactRow a;
      a.path_key = key;
      a.path = big.path;
      a.bytes = static_cast<i64>(big.size);
      a.artifact_class = classify_by_path(registry_, key)->id;
      if (verify_dir && key.rfind(verify_prefix, 0) == 0) {
        ATX_TRY(a.sha256, core::sha256_file(utf8_path(full(big.path))));
        a.sha_source = "verified";
        ++verified_;
      } else if (const auto d = declarations_.find(key); d != declarations_.end()) {
        a.sha256 = d->second.sha256;
        a.sha_source = "declared";
        a.declared_by = d->second.holder;
        ++declared_;
      } else {
        skip(big.path, "declared-only");
        continue;
      }
      add_artifact(a, writes);
      if (writes.size() >= std::max<usize>(1, opt_.segment_files)) {
        ATX_TRY_VOID(flush(db_, writes));
      }
    }
    return flush(db_, writes);
  }

  // Everything else under build-equity/, by name only.
  void list_build_equity() {
    const std::optional<std::string> base = resolver_.resolve(kBuildEquity);
    if (!base) {
      return;
    }
    std::vector<std::string> stack{*base};
    // Bounded by the directory count under build-equity/.
    while (!stack.empty()) {
      const std::string dir = std::move(stack.back());
      stack.pop_back();
      if (has_sealed_year(dir)) {
        skip(dir, "seal-name");
        continue;
      }
      for (const Entry &e : list_dir(full(dir))) {
        const std::string child = join_rel(dir, e.name);
        if (e.dir) {
          stack.push_back(child);
        } else if (e.file) {
          const std::string key = path_key(child);
          if (done_.count(key) == 0 && skips_.count(key) == 0) {
            skip(child, "outside-roots");
          }
        }
      }
    }
  }

  [[nodiscard]] core::Status write_skips() {
    std::vector<DbWrite> writes;
    const usize segment = std::max<usize>(1, opt_.segment_files);
    for (const auto &[key, s] : skips_) {
      writes.emplace_back([row = SkippedPathRow{run_id_, s.path, s.reason}](Database &d) {
        return upsert(d, row);
      });
      if (writes.size() >= segment) {
        ATX_TRY_VOID(flush(db_, writes));
      }
    }
    return flush(db_, writes);
  }

  Database &db_;
  const ClassRegistry &registry_;
  const CatalogOptions &opt_;
  CaseResolver resolver_;
  IngestContext ctx_;
  std::string run_id_;
  std::map<std::string, std::string> pending_; // path_key -> path as named
  std::set<std::string> done_;                 // path_keys taken from pending_
  std::set<std::string> dirs_done_;            // path_keys of walked dirs
  std::map<std::string, Declaration> declarations_;
  std::map<std::string, BigFile> big_;
  std::map<std::string, Skip> skips_; // path_key -> listed path and reason
  i64 verified_{};
  i64 declared_{};
};

} // namespace

core::Result<CatalogReport> run_catalog(Database &db, const ClassRegistry &registry,
                                        const CatalogOptions &opt) {
  Walk walk{db, registry, opt};
  return walk.run();
}

core::Result<IngestOneReport> ingest_one(Database &db, const ClassRegistry &registry,
                                         const IngestOneOptions &opt) {
  const ClassInfo *cls = registry.find(opt.class_id);
  if (cls == nullptr) {
    return core::Err(core::ErrorCode::InvalidArgument, "unknown class " + opt.class_id);
  }
  IngestContext ctx;
  ctx.root = normal_root(opt.root);
  ctx.ledger_head = opt.ledger_head;
  const std::optional<std::string> named = root_relative(ctx.root, opt.path);
  if (!named) {
    return core::Err(core::ErrorCode::PermissionDenied, opt.path + " is outside the root");
  }
  if (has_sealed_year(*named)) {
    return core::Err(core::ErrorCode::PermissionDenied,
                     *named + " holds a year token 2024-2099: never opened (seal)");
  }
  CaseResolver resolver{ctx.root};
  const std::optional<std::string> rel = resolver.resolve(*named);
  std::error_code ec;
  if (!rel || !std::filesystem::is_regular_file(ctx.root / fs_path(*rel), ec)) {
    return core::Err(core::ErrorCode::NotFound, *named + " is not a file");
  }
  const std::filesystem::path file = ctx.root / fs_path(*rel);
  const u64 size = std::filesystem::file_size(file, ec);
  if (ec || size > kMaxOpenBytes) {
    return core::Err(core::ErrorCode::PermissionDenied,
                     *rel + " is over 16 MiB (or unreadable): never opened; a catalog run "
                            "records the SHA-256 its manifest declares");
  }
  const std::optional<std::string> bytes = read_file(file);
  if (!bytes) {
    return core::Err(core::ErrorCode::IoError, "cannot read " + *rel);
  }
  ATX_TRY(Opened opened, open_file(ctx, registry, cls, *rel, *bytes));
  std::vector<DbWrite> writes;
  writes.emplace_back([row = opened.artifact](Database &d) -> core::Status {
    ATX_TRY_VOID(upsert(d, row));
    return upsert(d, ArtifactSeenRow{row.path_key, row.sha256, "ingest"});
  });
  if (opened.unparsed) {
    writes.emplace_back([row = SkippedPathRow{"ingest", *rel, "unparsed"}](Database &d) {
      return upsert(d, row);
    });
  }
  for (DbWrite &w : opened.result.writes) {
    writes.push_back(std::move(w));
  }
  ATX_TRY_VOID(flush(db, writes));
  IngestOneReport report;
  report.path = *rel;
  report.class_id = cls->id;
  report.unparsed = opened.unparsed;
  report.pins = opened.result.pins.size();
  return report;
}

} // namespace atx::engine::research::store::catalog
