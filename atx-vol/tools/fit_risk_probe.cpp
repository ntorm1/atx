// fit_risk_probe.cpp — the engine-linked core of atx-vol-fit-risk-probe (lane F, wave 3).
// Contract: tools/fit_risk_probe.hpp. Also compiled into atx-vol-tests (FitRiskProbe.*).

#include "fit_risk_probe.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/marketdata/spiderrock_store.hpp" // kSpiderRockVegaScale
#include "atx/vol/api/pricing/american.hpp"            // al_fast_profile
#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "atx/vol/api/risk/market_state.hpp"
#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_frame.hpp"
#include "atx/vol/api/risk/risk_kernel.hpp"

namespace atx::vol::fit_probe {

using atx::core::Err;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t kMaxJsonDepth = 64;

// ── CSV ────────────────────────────────────────────────────────────────────

// One record split into fields. A quoted field with doubled quotes is unescaped into
// `escaped` (a deque, so earlier views stay valid); every other field is a view into
// the line itself.
class CsvRecord {
public:
  [[nodiscard]] bool split(std::string_view line) {
    fields_.clear();
    escaped_.clear();
    std::size_t pos = 0;
    // Bounded: every iteration consumes at least one character or ends the record.
    while (true) {
      if (pos < line.size() && line[pos] == '"') {
        const std::size_t start = pos + 1;
        std::size_t end = start;
        bool doubled = false;
        while (end < line.size()) {
          if (line[end] == '"') {
            if (end + 1 < line.size() && line[end + 1] == '"') {
              doubled = true;
              end += 2;
              continue;
            }
            break;
          }
          ++end;
        }
        if (end >= line.size()) return false; // unterminated quote
        std::string_view body = line.substr(start, end - start);
        if (doubled) {
          std::string &out = escaped_.emplace_back();
          for (std::size_t i = 0; i < body.size(); ++i) {
            out.push_back(body[i]);
            if (body[i] == '"') ++i;
          }
          body = out;
        }
        fields_.push_back(body);
        pos = end + 1;
        if (pos == line.size()) return true;
        if (line[pos] != ',') return false;
        ++pos;
        continue;
      }
      const std::size_t comma = line.find(',', pos);
      if (comma == std::string_view::npos) {
        fields_.push_back(line.substr(pos));
        return true;
      }
      fields_.push_back(line.substr(pos, comma - pos));
      pos = comma + 1;
    }
  }
  [[nodiscard]] std::size_t size() const noexcept { return fields_.size(); }
  [[nodiscard]] std::string_view operator[](std::size_t i) const noexcept { return fields_[i]; }

private:
  std::vector<std::string_view> fields_;
  std::deque<std::string> escaped_;
};

// Iterates the lines of a CSV text (LF or CRLF), skipping a trailing empty line.
class LineReader {
public:
  explicit LineReader(std::string_view text) noexcept : text_(text) {}
  [[nodiscard]] bool next(std::string_view &line) noexcept {
    if (pos_ >= text_.size()) return false;
    const std::size_t nl = text_.find('\n', pos_);
    const std::size_t end = nl == std::string_view::npos ? text_.size() : nl;
    line = text_.substr(pos_, end - pos_);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    pos_ = end + 1;
    ++number_;
    return true;
  }
  [[nodiscard]] std::size_t number() const noexcept { return number_; }

private:
  std::string_view text_;
  std::size_t pos_{0};
  std::size_t number_{0};
};

class CsvTable {
public:
  // Reads the header and resolves `columns` by name. ParseError on a missing column.
  [[nodiscard]] static Result<CsvTable> open(std::string_view text,
                                             std::span<const std::string_view> columns) {
    CsvTable table(text);
    std::string_view header;
    if (!table.lines_.next(header)) return Err(ErrorCode::ParseError, "csv: empty input");
    if (!table.record_.split(header)) return Err(ErrorCode::ParseError, "csv: bad header");
    table.width_ = table.record_.size();
    for (const std::string_view name : columns) {
      std::size_t found = table.width_;
      for (std::size_t i = 0; i < table.width_; ++i) {
        if (table.record_[i] == name) {
          found = i;
          break;
        }
      }
      if (found == table.width_) {
        return Err(ErrorCode::ParseError, "csv: missing column " + std::string(name));
      }
      table.index_.push_back(found);
    }
    return table;
  }
  // Advances to the next non-empty record. ParseError on a ragged record.
  [[nodiscard]] Result<bool> next() {
    std::string_view line;
    while (lines_.next(line)) {
      if (line.empty()) continue;
      if (!record_.split(line) || record_.size() != width_) {
        return Err(ErrorCode::ParseError,
                   "csv: malformed record at line " + std::to_string(lines_.number()));
      }
      return true;
    }
    return false;
  }
  // The field of requested column `k` (in `columns` order).
  [[nodiscard]] std::string_view field(std::size_t k) const noexcept {
    return record_[index_[k]];
  }
  [[nodiscard]] std::size_t line() const noexcept { return lines_.number(); }

private:
  explicit CsvTable(std::string_view text) noexcept : lines_(text) {}
  LineReader lines_;
  CsvRecord record_;
  std::size_t width_{0};
  std::vector<std::size_t> index_;
};

[[nodiscard]] Error parse_error(const CsvTable &t, std::string_view what) {
  return Error(ErrorCode::ParseError,
               "csv: bad " + std::string(what) + " at line " + std::to_string(t.line()));
}

template <class T> [[nodiscard]] bool parse_integer(std::string_view s, T &out) noexcept {
  if (s.empty()) return false;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && end == s.data() + s.size();
}

// A blank field is NaN (the adapter writes nulls blank); anything else must parse whole.
[[nodiscard]] bool parse_real(std::string_view s, double &out) noexcept {
  if (s.empty()) {
    out = kNaN;
    return true;
  }
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && end == s.data() + s.size();
}

template <class Row>
[[nodiscard]] Status check_unique_rows(const std::vector<Row> &rows) {
  std::vector<std::uint64_t> ids;
  ids.reserve(rows.size());
  for (const Row &r : rows) ids.push_back(r.row_id);
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
    return Err(ErrorCode::InvalidArgument, "csv: duplicate row_id");
  }
  return atx::core::Ok();
}

// ── JSON (reader for manifests and the round trip; writer for the report) ─────

struct JsonValue {
  enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
  Kind kind{Kind::Null};
  bool boolean{false};
  std::string text; // a number's raw token, or a string's decoded value
  std::vector<JsonValue> items;
  std::vector<std::pair<std::string, JsonValue>> members;

  [[nodiscard]] const JsonValue *find(std::string_view key) const noexcept {
    for (const auto &[name, value] : members) {
      if (name == key) return &value;
    }
    return nullptr;
  }
};

class JsonParser {
public:
  explicit JsonParser(std::string_view text) noexcept : s_(text) {}

  [[nodiscard]] Result<JsonValue> parse_document() {
    JsonValue value;
    ATX_TRY_VOID(parse_value(value, 0));
    skip_ws();
    if (pos_ != s_.size()) return Err(ErrorCode::ParseError, "json: trailing characters");
    return value;
  }

private:
  void skip_ws() noexcept {
    while (pos_ < s_.size() &&
           (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) {
      ++pos_;
    }
  }
  [[nodiscard]] Error fail(std::string_view what) const {
    return Error(ErrorCode::ParseError,
                 "json: " + std::string(what) + " at offset " + std::to_string(pos_));
  }
  [[nodiscard]] bool literal(std::string_view word) noexcept {
    if (s_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
  }

  [[nodiscard]] Status parse_value(JsonValue &out, std::size_t depth) {
    if (depth > kMaxJsonDepth) return Err(fail("nesting too deep"));
    skip_ws();
    if (pos_ >= s_.size()) return Err(fail("unexpected end"));
    const char c = s_[pos_];
    if (c == '{') return parse_object(out, depth);
    if (c == '[') return parse_array(out, depth);
    if (c == '"') {
      out.kind = JsonValue::Kind::String;
      return parse_string(out.text);
    }
    if (literal("null")) {
      out.kind = JsonValue::Kind::Null;
      return atx::core::Ok();
    }
    if (literal("true")) {
      out.kind = JsonValue::Kind::Bool;
      out.boolean = true;
      return atx::core::Ok();
    }
    if (literal("false")) {
      out.kind = JsonValue::Kind::Bool;
      out.boolean = false;
      return atx::core::Ok();
    }
    return parse_number(out);
  }

  [[nodiscard]] Status parse_number(JsonValue &out) {
    const std::size_t start = pos_;
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
        ++pos_;
      } else {
        break;
      }
    }
    if (pos_ == start) return Err(fail("unexpected character"));
    out.kind = JsonValue::Kind::Number;
    out.text = std::string(s_.substr(start, pos_ - start));
    double check = 0.0;
    const auto [end, ec] = std::from_chars(out.text.data(), out.text.data() + out.text.size(),
                                           check);
    if (ec != std::errc{} && ec != std::errc::result_out_of_range) return Err(fail("bad number"));
    if (end != out.text.data() + out.text.size()) return Err(fail("bad number"));
    return atx::core::Ok();
  }

  static void append_utf8(std::string &out, std::uint32_t cp) {
    if (cp < 0x80u) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
      out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
      out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
  }

  [[nodiscard]] bool hex4(std::uint32_t &out) noexcept {
    if (pos_ + 4 > s_.size()) return false;
    const auto [end, ec] = std::from_chars(s_.data() + pos_, s_.data() + pos_ + 4, out, 16);
    if (ec != std::errc{} || end != s_.data() + pos_ + 4) return false;
    pos_ += 4;
    return true;
  }

  [[nodiscard]] Status parse_string(std::string &out) {
    ++pos_; // opening quote
    out.clear();
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"') return atx::core::Ok();
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) break;
      const char e = s_[pos_++];
      switch (e) {
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case '/': out.push_back('/'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'u': {
        std::uint32_t cp = 0;
        if (!hex4(cp)) return Err(fail("bad \\u escape"));
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
          std::uint32_t low = 0;
          if (!literal("\\u") || !hex4(low) || low < 0xDC00u || low > 0xDFFFu) {
            return Err(fail("bad surrogate pair"));
          }
          cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
        }
        append_utf8(out, cp);
        break;
      }
      default: return Err(fail("bad escape"));
      }
    }
    return Err(fail("unterminated string"));
  }

  [[nodiscard]] Status parse_array(JsonValue &out, std::size_t depth) {
    out.kind = JsonValue::Kind::Array;
    ++pos_;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      return atx::core::Ok();
    }
    while (pos_ < s_.size()) {
      ATX_TRY_VOID(parse_value(out.items.emplace_back(), depth + 1));
      skip_ws();
      if (pos_ >= s_.size()) break;
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == ']') {
        ++pos_;
        return atx::core::Ok();
      }
      return Err(fail("expected , or ]"));
    }
    return Err(fail("unterminated array"));
  }

  [[nodiscard]] Status parse_object(JsonValue &out, std::size_t depth) {
    out.kind = JsonValue::Kind::Object;
    ++pos_;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      return atx::core::Ok();
    }
    while (pos_ < s_.size()) {
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != '"') return Err(fail("expected key"));
      auto &member = out.members.emplace_back();
      ATX_TRY_VOID(parse_string(member.first));
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != ':') return Err(fail("expected :"));
      ++pos_;
      ATX_TRY_VOID(parse_value(member.second, depth + 1));
      skip_ws();
      if (pos_ >= s_.size()) break;
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == '}') {
        ++pos_;
        return atx::core::Ok();
      }
      return Err(fail("expected , or }"));
    }
    return Err(fail("unterminated object"));
  }

  std::string_view s_;
  std::size_t pos_{0};
};

class JsonWriter {
public:
  void begin_object() { open('{'); }
  void end_object() { close('}'); }
  void begin_array() { open('['); }
  void end_array() { close(']'); }
  void key(std::string_view name) {
    comma();
    string_token(name);
    out_.push_back(':');
    pending_value_ = true;
  }
  void value(std::string_view v) {
    comma();
    string_token(v);
  }
  void value(bool v) {
    comma();
    out_ += v ? "true" : "false";
  }
  void value(std::uint64_t v) {
    comma();
    std::array<char, 32> buf{};
    const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    static_cast<void>(ec);
    out_.append(buf.data(), end);
  }
  void value(std::int64_t v) {
    comma();
    std::array<char, 32> buf{};
    const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    static_cast<void>(ec);
    out_.append(buf.data(), end);
  }
  // Shortest round-trip representation; non-finite is null (JSON has no NaN).
  void value(double v) {
    comma();
    if (!std::isfinite(v)) {
      out_ += "null";
      return;
    }
    std::array<char, 64> buf{};
    const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    static_cast<void>(ec);
    out_.append(buf.data(), end);
  }
  template <class T> void field(std::string_view name, T v) {
    key(name);
    value(v);
  }
  [[nodiscard]] std::string take() { return std::move(out_); }

private:
  void open(char c) {
    comma();
    out_.push_back(c);
    first_ = true;
  }
  void close(char c) {
    out_.push_back(c);
    first_ = false;
  }
  void comma() {
    if (pending_value_) {
      pending_value_ = false;
      return;
    }
    if (!first_) out_.push_back(',');
    first_ = false;
  }
  void string_token(std::string_view v) {
    out_.push_back('"');
    for (const char c : v) {
      switch (c) {
      case '"': out_ += "\\\""; break;
      case '\\': out_ += "\\\\"; break;
      case '\n': out_ += "\\n"; break;
      case '\r': out_ += "\\r"; break;
      case '\t': out_ += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20u) {
          std::array<char, 8> buf{};
          const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(),
                                               static_cast<unsigned>(c), 16);
          static_cast<void>(ec);
          out_ += "\\u";
          out_.append(static_cast<std::size_t>(4 - (end - buf.data())), '0');
          out_.append(buf.data(), end);
        } else {
          out_.push_back(c);
        }
      }
    }
    out_.push_back('"');
  }
  std::string out_;
  bool first_{true};
  bool pending_value_{false};
};

// ── Shared names ────────────────────────────────────────────────────────────

constexpr std::array<std::string_view, kRowStateCount> kStateNames{
    "not_served", "price_only", "invalid_input", "evaluated"};
constexpr std::array<std::string_view, kRouteCount> kRouteNames{
    "none", "family", "cold_analytic", "cold_fd", "exercised", "share", "refused"};
constexpr std::array<std::string_view, kRiskLaneStatusCount> kLaneNames{
    "ok", "model_unavailable", "numeric_error", "invalid_contract", "rolled", "refused"};
constexpr std::array<std::string_view, kSanityKinds> kSanityNames{
    "delta_range", "gamma_neg", "vega_neg", "cp_delta"};

[[nodiscard]] const char *dividend_name(QuoteSliceDividendModel model) noexcept {
  switch (model) {
  case QuoteSliceDividendModel::ContinuousYield: return "continuous_yield";
  case QuoteSliceDividendModel::AggregatePresentValue: return "aggregate_pv_approximation";
  case QuoteSliceDividendModel::AggregateFundingPresentValue:
    return "aggregate_funding_pv_approximation";
  case QuoteSliceDividendModel::DatedCash: return "dated_cash";
  }
  return "unknown";
}

// ── Market reconstruction ───────────────────────────────────────────────────

// quote_bench_main.cpp `median`: NaN when any value is non-finite; even counts average the
// two middle values as 0.5 * a + 0.5 * b (that exact expression).
[[nodiscard]] double median(std::vector<double> values) {
  if (values.empty()) return kNaN;
  if (!std::all_of(values.begin(), values.end(), [](double x) { return std::isfinite(x); })) {
    return kNaN;
  }
  std::sort(values.begin(), values.end());
  const std::size_t mid = values.size() / 2U;
  return values.size() % 2U ? values[mid] : 0.5 * values[mid - 1U] + 0.5 * values[mid];
}

// The quote bench fits a row iff its adapter status is ok, its root carries no adjusted
// numeric suffix and it is American (quote_bench_main.cpp convert_row).
[[nodiscard]] bool fit_eligible(const QuoteRow &q) noexcept {
  if (q.input_status != "ok") return false;
  if (!q.root.empty() && q.root.back() >= '1' && q.root.back() <= '9') return false;
  return q.exercise == "American";
}

struct ChainBasis {
  bool computed{false}; // the chain has eligible rows
  bool valid{false};
  std::string label{"none"};
  QuoteSliceMarket market{};
  double rate_v{kNaN};
  std::int64_t base_expiry_ns{0};
};

struct ChainInfo {
  std::uint64_t chain_id{0};
  std::size_t first_row{0};
  std::vector<std::size_t> rows; // every quote row of the chain, input order
  ChainBasis basis{};
};

[[nodiscard]] bool market_ok(const QuoteSliceMarket &m) noexcept {
  return std::isfinite(m.spot) && m.spot > 0.0 && std::isfinite(m.calendar_years) &&
         m.calendar_years > 0.0 && std::isfinite(m.variance_years) && m.variance_years > 0.0 &&
         std::isfinite(m.rate) && std::isfinite(m.continuous_yield) &&
         std::isfinite(m.dividend_pv) && m.dividend_pv >= 0.0;
}

// Rebuilds one chain's fit basis exactly as the quote bench did: medians over the eligible
// rows, then (snapshot.one_spot) the forward-preserving rebase onto S* with every contract
// yield shifted and coalesced. Fills the per-row calendar yields of the eligible rows.
void build_basis(ChainInfo &chain, std::span<const QuoteRow> quotes,
                 const std::unordered_map<std::uint64_t, double> &snapshot_spots,
                 const ProbeOptions &options, std::vector<double> &calendar_yield) {
  std::vector<std::size_t> eligible;
  for (const std::size_t i : chain.rows) {
    if (fit_eligible(quotes[i])) eligible.push_back(i);
  }
  ChainBasis &b = chain.basis;
  if (eligible.empty()) return;
  b.computed = true;
  const auto column = [&](double QuoteRow::*member) {
    std::vector<double> values;
    values.reserve(eligible.size());
    for (const std::size_t i : eligible) values.push_back(quotes[i].*member);
    return median(std::move(values));
  };
  QuoteSliceMarket m;
  m.spot = column(&QuoteRow::spot);
  m.variance_years = column(&QuoteRow::years);
  m.calendar_years = column(&QuoteRow::years_calendar);
  m.rate = column(&QuoteRow::rate);
  m.continuous_yield = column(&QuoteRow::sdiv);
  m.dividend_pv = column(&QuoteRow::ddiv_pv);
  if (!market_ok(m)) {
    b.label = "invalid:market";
    return;
  }
  std::vector<QuoteSliceQuote> fit_quotes;
  fit_quotes.reserve(eligible.size());
  for (const std::size_t i : eligible) {
    const QuoteRow &q = quotes[i];
    QuoteSliceQuote fq{q.strike, q.is_call ? Side::Call : Side::Put, q.bid, q.ask};
    fq.continuous_yield = q.sdiv;
    fit_quotes.push_back(fq);
  }
  b.label = "own";
  if (const auto it = snapshot_spots.find(chain.chain_id); it != snapshot_spots.end()) {
    const auto rebased = rebase_quote_slice_spot(m, it->second, options.dividend_model);
    if (!rebased) {
      b.label = "invalid:rebase";
      return;
    }
    m = rebased->market;
    for (auto &fq : fit_quotes) {
      if (fq.continuous_yield) *fq.continuous_yield += rebased->yield_shift;
    }
    if (options.carry_price_tolerance > 0.0) {
      const auto coalesced = coalesce_quote_slice_carry(m, fit_quotes, options.dividend_model,
                                                        options.carry_price_tolerance);
      if (!coalesced) {
        b.label = "invalid:carry_conflict";
        return;
      }
    }
    b.label = "snapshot_spot";
  }
  for (std::size_t k = 0; k < eligible.size(); ++k) {
    calendar_yield[eligible[k]] = fit_quotes[k].continuous_yield.value_or(m.continuous_yield);
  }
  b.market = m;
  b.rate_v = shadow_rate(m);
  b.valid = std::isfinite(b.rate_v);
  if (!b.valid) b.label = "invalid:rate";
}

// ── Evaluation ──────────────────────────────────────────────────────────────

struct Unit {
  std::vector<std::size_t> chains; // indices into the chain table
};

struct Inputs {
  std::span<const QuoteRow> quotes;
  std::span<const PredictionRow> predictions;
  std::vector<std::ptrdiff_t> prediction_of; // per quote row, -1 when absent
  std::vector<std::ptrdiff_t> label_of;      // per quote row, -1 when absent
  std::span<const LabelRow> labels;
  std::vector<double> calendar_yield;        // per quote row; NaN unless eligible
  std::vector<ChainInfo> chains;
  std::vector<std::size_t> chain_of;         // per quote row
};

[[nodiscard]] std::optional<AlOpts> probe_al_opts() noexcept {
  // The fitter prices on the ACCURATE preset (QuoteSliceConfig::al_opts = nullopt); the
  // engine reads nullopt as "unset", so ask for the same scheme on the Fast profile, the
  // rounding-level twin of the accurate preset (|dP| <= 1e-12 max(P, 1), american.hpp).
  return al_fast_profile(std::nullopt);
}

// Classifies the served rows of one chain into ProbeRows (state, shadow basis), and lists
// the evaluable ones as legs; the engine fills prices later.
struct Leg {
  std::size_t row{0};
  std::uint32_t uid{0};
  std::int64_t expiry_ns{0};
};

void classify_chain(const Inputs &in, std::size_t chain_index, std::uint32_t uid,
                    const ProbeOptions &options, std::span<ProbeRow> out,
                    std::vector<Leg> &legs, std::vector<ExpiryCarry> &carry) {
  const ChainInfo &chain = in.chains[chain_index];
  // Distinct variance-clock yields of the chain -> +j ns expiry offsets (sorted by bits so
  // the offset is a function of the chain's own data, never of row order).
  std::map<std::uint64_t, std::int64_t> offset_of_q;
  for (const std::size_t i : chain.rows) {
    ProbeRow &row = out[i];
    const QuoteRow &q = in.quotes[i];
    row.row_id = q.row_id;
    row.chain_id = q.chain_id;
    row.side = q.is_call ? Side::Call : Side::Put;
    row.strike = q.strike;
    row.sigma = kNaN;
    row.predicted_price = kNaN;
    row.price = row.delta = row.gamma = row.vega = row.theta = kNaN;
    row.identity_price = row.reprice_abs = row.reprice_vol_bp = kNaN;
    row.T = row.rate = row.q = kNaN;
    row.vendor_delta = row.vendor_vega = kNaN;
    if (in.label_of[i] >= 0) {
      const LabelRow &l = in.labels[static_cast<std::size_t>(in.label_of[i])];
      row.vendor_delta = l.vendor_delta;
      row.vendor_vega = l.vendor_vega / kSpiderRockVegaScale;
    }
    row.state = RowState::NotServed;
    if (in.prediction_of[i] < 0) continue;
    const PredictionRow &p = in.predictions[static_cast<std::size_t>(in.prediction_of[i])];
    row.predicted_price = p.price;
    if (p.status == "ok_price_only") {
      row.state = RowState::PriceOnly;
      continue;
    }
    if (p.status != "ok") continue;
    row.sigma = p.vol;
    const bool vol_ok = std::isfinite(p.vol) && p.vol > 0.0;
    if (!chain.basis.valid || !vol_ok || !std::isfinite(in.calendar_yield[i])) {
      row.state = RowState::InvalidInput;
      continue;
    }
    const double qv = shadow_yield(chain.basis.market, in.calendar_yield[i],
                                   options.dividend_model);
    if (!std::isfinite(qv)) {
      row.state = RowState::InvalidInput;
      continue;
    }
    row.q = qv;
    row.rate = chain.basis.rate_v;
    row.state = RowState::Evaluated;
    offset_of_q.emplace(std::bit_cast<std::uint64_t>(qv), 0);
  }
  std::int64_t next = 0;
  for (auto &[bits, offset] : offset_of_q) {
    offset = next++;
    carry.push_back(ExpiryCarry{chain.basis.base_expiry_ns + offset, 0.0, chain.basis.rate_v,
                                std::bit_cast<double>(bits)});
  }
  for (const std::size_t i : chain.rows) {
    if (out[i].state != RowState::Evaluated) continue;
    const std::int64_t expiry =
        chain.basis.base_expiry_ns + offset_of_q.at(std::bit_cast<std::uint64_t>(out[i].q));
    legs.push_back(Leg{i, uid, expiry});
  }
}

void mark_engine_failure(const Inputs &in, const Unit &unit, std::span<ProbeRow> out) {
  for (const std::size_t c : unit.chains) {
    for (const std::size_t i : in.chains[c].rows) {
      if (out[i].state == RowState::Evaluated) out[i].state = RowState::InvalidInput;
    }
  }
}

// One underlier: every chain is its own synthetic uid on the shadow clock.
void evaluate_unit(const Inputs &in, const Unit &unit, const ProbeOptions &options,
                   std::span<ProbeRow> out) {
  MarketStateStorage storage;
  storage.ts_ns = in.quotes[in.chains[unit.chains.front()].first_row].snapshot_ns;
  storage.time = TimeSpec{};
  storage.term_carries.reserve(unit.chains.size());
  std::vector<Leg> legs;
  std::vector<std::uint32_t> uids;
  for (std::size_t k = 0; k < unit.chains.size(); ++k) {
    const std::size_t c = unit.chains[k];
    const auto uid = static_cast<std::uint32_t>(k);
    std::vector<ExpiryCarry> &carry = storage.term_carries.emplace_back();
    classify_chain(in, c, uid, options, out, legs, carry);
    uids.push_back(uid);
  }
  if (legs.empty()) return;
  for (std::size_t k = 0; k < unit.chains.size(); ++k) {
    const ChainBasis &b = in.chains[unit.chains[k]].basis;
    UnderlierState u;
    u.uid = uids[k];
    u.spot = b.valid ? b.market.spot : 1.0; // an invalid chain has no legs
    u.flat_rate = b.valid ? b.rate_v : 0.0;
    u.term_carry = storage.term_carries[k];
    storage.underliers.push_back(u);
  }
  std::vector<OptionLeg> options_legs;
  options_legs.reserve(legs.size());
  for (std::size_t l = 0; l < legs.size(); ++l) {
    const QuoteRow &q = in.quotes[legs[l].row];
    options_legs.push_back(OptionLeg{static_cast<std::uint64_t>(l), legs[l].uid, q.strike,
                                     legs[l].expiry_ns, q.is_call ? Side::Call : Side::Put, 1.0,
                                     100.0});
  }
  // Marks: the fitted vol per contract (resolve reads marks by exact key).
  storage.marks.resize(unit.chains.size());
  for (std::size_t k = 0; k < unit.chains.size(); ++k) storage.marks[k].uid = uids[k];
  for (std::size_t l = 0; l < legs.size(); ++l) {
    auto &m = storage.marks[legs[l].uid];
    m.strike.push_back(options_legs[l].K);
    m.expiry_ns.push_back(legs[l].expiry_ns);
    m.side.push_back(options_legs[l].side);
    m.sigma.push_back(out[legs[l].row].sigma);
  }
  storage.normalize_marks();
  auto book = RiskBook::create(options_legs, {});
  if (!book) return mark_engine_failure(in, unit, out);
  auto prepared = PreparedRiskBook::create(*book);
  if (!prepared) return mark_engine_failure(in, unit, out);
  const auto state = make_market_state(storage);
  if (!state) return mark_engine_failure(in, unit, out);
  const RiskNeeds needs{.second_order = false, .rho = false, .charm = false};
  const bool run_family = options.kernel != ProbeKernel::Cold;
  const bool run_cold = options.kernel != ProbeKernel::Family;
  RiskFrame family_frame;
  RiskFrame cold_frame;
  if (run_family) {
    FamilyKernelPolicy policy;
    policy.al_opts = probe_al_opts();
    policy.n_threads = 1;
    RiskWorkspace ws;
    if (!RiskKernel::family(policy).evaluate(*prepared, *state, needs, family_frame, ws)) {
      return mark_engine_failure(in, unit, out);
    }
  }
  if (run_cold) {
    ColdKernelPolicy policy;
    policy.al_opts = probe_al_opts();
    policy.n_threads = 1;
    RiskWorkspace ws;
    if (!RiskKernel::cold(policy).evaluate(*prepared, *state, needs, cold_frame, ws)) {
      return mark_engine_failure(in, unit, out);
    }
  }
  const RiskFrame &primary = run_family ? family_frame : cold_frame;
  const RiskFrame &identity = run_cold ? cold_frame : family_frame;
  const auto leg_row = prepared->leg_prepared_row();
  for (std::size_t l = 0; l < legs.size(); ++l) {
    const std::size_t r = leg_row[l];
    ProbeRow &row = out[legs[l].row];
    row.route = primary.route[r];
    row.flags = primary.flags[r];
    row.status = primary.status[r];
    row.T = primary.T[r];
    if (row.status == RiskLaneStatus::Ok) {
      row.price = primary.price[r];
      row.delta = primary.delta[r];
      row.gamma = primary.gamma[r];
      row.vega = primary.vega[r];
      row.theta = primary.theta[r];
      row.sanity = greek_sanity(row.side, row.delta, row.gamma, row.vega);
    }
    if (identity.status[r] == RiskLaneStatus::Ok) row.identity_price = identity.price[r];
    if (std::isfinite(row.identity_price) && std::isfinite(row.predicted_price)) {
      row.reprice_abs = std::fabs(row.identity_price - row.predicted_price);
      if (std::isfinite(row.vega) && row.vega >= kRepriceVegaFloor) {
        row.reprice_vol_bp = row.reprice_abs / row.vega * 1.0e4;
      }
    }
  }
  // Call-put delta pairs at one strike of one chain, away from the exercise edge.
  for (const std::size_t c : unit.chains) {
    std::map<std::uint64_t, std::array<std::ptrdiff_t, 2>> pairs; // strike bits -> {call, put}
    for (const std::size_t i : in.chains[c].rows) {
      const ProbeRow &row = out[i];
      if (row.state != RowState::Evaluated || row.status != RiskLaneStatus::Ok) continue;
      if (has_flag(row.flags, RiskFlag::NearExerciseEdge) || row.route == RiskRoute::Exercised) {
        continue;
      }
      auto [it, inserted] = pairs.try_emplace(std::bit_cast<std::uint64_t>(row.strike),
                                              std::array<std::ptrdiff_t, 2>{-1, -1});
      static_cast<void>(inserted);
      it->second[row.side == Side::Call ? 0 : 1] = static_cast<std::ptrdiff_t>(i);
    }
    for (const auto &[bits, pair] : pairs) {
      static_cast<void>(bits);
      if (pair[0] < 0 || pair[1] < 0) continue;
      ProbeRow &call = out[static_cast<std::size_t>(pair[0])];
      ProbeRow &put = out[static_cast<std::size_t>(pair[1])];
      if (cp_delta_violation(call.delta, put.delta, call.q, call.T)) {
        call.sanity |= sanity_mask(SanityBit::CpDelta);
        put.sanity |= sanity_mask(SanityBit::CpDelta);
      }
    }
  }
}

// ── JSON of a count block ───────────────────────────────────────────────────

void write_counts(JsonWriter &w, const ProbeCounts &c) {
  w.begin_object();
  w.field("rows", c.rows);
  w.key("state");
  w.begin_object();
  for (std::size_t k = 0; k < kRowStateCount; ++k) w.field(kStateNames[k], c.state[k]);
  w.end_object();
  w.key("routes");
  w.begin_object();
  for (std::size_t k = 0; k < kRouteCount; ++k) w.field(kRouteNames[k], c.route[k]);
  w.end_object();
  w.key("lane");
  w.begin_object();
  for (std::size_t k = 0; k < kRiskLaneStatusCount; ++k) w.field(kLaneNames[k], c.lane[k]);
  w.end_object();
  w.key("flags");
  w.begin_object();
  w.field("near_edge", c.near_edge);
  w.field("cold_fallback", c.cold_fallback);
  w.field("outside_box", c.outside_box);
  w.end_object();
  w.field("nonfinite_greek", c.nonfinite_greek);
  w.key("sanity_fail");
  w.begin_object();
  for (std::size_t k = 0; k < kSanityKinds; ++k) w.field(kSanityNames[k], c.sanity_fail[k]);
  w.end_object();
  w.key("reprice");
  w.begin_object();
  w.field("checked", c.reprice_checked);
  w.field("mismatch", c.reprice_mismatch);
  w.field("max_abs", c.reprice_max_abs);
  w.field("max_rel", c.reprice_max_rel);
  w.field("max_vol_bp", c.reprice_max_vol_bp);
  w.end_object();
  w.key("vendor");
  w.begin_object();
  w.field("n", c.vendor_n);
  w.field("delta_abs_sum", c.vendor_delta_abs.sum);
  w.field("delta_abs_comp", c.vendor_delta_abs.comp);
  w.field("vega_sym_rel_sum", c.vendor_vega_sym_rel.sum);
  w.field("vega_sym_rel_comp", c.vendor_vega_sym_rel.comp);
  w.field("delta_mae", vendor_delta_mae(c));
  w.field("vega_sym_rel", vendor_vega_sym_rel(c));
  w.end_object();
  w.end_object();
}

// ── JSON reading helpers ────────────────────────────────────────────────────

[[nodiscard]] Result<const JsonValue *> member(const JsonValue &obj, std::string_view key) {
  if (obj.kind != JsonValue::Kind::Object) {
    return Err(ErrorCode::ParseError, "json: expected object for " + std::string(key));
  }
  const JsonValue *v = obj.find(key);
  if (v == nullptr) return Err(ErrorCode::ParseError, "json: missing " + std::string(key));
  return v;
}

[[nodiscard]] Result<std::uint64_t> read_u64(const JsonValue &obj, std::string_view key) {
  ATX_TRY(const JsonValue *v, member(obj, key));
  std::uint64_t out = 0;
  if (v->kind != JsonValue::Kind::Number || !parse_integer(v->text, out)) {
    return Err(ErrorCode::ParseError, "json: " + std::string(key) + " is not an unsigned integer");
  }
  return out;
}

[[nodiscard]] Result<std::int64_t> read_i64(const JsonValue &obj, std::string_view key) {
  ATX_TRY(const JsonValue *v, member(obj, key));
  std::int64_t out = 0;
  if (v->kind != JsonValue::Kind::Number || !parse_integer(v->text, out)) {
    return Err(ErrorCode::ParseError, "json: " + std::string(key) + " is not an integer");
  }
  return out;
}

// A number, or null read back as NaN (the writer's encoding of a non-finite value).
[[nodiscard]] Result<double> read_f64(const JsonValue &obj, std::string_view key) {
  ATX_TRY(const JsonValue *v, member(obj, key));
  if (v->kind == JsonValue::Kind::Null) return kNaN;
  double out = 0.0;
  if (v->kind != JsonValue::Kind::Number || !parse_real(v->text, out)) {
    return Err(ErrorCode::ParseError, "json: " + std::string(key) + " is not a number");
  }
  return out;
}

[[nodiscard]] Result<std::string> read_str(const JsonValue &obj, std::string_view key) {
  ATX_TRY(const JsonValue *v, member(obj, key));
  if (v->kind != JsonValue::Kind::String) {
    return Err(ErrorCode::ParseError, "json: " + std::string(key) + " is not a string");
  }
  return v->text;
}

[[nodiscard]] Result<bool> read_bool(const JsonValue &obj, std::string_view key) {
  ATX_TRY(const JsonValue *v, member(obj, key));
  if (v->kind != JsonValue::Kind::Bool) {
    return Err(ErrorCode::ParseError, "json: " + std::string(key) + " is not a boolean");
  }
  return v->boolean;
}

template <std::size_t N>
[[nodiscard]] Status read_named(const JsonValue &obj, std::string_view key,
                                const std::array<std::string_view, N> &names,
                                std::array<std::uint64_t, N> &out) {
  ATX_TRY(const JsonValue *block, member(obj, key));
  for (std::size_t k = 0; k < N; ++k) {
    ATX_TRY(out[k], read_u64(*block, names[k]));
  }
  return atx::core::Ok();
}

[[nodiscard]] Result<ProbeCounts> read_counts(const JsonValue &obj) {
  ProbeCounts c;
  ATX_TRY(c.rows, read_u64(obj, "rows"));
  ATX_TRY_VOID(read_named(obj, "state", kStateNames, c.state));
  ATX_TRY_VOID(read_named(obj, "routes", kRouteNames, c.route));
  ATX_TRY_VOID(read_named(obj, "lane", kLaneNames, c.lane));
  ATX_TRY(const JsonValue *flags, member(obj, "flags"));
  ATX_TRY(c.near_edge, read_u64(*flags, "near_edge"));
  ATX_TRY(c.cold_fallback, read_u64(*flags, "cold_fallback"));
  ATX_TRY(c.outside_box, read_u64(*flags, "outside_box"));
  ATX_TRY(c.nonfinite_greek, read_u64(obj, "nonfinite_greek"));
  ATX_TRY_VOID(read_named(obj, "sanity_fail", kSanityNames, c.sanity_fail));
  ATX_TRY(const JsonValue *reprice, member(obj, "reprice"));
  ATX_TRY(c.reprice_checked, read_u64(*reprice, "checked"));
  ATX_TRY(c.reprice_mismatch, read_u64(*reprice, "mismatch"));
  ATX_TRY(c.reprice_max_abs, read_f64(*reprice, "max_abs"));
  ATX_TRY(c.reprice_max_rel, read_f64(*reprice, "max_rel"));
  ATX_TRY(c.reprice_max_vol_bp, read_f64(*reprice, "max_vol_bp"));
  ATX_TRY(const JsonValue *vendor, member(obj, "vendor"));
  ATX_TRY(c.vendor_n, read_u64(*vendor, "n"));
  ATX_TRY(c.vendor_delta_abs.sum, read_f64(*vendor, "delta_abs_sum"));
  ATX_TRY(c.vendor_delta_abs.comp, read_f64(*vendor, "delta_abs_comp"));
  ATX_TRY(c.vendor_vega_sym_rel.sum, read_f64(*vendor, "vega_sym_rel_sum"));
  ATX_TRY(c.vendor_vega_sym_rel.comp, read_f64(*vendor, "vega_sym_rel_comp"));
  return c;
}

void csv_real(std::string &out, double v) {
  if (!std::isfinite(v)) return; // blank
  std::array<char, 64> buf{};
  const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  static_cast<void>(ec);
  out.append(buf.data(), end);
}

void csv_u64(std::string &out, std::uint64_t v) {
  std::array<char, 32> buf{};
  const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  static_cast<void>(ec);
  out.append(buf.data(), end);
}

} // namespace

// ── Public: CSV readers ─────────────────────────────────────────────────────

Result<std::vector<QuoteRow>> parse_quotes_csv(std::string_view text) {
  static constexpr std::array<std::string_view, 18> kColumns{
      "row_id", "chain_id", "snapshot_ns", "underlier", "root", "expiry_days", "strike",
      "is_call", "exercise", "spot", "bid", "ask", "years", "years_calendar", "rate", "sdiv",
      "ddiv_pv", "input_status"};
  ATX_TRY(CsvTable table, CsvTable::open(text, kColumns));
  std::vector<QuoteRow> rows;
  // Bounded by the number of lines in `text`.
  while (true) {
    ATX_TRY(const bool more, table.next());
    if (!more) break;
    QuoteRow q;
    if (!parse_integer(table.field(0), q.row_id)) return Err(parse_error(table, "row_id"));
    if (!parse_integer(table.field(1), q.chain_id)) return Err(parse_error(table, "chain_id"));
    if (!table.field(2).empty() && !parse_integer(table.field(2), q.snapshot_ns)) {
      return Err(parse_error(table, "snapshot_ns"));
    }
    q.underlier = std::string(table.field(3));
    q.root = std::string(table.field(4));
    if (!table.field(5).empty() && !parse_integer(table.field(5), q.expiry_days)) {
      return Err(parse_error(table, "expiry_days"));
    }
    if (!parse_real(table.field(6), q.strike)) return Err(parse_error(table, "strike"));
    q.is_call = table.field(7) == "1";
    q.exercise = std::string(table.field(8));
    if (!parse_real(table.field(9), q.spot) || !parse_real(table.field(10), q.bid) ||
        !parse_real(table.field(11), q.ask) || !parse_real(table.field(12), q.years) ||
        !parse_real(table.field(13), q.years_calendar) || !parse_real(table.field(14), q.rate) ||
        !parse_real(table.field(15), q.sdiv) || !parse_real(table.field(16), q.ddiv_pv)) {
      return Err(parse_error(table, "number"));
    }
    q.input_status = std::string(table.field(17));
    rows.push_back(std::move(q));
  }
  ATX_TRY_VOID(check_unique_rows(rows));
  return rows;
}

Result<std::vector<PredictionRow>> parse_predictions_csv(std::string_view text) {
  static constexpr std::array<std::string_view, 5> kColumns{
      "row_id", "predicted_price", "predicted_vol", "status", "chain_id"};
  ATX_TRY(CsvTable table, CsvTable::open(text, kColumns));
  std::vector<PredictionRow> rows;
  while (true) {
    ATX_TRY(const bool more, table.next());
    if (!more) break;
    PredictionRow p;
    if (!parse_integer(table.field(0), p.row_id)) return Err(parse_error(table, "row_id"));
    if (!parse_real(table.field(1), p.price)) return Err(parse_error(table, "predicted_price"));
    if (!parse_real(table.field(2), p.vol)) return Err(parse_error(table, "predicted_vol"));
    p.status = std::string(table.field(3));
    if (!parse_integer(table.field(4), p.chain_id)) return Err(parse_error(table, "chain_id"));
    rows.push_back(std::move(p));
  }
  ATX_TRY_VOID(check_unique_rows(rows));
  return rows;
}

Result<std::vector<LabelRow>> parse_labels_csv(std::string_view text) {
  static constexpr std::array<std::string_view, 5> kColumns{
      "row_id", "reference_price", "reference_vol", "vendor_delta", "vendor_vega"};
  ATX_TRY(CsvTable table, CsvTable::open(text, kColumns));
  std::vector<LabelRow> rows;
  while (true) {
    ATX_TRY(const bool more, table.next());
    if (!more) break;
    LabelRow l;
    if (!parse_integer(table.field(0), l.row_id)) return Err(parse_error(table, "row_id"));
    if (!parse_real(table.field(1), l.reference_price) ||
        !parse_real(table.field(2), l.reference_vol) ||
        !parse_real(table.field(3), l.vendor_delta) ||
        !parse_real(table.field(4), l.vendor_vega)) {
      return Err(parse_error(table, "number"));
    }
    rows.push_back(l);
  }
  ATX_TRY_VOID(check_unique_rows(rows));
  return rows;
}

Result<std::vector<ChainSpot>> parse_basis_csv(std::string_view text) {
  static constexpr std::array<std::string_view, 2> kColumns{"chain_id", "spot"};
  ATX_TRY(CsvTable table, CsvTable::open(text, kColumns));
  std::vector<ChainSpot> rows;
  while (true) {
    ATX_TRY(const bool more, table.next());
    if (!more) break;
    ChainSpot b;
    if (!parse_integer(table.field(0), b.chain_id)) return Err(parse_error(table, "chain_id"));
    if (!parse_real(table.field(1), b.spot) || !std::isfinite(b.spot)) {
      return Err(parse_error(table, "spot"));
    }
    rows.push_back(b);
  }
  return rows;
}

// ── Public: governance ──────────────────────────────────────────────────────

Status labels_allowed(std::string_view manifest_json) {
  JsonParser parser(manifest_json);
  ATX_TRY(const JsonValue root, parser.parse_document());
  if (root.kind != JsonValue::Kind::Object) {
    return Err(ErrorCode::ParseError, "manifest: not a JSON object");
  }
  const JsonValue *split = root.find("rows_by_split");
  if (split == nullptr || split->kind != JsonValue::Kind::Object) {
    return Err(ErrorCode::ParseError, "manifest: rows_by_split missing");
  }
  std::uint64_t n = 0;
  if (split->members.size() != 1 || split->members[0].first != "development" ||
      split->members[0].second.kind != JsonValue::Kind::Number ||
      !parse_integer(split->members[0].second.text, n) || n == 0) {
    return Err(ErrorCode::PermissionDenied,
               "labels open only for a development-split dataset "
               "(rows_by_split == {development: n})");
  }
  return atx::core::Ok();
}

// ── Public: shadow clock ────────────────────────────────────────────────────

double shadow_rate(const QuoteSliceMarket &market) noexcept {
  // Same operand order as fit_quote_slice: rate * (calendar_years / variance_years).
  const double ratio = market.calendar_years / market.variance_years;
  return market.rate * ratio;
}

double shadow_yield(const QuoteSliceMarket &market, double calendar_yield,
                    QuoteSliceDividendModel model) noexcept {
  // quote_slice.cpp converted_yield / aggregate_log_retention, restated expression for
  // expression (the FitRiskProbe.ShadowClockReproducesFitCarry pin keeps them equal).
  double value = calendar_yield * (market.calendar_years / market.variance_years);
  if (model == QuoteSliceDividendModel::AggregatePresentValue ||
      model == QuoteSliceDividendModel::AggregateFundingPresentValue) {
    double retention = 0.0;
    if (market.dividend_pv != 0.0) {
      double fraction = market.dividend_pv / market.spot;
      if (model == QuoteSliceDividendModel::AggregateFundingPresentValue) {
        fraction = std::exp(std::log(market.dividend_pv) - std::log(market.spot) +
                            calendar_yield * market.calendar_years);
      }
      retention = fraction < 1.0 ? std::log1p(-fraction) : kNaN;
    }
    value -= retention / market.variance_years;
  }
  return value;
}

std::int64_t shadow_expiry_ns(std::int64_t ts_ns, double variance_years,
                              std::int64_t offset_ns) noexcept {
  return ts_ns + std::llround(variance_years * kCalendarYearNs) + offset_ns;
}

// ── Public: sanity ──────────────────────────────────────────────────────────

std::uint8_t greek_sanity(Side side, double delta, double gamma, double vega) noexcept {
  std::uint8_t bits = 0;
  if (std::isfinite(delta)) {
    const bool bad = side == Side::Call ? (delta < 0.0 || delta > 1.0)
                                        : (delta < -1.0 || delta > 0.0);
    if (bad) bits |= sanity_mask(SanityBit::DeltaRange);
  }
  if (std::isfinite(gamma) && gamma < 0.0) bits |= sanity_mask(SanityBit::GammaNeg);
  if (std::isfinite(vega) && vega < 0.0) bits |= sanity_mask(SanityBit::VegaNeg);
  return bits;
}

bool cp_delta_violation(double call_delta, double put_delta, double q, double T) noexcept {
  if (!std::isfinite(call_delta) || !std::isfinite(put_delta) || !std::isfinite(q) ||
      !std::isfinite(T)) {
    return false;
  }
  return std::fabs((call_delta - put_delta) - std::exp(-q * T)) > kCpDeltaSlack;
}

const char *to_string(ProbeKernel kernel) noexcept {
  switch (kernel) {
  case ProbeKernel::Family: return "family";
  case ProbeKernel::Cold: return "cold";
  case ProbeKernel::Both: return "both";
  }
  return "unknown";
}

const char *to_string(RowState state) noexcept {
  switch (state) {
  case RowState::NotServed: return "not_served";
  case RowState::PriceOnly: return "price_only";
  case RowState::InvalidInput: return "invalid_input";
  case RowState::Evaluated: return "evaluated";
  }
  return "unknown";
}

const char *to_string(SanityBit bit) noexcept {
  switch (bit) {
  case SanityBit::None: return "none";
  case SanityBit::DeltaRange: return "delta_range";
  case SanityBit::GammaNeg: return "gamma_neg";
  case SanityBit::VegaNeg: return "vega_neg";
  case SanityBit::CpDelta: return "cp_delta";
  }
  return "unknown";
}

// ── Public: aggregation ─────────────────────────────────────────────────────

void CompensatedSum::add(double x) noexcept {
  const double t = sum + x;
  // The branch keeps the compensation exact whichever operand is larger (Neumaier).
  if (std::fabs(sum) >= std::fabs(x)) {
    comp += (sum - t) + x;
  } else {
    comp += (x - t) + sum;
  }
  sum = t;
}

void accumulate(ProbeCounts &c, const ProbeRow &row) noexcept {
  ++c.rows;
  ++c.state[static_cast<std::size_t>(row.state)];
  if (row.state != RowState::Evaluated) return;
  ++c.route[static_cast<std::size_t>(row.route)];
  ++c.lane[static_cast<std::size_t>(row.status)];
  if (has_flag(row.flags, RiskFlag::NearExerciseEdge)) ++c.near_edge;
  if (has_flag(row.flags, RiskFlag::ColdFallback)) ++c.cold_fallback;
  if (has_flag(row.flags, RiskFlag::OutsideFamilyBox)) ++c.outside_box;
  if (row.status != RiskLaneStatus::Ok) return;
  if (!std::isfinite(row.delta) || !std::isfinite(row.gamma) || !std::isfinite(row.vega)) {
    ++c.nonfinite_greek;
  }
  for (std::size_t k = 0; k < kSanityKinds; ++k) {
    if ((row.sanity & (1u << k)) != 0u) ++c.sanity_fail[k];
  }
  if (std::isfinite(row.reprice_abs)) {
    ++c.reprice_checked;
    c.reprice_max_abs = std::max(c.reprice_max_abs, row.reprice_abs);
    if (row.predicted_price > 0.0) {
      c.reprice_max_rel = std::max(c.reprice_max_rel, row.reprice_abs / row.predicted_price);
    }
    if (std::isfinite(row.reprice_vol_bp)) {
      c.reprice_max_vol_bp = std::max(c.reprice_max_vol_bp, row.reprice_vol_bp);
    }
    if (row.reprice_abs > std::max(kRepriceRel * row.predicted_price, kRepriceAbs)) {
      ++c.reprice_mismatch;
    }
  }
  if (std::isfinite(row.delta) && std::isfinite(row.vendor_delta)) {
    ++c.vendor_n;
    c.vendor_delta_abs.add(std::fabs(row.delta - row.vendor_delta));
    const double scale = 0.5 * (std::fabs(row.vega) + std::fabs(row.vendor_vega));
    if (std::isfinite(row.vega) && std::isfinite(row.vendor_vega) && scale > 0.0) {
      c.vendor_vega_sym_rel.add(std::fabs(row.vega - row.vendor_vega) / scale);
    }
  }
}

double vendor_delta_mae(const ProbeCounts &c) noexcept {
  return c.vendor_n == 0 ? kNaN : c.vendor_delta_abs.value() / static_cast<double>(c.vendor_n);
}

double vendor_vega_sym_rel(const ProbeCounts &c) noexcept {
  return c.vendor_n == 0 ? kNaN
                         : c.vendor_vega_sym_rel.value() / static_cast<double>(c.vendor_n);
}

// ── Public: the run ─────────────────────────────────────────────────────────

Result<ProbeReport> run_fit_risk_probe(std::span<const QuoteRow> quotes,
                                       std::span<const PredictionRow> predictions,
                                       std::span<const LabelRow> labels,
                                       std::span<const ChainSpot> basis,
                                       const ProbeOptions &options,
                                       std::vector<ProbeRow> *rows) {
  if (!labels.empty() && !options.labels_allowed) {
    return Err(ErrorCode::PermissionDenied,
               "labels supplied for a dataset that is not development-only (labels_allowed)");
  }
  Inputs in;
  in.quotes = quotes;
  in.predictions = predictions;
  in.labels = labels;
  std::unordered_map<std::uint64_t, std::size_t> by_row;
  by_row.reserve(quotes.size());
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    if (!by_row.emplace(quotes[i].row_id, i).second) {
      return Err(ErrorCode::InvalidArgument, "quotes: duplicate row_id");
    }
  }
  in.prediction_of.assign(quotes.size(), -1);
  for (std::size_t p = 0; p < predictions.size(); ++p) {
    const auto it = by_row.find(predictions[p].row_id);
    if (it == by_row.end()) return Err(ErrorCode::InvalidArgument, "predictions: unknown row_id");
    if (in.prediction_of[it->second] >= 0) {
      return Err(ErrorCode::InvalidArgument, "predictions: duplicate row_id");
    }
    if (quotes[it->second].chain_id != predictions[p].chain_id) {
      return Err(ErrorCode::InvalidArgument, "predictions: chain_id disagrees with its quote");
    }
    in.prediction_of[it->second] = static_cast<std::ptrdiff_t>(p);
  }
  in.label_of.assign(quotes.size(), -1);
  for (std::size_t l = 0; l < labels.size(); ++l) {
    const auto it = by_row.find(labels[l].row_id);
    if (it == by_row.end()) return Err(ErrorCode::InvalidArgument, "labels: unknown row_id");
    in.label_of[it->second] = static_cast<std::ptrdiff_t>(l);
  }
  std::unordered_map<std::uint64_t, double> snapshot_spots;
  for (const ChainSpot &b : basis) snapshot_spots[b.chain_id] = b.spot;

  // Chains in first-appearance order; units (underliers) likewise.
  std::unordered_map<std::uint64_t, std::size_t> chain_index;
  std::unordered_map<std::string, std::size_t> unit_index;
  std::vector<Unit> units;
  in.chain_of.resize(quotes.size());
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto [it, fresh] = chain_index.try_emplace(quotes[i].chain_id, in.chains.size());
    if (fresh) {
      ChainInfo info;
      info.chain_id = quotes[i].chain_id;
      info.first_row = i;
      in.chains.push_back(std::move(info));
      const auto [u, new_unit] = unit_index.try_emplace(quotes[i].underlier, units.size());
      if (new_unit) units.emplace_back();
      units[u->second].chains.push_back(it->second);
    }
    in.chain_of[i] = it->second;
    in.chains[it->second].rows.push_back(i);
  }
  in.calendar_yield.assign(quotes.size(), kNaN);
  for (ChainInfo &chain : in.chains) {
    build_basis(chain, quotes, snapshot_spots, options, in.calendar_yield);
    if (chain.basis.valid) {
      chain.basis.base_expiry_ns = shadow_expiry_ns(quotes[chain.first_row].snapshot_ns,
                                                    chain.basis.market.variance_years);
    }
  }

  std::vector<ProbeRow> out(quotes.size());
  pricing_executor().run_dynamic(units.size(), options.threads,
                                 [&](std::size_t u, unsigned worker) {
                                   static_cast<void>(worker);
                                   evaluate_unit(in, units[u], options, out);
                                 });

  ProbeReport report;
  report.provenance.kernel = to_string(options.kernel);
  report.provenance.identity_kernel = options.kernel == ProbeKernel::Family ? "family" : "cold";
  report.provenance.al_opts = "accurate-fast-profile";
  report.provenance.dividend_model = dividend_name(options.dividend_model);
  report.provenance.carry_price_tolerance = options.carry_price_tolerance;
  const FamilyKernelPolicy family_defaults{};
  report.provenance.family_n_sigma = family_defaults.n_sigma;
  report.provenance.family_sigma_box_pad = family_defaults.sigma_box_pad;
  report.provenance.threads = options.threads;
  report.provenance.labels = !labels.empty();
  report.chains.reserve(in.chains.size());
  for (const ChainInfo &chain : in.chains) {
    const QuoteRow &first = quotes[chain.first_row];
    ChainProbe c;
    c.chain_id = chain.chain_id;
    c.underlier = first.underlier;
    c.root = first.root;
    c.snapshot_ns = first.snapshot_ns;
    c.expiry_days = first.expiry_days;
    c.variance_years = chain.basis.valid ? chain.basis.market.variance_years : kNaN;
    c.basis = chain.basis.label;
    report.chains.push_back(std::move(c));
  }
  // Serial, input order: every chain block and the totals.
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    accumulate(report.chains[in.chain_of[i]].counts, out[i]);
    accumulate(report.totals, out[i]);
  }
  if (rows != nullptr) *rows = std::move(out);
  return report;
}

// ── Public: JSON / CSV ──────────────────────────────────────────────────────

std::string probe_report_to_json(const ProbeReport &report) {
  JsonWriter w;
  w.begin_object();
  w.field("schema", std::string_view(report.schema));
  w.field("schema_version", static_cast<std::int64_t>(report.schema_version));
  const ProbeProvenance &p = report.provenance;
  w.key("provenance");
  w.begin_object();
  w.field("kernel", std::string_view(p.kernel));
  w.field("identity_kernel", std::string_view(p.identity_kernel));
  w.field("al_opts", std::string_view(p.al_opts));
  w.field("dividend_model", std::string_view(p.dividend_model));
  w.field("carry_price_tolerance", p.carry_price_tolerance);
  w.field("family_n_sigma", static_cast<std::uint64_t>(p.family_n_sigma));
  w.field("family_sigma_box_pad", p.family_sigma_box_pad);
  w.field("threads", static_cast<std::uint64_t>(p.threads));
  w.field("labels", p.labels);
  w.end_object();
  w.key("totals");
  write_counts(w, report.totals);
  w.key("chains");
  w.begin_array();
  for (const ChainProbe &c : report.chains) {
    w.begin_object();
    w.field("chain_id", c.chain_id);
    w.field("underlier", std::string_view(c.underlier));
    w.field("root", std::string_view(c.root));
    w.field("snapshot_ns", c.snapshot_ns);
    w.field("expiry_days", static_cast<std::int64_t>(c.expiry_days));
    w.field("variance_years", c.variance_years);
    w.field("basis", std::string_view(c.basis));
    w.key("counts");
    write_counts(w, c.counts);
    w.end_object();
  }
  w.end_array();
  w.end_object();
  std::string out = w.take();
  out.push_back('\n');
  return out;
}

Result<ProbeReport> probe_report_from_json(std::string_view text) {
  JsonParser parser(text);
  ATX_TRY(const JsonValue root, parser.parse_document());
  ProbeReport r;
  ATX_TRY(r.schema, read_str(root, "schema"));
  if (r.schema != kProbeSchema) {
    return Err(ErrorCode::InvalidArgument, "probe json: foreign schema " + r.schema);
  }
  ATX_TRY(const std::int64_t version, read_i64(root, "schema_version"));
  if (version != kProbeSchemaVersion) {
    return Err(ErrorCode::InvalidArgument,
               "probe json: unsupported schema_version " + std::to_string(version));
  }
  r.schema_version = kProbeSchemaVersion;
  ATX_TRY(const JsonValue *prov, member(root, "provenance"));
  ATX_TRY(r.provenance.kernel, read_str(*prov, "kernel"));
  ATX_TRY(r.provenance.identity_kernel, read_str(*prov, "identity_kernel"));
  ATX_TRY(r.provenance.al_opts, read_str(*prov, "al_opts"));
  ATX_TRY(r.provenance.dividend_model, read_str(*prov, "dividend_model"));
  ATX_TRY(r.provenance.carry_price_tolerance, read_f64(*prov, "carry_price_tolerance"));
  ATX_TRY(const std::uint64_t n_sigma, read_u64(*prov, "family_n_sigma"));
  r.provenance.family_n_sigma = static_cast<unsigned>(n_sigma);
  ATX_TRY(r.provenance.family_sigma_box_pad, read_f64(*prov, "family_sigma_box_pad"));
  ATX_TRY(const std::uint64_t threads, read_u64(*prov, "threads"));
  r.provenance.threads = static_cast<unsigned>(threads);
  ATX_TRY(r.provenance.labels, read_bool(*prov, "labels"));
  ATX_TRY(const JsonValue *totals, member(root, "totals"));
  ATX_TRY(r.totals, read_counts(*totals));
  ATX_TRY(const JsonValue *chains, member(root, "chains"));
  if (chains->kind != JsonValue::Kind::Array) {
    return Err(ErrorCode::ParseError, "probe json: chains is not an array");
  }
  r.chains.reserve(chains->items.size());
  for (const JsonValue &item : chains->items) {
    ChainProbe c;
    ATX_TRY(c.chain_id, read_u64(item, "chain_id"));
    ATX_TRY(c.underlier, read_str(item, "underlier"));
    ATX_TRY(c.root, read_str(item, "root"));
    ATX_TRY(c.snapshot_ns, read_i64(item, "snapshot_ns"));
    ATX_TRY(const std::int64_t expiry, read_i64(item, "expiry_days"));
    c.expiry_days = static_cast<std::int32_t>(expiry);
    ATX_TRY(c.variance_years, read_f64(item, "variance_years"));
    ATX_TRY(c.basis, read_str(item, "basis"));
    ATX_TRY(const JsonValue *counts, member(item, "counts"));
    ATX_TRY(c.counts, read_counts(*counts));
    r.chains.push_back(std::move(c));
  }
  return r;
}

std::string probe_rows_to_csv(std::span<const ProbeRow> rows) {
  std::string out =
      "row_id,chain_id,state,side,route,flags,lane,strike,sigma,predicted_price,price,delta,"
      "gamma,vega,theta,identity_price,reprice_abs,reprice_vol_bp,T,rate,q,sanity,"
      "vendor_delta,vendor_vega\n";
  out.reserve(out.size() + rows.size() * 200U);
  for (const ProbeRow &r : rows) {
    csv_u64(out, r.row_id);
    out.push_back(',');
    csv_u64(out, r.chain_id);
    out.push_back(',');
    out += kStateNames[static_cast<std::size_t>(r.state)];
    out.push_back(',');
    out += r.side == Side::Call ? "call" : "put";
    out.push_back(',');
    out += kRouteNames[static_cast<std::size_t>(r.route)];
    out.push_back(',');
    csv_u64(out, r.flags);
    out.push_back(',');
    out += r.state == RowState::Evaluated ? kLaneNames[static_cast<std::size_t>(r.status)]
                                          : std::string_view{};
    for (const double v : {r.strike, r.sigma, r.predicted_price, r.price, r.delta, r.gamma,
                           r.vega, r.theta, r.identity_price, r.reprice_abs, r.reprice_vol_bp,
                           r.T, r.rate, r.q}) {
      out.push_back(',');
      csv_real(out, v);
    }
    out.push_back(',');
    csv_u64(out, r.sanity);
    out.push_back(',');
    csv_real(out, r.vendor_delta);
    out.push_back(',');
    csv_real(out, r.vendor_vega);
    out.push_back('\n');
  }
  return out;
}

} // namespace atx::vol::fit_probe
