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

core::Result<LedgerFile> ledger_from_lines(std::string path,
                                           const std::vector<std::string> &physical) {
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
