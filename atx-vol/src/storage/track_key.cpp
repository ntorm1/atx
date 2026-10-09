#include "storage/track_key.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"
#include "backtest/backtest_template.hpp" // fingerprint_backtest_template
#include "storage/run_archive_schema.hpp" // ra_schema_hash
#include "atx/vol/api/core/version.hpp" // kVersionString

namespace atx::vol {
namespace {

// Bumped only if THIS encoder's byte layout changes for a reason unrelated to
// economics (a bug fix in field ordering, a new field appended). Kept
// separate from kBacktestEconomicsRev, which is reserved for changes that
// move the golden NAV.
constexpr std::uint32_t kCanonicalConfigEncodingVersion = 1;

// Appends fields as raw little-endian bytes, one field at a time -- never a
// struct memcpy, so no compiler-inserted padding byte (uninitialized, and
// therefore nondeterministic across builds/compilers) ever reaches the hash.
class CanonicalWriter {
public:
  void write_u8(std::uint8_t value) { bytes_.push_back(value); }

  void write_bool(bool value) { write_u8(value ? 1U : 0U); }

  void write_u32(std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
      write_u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  }

  void write_u64(std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
      write_u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  }

  void write_i64(std::int64_t value) { write_u64(static_cast<std::uint64_t>(value)); }

  // Signed zero normalized so -0.0 and 0.0 (the same economic value) always
  // encode identically. NaN payload bits pass through unchanged -- RunConfig
  // validation, not this encoder, is responsible for rejecting a NaN
  // economics field before it ever reaches make_track_key.
  void write_double(double value) {
    write_u64(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
  }

  template <class Enum> void write_enum(Enum value) {
    static_assert(std::is_enum_v<Enum>);
    write_u64(static_cast<std::uint64_t>(static_cast<std::underlying_type_t<Enum>>(value)));
  }

  void write_string(std::string_view value) {
    write_u64(static_cast<std::uint64_t>(value.size()));
    for (const char ch : value) {
      write_u8(static_cast<std::uint8_t>(ch));
    }
  }

  void write_bytes(std::span<const std::uint8_t> value) {
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }

  [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(bytes_); }

private:
  std::vector<std::uint8_t> bytes_;
};

void write_friction_model(CanonicalWriter &writer, const FrictionModel &frictions) {
  writer.write_enum(frictions.spread_kind);
  writer.write_double(frictions.half_spread_bps);
  writer.write_double(frictions.vol_tick);
  writer.write_double(frictions.impact_fraction);
  writer.write_double(frictions.per_contract_cost);
  writer.write_double(frictions.hedge_slippage_bps);
  writer.write_double(frictions.crossing_fraction_single);
  writer.write_double(frictions.crossing_fraction_complex);
  // The callable itself cannot be content-hashed; whether one is wired up at
  // all changes QuoteSide fill behaviour (see FrictionModel::quote_lookup's
  // doc), so its presence still belongs in the economics identity.
  writer.write_bool(static_cast<bool>(frictions.quote_lookup));
}

void write_financing_config(CanonicalWriter &writer, const FinancingConfig &financing) {
  writer.write_double(financing.borrow_rate);
  writer.write_bool(financing.finance_premium);
  writer.write_bool(financing.shares_carry);
  writer.write_double(financing.initial_cash);
  writer.write_u64(static_cast<std::uint64_t>(financing.share_dividends.size()));
  for (const ShareDividend &dividend : financing.share_dividends) {
    writer.write_u32(dividend.uid);
    writer.write_i64(dividend.ex_ts_ns);
    writer.write_double(dividend.amount);
  }
  writer.write_u32(financing.reference_uid);
  writer.write_bool(financing.flat_r.has_value());
  writer.write_double(financing.flat_r.value_or(0.0));
}

[[nodiscard]] std::string hex_u64(std::uint64_t value) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (std::size_t i = 0; i < 16; ++i) {
    const unsigned shift = static_cast<unsigned>((15 - i) * 4);
    out[i] = kHexDigits[(value >> shift) & 0xFU];
  }
  return out;
}

[[nodiscard]] std::array<std::uint8_t, 32> sha256_digest(std::span<const std::uint8_t> bytes) noexcept {
  atx::core::Sha256 hasher;
  [[maybe_unused]] const atx::core::Status update_status = hasher.update(std::as_bytes(bytes));
  assert(update_status.has_value() &&
        "Sha256::update cannot fail for a bounded canonical_config buffer");
  const atx::core::Result<std::array<std::byte, 32>> finalized = hasher.finalize();
  assert(finalized.has_value() && "Sha256::finalize cannot fail after exactly one update()");
  std::array<std::uint8_t, 32> digest{};
  if (finalized.has_value()) {
    for (std::size_t i = 0; i < digest.size(); ++i) {
      digest[i] = std::to_integer<std::uint8_t>((*finalized)[i]);
    }
  }
  return digest;
}

} // namespace

std::string TrackKey::hex() const {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string out(sha256.size() * 2, '0');
  for (std::size_t i = 0; i < sha256.size(); ++i) {
    out[i * 2] = kHexDigits[(sha256[i] >> 4U) & 0x0FU];
    out[i * 2 + 1] = kHexDigits[sha256[i] & 0x0FU];
  }
  return out;
}

namespace {

// -1 for anything outside '0'-'9'/'a'-'f' -- deliberately excludes 'A'-'F':
// hex() never emits uppercase, so an uppercase input did not round-trip
// through it (see track_key_from_hex's doc comment).
[[nodiscard]] int lowercase_hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

} // namespace

atx::core::Result<TrackKey> track_key_from_hex(std::string_view hex) {
  TrackKey key;
  if (hex.size() != key.sha256.size() * 2) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "track_key_from_hex: expected exactly " +
                              std::to_string(key.sha256.size() * 2) + " hex characters, got " +
                              std::to_string(hex.size()));
  }
  for (std::size_t i = 0; i < key.sha256.size(); ++i) {
    const int hi = lowercase_hex_digit(hex[i * 2]);
    const int lo = lowercase_hex_digit(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "track_key_from_hex: non-lowercase-hex character at offset " +
                                std::to_string(i * 2));
    }
    key.sha256[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return atx::core::Ok(key);
}

std::vector<std::uint8_t> canonical_config_bytes(const BacktestStrategyTemplate &strategy_template,
                                                  const RunConfig &run_config) {
  CanonicalWriter writer;
  writer.write_u32(kCanonicalConfigEncodingVersion);
  // BacktestStrategyTemplate's own economic fingerprint already covers every
  // template field that matters (backtest_template.cpp); reused rather than
  // re-derived so there is exactly one definition of "the template's
  // economics." Returns 0 for an invalid template -- callers are expected to
  // validate_backtest_template() before reaching this point, same contract
  // fingerprint_backtest_template itself already documents.
  writer.write_u64(fingerprint_backtest_template(strategy_template));

  writer.write_enum(run_config.query_pricing_tier);
  writer.write_enum(run_config.query_cache_build_policy);
  write_friction_model(writer, run_config.frictions);
  write_financing_config(writer, run_config.financing);
  writer.write_enum(run_config.unpriced);
  writer.write_enum(run_config.surface_provenance_policy);
  // FIX-ROUND 1 (post-review): reconcile_nav's fail-closed abort
  // (reconcile_row, backtest.cpp:3415-3429, propagated via ATX_TRY_VOID at
  // backtest.cpp:3804/:4168) means it belongs beside clock_gaps/margin_breach
  // below, not with the execution-only fields -- see track_key.hpp's
  // INCLUDED entry for the full rationale.
  writer.write_bool(run_config.reconcile_nav);
  writer.write_bool(run_config.book_entry_fill_slippage);
  writer.write_double(run_config.reconcile_nav_tol);
  writer.write_enum(run_config.swap_fixing_cadence);
  writer.write_enum(run_config.clock_gaps);
  writer.write_enum(run_config.margin_breach);
  writer.write_enum(run_config.exercise_policy);

  return std::move(writer).take();
}

std::string make_engine_id() {
  std::string id;
  id.reserve(kVersionString.size() + 40);
  id.append(kVersionString);
  id.push_back('|');
  id.append(std::to_string(kBacktestEconomicsRev));
  id.push_back('|');
  id.append(hex_u64(ra_schema_hash()));
  return id;
}

TrackKey make_track_key(std::span<const std::uint8_t> canonical_config, std::string_view engine_id,
                        std::span<const std::uint8_t, 32> data_snapshot_id) {
  CanonicalWriter framing;
  framing.write_u64(static_cast<std::uint64_t>(canonical_config.size()));
  framing.write_bytes(canonical_config);
  framing.write_string(engine_id);
  framing.write_bytes(data_snapshot_id); // fixed 32 bytes; no length prefix needed

  const std::vector<std::uint8_t> material = std::move(framing).take();

  TrackKey key;
  key.sha256 = sha256_digest(material);
  return key;
}

} // namespace atx::vol
