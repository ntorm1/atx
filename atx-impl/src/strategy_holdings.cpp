#include "strategy_holdings.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <exception>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"

namespace atx::impl::strategy::holdings {
namespace {
using namespace atx;
namespace co = atx::core;
using Json = nlohmann::json;
static_assert(std::endian::native == std::endian::little,
              "holdings.f64 is little-endian binary64: a big-endian host needs a byte swap");
static_assert(std::numeric_limits<f64>::is_iec559);
constexpr usize buffer_values = (1U << 20) / sizeof(f64); // 1 MiB
constexpr u64 row_bytes = row_width * sizeof(f64);
constexpr u64 max_json_bytes = 64ULL << 20;
constexpr u64 max_rows = 1ULL << 40;
constexpr u64 exact_integer_max = 1ULL << 53;
constexpr const char* session_columns =
    "session_index,session_ns,nav_post_bits,decision,first_row,rows";

std::string hex(std::span<const std::byte> bytes) {
  constexpr const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::byte b : bytes) {
    const auto v = std::to_integer<unsigned>(b);
    out.push_back(digits[v >> 4U]); out.push_back(digits[v & 15U]);
  }
  return out;
}
co::Result<std::string> read_capped(const std::filesystem::path& path, const char* what) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() <= 0 || static_cast<u64>(file.tellg()) > max_json_bytes)
    return co::Err(co::ErrorCode::InvalidArgument,
                   std::string("holdings: ") + what + " missing or oversized: " + path.string());
  std::string text(static_cast<usize>(file.tellg()), '\0');
  file.seekg(0);
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file) return co::Err(co::ErrorCode::IoError, std::string("holdings: ") + what + " read");
  return co::Ok(std::move(text));
}
tl::unexpected<co::Error> bad_index(const std::string& why) {
  return co::Err(co::ErrorCode::InvalidArgument, "holdings index: " + why);
}
// An exact non-negative integer below 2^53 stored as f64.
bool exact_index(f64 v, u64 bound, u64& out) {
  if (!(v >= 0) || !(v < static_cast<f64>(std::min(bound, exact_integer_max))) ||
      v != std::floor(v))
    return false;
  out = static_cast<u64>(v);
  return true;
}
co::Result<std::vector<SessionEntry>> parse_sessions(const Json& table, u64 rows) {
  if (!table.is_array() || table.empty())
    return bad_index("sessions must be a non-empty array");
  std::vector<SessionEntry> out;
  out.reserve(table.size());
  u64 next = 0;
  for (const auto& e : table) {
    if (!e.is_array() || e.size() != 6 || !e[0].is_number_unsigned() ||
        !e[1].is_number_integer() || !e[2].is_number_unsigned() || !e[3].is_number_unsigned() ||
        !e[4].is_number_unsigned() || !e[5].is_number_unsigned())
      return bad_index("session entry malformed");
    SessionEntry s;
    s.session_index = e[0].get<usize>(); s.session_ns = e[1].get<i64>();
    s.nav_post = std::bit_cast<f64>(e[2].get<u64>());
    const u64 decision = e[3].get<u64>();
    s.first_row = e[4].get<u64>(); s.rows = e[5].get<u64>();
    if (decision > 1 || s.first_row != next || s.rows > rows - next ||
        !std::isfinite(s.nav_post) || !(s.nav_post > 0) ||
        (!out.empty() && s.session_ns <= out.back().session_ns))
      return bad_index("session table must tile the rows in session order with NAV > 0");
    s.decision = decision != 0;
    next += s.rows;
    out.push_back(s);
  }
  if (next != rows) return bad_index("session table does not cover every row");
  return co::Ok(std::move(out));
}
struct Index {
  std::vector<u64> ids;
  std::vector<SessionEntry> sessions;
  std::string data_sha256;
  u64 rows{}, bytes{};
};
co::Result<Index> parse_index(const std::string& text) {
  const Json j = Json::parse(text);
  if (!j.is_object() || j.value("schema", std::string{}) != index_schema)
    return bad_index(std::string("schema must be ") + index_schema);
  const auto& data = j.at("data");
  Json names = Json::array();
  for (const char* name : column_names) names.push_back(name);
  if (data.at("file") != data_file || data.at("dtype") != "<f8" ||
      data.at("layout") != "row-major" || data.at("row_width") != row_width ||
      data.at("columns") != names)
    return bad_index("data layout differs from this build's");
  Index out;
  out.rows = data.at("rows").get<u64>();
  out.bytes = data.at("bytes").get<u64>();
  out.data_sha256 = data.at("sha256").get<std::string>();
  if (out.rows > max_rows || out.bytes != out.rows * row_bytes || out.data_sha256.size() != 64)
    return bad_index("data rows, bytes or sha256");
  const auto& ids = j.at("instrument_ids");
  if (!ids.is_array() || ids.empty()) return bad_index("instrument_ids");
  out.ids.reserve(ids.size());
  for (const auto& id : ids) {
    if (!id.is_number_unsigned()) return bad_index("instrument id");
    out.ids.push_back(id.get<u64>());
  }
  if (j.at("session_columns") != session_columns) return bad_index("session_columns");
  ATX_TRY(out.sessions, parse_sessions(j.at("sessions"), out.rows));
  return co::Ok(std::move(out));
}
// Resolves `path` to the index text: a directory must carry a complete v2 manifest whose
// files entry binds the index SHA-256.
co::Result<std::pair<std::filesystem::path, std::string>> index_of(const std::string& path) {
  const std::filesystem::path given(path);
  std::error_code ec;
  if (!std::filesystem::is_directory(given, ec)) {
    ATX_TRY(auto text, read_capped(given, "index"));
    return co::Ok(std::pair{given.parent_path(), std::move(text)});
  }
  ATX_TRY(const auto manifest_text, read_capped(given / manifest_file, "manifest.json"));
  const Json m = Json::parse(manifest_text);
  if (!m.is_object() || m.value("schema", std::string{}) != manifest_schema_v2 ||
      m.value("status", std::string{}) != "complete")
    return co::Err(co::ErrorCode::InvalidArgument,
                   std::string("holdings: manifest.json must be ") + manifest_schema_v2 +
                       " with status complete (an f64 holdings directory)");
  ATX_TRY(auto text, read_capped(given / index_file, "index"));
  ATX_TRY(const auto sha, co::sha256_hex(std::string_view{text}));
  const auto files = m.find("files");
  if (files == m.end() || !files->is_object() || !files->contains(index_file) ||
      files->at(index_file) != sha)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "holdings: holdings_index.json differs from the manifest's SHA-256");
  return co::Ok(std::pair{given, std::move(text)});
}
co::Result<NavHolding> unpack(const Row& r, usize session, std::span<const u64> ids,
                              f64 nav_post) {
  const auto bad = co::Err(co::ErrorCode::InvalidArgument, "holdings: row malformed");
  u64 row_session = 0, name = 0, flags = 0;
  if (!exact_index(r[col_session], u64{session} + 1, row_session) || row_session != session ||
      !exact_index(r[col_name], ids.size(), name) ||
      !exact_index(r[col_flags], u64{1} << 32U, flags))
    return bad;
  const auto field = [flags](u32 shift) { return static_cast<u32>((flags >> shift) & 0xFFU); };
  constexpr u64 known = flag_member | flag_stale | flag_order_placed | flag_locate_blocked |
                        flag_order_working | (0xFFULL << tier_shift) |
                        (0xFFULL << tier_missing_shift) | (0xFFULL << fill_shift);
  if ((flags & ~known) != 0 || field(tier_shift) > max_tier || field(tier_missing_shift) > 1U ||
      field(fill_shift) > max_fill)
    return bad;
  NavHolding h;
  h.index = static_cast<usize>(name); h.instrument_id = ids[h.index];
  h.member = (flags & flag_member) != 0; h.stale = (flags & flag_stale) != 0;
  h.tier = static_cast<u8>(field(tier_shift));
  h.tier_missing = static_cast<u8>(field(tier_missing_shift));
  h.held_dollars = r[col_held]; h.held_weight = h.held_dollars / nav_post;
  h.fill = static_cast<NavFillStatus>(field(fill_shift));
  h.filled_dollars = r[col_filled]; h.fill_cost_dollars = r[col_fill_cost];
  h.unfilled_dollars = r[col_unfilled];
  h.desired = r[col_desired]; h.rule_weight = r[col_rule]; h.target_weight = r[col_target];
  h.order_placed = (flags & flag_order_placed) != 0;
  h.locate_blocked = (flags & flag_locate_blocked) != 0;
  h.order_working = (flags & flag_order_working) != 0;
  h.order_dollars = r[col_order];
  return co::Ok(h);
}
co::Result<std::vector<f64>> read_rows(const std::filesystem::path& file, u64 first, u64 rows) {
  std::vector<f64> values(static_cast<usize>(rows * row_width));
  if (!rows) return co::Ok(std::move(values));
  std::ifstream in(file, std::ios::binary);
  in.seekg(static_cast<std::streamoff>(first * row_bytes));
  // SAFETY: f64 is trivially copyable and the file is little-endian binary64 on a
  // little-endian IEEE host (static_asserts above); char aliasing is permitted.
  in.read(reinterpret_cast<char*>(values.data()),
          static_cast<std::streamsize>(values.size() * sizeof(f64)));
  if (!in) return co::Err(co::ErrorCode::IoError, "holdings: holdings.f64 read");
  return co::Ok(std::move(values));
}
} // namespace

Row pack(usize session, const NavHolding& h) noexcept {
  u32 flags = (h.member ? flag_member : 0U) | (h.stale ? flag_stale : 0U) |
              (h.order_placed ? flag_order_placed : 0U) |
              (h.locate_blocked ? flag_locate_blocked : 0U) |
              (h.order_working ? flag_order_working : 0U);
  flags |= u32{h.tier} << tier_shift;
  flags |= u32{h.tier_missing} << tier_missing_shift;
  flags |= u32{static_cast<u8>(h.fill)} << fill_shift;
  return Row{static_cast<f64>(session), static_cast<f64>(h.index), static_cast<f64>(flags),
             h.held_dollars, h.filled_dollars, h.fill_cost_dollars, h.unfilled_dollars,
             h.desired, h.rule_weight, h.target_weight, h.order_dollars};
}

co::Status BinaryAppender::open(const std::filesystem::path& path) {
  file_.open(path, std::ios::binary);
  if (!file_) return co::Err(co::ErrorCode::IoError, "holdings: holdings.f64 output");
  buffer_.reserve(buffer_values);
  return co::Ok();
}
co::Status BinaryAppender::append(const Row& row) {
  if (buffer_.size() + row_width > buffer_values) ATX_TRY_VOID(flush());
  buffer_.insert(buffer_.end(), row.begin(), row.end());
  ++rows_;
  return co::Ok();
}
co::Status BinaryAppender::flush() {
  if (buffer_.empty()) return co::Ok();
  const auto bytes = std::as_bytes(std::span<const f64>(buffer_));
  ATX_TRY_VOID(sha_.update(bytes));
  // SAFETY: ostream::write takes char; the bytes are f64 object representations.
  file_.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  buffer_.clear();
  if (!file_) return co::Err(co::ErrorCode::IoError, "holdings: holdings.f64 write");
  return co::Ok();
}
co::Result<BinaryAppender::Closed> BinaryAppender::close() {
  ATX_TRY_VOID(flush());
  file_.close();
  if (!file_) return co::Err(co::ErrorCode::IoError, "holdings: holdings.f64 close");
  ATX_TRY(const auto digest, sha_.finalize());
  return co::Ok(Closed{hex(digest), rows_ * row_bytes, rows_});
}

co::Result<std::string> write_index(const std::filesystem::path& path,
                                    std::span<const u64> ids,
                                    std::span<const SessionEntry> sessions,
                                    const BinaryAppender::Closed& data) {
  Json names = Json::array();
  for (const char* name : column_names) names.push_back(name);
  Json table = Json::array(), navs = Json::array();
  u64 next = 0;
  for (const auto& s : sessions) {
    if (s.first_row != next)
      return co::Err(co::ErrorCode::Internal, "holdings: session table out of order");
    next += s.rows;
    table.push_back(Json::array({s.session_index, s.session_ns,
                                 std::bit_cast<u64>(s.nav_post), s.decision ? 1U : 0U,
                                 s.first_row, s.rows}));
    navs.push_back(s.nav_post);
  }
  if (next != data.rows)
    return co::Err(co::ErrorCode::Internal, "holdings: session table does not tile the rows");
  const Json index{{"schema", index_schema},
      {"data", {{"file", data_file}, {"dtype", "<f8"}, {"layout", "row-major"},
                {"row_width", row_width}, {"columns", names}, {"rows", data.rows},
                {"bytes", data.bytes}, {"sha256", data.sha256}}},
      {"flags", {{"member", flag_member}, {"stale", flag_stale},
                 {"order_placed", flag_order_placed}, {"locate_blocked", flag_locate_blocked},
                 {"order_working", flag_order_working}, {"tier_shift", tier_shift},
                 {"tier_missing_shift", tier_missing_shift}, {"fill_shift", fill_shift},
                 {"tier_values", "0 none, 1 gc, 2 warm, 3 special"},
                 {"fill_values", "0 none, 1 complete, 2 capped, 3 blocked-absent, "
                                 "4 blocked-liquidity"}}},
      {"derived", "held_weight = held_dollars / nav_post (bit for bit the replay's "
                  "expression); side = sign(held_dollars); planned_trade_weight = "
                  "target_weight - held_weight on decision sessions (else nan); "
                  "planned_trade_dollars = order_dollars - held_dollars when order_placed "
                  "(else 0); session = ordinal into sessions, name = index into "
                  "instrument_ids (the role order)"},
      {"instrument_ids", std::vector<u64>(ids.begin(), ids.end())},
      {"session_columns", session_columns}, {"sessions", table},
      {"nav_post", navs}};
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "holdings: index output");
  const std::string text = index.dump(1) + "\n";
  file << text;
  file.close();
  if (!file) return co::Err(co::ErrorCode::IoError, "holdings: index close");
  return co::sha256_hex(std::string_view{text});
}

co::Result<SessionRead> read_session(const std::string& path, i64 session_ns) {
  try {
    ATX_TRY(const auto located, index_of(path));
    ATX_TRY(auto index, parse_index(located.second));
    const auto data = located.first / data_file;
    std::error_code ec;
    const auto size = std::filesystem::file_size(data, ec);
    if (ec || size != index.bytes)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "holdings: holdings.f64 missing or its size differs from the index");
    ATX_TRY(const auto sha, co::sha256_file(data.string()));
    if (sha != index.data_sha256)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "holdings: holdings.f64 differs from the index's SHA-256");
    const auto& table = index.sessions;
    const auto it = std::lower_bound(table.begin(), table.end(), session_ns,
        [](const SessionEntry& e, i64 key) { return e.session_ns < key; });
    if (it == table.end() || it->session_ns != session_ns)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "holdings: the as-of session is not in the holdings (the replay reports "
                     "decision and execution sessions only)");
    const usize ordinal = static_cast<usize>(it - table.begin());
    ATX_TRY(const auto values, read_rows(data, it->first_row, it->rows));
    SessionRead out;
    out.session = *it;
    out.names.reserve(static_cast<usize>(it->rows));
    for (usize r = 0; r < it->rows; ++r) {
      Row row{};
      std::copy_n(values.begin() + static_cast<std::ptrdiff_t>(r * row_width), row_width,
                  row.begin());
      ATX_TRY(auto h, unpack(row, ordinal, index.ids, it->nav_post));
      if (!out.names.empty() && h.index <= out.names.back().index)
        return co::Err(co::ErrorCode::InvalidArgument,
                       "holdings: names must ascend within a session");
      out.names.push_back(h);
    }
    out.ids = std::move(index.ids);
    return co::Ok(std::move(out));
  } catch (const nlohmann::json::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("holdings: ") + e.what());
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "holdings: allocation failed");
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("holdings: ") + e.what());
  }
}

bool is_f64_layout(const std::string& path) {
  const std::filesystem::path p(path);
  std::error_code ec;
  return std::filesystem::is_directory(p, ec) || p.extension() == ".json";
}
} // namespace atx::impl::strategy::holdings
