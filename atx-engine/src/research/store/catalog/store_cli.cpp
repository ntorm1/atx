// The atx-research-store command line (store_cli.hpp).

#include "atx/engine/research/store/catalog/store_cli.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <ostream>
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
#include "atx/engine/research/store/catalog/catalog.hpp"
#include "atx/engine/research/store/catalog/classify.hpp"
#include "atx/engine/research/store/catalog/ingest.hpp"
#include "atx/engine/research/store/catalog/ops_records.hpp"
#include "atx/engine/research/store/catalog/query.hpp"
#include "atx/engine/research/store/catalog/seal_guard.hpp"
#include "atx/engine/research/store/ops_cache.hpp"
#include "atx/engine/research/store/ops_core.hpp"
#include "atx/engine/research/store/rows_core.hpp"
#include "atx/engine/research/store/store.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace atx::engine::research::store::catalog {
namespace {

using core::db::Database;

constexpr std::string_view kUsage =
    "usage: atx-research-store <command> [options]\n"
    "  init --catalog DB\n"
    "  catalog --catalog DB --root R [--specs GLOB]... [--ledger L]... [--include P]...\n"
    "          [--rebuild] [--verify-payloads DIR] [--classes FILE]\n"
    "  ingest --catalog DB --class C --path P [--root R] [--classes FILE]\n"
    "  verify --catalog DB --pins [--spec S] [--strict]\n"
    "  query {artifacts|pins|stale-pins|runs|timings|producers} --catalog DB [--json]\n"
    "  digest --catalog DB\n"
    "  dump --catalog DB --table T\n"
    "  cache init DIR [--import]\n"
    "  cache prune --base DIR\n"
    "  schema --json --db catalog|cache\n"
    "  quick-check (--catalog DB | --cache DIR)\n"
    "exit: 0 ok, 2 usage, 3 refusal, 4 error\n"
    "Only init, catalog and cache init create a store; every other verb refuses a missing one.\n"
    "cache init creates DIR/index.sqlite, the only way a cache index is made. WARNING (ruling\n"
    "SQL1-MON): book_monitor.py --fit-work does not yet read records that exist only in a\n"
    "SQLite cache index; never point it at an indexed fit-work dir. No P9 research cell uses a\n"
    "cache index (ruling SQL-11): root's identity runs only, and root deletes the index after.\n";

constexpr std::string_view kMonitorWarning =
    "warning (SQL1-MON): book_monitor.py --fit-work does not yet read records stored only in "
    "this index; no P9 cell uses a cache index (SQL-11); delete it after the identity run\n";

constexpr std::array<std::string_view, 14> kValueOptions{
    "--catalog", "--root", "--specs", "--ledger", "--include", "--verify-payloads", "--classes",
    "--class",   "--path", "--spec",  "--table",  "--db",      "--base",            "--cache"};
constexpr std::array<std::string_view, 7> kFlags{"--rebuild", "--strict", "--json", "--pins",
                                                 "--import",  "--help",   "-h"};

struct Args {
  std::vector<std::string> positional;
  std::map<std::string, std::vector<std::string>, std::less<>> values;
  std::vector<std::string> flags;
  std::string error;

  [[nodiscard]] bool flag(std::string_view f) const {
    return std::find(flags.begin(), flags.end(), f) != flags.end();
  }
  [[nodiscard]] std::optional<std::string> one(std::string_view key) const {
    const auto it = values.find(key);
    if (it == values.end() || it->second.empty()) {
      return std::nullopt;
    }
    return it->second.back();
  }
  [[nodiscard]] std::vector<std::string> all(std::string_view key) const {
    const auto it = values.find(key);
    return it == values.end() ? std::vector<std::string>{} : it->second;
  }
};

[[nodiscard]] Args parse(std::span<const std::string> args) {
  Args out;
  for (usize i = 0; i < args.size(); ++i) {
    const std::string &a = args[i];
    if (a.rfind("-", 0) != 0) {
      out.positional.push_back(a);
      continue;
    }
    if (std::find(kValueOptions.begin(), kValueOptions.end(), a) != kValueOptions.end()) {
      if (i + 1 >= args.size()) {
        out.error = a + " needs a value";
        return out;
      }
      out.values[a].push_back(args[++i]);
      continue;
    }
    if (std::find(kFlags.begin(), kFlags.end(), a) != kFlags.end()) {
      out.flags.push_back(a);
      continue;
    }
    out.error = "unknown option " + a;
    return out;
  }
  return out;
}

// A refusal (exit 3): a sealed / oversized / outside-root path, a changed ledger line, a store
// in use (PermissionDenied), a store newer than this build (NotImplemented).
[[nodiscard]] int refusal_or_error(const core::Error &e) {
  const bool refused = e.code() == core::ErrorCode::PermissionDenied ||
                       e.code() == core::ErrorCode::NotImplemented;
  return refused ? kExitRefusal : kExitError;
}

[[nodiscard]] int fail(std::ostream &err, const core::Error &e, int code) {
  err << "atx-research-store: " << e.to_string() << "\n";
  return code;
}

// Opening a store: a foreign application_id, a non-store file or schema drift
// (InvalidArgument), a newer schema (NotImplemented) and no store at all for a verb that never
// creates one (NotFound: only `init`, `catalog` and `cache init` create) are refusals.
[[nodiscard]] int open_failure(std::ostream &err, const core::Error &e) {
  const bool refused = e.code() == core::ErrorCode::InvalidArgument ||
                       e.code() == core::ErrorCode::NotImplemented ||
                       e.code() == core::ErrorCode::PermissionDenied ||
                       e.code() == core::ErrorCode::NotFound;
  return fail(err, e, refused ? kExitRefusal : kExitError);
}

[[nodiscard]] int usage(std::ostream &err, std::string_view why) {
  err << "atx-research-store: " << why << "\n" << kUsage;
  return kExitUsage;
}

// The running executable's SHA-256 (store_info created_by, catalog_run.store_exe_sha256).
[[nodiscard]] std::optional<std::string> self_sha256() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(32768);
  const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (size == 0 || size >= buffer.size()) {
    return std::nullopt;
  }
  const std::filesystem::path exe{std::wstring(buffer.data(), size)};
#else
  std::error_code ec;
  const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) {
    return std::nullopt;
  }
#endif
  auto digest = core::sha256_file(utf8_path(exe));
  return digest ? std::optional<std::string>{*digest} : std::nullopt;
}

[[nodiscard]] core::Status stamp_created_by(Database &db) {
  const std::optional<std::string> exe = self_sha256();
  if (!exe) {
    return core::Ok();
  }
  return core::db::with_immediate(db, [&](Database &d) -> core::Status {
    return upsert(d, StoreInfoRow{"created_by", *exe});
  });
}

// --classes, else <root>/atx-engine/schemas/research_store/classes.json, else the build's copy.
[[nodiscard]] core::Result<ClassRegistry> registry_for(const Args &a,
                                                       const std::filesystem::path &root) {
  if (const std::optional<std::string> given = a.one("--classes"); given) {
    return load_class_registry(*given);
  }
  const std::filesystem::path in_root =
      root / "atx-engine" / "schemas" / "research_store" / "classes.json";
  std::error_code ec;
  if (std::filesystem::is_regular_file(in_root, ec)) {
    return load_class_registry(utf8_path(in_root));
  }
#if defined(ATX_RESEARCH_STORE_CLASSES)
  return load_class_registry(ATX_RESEARCH_STORE_CLASSES);
#else
  return core::Err(core::ErrorCode::NotFound, "no classes.json: pass --classes FILE");
#endif
}

// Remove a catalog and its WAL files, only after this process proved no other connection holds
// it (a TRUNCATE checkpoint that completes).
[[nodiscard]] core::Status remove_catalog(const std::string &path) {
  std::error_code ec;
  if (!std::filesystem::exists(fs_path(path), ec)) {
    return core::Ok();
  }
  {
    ATX_TRY(Database db, open_catalog(path, StoreOpen::Existing));
    if (!core::db::checkpoint_truncate(db)) {
      return core::Err(core::ErrorCode::PermissionDenied,
                       path + " is in use (its WAL checkpoint did not complete): not rebuilt");
    }
  }
  for (const std::string suffix : {"", "-wal", "-shm"}) {
    std::filesystem::remove(fs_path(path + suffix), ec);
    if (ec) {
      return core::Err(core::ErrorCode::IoError, "cannot remove " + path + suffix);
    }
  }
  return core::Ok();
}

[[nodiscard]] std::optional<Database> open_or_report(const std::optional<std::string> &path,
                                                     StoreOpen how,
                                                     std::ostream &err, int &code) {
  if (!path) {
    code = usage(err, "--catalog DB is required");
    return std::nullopt;
  }
  auto db = open_catalog(*path, how);
  if (!db) {
    code = open_failure(err, db.error());
    return std::nullopt;
  }
  return std::move(*db);
}

int cmd_init(const Args &a, std::ostream &out, std::ostream &err) {
  int code = kExitOk;
  std::optional<Database> db =
      open_or_report(a.one("--catalog"), StoreOpen::CreateIfMissing, err, code);
  if (!db) {
    return code;
  }
  if (auto s = stamp_created_by(*db); !s) {
    return fail(err, s.error(), kExitError);
  }
  out << "catalog " << *a.one("--catalog") << ": schema " << std::string{kStoreSchemaId}
      << " (catalog_core, catalog_records)\n";
  return kExitOk;
}

int cmd_catalog(const Args &a, std::ostream &out, std::ostream &err) {
  const std::optional<std::string> path = a.one("--catalog");
  const std::optional<std::string> root = a.one("--root");
  if (!path || !root) {
    return usage(err, "catalog needs --catalog DB and --root R");
  }
  if (a.flag("--rebuild")) {
    if (auto s = remove_catalog(*path); !s) {
      return fail(err, s.error(), refusal_or_error(s.error()));
    }
  }
  auto registry = registry_for(a, fs_path(*root));
  if (!registry) {
    return fail(err, registry.error(), kExitError);
  }
  int code = kExitOk;
  std::optional<Database> db = open_or_report(path, StoreOpen::CreateIfMissing, err, code);
  if (!db) {
    return code;
  }
  if (auto s = stamp_created_by(*db); !s) {
    return fail(err, s.error(), kExitError);
  }
  CatalogOptions opt;
  opt.root = fs_path(*root);
  opt.spec_globs = a.all("--specs");
  opt.ledgers = a.all("--ledger");
  opt.includes = a.all("--include");
  opt.verify_payloads_dir = a.one("--verify-payloads");
  opt.store_exe_sha256 = self_sha256();
  opt.ledger_head = default_ledger_head();
  auto report = run_catalog(*db, *registry, opt);
  if (!report) {
    return fail(err, report.error(), refusal_or_error(report.error()));
  }
  out << "catalog_run " << report->catalog_run_id << "\n"
      << "files_seen " << report->files_seen << " verified " << report->files_verified
      << " declared " << report->files_declared << " skipped " << report->files_skipped << "\n"
      << "catalog_digest " << report->catalog_digest << "\n"
      << "seconds " << report->seconds << "\n"
      << "checkpoint " << (report->checkpointed ? "ok" : "busy") << "\n";
  return kExitOk;
}

int cmd_ingest(const Args &a, std::ostream &out, std::ostream &err) {
  const std::optional<std::string> cls = a.one("--class");
  const std::optional<std::string> file = a.one("--path");
  if (!cls || !file) {
    return usage(err, "ingest needs --catalog DB --class C --path P");
  }
  const std::string root = a.one("--root").value_or(std::string{"."});
  auto registry = registry_for(a, fs_path(root));
  if (!registry) {
    return fail(err, registry.error(), kExitError);
  }
  int code = kExitOk;
  std::optional<Database> db = open_or_report(a.one("--catalog"), StoreOpen::Existing, err, code);
  if (!db) {
    return code;
  }
  IngestOneOptions opt;
  opt.root = fs_path(root);
  opt.class_id = *cls;
  opt.path = *file;
  opt.ledger_head = default_ledger_head();
  auto report = ingest_one(*db, *registry, opt);
  if (!report) {
    const bool usage_error = report.error().code() == core::ErrorCode::InvalidArgument;
    return fail(err, report.error(),
                usage_error ? kExitUsage : refusal_or_error(report.error()));
  }
  out << "ingested " << report->path << " as " << report->class_id << " (pins " << report->pins
      << ")" << (report->unparsed ? " unparsed" : "") << "\n";
  return kExitOk;
}

int cmd_verify(const Args &a, std::ostream &out, std::ostream &err) {
  if (!a.flag("--pins")) {
    return usage(err, "verify needs --pins");
  }
  int code = kExitOk;
  std::optional<Database> db = open_or_report(a.one("--catalog"), StoreOpen::Existing, err, code);
  if (!db) {
    return code;
  }
  std::optional<std::string> holder = a.one("--spec");
  if (holder) {
    std::replace(holder->begin(), holder->end(), '\\', '/');
  }
  auto artifacts = artifact_counts(*db);
  auto counts = pin_counts(*db, holder);
  auto problems = pin_problems(*db, holder);
  if (!artifacts || !counts || !problems) {
    const core::Error &e =
        !artifacts ? artifacts.error() : (!counts ? counts.error() : problems.error());
    return fail(err, e, kExitError);
  }
  for (const ShaSourceCount &c : *artifacts) {
    out << "artifacts " << c.sha_source << " " << c.count << "\n";
  }
  std::map<std::string, i64> by_state{
      {"ok", 0}, {"declared", 0}, {"stale", 0}, {"missing", 0}, {"unresolved", 0}};
  for (const PinCount &c : *counts) {
    by_state[c.state] += c.count;
  }
  for (const std::string_view state : {"ok", "declared", "stale", "missing", "unresolved"}) {
    out << "pins " << state << " " << by_state[std::string{state}] << "\n";
  }
  for (const PinCount &c : *counts) {
    out << "pin " << c.pin_kind << " " << c.state << " " << c.count << "\n";
  }
  for (const PinProblem &p : *problems) {
    out << p.state << " " << p.holder_path << "#" << p.pointer << " (" << p.pin_kind << ") -> "
        << p.target_path.value_or("?") << " pinned " << p.target_sha256 << " catalogued "
        << p.artifact_sha256.value_or("none") << "\n";
  }
  if (a.flag("--strict") && !problems->empty()) {
    err << "atx-research-store: " << problems->size() << " stale or missing pins (--strict)\n";
    return kExitRefusal;
  }
  return kExitOk;
}

int cmd_query(const Args &a, std::ostream &out, std::ostream &err) {
  if (a.positional.size() != 2) {
    return usage(err, "query needs one of artifacts, pins, stale-pins, runs, timings, producers");
  }
  int code = kExitOk;
  std::optional<Database> db = open_or_report(a.one("--catalog"), StoreOpen::Existing, err, code);
  if (!db) {
    return code;
  }
  auto lines =
      run_query(*db, a.positional[1], a.flag("--json") ? QueryFormat::Json : QueryFormat::Text);
  if (!lines) {
    return lines.error().code() == core::ErrorCode::InvalidArgument
               ? usage(err, lines.error().message())
               : fail(err, lines.error(), kExitError);
  }
  for (const std::string &line : *lines) {
    out << line << "\n";
  }
  return kExitOk;
}

int cmd_digest(const Args &a, std::ostream &out, std::ostream &err) {
  int code = kExitOk;
  std::optional<Database> db = open_or_report(a.one("--catalog"), StoreOpen::Existing, err, code);
  if (!db) {
    return code;
  }
  auto digest = catalog_digest(*db, catalog_groups());
  if (!digest) {
    return fail(err, digest.error(), kExitError);
  }
  out << *digest << "\n";
  return kExitOk;
}

int cmd_dump(const Args &a, std::ostream &out, std::ostream &err) {
  const std::optional<std::string> table = a.one("--table");
  if (!table) {
    return usage(err, "dump needs --table T");
  }
  int code = kExitOk;
  std::optional<Database> db = open_or_report(a.one("--catalog"), StoreOpen::Existing, err, code);
  if (!db) {
    return code;
  }
  auto lines = dump_table(*db, *table);
  if (!lines) {
    return lines.error().code() == core::ErrorCode::InvalidArgument
               ? usage(err, lines.error().message())
               : fail(err, lines.error(), kExitError);
  }
  for (const std::string &line : *lines) {
    out << line << "\n";
  }
  return kExitOk;
}

int cmd_cache(const Args &a, std::ostream &out, std::ostream &err) {
  const std::string sub = a.positional.size() >= 2 ? a.positional[1] : std::string{};
  if (sub == "init") {
    if (a.positional.size() != 3) {
      return usage(err, "cache init needs DIR");
    }
    const std::filesystem::path dir = fs_path(a.positional[2]);
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
      err << "atx-research-store: " << a.positional[2] << " is not a directory\n";
      return kExitRefusal;
    }
    auto db = create_cache(dir);
    if (!db) {
      return open_failure(err, db.error());
    }
    if (auto s = stamp_created_by(*db); !s) {
      return fail(err, s.error(), kExitError);
    }
    out << "cache index " << utf8_path(dir / std::string{kCacheIndexName}) << "\n";
    err << kMonitorWarning;
    if (a.flag("--import")) {
      auto report = import_legacy_records(*db, dir);
      if (!report) {
        return fail(err, report.error(), kExitError);
      }
      out << "imported " << report->imported << " present " << report->present << " rejected "
          << report->rejected << "\n";
    }
    return kExitOk;
  }
  if (sub == "prune") {
    const std::optional<std::string> base = a.one("--base");
    if (!base) {
      return usage(err, "cache prune needs --base DIR");
    }
    const std::filesystem::path dir = fs_path(*base);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(dir / std::string{kCacheIndexName}, ec)) {
      err << "atx-research-store: no cache index in " << *base << "\n";
      return kExitRefusal;
    }
    auto db = open_cache(dir);
    if (!db) {
      return open_failure(err, db.error());
    }
    auto deleted = prune_cache(*db, dir);
    if (!deleted) {
      return fail(err, deleted.error(), kExitError);
    }
    out << "pruned " << *deleted << " record rows\n";
    return kExitOk;
  }
  return usage(err, "cache needs init DIR [--import] or prune --base DIR");
}

int cmd_schema(const Args &a, std::ostream &out, std::ostream &err) {
  const std::optional<std::string> kind = a.one("--db");
  if (!a.flag("--json") || !kind || (*kind != "catalog" && *kind != "cache")) {
    return usage(err, "schema needs --json --db catalog|cache");
  }
  if (*kind == "catalog") {
    out << schema_json(DbKind::Catalog, catalog_groups());
  } else {
    const GroupOps *const groups[] = {&cache_group()};
    out << schema_json(DbKind::Cache, groups);
  }
  return kExitOk;
}

int cmd_quick_check(const Args &a, std::ostream &out, std::ostream &err) {
  const std::optional<std::string> catalog = a.one("--catalog");
  const std::optional<std::string> cache = a.one("--cache");
  if (catalog.has_value() == cache.has_value()) {
    return usage(err, "quick-check needs --catalog DB or --cache DIR");
  }
  auto db = catalog ? open_catalog(*catalog, StoreOpen::Existing) : open_cache(fs_path(*cache));
  if (!db) {
    return open_failure(err, db.error());
  }
  auto result = core::db::quick_check(*db);
  if (!result) {
    return fail(err, result.error(), kExitError);
  }
  out << *result << "\n";
  return *result == "ok" ? kExitOk : kExitError;
}

} // namespace

int run_store_cli(std::span<const std::string> args, std::ostream &out, std::ostream &err) {
  const Args a = parse(args);
  if (a.flag("--help") || a.flag("-h")) {
    out << kUsage;
    return kExitOk;
  }
  if (!a.error.empty()) {
    return usage(err, a.error);
  }
  if (a.positional.empty()) {
    return usage(err, "no command");
  }
  const std::string &command = a.positional.front();
  if (command != "query" && command != "cache" && a.positional.size() != 1) {
    return usage(err, "unexpected argument " + a.positional[1]);
  }
  if (command == "init") {
    return cmd_init(a, out, err);
  }
  if (command == "catalog") {
    return cmd_catalog(a, out, err);
  }
  if (command == "ingest") {
    return cmd_ingest(a, out, err);
  }
  if (command == "verify") {
    return cmd_verify(a, out, err);
  }
  if (command == "query") {
    return cmd_query(a, out, err);
  }
  if (command == "digest") {
    return cmd_digest(a, out, err);
  }
  if (command == "dump") {
    return cmd_dump(a, out, err);
  }
  if (command == "cache") {
    return cmd_cache(a, out, err);
  }
  if (command == "schema") {
    return cmd_schema(a, out, err);
  }
  if (command == "quick-check") {
    return cmd_quick_check(a, out, err);
  }
  return usage(err, "unknown command " + command);
}

} // namespace atx::engine::research::store::catalog
