#include "serialize_panel.hpp"

#include <cstring>
#include <fstream>
#include <vector>

#include "artifacts.hpp"          // atx::impl::fnv1a64
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace atx::impl {

namespace {

// Magic bytes: "APNL" stored as a u32 little-endian in the file.
// 'A'=0x41, 'P'=0x50, 'N'=0x4E, 'L'=0x4C -> LE u32 = 0x4C4E5041
constexpr atx::u32 kMagic   = 0x4C4E5041u;
constexpr atx::u32 kVersion = 1u;

// ---------------------------------------------------------------------------
// Little-endian append helpers (no UB: memcpy for all types).
// ---------------------------------------------------------------------------

void put_u32(std::vector<unsigned char>& buf, atx::u32 v) {
    unsigned char b[4];
    b[0] = static_cast<unsigned char>(v & 0xFFu);
    b[1] = static_cast<unsigned char>((v >> 8)  & 0xFFu);
    b[2] = static_cast<unsigned char>((v >> 16) & 0xFFu);
    b[3] = static_cast<unsigned char>((v >> 24) & 0xFFu);
    buf.insert(buf.end(), b, b + 4);
}

void put_u64(std::vector<unsigned char>& buf, atx::u64 v) {
    unsigned char b[8];
    b[0] = static_cast<unsigned char>(v & 0xFFu);
    b[1] = static_cast<unsigned char>((v >> 8)  & 0xFFu);
    b[2] = static_cast<unsigned char>((v >> 16) & 0xFFu);
    b[3] = static_cast<unsigned char>((v >> 24) & 0xFFu);
    b[4] = static_cast<unsigned char>((v >> 32) & 0xFFu);
    b[5] = static_cast<unsigned char>((v >> 40) & 0xFFu);
    b[6] = static_cast<unsigned char>((v >> 48) & 0xFFu);
    b[7] = static_cast<unsigned char>((v >> 56) & 0xFFu);
    buf.insert(buf.end(), b, b + 8);
}

// Append a double's bit pattern as 8 LE bytes (no reinterpret_cast: use memcpy).
void put_f64(std::vector<unsigned char>& buf, atx::f64 v) {
    atx::u64 bits{};
    std::memcpy(&bits, &v, 8);
    put_u64(buf, bits);
}

// ---------------------------------------------------------------------------
// Little-endian read helpers with bounds checking.
// ---------------------------------------------------------------------------

using Cursor = atx::usize;

[[nodiscard]] atx::core::Result<atx::u32>
get_u32(const std::vector<unsigned char>& buf, Cursor& pos) {
    if (pos + 4 > buf.size()) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "panel file truncated (u32 read)");
    }
    atx::u32 v =
        static_cast<atx::u32>(buf[pos])
        | (static_cast<atx::u32>(buf[pos + 1]) << 8)
        | (static_cast<atx::u32>(buf[pos + 2]) << 16)
        | (static_cast<atx::u32>(buf[pos + 3]) << 24);
    pos += 4;
    return atx::core::Ok(v);
}

[[nodiscard]] atx::core::Result<atx::u64>
get_u64(const std::vector<unsigned char>& buf, Cursor& pos) {
    if (pos + 8 > buf.size()) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "panel file truncated (u64 read)");
    }
    atx::u64 v =
        static_cast<atx::u64>(buf[pos])
        | (static_cast<atx::u64>(buf[pos + 1]) << 8)
        | (static_cast<atx::u64>(buf[pos + 2]) << 16)
        | (static_cast<atx::u64>(buf[pos + 3]) << 24)
        | (static_cast<atx::u64>(buf[pos + 4]) << 32)
        | (static_cast<atx::u64>(buf[pos + 5]) << 40)
        | (static_cast<atx::u64>(buf[pos + 6]) << 48)
        | (static_cast<atx::u64>(buf[pos + 7]) << 56);
    pos += 8;
    return atx::core::Ok(v);
}

[[nodiscard]] atx::core::Result<atx::f64>
get_f64(const std::vector<unsigned char>& buf, Cursor& pos) {
    ATX_TRY(auto bits, get_u64(buf, pos));
    atx::f64 v{};
    std::memcpy(&v, &bits, 8);
    return atx::core::Ok(v);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// write_panel
// ---------------------------------------------------------------------------

atx::core::Result<atx::u64>
write_panel(const atx::engine::alpha::Panel& panel, const std::string& path) {
    using namespace atx::engine::alpha;

    const atx::usize D = panel.dates();
    const atx::usize I = panel.instruments();
    const atx::usize F = panel.num_fields();

    // Build the payload into an in-memory buffer.
    std::vector<unsigned char> buf;
    // Rough capacity estimate to reduce reallocations.
    buf.reserve(8 + 8 + 8 + 8    // magic + version + dates + instruments + num_fields
                + F * 64          // field names (estimate)
                + F * D * I * 8   // field data
                + D * I);         // universe mask

    // 1. Header: magic, version, dates, instruments, num_fields.
    put_u32(buf, kMagic);
    put_u32(buf, kVersion);
    put_u64(buf, static_cast<atx::u64>(D));
    put_u64(buf, static_cast<atx::u64>(I));
    put_u64(buf, static_cast<atx::u64>(F));

    // 2. Per-field: u32 name_len + name bytes (no NUL).
    for (atx::usize f = 0; f < F; ++f) {
        const std::string& name = panel.field_name(f);
        put_u32(buf, static_cast<atx::u32>(name.size()));
        for (char c : name) {
            buf.push_back(static_cast<unsigned char>(c));
        }
    }

    // 3. Per-field column: dates*instruments f64 values (date-major).
    for (atx::usize f = 0; f < F; ++f) {
        std::span<const atx::f64> col = panel.field_all(static_cast<FieldId>(f));
        for (atx::f64 v : col) {
            put_f64(buf, v);
        }
    }

    // 4. Universe mask: dates*instruments u8 bytes.
    for (atx::usize d = 0; d < D; ++d) {
        for (atx::usize i = 0; i < I; ++i) {
            buf.push_back(panel.in_universe(d, i) ? static_cast<unsigned char>(1)
                                                   : static_cast<unsigned char>(0));
        }
    }

    // Compute digest over payload.
    const atx::u64 digest = fnv1a64(buf.data(), buf.size());

    // Append 8-byte LE trailer.
    put_u64(buf, digest);

    // Write to file.
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "write_panel: cannot open '" + path + "' for writing");
    }
    ofs.write(reinterpret_cast<const char*>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    if (!ofs) {
        return atx::core::Err(atx::core::ErrorCode::IoError,
                              "write_panel: write failed for '" + path + "'");
    }

    return atx::core::Ok(digest);
}

// ---------------------------------------------------------------------------
// read_panel
// ---------------------------------------------------------------------------

atx::core::Result<atx::engine::alpha::Panel>
read_panel(const std::string& path) {
    using namespace atx::engine::alpha;

    // Read entire file into memory.
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "read_panel: cannot open '" + path + "'");
    }
    const auto file_size = static_cast<atx::usize>(ifs.tellg());
    if (file_size < 8) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "read_panel: file too small (no trailer)");
    }
    ifs.seekg(0);
    std::vector<unsigned char> buf(file_size);
    ifs.read(reinterpret_cast<char*>(buf.data()),
             static_cast<std::streamsize>(file_size));
    if (!ifs) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "read_panel: read failed for '" + path + "'");
    }

    // Payload is everything except the last 8-byte trailer.
    const atx::usize payload_size = file_size - 8u;

    // Read trailer digest (last 8 bytes, LE).
    atx::u64 stored_digest{};
    {
        Cursor tc = payload_size;
        ATX_TRY(stored_digest, get_u64(buf, tc));
    }

    // Recompute payload digest.
    const atx::u64 computed_digest = fnv1a64(buf.data(), payload_size);
    if (computed_digest != stored_digest) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "read_panel: panel digest mismatch");
    }

    // Parse payload.
    Cursor pos = 0;

    // 1. Header.
    ATX_TRY(auto magic,   get_u32(buf, pos));
    ATX_TRY(auto version, get_u32(buf, pos));
    if (magic != kMagic) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "read_panel: bad magic number");
    }
    if (version != kVersion) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "read_panel: unsupported version");
    }
    ATX_TRY(auto dates_u64,       get_u64(buf, pos));
    ATX_TRY(auto instruments_u64, get_u64(buf, pos));
    ATX_TRY(auto num_fields_u64,  get_u64(buf, pos));

    const atx::usize D = static_cast<atx::usize>(dates_u64);
    const atx::usize I = static_cast<atx::usize>(instruments_u64);
    const atx::usize F = static_cast<atx::usize>(num_fields_u64);
    const atx::usize cells = D * I;

    // 2. Field names.
    std::vector<std::string> names;
    names.reserve(F);
    for (atx::usize f = 0; f < F; ++f) {
        ATX_TRY(auto name_len, get_u32(buf, pos));
        if (pos + static_cast<atx::usize>(name_len) > payload_size) {
            return atx::core::Err(atx::core::ErrorCode::ParseError,
                                  "read_panel: field name extends beyond payload");
        }
        names.emplace_back(reinterpret_cast<const char*>(buf.data() + pos),
                           static_cast<atx::usize>(name_len));
        pos += static_cast<atx::usize>(name_len);
    }

    // 3. Field columns (dates*instruments f64 each).
    std::vector<std::vector<atx::f64>> columns;
    columns.reserve(F);
    for (atx::usize f = 0; f < F; ++f) {
        std::vector<atx::f64> col;
        col.reserve(cells);
        for (atx::usize c = 0; c < cells; ++c) {
            ATX_TRY(auto v, get_f64(buf, pos));
            col.push_back(v);
        }
        columns.push_back(std::move(col));
    }

    // 4. Universe mask.
    if (pos + cells > payload_size) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "read_panel: universe mask extends beyond payload");
    }
    std::vector<std::uint8_t> universe(buf.data() + pos, buf.data() + pos + cells);
    pos += cells;

    if (pos != payload_size) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "read_panel: trailing bytes before digest trailer");
    }

    // Reconstruct Panel.
    return Panel::create(D, I, std::move(names), std::move(columns), std::move(universe));
}

} // namespace atx::impl
