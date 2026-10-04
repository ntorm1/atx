#pragma once

// atx::engine::research::eval -- the net series the eval verb reads (platform P9 B2, K-P9-5).
// A series file is a CSV whose header names the columns session_ns and net_return (other
// columns ignored); each row is one return session: session_ns an integer (ns since the
// epoch, UTC), net_return a decimal float (Python repr; nan / inf accepted). nav_summ.py writes
// them from its net_series (the return rows of a NAV daily CSV), so row order is the NAV's.
// A session at or after the research seal (data/research_window.hpp) refuses the file before
// a statistic is formed; a repeated session is refused.
//
// Alignment is the Python's: align (paired) and align_many (ONC, PBO) keep the sessions every
// series has, ascending.

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::eval {

struct NetSeries {
  std::string label;
  std::string path;
  std::vector<i64> sessions; // file order
  std::vector<f64> nets;     // file order
};

// Parse a series text. InvalidArgument on a missing column, a malformed number, a sealed or
// repeated session.
[[nodiscard]] core::Result<NetSeries> parse_net_series(std::string label, std::string path,
                                                       std::string_view text);
// Read one series file. IoError when it cannot be read.
[[nodiscard]] core::Result<NetSeries> read_net_series(std::string label,
                                                      const std::filesystem::path &path);

struct Aligned {
  std::vector<i64> sessions; // common sessions, ascending
  std::vector<f64> matrix;   // sessions x series, date-major
  usize columns{};
};

// The common sessions of the series and their values (backtest_integrity.align_many).
[[nodiscard]] Aligned align_many(std::span<const NetSeries *const> series);

} // namespace atx::engine::research::eval
