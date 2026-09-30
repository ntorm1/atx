#include "atx/engine/data/strategy_data.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/data/research_window.hpp"

namespace atx::engine::data {
namespace {
using Json = nlohmann::json;
constexpr i64 kDay = 86'400'000'000'000LL;
constexpr u64 kManifestLimit = 1ULL << 20;
// research_window.hpp: nothing at or after the seal is read; the refusal names the window.
std::string seal_refusal(std::string_view what) {
  return "strategy role: " + std::string(what) + " at or after the research seal " +
         std::string(kSealBeginDate) + " (" + std::string(kResearchWindowId) + ")";
}
bool hash_valid(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
std::string hex(const std::array<std::byte, 32>& bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out(64, '0');
  for (usize i = 0; i < bytes.size(); ++i) {
    const auto b = std::to_integer<unsigned>(bytes[i]);
    out[2 * i] = digits[b >> 4]; out[2 * i + 1] = digits[b & 15];
  }
  return out;
}
template<class T> core::Status read_vector(const std::filesystem::path& base,
    const Json& files, std::string_view filename, std::vector<T>& out, usize count) {
  const auto name = std::string(filename);
  const auto& receipt = files.at(name);
  const auto bytes = static_cast<u64>(count) * sizeof(T);
  const auto expected = receipt.at("sha256").get<std::string>();
  if (receipt.at("bytes").get<u64>() != bytes || !hash_valid(expected))
    return core::Err(core::ErrorCode::InvalidArgument, "strategy role: invalid file receipt");
  std::ifstream f(base / name, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() < 0 || static_cast<u64>(f.tellg()) != bytes)
    return core::Err(core::ErrorCode::IoError, "strategy role: captured file extent mismatch: " + name);
  f.seekg(0); out.resize(count);
  auto destination = std::as_writable_bytes(std::span(out));
  core::Sha256 sha;
  for (usize offset = 0; offset < destination.size();) {
    const auto size = std::min(usize{64 * 1024}, destination.size() - offset);
    auto chunk = destination.subspan(offset, size);
    f.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(size));
    if (!f) return core::Err(core::ErrorCode::IoError, "strategy role: truncated file: " + name);
    ATX_TRY_VOID(sha.update(chunk)); offset += size;
  }
  if (f.peek() != std::char_traits<char>::eof())
    return core::Err(core::ErrorCode::IoError, "strategy role: changed file extent: " + name);
  ATX_TRY(auto digest, sha.finalize());
  if (hex(digest) != expected)
    return core::Err(core::ErrorCode::InvalidArgument, "strategy role: payload SHA mismatch: " + name);
  return core::Ok();
}
} // namespace

core::Result<StrategyRoleData> read_strategy_role(const std::string& path, u64 max_bytes) {
  try {
    if constexpr (std::endian::native != std::endian::little)
      return core::Err(core::ErrorCode::Unavailable, "strategy role: little-endian host required");
    // Includes parsed manifest, string/vector headers, file/hash scratch and axes.
    constexpr u64 overhead = 16ULL << 20;
    if (max_bytes < overhead)
      return core::Err(core::ErrorCode::Unavailable, "strategy role: metadata budget");
    std::ifstream manifest(path, std::ios::binary | std::ios::ate);
    if (!manifest || manifest.tellg() <= 0 || static_cast<u64>(manifest.tellg()) > kManifestLimit)
      return core::Err(core::ErrorCode::InvalidArgument, "strategy role: missing/oversized manifest");
    std::string text(static_cast<usize>(manifest.tellg()), '\0'); manifest.seekg(0);
    manifest.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!manifest || manifest.peek() != std::char_traits<char>::eof())
      return core::Err(core::ErrorCode::IoError, "strategy role: changed manifest extent");
    const auto j = Json::parse(text);
    if (j.at("schema") != "atx.recent-research-role/v1" ||
        j.at("status") != "complete" || j.at("instrument_namespace") != "spiderrock.securityID" ||
        j.at("close_basis") != "f64(raw-f32-close)*f64-cumulReturnFactor" ||
        j.at("volume_basis") != "raw-share-volume" ||
        j.at("clock_recipe") != "modeled-session+22h-mark+23h-decision-v1" ||
        j.at("common_stock_verified") != false || j.at("historical_vintage_verified") != false)
      return core::Err(core::ErrorCode::InvalidArgument, "strategy role: unsupported research recipe");
    const auto dates = j.at("dates").get<u64>(), names = j.at("instruments").get<u64>();
    if (!dates || dates > 4096 || !names || names > 20000 ||
        dates * names > (max_bytes - overhead) / 26 ||
        dates * names * 26 > max_bytes - overhead - std::min(max_bytes - overhead, dates * 24 + names * 8))
      return core::Err(core::ErrorCode::Unavailable, "strategy role: owned panel budget");
    const auto d = static_cast<usize>(dates), n = static_cast<usize>(names), cells = d * n;
    ATX_TRY(auto empty_panel, alpha::Panel::create(0, 0, {}, {}, {}));
    StrategyRoleData out{std::move(empty_panel)};
    out.score_begin = j.at("score_begin").get<usize>(); out.score_end = j.at("score_end").get<usize>();
    const auto start = j.at("score_start_ns").get<i64>(), end = j.at("score_end_ns").get<i64>();
    if (out.score_begin < 383 || out.score_begin >= out.score_end || out.score_end != d ||
        start <= 0 || end <= start)
      return core::Err(core::ErrorCode::InvalidArgument, "strategy role: warmup/score contract");
    if (end > kSealBeginNs)
      return core::Err(core::ErrorCode::InvalidArgument, seal_refusal("score end"));
    out.source_sha256 = j.at("source_sha256").get<std::string>();
    out.membership_recipe = j.at("membership_recipe").get<std::string>();
    out.clock_recipe = j.at("clock_recipe").get<std::string>();
    if (!hash_valid(out.source_sha256) || out.membership_recipe.empty() || out.membership_recipe.size() > 4096)
      return core::Err(core::ErrorCode::InvalidArgument, "strategy role: source/membership identity");
    const auto membership = Json::parse(out.membership_recipe);
    const auto top_n = membership.at("top_n").get<usize>();
    if (membership.at("rule") != "research-prior63-usd-adv-topn-v1" ||
        membership.at("lookback_sessions") != 63 || membership.at("lag_sessions") != 1 ||
        membership.at("min_raw_price_exclusive") != 5 || membership.at("min_adv_exclusive") != 5'000'000 ||
        membership.at("ties") != "securityID-ascending" ||
        membership.at("missing") != "complete-prior-calendar-window-required" ||
        membership.at("common_stock_verified") != false || top_n < 2 || top_n > 20000 ||
        j.at("declared_output_bytes").get<u64>() != dates * names * 26 + dates * 8 + names * 8)
      return core::Err(core::ErrorCode::InvalidArgument, "strategy role: unknown membership/byte recipe");
    ATX_TRY(out.manifest_sha256, core::sha256_hex(text));
    const auto base = std::filesystem::path(path).parent_path(); const auto& files = j.at("files");
    ATX_TRY_VOID(read_vector(base, files, "sessions.i64", out.session_keys, d));
    ATX_TRY_VOID(read_vector(base, files, "ids.u64", out.instrument_ids, n));
    if (out.session_keys.front() <= 0 || out.session_keys.back() >= end ||
        std::adjacent_find(out.session_keys.begin(), out.session_keys.end(), std::greater_equal<i64>{}) != out.session_keys.end() ||
        out.instrument_ids.front() == 0 ||
        std::adjacent_find(out.instrument_ids.begin(), out.instrument_ids.end(), std::greater_equal<u64>{}) != out.instrument_ids.end() ||
        static_cast<usize>(std::lower_bound(out.session_keys.begin(), out.session_keys.end(), start) - out.session_keys.begin()) != out.score_begin)
      return core::Err(core::ErrorCode::InvalidArgument, "strategy role: invalid axes/score boundary");
    out.mark_times_ns.reserve(d); out.decision_times_ns.reserve(d);
    for (const auto session : out.session_keys) {
      if (session % kDay != 0)
        return core::Err(core::ErrorCode::InvalidArgument, "strategy role: non-session axis");
      if (is_sealed(session))
        return core::Err(core::ErrorCode::InvalidArgument, seal_refusal("session"));
      out.mark_times_ns.push_back(session + 22 * kDay / 24);
      out.decision_times_ns.push_back(session + 23 * kDay / 24);
    }
    std::vector<u8> present;
    ATX_TRY_VOID(read_vector(base, files, "present.u8", present, cells));
    ATX_TRY_VOID(read_vector(base, files, "member.u8", out.decision_member, cells));
    std::vector<std::vector<f64>> fields(3);
    constexpr std::array<std::string_view, 3> filenames{"close.f64", "raw_close.f64", "volume.f64"};
    for (usize f = 0; f < 3; ++f) ATX_TRY_VOID(read_vector(base, files, filenames[f], fields[f], cells));
    for (usize i = 0; i < cells; ++i) {
      if (present[i] > 1 || out.decision_member[i] > 1)
        return core::Err(core::ErrorCode::InvalidArgument, "strategy role: non-binary mask");
      for (usize f = 0; f < 3; ++f) {
        const auto x = fields[f][i];
        if ((!present[i] && !std::isnan(x)) || (present[i] &&
            (!std::isfinite(x) || (f == 2 ? x < 0 : x <= 0))))
          return core::Err(core::ErrorCode::InvalidArgument, "strategy role: missing/finite field contract");
      }
    }
    for (usize t = 0; t < d; ++t) {
      const auto first = out.decision_member.begin() + static_cast<std::ptrdiff_t>(t * n);
      const auto count = static_cast<usize>(std::count(first, first + static_cast<std::ptrdiff_t>(n), u8{1}));
      if (count > top_n || (t < 63 && count != 0))
        return core::Err(core::ErrorCode::InvalidArgument, "strategy role: membership count/warmup mismatch");
    }
    ATX_TRY(out.panel, alpha::Panel::create(d, n, {"close", "raw_close", "volume"}, std::move(fields), std::move(present)));
    return out;
  } catch (const std::exception& e) {
    return core::Err(core::ErrorCode::InvalidArgument, std::string("strategy role: ") + e.what());
  }
}

core::Result<bool> role_delisting_returns_applied(std::string_view manifest_text) {
  const auto malformed = [](const char* what) {
    return core::Err(core::ErrorCode::InvalidArgument, std::string("strategy role: manifest ") + what);
  };
  try {
    const auto j = Json::parse(manifest_text.begin(), manifest_text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return malformed("is not a JSON object");
    const auto universe = j.find("universe");
    if (universe == j.end()) return core::Ok(false);
    if (!universe->is_object()) return malformed("universe is not an object");
    const auto delisting = universe->find("delisting");
    if (delisting == universe->end()) return core::Ok(false);
    if (!delisting->is_object()) return malformed("universe.delisting is not an object");
    const auto applied = delisting->find("returns_applied");
    if (applied == delisting->end() || !applied->is_boolean())
      return malformed("universe.delisting lacks a boolean returns_applied");
    return core::Ok(applied->get<bool>());
  } catch (const std::exception& e) {
    return core::Err(core::ErrorCode::InvalidArgument, std::string("strategy role: ") + e.what());
  }
}

core::Status refuse_delisting_returns_signal_role(std::string_view manifest_text, std::string_view manifest_path) {
  const auto applied = role_delisting_returns_applied(manifest_text);
  if (!applied)
    return core::Err(applied.error().code(), applied.error().message() + ": " + std::string(manifest_path));
  if (!*applied) return core::Ok();
  return core::Err(core::ErrorCode::InvalidArgument,
      "strategy role " + std::string(manifest_path) + " was built with --delisting-returns "
      "(universe.delisting.returns_applied true): its imputed terminal returns may mark the NAV replay's "
      "books only (Ruling E-10), so it is refused as a signal role");
}
} // namespace atx::engine::data
