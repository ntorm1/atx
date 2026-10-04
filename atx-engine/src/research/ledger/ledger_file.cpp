#include "atx/engine/research/ledger/ledger_file.hpp"

#include <fstream>
#include <ios>
#include <iterator>
#include <system_error>
#include <utility>

#include "json_line.hpp"

namespace atx::engine::research::ledger {

std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> out;
  usize start = 0;
  while (start < text.size()) {
    const usize stop = text.find('\n', start);
    const usize end = stop == std::string_view::npos ? text.size() : stop;
    out.emplace_back(strip_line_end(text.substr(start, end - start)));
    if (stop == std::string_view::npos) {
      break;
    }
    start = stop + 1U;
  }
  return out;
}

namespace {

// Python truthiness of a JSON value (null, false, 0, "", [] and {} are false).
[[nodiscard]] bool truthy(const nlohmann::json &v) {
  if (v.is_null()) {
    return false;
  }
  if (v.is_boolean()) {
    return v.get<bool>();
  }
  if (v.is_number()) {
    return v.get<f64>() != 0.0;
  }
  return !v.empty(); // string, array, object
}

// Review B2 S3: inputs the Python reader refuses and nlohmann would accept, refused here too so
// both read the same ledgers: a UTF-8 BOM at a line start (json.loads refuses it), a raw CR, NEL
// (U+0085), LINE or PARAGRAPH SEPARATOR (U+2028 / U+2029) inside a line (str.splitlines breaks the
// line there).
[[nodiscard]] core::Status python_line_parity(std::string_view line, usize physical_line) {
  const std::string where = "trial ledger: line " + std::to_string(physical_line);
  if (line.starts_with("\xEF\xBB\xBF")) {
    return core::Err(core::ErrorCode::ParseError,
                     where + " starts with a UTF-8 BOM (the Python reader refuses it)");
  }
  for (const std::string_view breaker : {std::string_view("\r"), std::string_view("\xC2\x85"),
                                         std::string_view("\xE2\x80\xA8"),
                                         std::string_view("\xE2\x80\xA9")}) {
    if (line.find(breaker) != std::string_view::npos) {
      return core::Err(core::ErrorCode::ParseError,
                       where + " holds a raw CR, U+0085, U+2028 or U+2029 (Python's splitlines "
                               "breaks the line there)");
    }
  }
  return core::Ok();
}

// The Python's invalid_ids reads (defect or {}).get("invalid"): a truthy defect that is no
// object raises there (review B2 S3).
[[nodiscard]] core::Status python_record_parity(const nlohmann::json &rec, usize physical_line) {
  const auto defect = rec.find("defect");
  if (defect != rec.end() && truthy(*defect) && !defect->is_object()) {
    return core::Err(core::ErrorCode::ParseError,
                     "trial ledger: line " + std::to_string(physical_line) +
                         ": defect is not an object (the Python reader refuses it)");
  }
  return core::Ok();
}

} // namespace

core::Result<LedgerFile> ledger_from_lines(std::string path,
                                           const std::vector<std::string> &physical) {
  for (usize k = 0; k < physical.size(); ++k) {
    if (!is_blank_line(physical[k])) {
      ATX_TRY_VOID(python_line_parity(physical[k], k + 1U));
    }
  }
  ATX_TRY(ChainHead head, verify_chain(physical));
  LedgerFile out;
  out.path = std::move(path);
  out.head = std::move(head);
  for (usize k = 0; k < physical.size(); ++k) {
    if (is_blank_line(physical[k])) {
      continue;
    }
    ATX_TRY(nlohmann::json rec,
            detail::parse_json_line(physical[k], "line " + std::to_string(k + 1U)));
    ATX_TRY_VOID(python_record_parity(rec, k + 1U));
    out.lines.push_back(physical[k]);
    out.records.push_back(std::move(rec));
  }
  return core::Ok(std::move(out));
}

core::Result<LedgerFile> read_ledger(const std::filesystem::path &path, MissingLedger missing) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (missing == MissingLedger::Refuse) {
      return core::Err(core::ErrorCode::NotFound,
                       "trial ledger " + path.string() +
                           " does not exist (N and V are read from it; an absent ledger would "
                           "read as N 0)");
    }
    LedgerFile empty;
    empty.path = path.generic_string();
    return core::Ok(std::move(empty));
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return core::Err(core::ErrorCode::IoError, "trial ledger: cannot open " + path.string());
  }
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (in.bad()) {
    return core::Err(core::ErrorCode::IoError, "trial ledger: cannot read " + path.string());
  }
  return ledger_from_lines(path.generic_string(), split_lines(text));
}

} // namespace atx::engine::research::ledger
