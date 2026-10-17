#include "atx/engine/research/eval/eval_series.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

#include "atx/engine/data/research_window.hpp"

namespace atx::engine::research::eval {
namespace {

constexpr u64 kSeriesLimit = 256ULL << 20;

[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char sep) {
  std::vector<std::string_view> out;
  usize begin = 0;
  while (true) {
    const usize end = text.find(sep, begin);
    if (end == std::string_view::npos) {
      out.push_back(text.substr(begin));
      return out;
    }
    out.push_back(text.substr(begin, end - begin));
    begin = end + 1U;
  }
}

[[nodiscard]] std::string_view strip_cr(std::string_view line) noexcept {
  return (!line.empty() && line.back() == '\r') ? line.substr(0, line.size() - 1U) : line;
}

[[nodiscard]] std::optional<usize> column(const std::vector<std::string_view> &header,
                                          std::string_view name) {
  const auto it = std::find(header.begin(), header.end(), name);
  if (it == header.end()) {
    return std::nullopt;
  }
  return static_cast<usize>(std::distance(header.begin(), it));
}

[[nodiscard]] core::Error invalid(const std::string &path, const std::string &message) {
  return core::Error(core::ErrorCode::InvalidArgument,
                     "atx-research-eval: series " + path + ": " + message);
}

using SessionValue = std::pair<i64, f64>;

[[nodiscard]] std::vector<SessionValue> sorted_pairs(const NetSeries &s) {
  std::vector<SessionValue> out;
  out.reserve(s.sessions.size());
  for (usize k = 0; k < s.sessions.size(); ++k) {
    out.emplace_back(s.sessions[k], s.nets[k]);
  }
  std::sort(out.begin(), out.end(),
            [](const SessionValue &x, const SessionValue &y) { return x.first < y.first; });
  return out;
}

} // namespace

core::Result<NetSeries> parse_net_series(std::string label, std::string path,
                                         std::string_view text) {
  std::vector<std::string_view> lines = split(text, '\n');
  if (lines.empty()) {
    return core::Err(invalid(path, "empty file"));
  }
  const std::vector<std::string_view> header = split(strip_cr(lines.front()), ',');
  const std::optional<usize> k_session = column(header, "session_ns");
  const std::optional<usize> k_net = column(header, "net_return");
  if (!k_session || !k_net) {
    return core::Err(invalid(path, "the header must name session_ns and net_return"));
  }
  const usize width = std::max(*k_session, *k_net) + 1U;
  NetSeries out;
  out.label = std::move(label);
  out.path = std::move(path);
  for (usize row = 1; row < lines.size(); ++row) {
    const std::string_view line = strip_cr(lines[row]);
    if (line.empty()) {
      continue;
    }
    const std::vector<std::string_view> cells = split(line, ',');
    if (cells.size() < width) {
      return core::Err(invalid(out.path, "row " + std::to_string(row + 1U) + " is short"));
    }
    const std::string_view s = cells[*k_session];
    const std::string_view v = cells[*k_net];
    i64 session = 0;
    const auto rs = std::from_chars(s.data(), s.data() + s.size(), session);
    f64 net = 0.0;
    const auto rv = std::from_chars(v.data(), v.data() + v.size(), net);
    if (rs.ec != std::errc{} || rs.ptr != s.data() + s.size() || rv.ec != std::errc{} ||
        rv.ptr != v.data() + v.size()) {
      return core::Err(invalid(out.path, "row " + std::to_string(row + 1U) + " is not numeric"));
    }
    if (atx::engine::data::is_sealed(session)) {
      return core::Err(invalid(out.path, "a session at or after the seal " +
                                             std::string(atx::engine::data::kSealBeginDate) +
                                             " (nothing sealed is read)"));
    }
    out.sessions.push_back(session);
    out.nets.push_back(net);
  }
  std::vector<i64> sorted = out.sessions;
  std::sort(sorted.begin(), sorted.end());
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    return core::Err(invalid(out.path, "a repeated session_ns"));
  }
  return core::Ok(std::move(out));
}

core::Result<NetSeries> read_net_series(std::string label, const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    return core::Err(core::ErrorCode::IoError, "atx-research-eval: cannot open " + path.string());
  }
  const std::streamoff size = in.tellg();
  if (size < 0 || static_cast<u64>(size) > kSeriesLimit) {
    return core::Err(invalid(path.string(), "unreadable or oversized"));
  }
  std::string text(static_cast<usize>(size), '\0');
  in.seekg(0);
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!in) {
    return core::Err(core::ErrorCode::IoError, "atx-research-eval: cannot read " + path.string());
  }
  return parse_net_series(std::move(label), path.string(), text);
}

Aligned align_many(std::span<const NetSeries *const> series) {
  Aligned out;
  out.columns = series.size();
  if (series.empty()) {
    return out;
  }
  std::vector<std::vector<SessionValue>> pairs;
  pairs.reserve(series.size());
  for (const NetSeries *s : series) {
    pairs.push_back(sorted_pairs(*s));
  }
  std::vector<i64> common;
  for (const SessionValue &p : pairs.front()) {
    common.push_back(p.first);
  }
  for (usize k = 1; k < pairs.size(); ++k) {
    std::vector<i64> next;
    for (const SessionValue &p : pairs[k]) {
      if (std::binary_search(common.begin(), common.end(), p.first)) {
        next.push_back(p.first);
      }
    }
    common = std::move(next);
  }
  out.matrix.resize(common.size() * series.size());
  for (usize t = 0; t < common.size(); ++t) {
    for (usize k = 0; k < pairs.size(); ++k) {
      const auto it = std::lower_bound(
          pairs[k].begin(), pairs[k].end(), common[t],
          [](const SessionValue &p, i64 session) { return p.first < session; });
      out.matrix[t * series.size() + k] = it->second;
    }
  }
  out.sessions = std::move(common);
  return out;
}

} // namespace atx::engine::research::eval
