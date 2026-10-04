// `cache init --import` and `cache prune` over a cache index (store_cli.hpp). The import mirrors
// atx-engine/tools/record_store.py exactly: a legacy file is accepted only under that module's
// checks (schema atx.record-store/v1, the directory's kind, the file name = SHA-256 of the
// canonical {kind, key}, a dict body, content_sha256 = SHA-256 of the canonical {schema, kind,
// key, body}), and its row carries the texts record_store._write_row stores (canonical key;
// json.dumps(body, separators=(",", ":")) in insertion order). Python's json text is produced by
// detail::py_dumps, so a mismatch in that writer shows up as a rejected content SHA-256, never
// as a wrong row.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
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
#include "atx/engine/research/store/catalog/seal_guard.hpp"
#include "atx/engine/research/store/catalog/store_cli.hpp"
#include "atx/engine/research/store/ops_cache.hpp"
#include "atx/engine/research/store/rows_cache.hpp"
#include "research/store/catalog/catalog_detail.hpp"

namespace atx::engine::research::store::catalog {
namespace {

using core::db::Database;
using detail::OJson;

constexpr std::string_view kRecordSchema = "atx.record-store/v1";
constexpr usize kInlineBodyMax = 1U << 20U; // record_store.INLINE_BODY_MAX
constexpr usize kImportSegment = 500;

struct Candidate {
  std::filesystem::path file;
  std::string root; // partition
  std::string kind;
};

[[nodiscard]] std::vector<std::filesystem::path> children(const std::filesystem::path &dir,
                                                          bool dirs) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  std::filesystem::directory_iterator it{
      dir, std::filesystem::directory_options::skip_permission_denied, ec};
  // Bounded by the directory's entry count.
  for (; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
    std::error_code type_ec;
    if (it->is_symlink(type_ec)) {
      continue;
    }
    const bool is_dir = it->is_directory(type_ec);
    if (is_dir == dirs && (dirs || it->path().extension() == ".json")) {
      out.push_back(it->path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// <dir>/<kind>/*.json (partition "") and <dir>/<root>/<kind>/*.json (partition <root>). Seal
// backstop (seal_guard.hpp): a partition or kind directory whose name holds a standalone year
// token 2024-2099 is never listed or descended into; `sealed` counts them. File names are not
// free text (legacy_row opens only 64-hex names), so the names checked here are the only free
// path text below `dir`.
[[nodiscard]] std::vector<Candidate> candidates(const std::filesystem::path &dir, i64 &sealed) {
  std::vector<Candidate> out;
  for (const std::filesystem::path &first : children(dir, true)) {
    const std::string first_name = utf8_path(first.filename());
    if (first_name == "objects") {
      continue;
    }
    if (has_sealed_year(first_name)) {
      ++sealed;
      continue;
    }
    for (const std::filesystem::path &file : children(first, false)) {
      out.push_back(Candidate{file, std::string{}, first_name});
    }
    for (const std::filesystem::path &second : children(first, true)) {
      const std::string second_name = utf8_path(second.filename());
      if (has_sealed_year(second_name)) {
        ++sealed;
        continue;
      }
      for (const std::filesystem::path &file : children(second, false)) {
        out.push_back(Candidate{file, first_name, second_name});
      }
    }
  }
  return out;
}

[[nodiscard]] core::Result<std::string> canonical_sha(const OJson &value) {
  ATX_TRY(const std::string text, detail::py_dumps(value, true));
  return core::sha256_hex(text);
}

// The row of one legacy file, or nullopt when record_store would not accept the file.
[[nodiscard]] core::Result<std::optional<RecordRow>> legacy_row(const Candidate &c) {
  // record_store names a record file by the SHA-256 of its canonical {kind, key}: any other
  // name is rejected unopened (the stem check below would reject it after opening).
  if (!detail::is_sha256(utf8_path(c.file.stem()))) {
    return std::optional<RecordRow>{};
  }
  std::error_code ec;
  const auto size = std::filesystem::file_size(c.file, ec);
  if (ec || size > kMaxOpenBytes) {
    return std::optional<RecordRow>{};
  }
  std::ifstream in{c.file, std::ios::binary};
  const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  const std::optional<OJson> doc = detail::parse_strict(bytes);
  const OJson *schema = doc ? detail::member(*doc, "schema") : nullptr;
  const OJson *kind = doc ? detail::member(*doc, "kind") : nullptr;
  const OJson *key = doc ? detail::member(*doc, "key") : nullptr;
  const OJson *body = doc ? detail::member(*doc, "body") : nullptr;
  const OJson *content = doc ? detail::member(*doc, "content_sha256") : nullptr;
  if (schema == nullptr || !schema->is_string() ||
      schema->get_ref<const std::string &>() != kRecordSchema || kind == nullptr ||
      !kind->is_string() || kind->get<std::string>() != c.kind || key == nullptr ||
      body == nullptr || !body->is_object() || content == nullptr || !content->is_string()) {
    return std::optional<RecordRow>{};
  }
  OJson keyed = OJson::object();
  keyed["kind"] = c.kind;
  keyed["key"] = *key;
  ATX_TRY(std::string key_sha, canonical_sha(keyed));
  if (utf8_path(c.file.stem()) != key_sha) {
    return std::optional<RecordRow>{};
  }
  OJson whole = OJson::object();
  whole["schema"] = std::string{kRecordSchema};
  whole["kind"] = c.kind;
  whole["key"] = *key;
  whole["body"] = *body;
  ATX_TRY(std::string content_sha, canonical_sha(whole));
  if (content->get<std::string>() != content_sha) {
    return std::optional<RecordRow>{};
  }
  RecordRow row;
  row.root = c.root;
  row.kind = c.kind;
  row.key_sha256 = std::move(key_sha);
  ATX_TRY(row.key, detail::py_dumps(*key, true));
  ATX_TRY(std::string body_text, detail::py_dumps(*body, false));
  row.body_bytes = static_cast<i64>(body_text.size());
  row.body = std::move(body_text);
  row.content_sha256 = std::move(content_sha);
  return std::optional<RecordRow>{std::move(row)};
}

// record_store._publish: an existing object of the same size is the same object (content name).
[[nodiscard]] core::Status publish_object(const std::filesystem::path &path,
                                          std::string_view data) {
  std::error_code ec;
  if (std::filesystem::file_size(path, ec) == data.size() && !ec) {
    return core::Ok();
  }
  std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path partial = path;
  partial += ".partial-import";
  {
    std::ofstream outf{partial, std::ios::binary | std::ios::trunc};
    outf.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!outf) {
      return core::Err(core::ErrorCode::IoError, "cannot write " + utf8_path(partial));
    }
  }
  std::filesystem::rename(partial, path, ec);
  if (ec) {
    return core::Err(core::ErrorCode::IoError, "cannot publish " + utf8_path(path));
  }
  return core::Ok();
}

[[nodiscard]] core::Result<bool> row_exists(Database &db, const RecordRow &row) {
  ATX_TRY(core::db::Statement stmt, db.prepare("SELECT 1 FROM record WHERE root = ?1 AND kind = "
                                               "?2 AND key_sha256 = ?3;"));
  ATX_TRY_VOID(stmt.bind(1, std::string_view{row.root}));
  ATX_TRY_VOID(stmt.bind(2, std::string_view{row.kind}));
  ATX_TRY_VOID(stmt.bind(3, std::string_view{row.key_sha256}));
  ATX_TRY(const core::db::Statement::Step step, stmt.step());
  return step == core::db::Statement::Step::Row;
}

[[nodiscard]] core::Status write_rows(Database &db, std::vector<RecordRow> &rows,
                                      CacheImportReport &report) {
  if (rows.empty()) {
    return core::Ok();
  }
  i64 imported = 0;
  i64 present = 0;
  ATX_TRY_VOID(core::db::with_immediate(db, [&](Database &d) -> core::Status {
    imported = 0;
    present = 0;
    for (const RecordRow &row : rows) {
      ATX_TRY(const bool exists, row_exists(d, row));
      if (exists) {
        ++present;
        continue;
      }
      ATX_TRY_VOID(insert(d, row));
      ++imported;
    }
    return core::Ok();
  }));
  report.imported += imported;
  report.present += present;
  rows.clear();
  return core::Ok();
}

} // namespace

core::Result<CacheImportReport> import_legacy_records(Database &cache,
                                                      const std::filesystem::path &dir) {
  if (sealed_root(dir)) {
    return core::Err(core::ErrorCode::PermissionDenied, sealed_root_message(dir));
  }
  CacheImportReport report;
  std::vector<RecordRow> rows;
  for (const Candidate &c : candidates(dir, report.sealed)) {
    ATX_TRY(std::optional<RecordRow> row, legacy_row(c));
    if (!row) {
      ++report.rejected;
      continue;
    }
    if (row->body->size() > kInlineBodyMax) {
      const std::string relative =
          "objects/" + row->content_sha256.substr(0, 2) + "/" + row->content_sha256 + ".json";
      ATX_TRY_VOID(publish_object(dir / fs_path(relative), *row->body));
      row->body_file = relative;
      row->body.reset();
    }
    rows.push_back(std::move(*row));
    if (rows.size() >= kImportSegment) {
      ATX_TRY_VOID(write_rows(cache, rows, report));
    }
  }
  ATX_TRY_VOID(write_rows(cache, rows, report));
  return report;
}

core::Result<i64> prune_cache(Database &cache, const std::filesystem::path &dir) {
  std::vector<std::string> gone;
  {
    ATX_TRY(core::db::Statement stmt,
            cache.prepare("SELECT DISTINCT root FROM record WHERE root <> '' ORDER BY root;"));
    // Bounded by the number of partitions.
    for (;;) {
      ATX_TRY(const core::db::Statement::Step step, stmt.step());
      if (step == core::db::Statement::Step::Done) {
        break;
      }
      ATX_TRY(std::string root, stmt.checked_text(0));
      std::error_code ec;
      if (!std::filesystem::is_directory(dir / fs_path(root), ec)) {
        gone.push_back(std::move(root));
      }
    }
  }
  i64 deleted = 0;
  ATX_TRY_VOID(core::db::with_immediate(cache, [&](Database &d) -> core::Status {
    deleted = 0;
    for (const std::string &root : gone) {
      ATX_TRY_VOID(detail::delete_where(d, "record", "root", root));
      deleted += d.changes();
    }
    return core::Ok();
  }));
  return deleted;
}

} // namespace atx::engine::research::store::catalog
