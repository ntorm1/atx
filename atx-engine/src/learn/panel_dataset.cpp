#include "atx/engine/learn/panel_dataset.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <utility>

#include "atx/core/sha256.hpp"
#include "atx/tsdb/mapping.hpp"

namespace atx::engine::learn {
namespace {
using namespace atx;
using core::Err; using core::ErrorCode; using core::Ok; using core::Result; using core::Status;
namespace fs = std::filesystem;
constexpr u64 kMetadataLimit = 8ULL * 1024 * 1024;
constexpr u64 kBlockLimit = 256ULL * 1024 * 1024;
constexpr i64 kSeal = 1'577'836'800'000'000'000LL;
constexpr u64 kMissing = 0x7ff8000000000000ULL;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr std::string_view kRule = "rank-residual-v2;rank-average-ties;missing-zero-plus-indicator;prior-daily-sd-sqrt-h;scaled-then-date-demean;no-exposure-residual";
static_assert(std::endian::native == std::endian::little && sizeof(f32) == 4 && sizeof(f64) == 8 &&
    std::numeric_limits<f32>::is_iec559 && std::numeric_limits<f64>::is_iec559);

bool valid_hash(std::string_view text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool valid_text(std::string_view text, usize limit) { return !text.empty() && text.size() <= limit && text.find('\0') == std::string_view::npos; }
u64 align8(u64 x) { return (x + 7) & ~u64{7}; }
void put(u8* out, u64 x, usize n = 8) { for (usize i = 0; i < n; ++i) out[i] = static_cast<u8>(x >> (8 * i)); }
u64 get(const u8* in, usize n = 8) { u64 x = 0; for (usize i = 0; i < n; ++i) x |= u64{in[i]} << (8 * i); return x; }
void number(std::string& out, u64 x) { std::array<u8, 8> b{}; put(b.data(), x); out.append(reinterpret_cast<const char*>(b.data()), b.size()); }
void text(std::string& out, std::string_view x) { number(out, x.size()); out.append(x); }
struct Cursor {
  std::string_view bytes; usize pos{};
  u64 number() {
    if (bytes.size() - pos < 8) throw std::invalid_argument("short number");
    const auto value = get(reinterpret_cast<const u8*>(bytes.data()) + pos); pos += 8; return value;
  }
  usize count(usize maximum) {
    const auto value = number();
    if (value > maximum || value > (bytes.size() - pos) / 8) throw std::invalid_argument("count bound");
    return static_cast<usize>(value);
  }
  std::string text(usize maximum) {
    const auto length = number();
    if (length > maximum || length > bytes.size() - pos) throw std::invalid_argument("text bound");
    std::string value(bytes.substr(pos, static_cast<usize>(length))); pos += static_cast<usize>(length); return value;
  }
};
struct Layout { u64 feature_stride{}, label_stride{}, labels{}, present{}, member{}, clocks{}, bytes{}; };
Layout layout(const PanelDatasetConfig& c, usize dates) {
  const u64 cells = u64{dates} * c.instrument_ids.size();
  Layout l;
  l.feature_stride = align8(cells * 4); l.label_stride = cells * 8;
  l.labels = 48 + l.feature_stride * (2 * c.feature_names.size());
  l.present = l.labels + l.label_stride * c.holding_horizons.size();
  l.member = l.present + align8(cells); l.clocks = l.member + align8(cells);
  l.bytes = l.clocks + dates * 8; return l;
}
struct BlockInfo { usize begin{}, dates{}; u64 bytes{}; std::string sha; };
std::string block_name(usize index) { return "block-" + std::to_string(index) + ".bin"; }
struct Budget { std::mutex mutex; u64 used{}, limit{}; u32 count{}, maximum{}; };
Status write_exclusive(const fs::path& path, std::span<const u8> bytes) {
#if defined(_WIN32)
  std::FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"wbx") != 0) file = nullptr;
#else
  auto* file = std::fopen(path.c_str(), "wbx");
#endif
  if (!file) return Err(ErrorCode::IoError, "dataset: exclusive create failed");
  const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  const bool flushed = std::fflush(file) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!written || !flushed || !closed) return Err(ErrorCode::IoError, "dataset: write/flush/close failed");
  return Ok();
}
void encode_config(std::string& out, const PanelDatasetConfig& c) {
  text(out, kRule);
  number(out, c.session_keys.size()); for (auto x : c.session_keys) number(out, static_cast<u64>(x));
  number(out, c.instrument_ids.size()); for (auto x : c.instrument_ids) number(out, static_cast<u64>(x));
  number(out, c.feature_names.size()); for (const auto& x : c.feature_names) text(out, x);
  text(out, c.instrument_namespace); text(out, c.source_sha256); text(out, c.source_recipe);
  number(out, c.holding_horizons.size()); for (auto x : c.holding_horizons) number(out, x);
  number(out, c.feature_max_lookback); number(out, c.execution_delay); number(out, c.volatility_window); number(out, c.volatility_min_observations);
  number(out, std::bit_cast<u64>(c.daily_volatility_floor)); number(out, c.maturity_end);
  number(out, c.block_dates); number(out, c.max_working_bytes); number(out, c.max_mapped_bytes); number(out, c.max_open_blocks);
}
PanelDatasetConfig decode_config(Cursor& r) {
  PanelDatasetConfig c;
  if (r.text(512) != kRule) throw std::invalid_argument("unsupported recipe");
  const auto dates = r.count(100000); c.session_keys.reserve(dates);
  for (usize i = 0; i < dates; ++i) c.session_keys.push_back(std::bit_cast<i64>(r.number()));
  const auto names = r.count(100000); c.instrument_ids.reserve(names);
  for (usize i = 0; i < names; ++i) c.instrument_ids.push_back(std::bit_cast<i64>(r.number()));
  const auto features = r.count(4096); c.feature_names.reserve(features);
  for (usize i = 0; i < features; ++i) c.feature_names.push_back(r.text(256));
  c.instrument_namespace = r.text(128); c.source_sha256 = r.text(64); c.source_recipe = r.text(1024 * 1024);
  const auto horizons = r.count(16); c.holding_horizons.clear();
  for (usize i = 0; i < horizons; ++i) { const auto x = r.number(); if (x > 65535) throw std::invalid_argument("horizon width"); c.holding_horizons.push_back(static_cast<u16>(x)); }
  const auto lookback = r.number(), delay = r.number(), window = r.number(), minimum = r.number();
  if (lookback > 65535 || delay > 65535 || window > 65535 || minimum > 65535) throw std::invalid_argument("recipe width");
  c.feature_max_lookback = static_cast<u16>(lookback);
  c.execution_delay = static_cast<u16>(delay); c.volatility_window = static_cast<u16>(window); c.volatility_min_observations = static_cast<u16>(minimum);
  c.daily_volatility_floor = std::bit_cast<f64>(r.number()); c.maturity_end = static_cast<usize>(r.number());
  c.block_dates = static_cast<usize>(r.number()); c.max_working_bytes = r.number(); c.max_mapped_bytes = r.number();
  const auto handles = r.number(); if (handles > 256) throw std::invalid_argument("handle width"); c.max_open_blocks = static_cast<u32>(handles);
  return c;
}

// Rank one cross-section in original f64; narrow only the bounded final ranks.
void rank_column(std::span<const f64> raw, std::span<const u8> present, std::span<const u8> member,
    std::vector<usize>& order, u8* ranks, u8* missing) {
  order.clear();
  for (usize i = 0; i < raw.size(); ++i) {
    const bool observed = present[i] && std::isfinite(raw[i]);
    put(ranks + i * 4, 0, 4);
    put(missing + i * 4, std::bit_cast<u32>(observed ? f32{0} : f32{1}), 4);
    if (member[i] && observed) order.push_back(i);
  }
  std::sort(order.begin(), order.end(), [&](usize a, usize b) { return raw[a] < raw[b] || (raw[a] == raw[b] && a < b); });
  for (usize first = 0; first < order.size();) {
    usize end = first + 1; while (end < order.size() && raw[order[end]] == raw[order[first]]) ++end;
    const f64 rank = order.size() > 1 ? (static_cast<f64>(first) + static_cast<f64>(end - 1)) * .5 / static_cast<f64>(order.size() - 1) - .5 : 0;
    for (usize j = first; j < end; ++j) put(ranks + order[j] * 4, std::bit_cast<u32>(static_cast<f32>(rank)), 4);
    first = end;
  }
}

// Deterministic chronological Welford over a bounded strictly-prior ring.
void prior_volatility(std::span<const f64> history, usize dates, usize next, usize names,
    const PanelDatasetConfig& c, std::span<f64> output) {
  const usize count = std::min(dates, static_cast<usize>(c.volatility_window));
  for (usize i = 0; i < names; ++i) {
    usize used = 0; f64 mean = 0, m2 = 0;
    for (usize t = 0; t < count; ++t) {
      const auto slot = dates < c.volatility_window ? t : (next + t) % c.volatility_window;
      const f64 value = history[slot * names + i]; if (!std::isfinite(value)) continue;
      ++used; const f64 delta = value - mean; mean += delta / static_cast<f64>(used); m2 += delta * (value - mean);
    }
    output[i] = used >= c.volatility_min_observations && std::isfinite(m2) && m2 >= 0
        ? std::max(c.daily_volatility_floor, std::sqrt(m2 / static_cast<f64>(used - 1))) : kNaN;
  }
}
} // namespace

Result<PanelDatasetSizing> preflight_panel_dataset(const PanelDatasetConfig& c) {
  const auto t = c.session_keys.size(), n = c.instrument_ids.size(), f = c.feature_names.size();
  if (!t || t > 100000 || !n || n > 100000 || !f || f > 4096 || !c.block_dates || c.block_dates > 256 ||
      c.holding_horizons.empty() || c.holding_horizons.size() > 16 || c.volatility_window < 2 ||
      c.volatility_min_observations < 2 || c.volatility_min_observations > c.volatility_window ||
      !std::isfinite(c.daily_volatility_floor) || c.daily_volatility_floor <= 0 || c.daily_volatility_floor >= 1 ||
      c.maturity_end > t || !c.max_open_blocks || c.max_open_blocks > 256 ||
      !valid_text(c.instrument_namespace, 128) || !valid_hash(c.source_sha256) || !valid_text(c.source_recipe, 1024 * 1024))
    return Err(ErrorCode::InvalidArgument, "dataset: invalid shape/recipe/budget");
  for (usize i = 0; i < t; ++i) if (c.session_keys[i] <= 0 || c.session_keys[i] >= kSeal || (i && c.session_keys[i - 1] >= c.session_keys[i]))
    return Err(ErrorCode::InvalidArgument, "dataset: session axis must be ordered and sealed");
  for (usize i = 0; i < n; ++i) if (c.instrument_ids[i] <= 0 || (i && c.instrument_ids[i - 1] >= c.instrument_ids[i]))
    return Err(ErrorCode::InvalidArgument, "dataset: canonical sorted unique instrument IDs required");
  for (usize i = 0; i < f; ++i) {
    if (!valid_text(c.feature_names[i], 256)) return Err(ErrorCode::InvalidArgument, "dataset: invalid feature name");
    for (usize j = 0; j < i; ++j) if (c.feature_names[i] == c.feature_names[j]) return Err(ErrorCode::InvalidArgument, "dataset: duplicate feature name");
  }
  for (usize i = 0; i < c.holding_horizons.size(); ++i) if (!c.holding_horizons[i] ||
      static_cast<u32>(c.holding_horizons[i]) + c.execution_delay > 65535 || (i && c.holding_horizons[i - 1] >= c.holding_horizons[i]))
    return Err(ErrorCode::InvalidArgument, "dataset: horizons must increase and endpoint fit maturity metadata");
  const u64 blocks = (t + c.block_dates - 1) / c.block_dates;
  const u64 metadata = 65536 + u64{t + n} * 16 + u64{f} * 512 + c.source_recipe.size() * 4 + blocks * 192;
  const auto block = layout(c, std::min(c.block_dates, t));
  const u64 scratch = u64{n} * (u64{f} * 8 + u64{c.volatility_window} * 8 + 192);
  const u64 working = block.bytes + scratch + metadata * 4;
  const u64 payload = u64{t / c.block_dates} * layout(c, c.block_dates).bytes +
      (t % c.block_dates ? layout(c, t % c.block_dates).bytes : 0);
  if (metadata > kMetadataLimit || block.bytes > kBlockLimit || block.bytes > c.max_mapped_bytes ||
      working > c.max_working_bytes || payload > (1ULL << 40))
    return Err(ErrorCode::InvalidArgument, "dataset: metadata/block/working/mapping/artifact admission exceeded");
  return Ok(PanelDatasetSizing{payload, block.bytes, working, metadata});
}

Result<PanelDatasetBuildResult> build_panel_dataset(PanelDatasetSource& source, const PanelDatasetConfig& config,
    const std::string& directory) {
  ATX_TRY(auto sizing, preflight_panel_dataset(config)); (void)sizing;
  auto c = config; if (!c.maturity_end) c.maturity_end = c.session_keys.size();
  std::error_code ec;
  if (!fs::create_directory(directory, ec) || ec) return Err(ErrorCode::IoError, "dataset: fresh output directory and existing parent required");
  const auto n = c.instrument_ids.size(), f = c.feature_names.size();
  std::vector<f64> raw(n * f), close(n), previous(n, kNaN), entry(n), endpoint(n), volatility(n), labels(n);
  std::vector<f64> history(static_cast<usize>(c.volatility_window) * n, kNaN);
  std::vector<u8> present(n), member(n); std::vector<usize> order; order.reserve(n);
  usize history_dates = 0, next = 0;
  std::vector<BlockInfo> blocks; blocks.reserve((c.session_keys.size() + c.block_dates - 1) / c.block_dates);
  PanelDatasetBuildResult result; result.finite_labels.assign(c.holding_horizons.size(), 0);
  for (usize begin = 0; begin < c.session_keys.size(); begin += c.block_dates) {
    const auto dates = std::min(c.block_dates, c.session_keys.size() - begin);
    const auto l = layout(c, dates); std::vector<u8> buffer(static_cast<usize>(l.bytes), 0);
    std::memcpy(buffer.data(), "ATXMCB2\0", 8); put(buffer.data() + 8, begin); put(buffer.data() + 16, dates);
    put(buffer.data() + 24, n); put(buffer.data() + 32, 2 * f); put(buffer.data() + 40, c.holding_horizons.size());
    for (usize local = 0; local < dates; ++local) {
      const auto date = begin + local; i64 decision{};
      ATX_TRY_VOID(source.read_features(date, raw, present, member, decision));
      if (decision < 0 || decision >= c.session_keys[date]) return Err(ErrorCode::InvalidArgument, "dataset: membership clock must be strictly prior");
      for (usize i = 0; i < n; ++i) {
        if (present[i] > 1 || member[i] > 1 || (member[i] && decision == 0)) return Err(ErrorCode::InvalidArgument, "dataset: invalid masks/clock");
        buffer[l.present + local * n + i] = present[i]; buffer[l.member + local * n + i] = member[i];
        result.member_rows += member[i];
      }
      put(buffer.data() + l.clocks + local * 8, static_cast<u64>(decision));
      for (usize field = 0; field < f; ++field) rank_column(std::span<const f64>(raw).subspan(field * n, n), present, member, order,
          buffer.data() + 48 + field * l.feature_stride + local * n * 4,
          buffer.data() + 48 + (f + field) * l.feature_stride + local * n * 4);
      ATX_TRY_VOID(source.read_close(date, close));
      prior_volatility(history, history_dates, next, n, c, volatility);
      const usize entry_date = date + c.execution_delay;
      if (entry_date < c.maturity_end) {
        if (entry_date == date) entry = close;
        else { ATX_TRY_VOID(source.read_close(entry_date, entry)); }
      }
      for (usize h = 0; h < c.holding_horizons.size(); ++h) {
        const usize maturity = entry_date + c.holding_horizons[h];
        std::fill(labels.begin(), labels.end(), kNaN);
        if (maturity < c.maturity_end) {
          ATX_TRY_VOID(source.read_close(maturity, endpoint));
          usize finite = 0; f64 sum = 0, correction = 0;
          for (usize i = 0; i < n; ++i) {
            if (!member[i] || !std::isfinite(volatility[i]) || !std::isfinite(entry[i]) || entry[i] <= 0 ||
                !std::isfinite(endpoint[i]) || endpoint[i] <= 0) continue;
            const f64 value = (endpoint[i] / entry[i] - 1) / (volatility[i] * std::sqrt(static_cast<f64>(c.holding_horizons[h])));
            if (!std::isfinite(value)) continue;
            labels[i] = value; ++finite;
            const f64 adjusted = value - correction, updated = sum + adjusted;
            correction = (updated - sum) - adjusted; sum = updated;
          }
          const f64 mean = finite >= 2 ? sum / static_cast<f64>(finite) : kNaN;
          for (auto& value : labels) { value -= mean; if (!std::isfinite(value)) value = kNaN; }
        }
        for (usize i = 0; i < n; ++i) {
          put(buffer.data() + l.labels + h * l.label_stride + (local * n + i) * 8,
              std::isfinite(labels[i]) ? std::bit_cast<u64>(labels[i]) : kMissing);
          result.finite_labels[h] += std::isfinite(labels[i]) ? 1U : 0U;
        }
      }
      // Only after feature/label normalization may the return ending today enter
      // the ring, making it available to the NEXT date's volatility estimate.
      if (date != 0) {
        for (usize i = 0; i < n; ++i) {
          const f64 value = std::isfinite(close[i]) && close[i] > 0 && std::isfinite(previous[i]) && previous[i] > 0
              ? close[i] / previous[i] - 1 : kNaN;
          history[next * n + i] = std::isfinite(value) ? value : kNaN;
        }
        ++history_dates; next = (next + 1) % c.volatility_window;
      }
      previous = close;
    }
    ATX_TRY(auto sha, core::sha256_hex(std::as_bytes(std::span(buffer))));
    ATX_TRY_VOID(write_exclusive(fs::path(directory) / block_name(blocks.size()), buffer));
    blocks.push_back({begin, dates, l.bytes, std::move(sha)});
  }
  std::string manifest("ATXMLD2\0", 8); encode_config(manifest, c); number(manifest, result.member_rows);
  for (auto count : result.finite_labels) number(manifest, count);
  number(manifest, blocks.size());
  for (const auto& b : blocks) { number(manifest, b.begin); number(manifest, b.dates); number(manifest, b.bytes); text(manifest, b.sha); }
  ATX_TRY(auto body_sha, core::sha256_hex(manifest)); manifest += body_sha;
  ATX_TRY_VOID(write_exclusive(fs::path(directory) / "manifest.partial", std::span(reinterpret_cast<const u8*>(manifest.data()), manifest.size())));
  fs::create_hard_link(fs::path(directory) / "manifest.partial", fs::path(directory) / "manifest.bin", ec);
  if (ec) return Err(ErrorCode::IoError, "dataset: publish-last manifest failed");
  ATX_TRY(result.manifest_sha256, core::sha256_hex(manifest)); return Ok(std::move(result));
}

struct PanelDataset::Impl {
  fs::path directory; PanelDatasetConfig config; std::string sha; std::vector<BlockInfo> blocks;
  std::shared_ptr<Budget> budget;
};
struct PanelDatasetBlock::Impl {
  atx::tsdb::Mapping mapping; Layout layout; usize begin{}, dates{}, names{}, features{}, horizons{};
  std::shared_ptr<Budget> budget; u64 reserved{};
  ~Impl() {
    mapping = atx::tsdb::Mapping{}; // release pages/virtual address BEFORE budget credits
    if (budget) { const std::lock_guard lock(budget->mutex); budget->used -= reserved; --budget->count; }
  }
};
PanelDataset::PanelDataset(std::shared_ptr<Impl> p) : impl_(std::move(p)) {}
PanelDatasetBlock::PanelDatasetBlock(std::shared_ptr<const Impl> p) : impl_(std::move(p)) {}
const PanelDatasetConfig& PanelDataset::config() const noexcept { return impl_->config; }
std::string_view PanelDataset::manifest_sha256() const noexcept { return impl_->sha; }
usize PanelDataset::blocks() const noexcept { return impl_->blocks.size(); }
usize PanelDatasetBlock::begin_date() const noexcept { return impl_->begin; }
usize PanelDatasetBlock::dates() const noexcept { return impl_->dates; }
usize PanelDatasetBlock::instruments() const noexcept { return impl_->names; }
Result<std::span<const f32>> PanelDatasetBlock::feature(usize field) const {
  if (field >= impl_->features) return Err(ErrorCode::InvalidArgument, "dataset: feature index");
  return Ok(std::span(reinterpret_cast<const f32*>(impl_->mapping.base() + 48 + field * impl_->layout.feature_stride), impl_->dates * impl_->names));
}
Result<std::span<const f64>> PanelDatasetBlock::label(usize horizon) const {
  if (horizon >= impl_->horizons) return Err(ErrorCode::InvalidArgument, "dataset: label index");
  return Ok(std::span(reinterpret_cast<const f64*>(impl_->mapping.base() + impl_->layout.labels + horizon * impl_->layout.label_stride), impl_->dates * impl_->names));
}
std::span<const u8> PanelDatasetBlock::present() const noexcept { return {impl_->mapping.base() + impl_->layout.present, impl_->dates * impl_->names}; }
std::span<const u8> PanelDatasetBlock::member() const noexcept { return {impl_->mapping.base() + impl_->layout.member, impl_->dates * impl_->names}; }

Result<PanelDataset> PanelDataset::open(const std::string& directory, std::string_view expected, u64 max_bytes, u32 max_blocks) {
  if ((!expected.empty() && !valid_hash(expected)) || !max_bytes || !max_blocks || max_blocks > 256)
    return Err(ErrorCode::InvalidArgument, "dataset: reader identity/budget");
  std::ifstream input(fs::path(directory) / "manifest.bin", std::ios::binary | std::ios::ate);
  if (!input) return Err(ErrorCode::IoError, "dataset: no published manifest");
  const auto extent = static_cast<std::streamoff>(input.tellg());
  if (extent < 72 || static_cast<u64>(extent) > kMetadataLimit) return Err(ErrorCode::InvalidArgument, "dataset: manifest extent");
  std::string bytes(static_cast<usize>(extent), '\0'); input.seekg(0); input.read(bytes.data(), extent);
  if (!input || bytes.substr(0, 8) != std::string("ATXMLD2\0", 8)) return Err(ErrorCode::InvalidArgument, "dataset: manifest magic/read");
  ATX_TRY(auto sha, core::sha256_hex(bytes));
  const std::string_view body(bytes.data(), bytes.size() - 64);
  ATX_TRY(auto body_sha, core::sha256_hex(body));
  if ((!expected.empty() && sha != expected) || body_sha != bytes.substr(bytes.size() - 64)) return Err(ErrorCode::InvalidArgument, "dataset: manifest SHA mismatch");
  auto p = std::make_shared<Impl>(); p->directory = directory; p->sha = std::move(sha);
  try {
    Cursor r{body, 8}; p->config = decode_config(r);
    if (!p->config.maturity_end) throw std::invalid_argument("unresolved maturity endpoint");
    ATX_TRY(auto sizing, preflight_panel_dataset(p->config));
    if (sizing.largest_block_bytes > max_bytes) return Err(ErrorCode::InvalidArgument, "dataset: reader mapping budget too small");
    const auto cells = u64{p->config.session_keys.size()} * p->config.instrument_ids.size();
    if (r.number() > cells) throw std::invalid_argument("member count");
    for (usize h = 0; h < p->config.holding_horizons.size(); ++h) if (r.number() > cells) throw std::invalid_argument("label count");
    const auto count = r.count(100000);
    const auto expected_count = (p->config.session_keys.size() + p->config.block_dates - 1) / p->config.block_dates;
    if (count != expected_count) throw std::invalid_argument("block count");
    p->blocks.reserve(count); usize next = 0;
    for (usize i = 0; i < count; ++i) {
      const auto begin = r.number(), dates = r.number(), length = r.number(); auto hash = r.text(64);
      const auto expected_dates = std::min(p->config.block_dates, p->config.session_keys.size() - next);
      if (begin != next || dates != expected_dates || length != layout(p->config, expected_dates).bytes || !valid_hash(hash))
        throw std::invalid_argument("block geometry/hash");
      std::error_code ec; const auto actual = fs::file_size(p->directory / block_name(i), ec);
      if (ec || actual != length) throw std::invalid_argument("block extent changed");
      p->blocks.push_back({next, expected_dates, length, std::move(hash)}); next += expected_dates;
    }
    if (r.pos != body.size()) throw std::invalid_argument("trailing metadata");
  } catch (const std::invalid_argument& e) { return Err(ErrorCode::InvalidArgument, std::string("dataset: ") + e.what()); }
  p->budget = std::make_shared<Budget>(); p->budget->limit = max_bytes; p->budget->maximum = max_blocks;
  return Ok(PanelDataset(std::move(p)));
}

Result<PanelDatasetBlock> PanelDataset::open_block(usize index) const {
  if (index >= impl_->blocks.size()) return Err(ErrorCode::InvalidArgument, "dataset: block index");
  const auto& info = impl_->blocks[index]; auto p = std::make_shared<PanelDatasetBlock::Impl>();
  {
    const std::lock_guard lock(impl_->budget->mutex);
    if (impl_->budget->count == impl_->budget->maximum || info.bytes > impl_->budget->limit - impl_->budget->used)
      return Err(ErrorCode::InvalidArgument, "dataset: live mapping/handle budget exhausted");
    impl_->budget->used += info.bytes; ++impl_->budget->count; p->budget = impl_->budget; p->reserved = info.bytes;
  }
  ATX_TRY(p->mapping, atx::tsdb::Mapping::map_file_ro((impl_->directory / block_name(index)).string(), info.bytes, info.bytes));
  ATX_TRY(auto hash, core::sha256_hex(std::as_bytes(std::span(p->mapping.base(), p->mapping.size()))));
  if (hash != info.sha) return Err(ErrorCode::InvalidArgument, "dataset: block SHA mismatch");
  const auto& c = impl_->config; p->begin = info.begin; p->dates = info.dates; p->names = c.instrument_ids.size();
  p->features = c.feature_names.size() * 2; p->horizons = c.holding_horizons.size(); p->layout = layout(c, p->dates);
  const auto* bytes = p->mapping.base(); const auto cells = p->dates * p->names; const auto& l = p->layout;
  if (std::memcmp(bytes, "ATXMCB2\0", 8) != 0 || get(bytes + 8) != p->begin || get(bytes + 16) != p->dates ||
      get(bytes + 24) != p->names || get(bytes + 32) != p->features || get(bytes + 40) != p->horizons)
    return Err(ErrorCode::InvalidArgument, "dataset: block header mismatch");
  for (usize date = 0; date < p->dates; ++date) {
    const auto clock = std::bit_cast<i64>(get(bytes + l.clocks + date * 8));
    if (clock < 0 || clock >= c.session_keys[p->begin + date]) return Err(ErrorCode::InvalidArgument, "dataset: invalid stored membership clock");
    for (usize i = 0; i < p->names; ++i) {
      const auto cell = date * p->names + i;
      if (bytes[l.present + cell] > 1 || bytes[l.member + cell] > 1 || (bytes[l.member + cell] && clock == 0))
        return Err(ErrorCode::InvalidArgument, "dataset: invalid stored masks");
    }
  }
  for (usize f = 0; f < p->features; ++f) for (usize i = 0; i < cells; ++i) {
    const auto value = std::bit_cast<f32>(static_cast<u32>(get(bytes + 48 + f * l.feature_stride + i * 4, 4)));
    if (!std::isfinite(value) || (f < c.feature_names.size() ? std::abs(value) > .5F : (value != 0 && value != 1)))
      return Err(ErrorCode::InvalidArgument, "dataset: invalid stored rank/indicator");
  }
  for (usize h = 0; h < p->horizons; ++h) for (usize i = 0; i < cells; ++i) {
    const auto bits = get(bytes + l.labels + h * l.label_stride + i * 8); const auto value = std::bit_cast<f64>(bits);
    const auto maturity = p->begin + i / p->names + c.execution_delay + c.holding_horizons[h];
    if ((!std::isfinite(value) && bits != kMissing) || (std::isfinite(value) && (!bytes[l.member + i] || maturity >= c.maturity_end)))
      return Err(ErrorCode::InvalidArgument, "dataset: invalid stored label/maturity");
  }
  return Ok(PanelDatasetBlock(std::move(p)));
}

} // namespace atx::engine::learn
