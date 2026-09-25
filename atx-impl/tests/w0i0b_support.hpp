// W0-I0b test support: a byte-exact membership.bin writer (the layout documented in
// atx/engine/data/point_in_time_universe.hpp, §4.7) and CLI helpers. Synthetic
// fixtures only; nothing here is market data.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "atx/core/types.hpp"
#include "atx/engine/data/point_in_time_universe.hpp"
#include "config.hpp"
#include "dispatch.hpp"

namespace atx_test_w0_i0b_support {

// One rebalance of a single-cut image: members effective from `effective_key`.
struct Rebalance {
    atx::i64 rank_key{};
    atx::i64 effective_key{};
    std::vector<atx::i64> ids; // strictly ascending
};

inline void put_u32(std::string &out, std::uint32_t v) {
    char bytes[4];
    std::memcpy(bytes, &v, 4);
    out.append(bytes, 4);
}
inline void put_i64(std::string &out, std::int64_t v) {
    char bytes[8];
    std::memcpy(bytes, &v, 8);
    out.append(bytes, 8);
}
inline void put_u64(std::string &out, std::uint64_t v) {
    char bytes[8];
    std::memcpy(bytes, &v, 8);
    out.append(bytes, 8);
}
inline void put_f64(std::string &out, double v) {
    char bytes[8];
    std::memcpy(bytes, &v, 8);
    out.append(bytes, 8);
}

// Encode a one-cut (top_n, band_bp) image. Little-endian host (x64) assumed, as the
// codec itself documents little-endian throughout.
inline std::string encode_membership(std::uint32_t top_n, std::uint32_t band_bp,
                                     const std::vector<Rebalance> &rebalances) {
    std::string out(atx::engine::data::kPitMembershipMagic);
    put_u32(out, atx::engine::data::kPitMembershipVersion);
    put_u32(out, 63);  // adv_window
    put_u32(out, 42);  // min_valid_observations
    put_f64(out, 1.0); // min_raw_price_exclusive
    put_u32(out, 1);
    put_u32(out, top_n);
    put_u32(out, 1);
    put_u32(out, band_bp);
    put_u32(out, static_cast<std::uint32_t>(rebalances.size()));
    for (const auto &r : rebalances) {
        put_i64(out, r.rank_key);
        put_i64(out, r.effective_key);
        put_u32(out, static_cast<std::uint32_t>(r.ids.size()));
        for (const auto id : r.ids) put_i64(out, id);
        for (std::size_t i = 0; i < r.ids.size(); ++i) put_u32(out, static_cast<std::uint32_t>(i + 1));
    }
    put_u64(out, atx::engine::data::pit_fnv1a64(out));
    return out;
}

inline void write_bytes(const std::filesystem::path &path, const std::string &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline auto parse(std::vector<std::string> arguments) {
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    return atx::impl::parse_args(static_cast<int>(argv.size()), argv.data());
}

struct DispatchOutcome {
    int code{};
    std::string out;
    std::string err;
};

inline DispatchOutcome dispatch(std::vector<std::string> arguments) {
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    std::ostringstream out, err;
    const int code =
        atx::impl::dispatch(static_cast<int>(argv.size()), argv.data(), out, err);
    return {code, out.str(), err.str()};
}

} // namespace atx_test_w0_i0b_support
