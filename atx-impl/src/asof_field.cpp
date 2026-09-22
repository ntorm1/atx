#include "asof_field.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "atx/engine/data/orats_history.hpp" // detail::date_to_nanos (strict YYYY-MM-DD)

namespace atx::impl {

namespace {

using EC = atx::core::ErrorCode;

constexpr atx::i64 kNanosPerDay = 86400LL * 1000000000LL;
// Bounds an operator-supplied path; FINRA short interest for the full history is
// well under 1 GiB of CSV.
constexpr std::streamoff kMaxAsofBytes = 4LL * 1024LL * 1024LL * 1024LL;
constexpr std::string_view kHeader = "security_id,available_at,value";

[[nodiscard]] atx::i64 floor_div_day(atx::i64 nanos) noexcept {
    atx::i64 q = nanos / kNanosPerDay;
    if (nanos % kNanosPerDay != 0 && nanos < 0) --q;
    return q;
}

[[nodiscard]] bool is_nan_literal(std::string_view s) noexcept {
    if (s.size() != 3) return false;
    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return lower(s[0]) == 'n' && lower(s[1]) == 'a' && lower(s[2]) == 'n';
}

[[nodiscard]] atx::core::Error row_error(atx::usize line_no, std::string_view line,
                                         std::string_view why) {
    std::string shown{line.substr(0, std::min<atx::usize>(line.size(), 120))};
    return atx::core::Error(EC::ParseError, "asof csv: line " + std::to_string(line_no) + ": " +
                                              std::string(why) + ": '" + shown + "'");
}

[[nodiscard]] atx::core::Result<AsofRow> parse_row(std::string_view line, atx::usize line_no) {
    const auto c1 = line.find(',');
    const auto c2 = c1 == std::string_view::npos ? c1 : line.find(',', c1 + 1);
    if (c1 == std::string_view::npos || c2 == std::string_view::npos ||
        line.find(',', c2 + 1) != std::string_view::npos) {
        return atx::core::Err(row_error(line_no, line, "expected exactly 3 fields"));
    }
    const std::string_view id_text = line.substr(0, c1);
    const std::string_view date_text = line.substr(c1 + 1, c2 - c1 - 1);
    const std::string_view value_text = line.substr(c2 + 1);

    AsofRow row;
    const auto id_end = id_text.data() + id_text.size();
    const auto id_r = std::from_chars(id_text.data(), id_end, row.security_id);
    if (id_text.empty() || id_r.ec != std::errc{} || id_r.ptr != id_end || row.security_id <= 0) {
        return atx::core::Err(row_error(line_no, line, "security_id is not a positive i64"));
    }
    const auto ns = atx::engine::data::detail::date_to_nanos(date_text);
    if (!ns.has_value()) {
        return atx::core::Err(row_error(line_no, line, "available_at is not YYYY-MM-DD"));
    }
    row.available_day = *ns / kNanosPerDay; // exact: date_to_nanos returns midnight

    if (value_text.empty() || is_nan_literal(value_text)) {
        row.value = std::numeric_limits<atx::f64>::quiet_NaN();
    } else {
        const auto v_end = value_text.data() + value_text.size();
        const auto v_r = std::from_chars(value_text.data(), v_end, row.value,
                                         std::chars_format::general);
        if (v_r.ec != std::errc{} || v_r.ptr != v_end || !std::isfinite(row.value)) {
            return atx::core::Err(row_error(line_no, line, "value is not a finite decimal"));
        }
    }
    return atx::core::Ok(row);
}

} // namespace

bool is_dsl_identifier(std::string_view name) noexcept {
    if (name.empty()) return false;
    const auto alpha = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    if (!alpha(name.front())) return false;
    return std::all_of(name.begin() + 1, name.end(),
                       [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}

atx::core::Result<AsofTable> parse_asof_csv(std::string_view text) {
    AsofTable table;
    atx::usize line_no = 0;
    std::string_view rest = text;
    bool header_seen = false;
    while (!rest.empty()) {
        const auto nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!header_seen) {
            if (line != kHeader) {
                return atx::core::Err(EC::InvalidArgument,
                    "asof csv: header must be exactly '" + std::string(kHeader) + "'");
            }
            header_seen = true;
            continue;
        }
        ATX_TRY(auto row, parse_row(line, line_no));
        table.rows.push_back(row);
    }
    if (!header_seen) {
        return atx::core::Err(EC::InvalidArgument,
            "asof csv: empty file (header must be exactly '" + std::string(kHeader) + "')");
    }
    std::sort(table.rows.begin(), table.rows.end(), [](const AsofRow &a, const AsofRow &b) {
        return a.security_id != b.security_id ? a.security_id < b.security_id
                                              : a.available_day < b.available_day;
    });
    for (atx::usize i = 1; i < table.rows.size(); ++i) {
        const AsofRow &a = table.rows[i - 1];
        const AsofRow &b = table.rows[i];
        if (a.security_id == b.security_id && a.available_day == b.available_day) {
            return atx::core::Err(EC::InvalidArgument,
                "asof csv: duplicate (security_id, available_at) for security_id " +
                    std::to_string(a.security_id) + " at day " +
                    std::to_string(a.available_day) + " since 1970-01-01");
        }
    }
    return atx::core::Ok(std::move(table));
}

atx::core::Result<std::string> read_asof_bytes(const std::string &path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return atx::core::Err(EC::IoError, "asof csv: cannot open: " + path);
    }
    const std::streamoff size = in.tellg();
    if (size < 0 || size > kMaxAsofBytes) {
        return atx::core::Err(EC::InvalidArgument,
            "asof csv: missing or implausibly large: " + path);
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.seekg(0, std::ios::beg);
    if (size > 0 && !in.read(bytes.data(), static_cast<std::streamsize>(size))) {
        return atx::core::Err(EC::IoError, "asof csv: cannot read: " + path);
    }
    return atx::core::Ok(std::move(bytes));
}

atx::core::Result<AsofColumn>
build_asof_column(const AsofTable &table, std::span<const atx::i64> session_keys,
                  std::span<const std::string> instrument_ids, atx::i64 max_stale_days) {
    if (max_stale_days < 0) {
        return atx::core::Err(EC::InvalidArgument, "asof: max_stale_days must be >= 0");
    }
    const atx::usize n_dates = session_keys.size();
    const atx::usize n_inst = instrument_ids.size();

    std::unordered_map<atx::i64, atx::usize> column_of;
    column_of.reserve(n_inst);
    for (atx::usize n = 0; n < n_inst; ++n) {
        const std::string &text = instrument_ids[n];
        atx::i64 id = 0;
        const auto end = text.data() + text.size();
        const auto r = std::from_chars(text.data(), end, id);
        if (r.ec != std::errc{} || r.ptr != end || id <= 0) {
            return atx::core::Err(EC::InvalidArgument,
                "asof: panel instrument id is not a positive i64: '" + text + "'");
        }
        column_of.emplace(id, n);
    }

    std::vector<atx::i64> session_day(n_dates);
    for (atx::usize d = 0; d < n_dates; ++d) {
        if (d > 0 && session_keys[d] <= session_keys[d - 1]) {
            return atx::core::Err(EC::InvalidArgument, "asof: session keys not increasing");
        }
        session_day[d] = floor_div_day(session_keys[d]);
    }

    AsofColumn out;
    out.values.assign(n_dates * n_inst, std::numeric_limits<atx::f64>::quiet_NaN());

    const std::vector<AsofRow> &rows = table.rows;
    atx::usize begin = 0;
    while (begin < rows.size()) {
        // [begin, end) is one security's run, ascending in available_day.
        atx::usize end = begin + 1;
        while (end < rows.size() && rows[end].security_id == rows[begin].security_id) ++end;
        const auto it = column_of.find(rows[begin].security_id);
        if (it == column_of.end()) {
            out.rows_ignored_unknown_id += end - begin;
            begin = end;
            continue;
        }
        out.rows_matched += end - begin;
        const atx::usize n = it->second;
        // Two-pointer forward fill: k is one past the latest row with
        // available_day < session_day[d] (STRICT — see the join rule in the header).
        atx::usize k = begin;
        for (atx::usize d = 0; d < n_dates; ++d) {
            while (k < end && rows[k].available_day < session_day[d]) ++k;
            if (k == begin) continue; // nothing visible yet -> NaN
            const AsofRow &row = rows[k - 1];
            const atx::i64 age = session_day[d] - row.available_day; // >= 1
            if (max_stale_days > 0 && age > max_stale_days) continue; // stale -> NaN
            out.values[d * n_inst + n] = row.value;
        }
        begin = end;
    }
    return atx::core::Ok(std::move(out));
}

} // namespace atx::impl
