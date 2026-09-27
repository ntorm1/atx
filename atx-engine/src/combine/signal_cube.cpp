#include "atx/engine/combine/signal_cube.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <numeric>
#include <set>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/combine/signal_store.hpp"

namespace atx::engine::combine {
namespace {
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;
using atx::f64;
using atx::u64;
using atx::usize;
using nlohmann::json;
namespace fs = std::filesystem;
constexpr usize kStats = 14;
constexpr f64 kNan = std::numeric_limits<f64>::quiet_NaN();
constexpr std::int16_t kMissing = std::numeric_limits<std::int16_t>::min();
constexpr u64 kManifestLimit = 16ULL * 1024ULL * 1024ULL;

bool mul(u64 a, u64 b, u64& out) noexcept {
    if (b != 0 && a > std::numeric_limits<u64>::max() / b) return false;
    out = a * b;
    return true;
}
bool add(u64 a, u64 b, u64& out) noexcept {
    if (a > std::numeric_limits<u64>::max() - b) return false;
    out = a + b;
    return true;
}
bool hash_text(std::string_view s) noexcept {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
usize stat_width(const SignalCubeConfig& c) noexcept {
    return c.stat_precision == CubeStatPrecision::ExactF64V1 ? 8U : 4U;
}
Result<usize> stat_slot(CubeStat stat, usize horizon) {
    if (horizon >= 4) return Err(ErrorCode::InvalidArgument, "signal cube: horizon index out of range");
    switch (stat) {
    case CubeStat::PearsonIc: return Ok(horizon);
    case CubeStat::RankIc: return Ok(4U + horizon);
    case CubeStat::NeutralIc: return Ok(8U + horizon);
    case CubeStat::LagOne: return Ok(usize{12});
    case CubeStat::Coverage: return Ok(usize{13});
    }
    return Err(ErrorCode::InvalidArgument, "signal cube: unknown statistic");
}
std::array<f64, kStats> flatten(const SignalCubeStats& s) {
    std::array<f64, kStats> out{};
    std::copy(s.pearson.begin(), s.pearson.end(), out.begin());
    std::copy(s.rank.begin(), s.rank.end(), out.begin() + 4);
    std::copy(s.neutral.begin(), s.neutral.end(), out.begin() + 8);
    out[12] = s.lag_one; out[13] = s.coverage;
    return out;
}
SignalCubeStats expand(const std::array<f64, kStats>& v) {
    SignalCubeStats out;
    std::copy_n(v.begin(), 4, out.pearson.begin());
    std::copy_n(v.begin() + 4, 4, out.rank.begin());
    std::copy_n(v.begin() + 8, 4, out.neutral.begin());
    out.lag_one = v[12]; out.coverage = v[13];
    return out;
}
void put_word(unsigned char* dst, u64 word, usize bytes) noexcept {
    for (usize b = 0; b < bytes; ++b) { dst[b] = static_cast<unsigned char>(word & 255U); word >>= 8U; }
}
u64 get_word(const unsigned char* src, usize bytes) noexcept {
    u64 word = 0;
    for (usize b = 0; b < bytes; ++b) word |= static_cast<u64>(src[b]) << (8U * b);
    return word;
}
Result<std::array<f64, kStats>> read_stat_values(std::istream& file, usize width) {
    std::array<unsigned char, kStats * 8> bytes{};
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(kStats * width));
    if (!file) return Err(ErrorCode::IoError, "signal cube: truncated stat row");
    std::array<f64, kStats> out{};
    for (usize s = 0; s < kStats; ++s) {
        const u64 word = get_word(bytes.data() + s * width, width);
        out[s] = width == 8 ? std::bit_cast<f64>(word)
                           : static_cast<f64>(std::bit_cast<float>(static_cast<atx::u32>(word)));
        if (std::isinf(out[s])) return Err(ErrorCode::ParseError, "signal cube: infinite statistic");
        if (s >= 8 && s < 12 && !std::isnan(out[s]))
            return Err(ErrorCode::ParseError, "signal cube: reserved neutral statistic is not unavailable");
        if ((s < 8 || s == 12) && std::isfinite(out[s]) && std::abs(out[s]) > 1.0 + 1e-12)
            return Err(ErrorCode::ParseError, "signal cube: correlation outside its range");
        if (s == 13 && std::isfinite(out[s]) && (out[s] < 0.0 || out[s] > 1.0))
            return Err(ErrorCode::ParseError, "signal cube: invalid coverage");
    }
    return Ok(out);
}
json config_json(const SignalCubeConfig& c) {
    return {{"dates", c.dates}, {"alphas", c.alphas}, {"instruments", c.instruments},
        {"chunk_dates", c.chunk_dates}, {"quantization_step", c.quantization_step},
        {"normalization", static_cast<int>(c.normalization)},
        {"return_treatment", static_cast<int>(c.return_treatment)},
        {"stat_precision", static_cast<int>(c.stat_precision)}, {"winsor", c.winsor},
        {"horizons", c.horizons}, {"execution_delay", c.execution_delay},
        {"maturity_end", c.maturity_end}, {"max_working_bytes", c.max_working_bytes},
        {"source_sha256", c.source_sha256}, {"transform_identity", c.transform_identity},
        {"return_identity", c.return_identity}, {"membership_identity", c.membership_identity},
        {"alpha_identities", c.alpha_identities}};
}
SignalCubeConfig parse_config(const json& j) {
    SignalCubeConfig c;
    c.dates = j.at("dates").get<usize>(); c.alphas = j.at("alphas").get<usize>();
    c.instruments = j.at("instruments").get<usize>(); c.chunk_dates = j.at("chunk_dates").get<usize>();
    c.quantization_step = j.at("quantization_step").get<f64>();
    c.normalization = static_cast<CubeNormalize>(j.at("normalization").get<int>());
    c.return_treatment = static_cast<CubeReturnTreatment>(j.at("return_treatment").get<int>());
    c.stat_precision = static_cast<CubeStatPrecision>(j.at("stat_precision").get<int>());
    c.winsor = j.at("winsor").get<f64>(); c.horizons = j.at("horizons").get<std::array<usize, 4>>();
    c.execution_delay = j.at("execution_delay").get<usize>(); c.maturity_end = j.at("maturity_end").get<usize>();
    c.max_working_bytes = j.at("max_working_bytes").get<u64>();
    c.source_sha256 = j.at("source_sha256").get<std::string>();
    c.transform_identity = j.at("transform_identity").get<std::string>();
    c.return_identity = j.at("return_identity").get<std::string>();
    c.membership_identity = j.at("membership_identity").get<std::string>();
    c.alpha_identities = j.at("alpha_identities").get<std::vector<std::string>>();
    return c;
}
struct Chunk {
    usize begin{}, end{};
    std::string signals, stats, signal_sha, stat_sha;
    u64 signal_bytes{}, stat_bytes{};
};
json chunk_json(const Chunk& c) {
    return {{"begin", c.begin}, {"end", c.end}, {"signals", c.signals}, {"stats", c.stats},
        {"signal_sha256", c.signal_sha}, {"stat_sha256", c.stat_sha},
        {"signal_bytes", c.signal_bytes}, {"stat_bytes", c.stat_bytes}};
}
std::string chunk_name(usize index, bool stats) {
    return std::string{stats ? "stats-" : "signals-"} + std::to_string(index) + ".bin";
}
void average_ranks(std::span<const f64> values, std::span<f64> ranks, std::vector<usize>& order) {
    order.resize(values.size()); std::iota(order.begin(), order.end(), usize{0});
    std::sort(order.begin(), order.end(), [&](usize a, usize b) {
        return values[a] < values[b] || (values[a] == values[b] && a < b);
    });
    for (usize first = 0; first < order.size();) {
        usize end = first + 1;
        while (end < order.size() && values[order[end]] == values[order[first]]) ++end;
        const f64 rank = 0.5 * static_cast<f64>(first + end - 1);
        for (usize i = first; i < end; ++i) ranks[order[i]] = rank;
        first = end;
    }
}
Status seek(std::ifstream& file, u64 offset) {
    file.seekg(static_cast<std::streamoff>(offset));
    if (!file) return Err(ErrorCode::IoError, "signal cube: seek failed");
    return Ok();
}
} // namespace

Result<SignalCubeSizing> preflight_signal_cube(const SignalCubeConfig& c) {
    if (c.dates == 0 || c.alphas == 0 || c.instruments == 0 || c.chunk_dates == 0 ||
        c.max_working_bytes == 0 || c.maturity_end > c.dates)
        return Err(ErrorCode::InvalidArgument, "signal cube: invalid geometry or maturity");
    if (!std::isfinite(c.quantization_step) || c.quantization_step <= 0 || c.quantization_step > 0.002 ||
        !std::isfinite(c.winsor) || c.winsor <= 0)
        return Err(ErrorCode::InvalidArgument, "signal cube: invalid quantization/winsor range");
    if (c.normalization != CubeNormalize::AlreadyNormalizedV1 &&
        c.normalization != CubeNormalize::ZScoreClipV2 && c.normalization != CubeNormalize::ZScoreRestandardizeV1)
        return Err(ErrorCode::InvalidArgument, "signal cube: unknown normalization");
    if ((c.return_treatment != CubeReturnTreatment::RawV1 && c.return_treatment != CubeReturnTreatment::WinsorizedV2) ||
        (c.stat_precision != CubeStatPrecision::ExactF64V1 && c.stat_precision != CubeStatPrecision::Float32V1))
        return Err(ErrorCode::InvalidArgument, "signal cube: unknown statistic policy");
    if (c.normalization == CubeNormalize::ZScoreClipV2 && c.winsor > c.quantization_step * 32767.0)
        return Err(ErrorCode::InvalidArgument, "signal cube: quantizer cannot represent the configured clip");
    for (usize h = 0; h < 4; ++h) {
        if (c.horizons[h] == 0 || (h > 0 && c.horizons[h] <= c.horizons[h-1]) ||
            c.horizons[h] > std::numeric_limits<usize>::max() - c.execution_delay)
            return Err(ErrorCode::InvalidArgument, "signal cube: horizons must increase without delay overflow");
    }
    if (!hash_text(c.source_sha256) || c.transform_identity.empty() || c.return_identity.empty() ||
        c.membership_identity.empty() || c.alpha_identities.size() != c.alphas)
        return Err(ErrorCode::InvalidArgument, "signal cube: source/transform/return/membership/alpha identities required");
    std::set<std::string_view> ids;
    u64 metadata = 65536; // manifest/vector/stream overhead allowance, excluding caller inputs
    for (const auto* identity : {&c.transform_identity, &c.return_identity, &c.membership_identity}) {
        u64 escaped = 0;
        if (!mul(identity->size(), 6, escaped) || !add(metadata, escaped, metadata))
            return Err(ErrorCode::OutOfRange, "signal cube: identity size overflow");
    }
    for (const auto& id : c.alpha_identities) {
        u64 escaped = 0;
        if (id.empty() || !ids.insert(id).second || !mul(id.size(), 6, escaped) ||
            !add(metadata, escaped, metadata) || !add(metadata, 64, metadata))
            return Err(ErrorCode::InvalidArgument, "signal cube: invalid alpha identities");
    }
    const u64 chunks = (c.dates - 1U) / c.chunk_dates + 1U;
    u64 entries = 0;
    if (!mul(chunks, 1024, entries) || !add(metadata, entries, metadata) || metadata > kManifestLimit / 2U)
        return Err(ErrorCode::InvalidArgument, "signal cube: manifest metadata budget exceeded");
    SignalCubeSizing s;
    u64 rows = 0, cells = 0, previous = 0, scratch = 0;
    if (!mul(c.dates, c.alphas, rows) || !mul(rows, c.instruments, cells) ||
        !mul(cells, 2, s.signal_bytes) || !mul(rows, kStats * stat_width(c), s.stat_bytes) ||
        !mul(c.alphas, c.instruments, previous) || !mul(previous, sizeof(f64), previous) ||
        !mul(c.instruments, 6U * sizeof(f64) + sizeof(usize) + 2U, scratch) ||
        !add(previous, scratch, s.writer_working_bytes) || !add(s.writer_working_bytes, metadata, s.writer_working_bytes) ||
        s.signal_bytes > static_cast<u64>(std::numeric_limits<std::streamoff>::max()) ||
        s.stat_bytes > static_cast<u64>(std::numeric_limits<std::streamoff>::max()) ||
        s.writer_working_bytes > static_cast<u64>(std::numeric_limits<usize>::max()) ||
        s.writer_working_bytes > c.max_working_bytes)
        return Err(ErrorCode::OutOfRange, "signal cube: storage overflow or writer memory budget exceeded");
    return Ok(s);
}

struct SignalCubeWriter::Impl {
    fs::path directory;
    SignalCubeConfig cfg;
    usize next_date{}, next_alpha{};
    bool failed{}, finished{};
    std::ofstream signals, stats;
    std::vector<Chunk> chunks;
    std::vector<f64> previous, row, fwd, x, y, xr, yr;
    std::vector<usize> order;
    std::vector<unsigned char> encoded;

    Status start_chunk() {
        const usize index = chunks.size();
        signals.open(directory / (chunk_name(index, false) + ".part"), std::ios::binary | std::ios::trunc);
        stats.open(directory / (chunk_name(index, true) + ".part"), std::ios::binary | std::ios::trunc);
        if (!signals || !stats) { failed = true; return Err(ErrorCode::IoError, "signal cube: cannot create chunks"); }
        return Ok();
    }
    Status close_chunk() {
        signals.close(); stats.close();
        if (!signals || !stats) { failed = true; return Err(ErrorCode::IoError, "signal cube: chunk close failed"); }
        const usize index = chunks.size();
        Chunk c;
        c.begin = index * cfg.chunk_dates; c.end = next_date;
        c.signals = chunk_name(index, false); c.stats = chunk_name(index, true);
        std::error_code ec;
        fs::rename(directory / (c.signals + ".part"), directory / c.signals, ec);
        if (!ec) fs::rename(directory / (c.stats + ".part"), directory / c.stats, ec);
        if (ec) { failed = true; return Err(ErrorCode::IoError, "signal cube: chunk publish failed: " + ec.message()); }
        auto sh = atx::core::sha256_file((directory / c.signals).string());
        auto th = atx::core::sha256_file((directory / c.stats).string());
        if (!sh || !th) { failed = true; return Err(ErrorCode::IoError, "signal cube: chunk hashing failed"); }
        c.signal_sha = *sh; c.stat_sha = *th;
        c.signal_bytes = static_cast<u64>(c.end - c.begin) * cfg.alphas * cfg.instruments * 2U;
        c.stat_bytes = static_cast<u64>(c.end - c.begin) * cfg.alphas * kStats * stat_width(cfg);
        chunks.push_back(std::move(c));
        return Ok();
    }
};

SignalCubeWriter::SignalCubeWriter(std::unique_ptr<Impl> p) : impl_{std::move(p)} {}
SignalCubeWriter::SignalCubeWriter(SignalCubeWriter&&) noexcept = default;
SignalCubeWriter& SignalCubeWriter::operator=(SignalCubeWriter&&) noexcept = default;
SignalCubeWriter::~SignalCubeWriter() = default;
const SignalCubeConfig& SignalCubeWriter::config() const noexcept { return impl_->cfg; }

Result<SignalCubeWriter> SignalCubeWriter::create(const fs::path& directory, SignalCubeConfig cfg) {
    if (cfg.maturity_end == 0) cfg.maturity_end = cfg.dates;
    ATX_TRY_VOID(preflight_signal_cube(cfg));
    std::error_code ec;
    if (!fs::create_directory(directory, ec))
        return Err(ErrorCode::IoError, "signal cube: output must be a fresh directory: " + ec.message());
    auto p = std::make_unique<Impl>();
    p->directory = directory; p->cfg = std::move(cfg);
    const usize n = p->cfg.instruments;
    p->previous.assign(p->cfg.alphas * n, kNan);
    p->row.resize(n); p->fwd.resize(n); p->x.reserve(n); p->y.reserve(n);
    p->xr.resize(n); p->yr.resize(n); p->order.resize(n); p->encoded.resize(n * 2U);
    return Ok(SignalCubeWriter{std::move(p)});
}

Status SignalCubeWriter::append_row(usize date, usize alpha, std::span<const f64> signal,
    const std::array<std::span<const f64>, 4>& labels, std::span<const atx::u8> membership) {
    auto& p = *impl_; const auto& c = p.cfg; const usize n = c.instruments;
    if (p.failed || p.finished || date != p.next_date || alpha != p.next_alpha || date >= c.dates ||
        signal.size() != n || (!membership.empty() && membership.size() != n))
        return Err(ErrorCode::InvalidArgument, "signal cube: closed writer, wrong shape or nonsequential row");
    for (const auto h : labels)
        if (!h.empty() && h.size() != n) return Err(ErrorCode::InvalidArgument, "signal cube: label shape mismatch");
    if (std::any_of(membership.begin(), membership.end(), [](atx::u8 v) { return v > 1U; }))
        return Err(ErrorCode::InvalidArgument, "signal cube: membership must be binary");
    usize eligible = 0, finite = 0;
    for (usize i = 0; i < n; ++i) {
        const bool member = membership.empty() || membership[i] != 0;
        eligible += member ? 1U : 0U;
        p.row[i] = member && std::isfinite(signal[i]) ? signal[i] : kNan;
    }
    const auto norm = c.normalization == CubeNormalize::AlreadyNormalizedV1 ? SignalNormalize::None
        : c.normalization == CubeNormalize::ZScoreClipV2 ? SignalNormalize::ZScore : SignalNormalize::ZScoreRestandardizeV1;
    signal_detail::zscore_row(p.row, c.winsor, norm);
    for (usize i = 0; i < n; ++i) {
        std::int16_t q = kMissing;
        if (std::isfinite(p.row[i])) {
            const f64 scaled = p.row[i] / c.quantization_step;
            if (!std::isfinite(scaled) || std::abs(scaled) > 32767.0)
                return Err(ErrorCode::OutOfRange, "signal cube: normalized signal exceeds quantizer; no silent clipping");
            q = static_cast<std::int16_t>(std::llround(scaled)); ++finite;
        }
        put_word(p.encoded.data() + 2U * i, std::bit_cast<std::uint16_t>(q), 2);
    }
    SignalCubeStats result;
    result.pearson.fill(kNan); result.rank.fill(kNan); result.neutral.fill(kNan);
    result.coverage = eligible == 0 ? kNan : static_cast<f64>(finite) / static_cast<f64>(eligible);
    const auto prior = std::span<const f64>{p.previous}.subspan(alpha * n, n);
    result.lag_one = date == 0 ? kNan : cross_section_corr(p.row, prior);
    for (usize h = 0; h < 4; ++h) {
        const usize lag = c.execution_delay + c.horizons[h];
        if (labels[h].empty() || date >= c.maturity_end || lag >= c.maturity_end - date) continue;
        for (usize i = 0; i < n; ++i)
            p.fwd[i] = (membership.empty() || membership[i] != 0) ? labels[h][i] : kNan;
        if (c.return_treatment == CubeReturnTreatment::WinsorizedV2)
            signal_detail::winsorize_row_copy(p.fwd, p.fwd, c.winsor);
        result.pearson[h] = cross_section_corr(p.row, p.fwd);
        p.x.clear(); p.y.clear();
        for (usize i = 0; i < n; ++i) if (std::isfinite(p.row[i]) && std::isfinite(p.fwd[i])) {
            p.x.push_back(p.row[i]); p.y.push_back(p.fwd[i]);
        }
        if (p.x.size() >= 3) {
            const auto xr = std::span<f64>{p.xr}.first(p.x.size());
            const auto yr = std::span<f64>{p.yr}.first(p.y.size());
            average_ranks(p.x, xr, p.order); average_ranks(p.y, yr, p.order);
            result.rank[h] = cross_section_corr(xr, yr);
        }
    }
    if (!p.signals.is_open()) ATX_TRY_VOID(p.start_chunk());
    const auto values = flatten(result); const usize width = stat_width(c);
    std::array<unsigned char, kStats * 8> stat_bytes{};
    for (usize s = 0; s < kStats; ++s) {
        const u64 word = width == 8 ? std::bit_cast<u64>(values[s])
            : std::bit_cast<atx::u32>(static_cast<float>(values[s]));
        put_word(stat_bytes.data() + s * width, word, width);
    }
    p.signals.write(reinterpret_cast<const char*>(p.encoded.data()), static_cast<std::streamsize>(p.encoded.size()));
    p.stats.write(reinterpret_cast<const char*>(stat_bytes.data()), static_cast<std::streamsize>(kStats * width));
    if (!p.signals || !p.stats) { p.failed = true; return Err(ErrorCode::IoError, "signal cube: row write failed"); }
    std::copy(p.row.begin(), p.row.end(), p.previous.begin() + static_cast<std::ptrdiff_t>(alpha * n));
    ++p.next_alpha;
    if (p.next_alpha == c.alphas) {
        p.next_alpha = 0; ++p.next_date;
        if (p.next_date % c.chunk_dates == 0 || p.next_date == c.dates) ATX_TRY_VOID(p.close_chunk());
    }
    return Ok();
}

Result<std::string> SignalCubeWriter::finish() {
    auto& p = *impl_;
    if (p.failed || p.finished || p.next_date != p.cfg.dates || p.next_alpha != 0)
        return Err(ErrorCode::InvalidArgument, "signal cube: incomplete, failed or already published writer");
    json chunks = json::array();
    for (const auto& c : p.chunks) chunks.push_back(chunk_json(c));
    json manifest{{"schema", "atx.signal-cube"}, {"version", 1}, {"layout", "date-alpha-instrument"},
        {"byte_order", "little-endian"}, {"signal_encoding", "int16"}, {"missing_code", kMissing},
        {"stat_layout", "pearson[4],rank[4],neutral[4],lag_one,coverage"},
        {"statistics_before_quantization", true}, {"neutral_available", false},
        {"coverage_denominator", "decision-date eligible instruments"},
        {"config", config_json(p.cfg)}, {"chunks", std::move(chunks)}};
    const std::string body = manifest.dump(2);
    if (body.size() > kManifestLimit) { p.failed = true; return Err(ErrorCode::OutOfRange, "signal cube: manifest too large"); }
    const auto part = p.directory / "manifest.json.part";
    std::ofstream file(part, std::ios::binary | std::ios::trunc);
    file.write(body.data(), static_cast<std::streamsize>(body.size())); file.close();
    if (!file) { p.failed = true; return Err(ErrorCode::IoError, "signal cube: manifest write failed"); }
    ATX_TRY(auto hash, atx::core::sha256_hex(body));
    std::error_code ec; fs::rename(part, p.directory / "manifest.json", ec);
    if (ec) { p.failed = true; return Err(ErrorCode::IoError, "signal cube: manifest publish failed: " + ec.message()); }
    p.finished = true;
    return Ok(std::move(hash));
}

struct SignalCube::Impl {
    fs::path directory;
    SignalCubeConfig cfg;
    std::string manifest_sha;
    std::vector<Chunk> chunks;
};
SignalCube::SignalCube(std::shared_ptr<const Impl> p) : impl_{std::move(p)} {}
const SignalCubeConfig& SignalCube::config() const noexcept { return impl_->cfg; }
std::string_view SignalCube::manifest_sha256() const noexcept { return impl_->manifest_sha; }

Result<SignalCube> SignalCube::open(const fs::path& directory, std::string_view expected) {
    std::error_code ec;
    const auto path = directory / "manifest.json";
    const auto bytes = fs::file_size(path, ec);
    if (ec || bytes == 0 || bytes > kManifestLimit)
        return Err(ErrorCode::IoError, "signal cube: missing or oversized completion manifest");
    std::string body(static_cast<usize>(bytes), '\0');
    std::ifstream file(path, std::ios::binary); file.read(body.data(), static_cast<std::streamsize>(body.size()));
    if (!file) return Err(ErrorCode::IoError, "signal cube: manifest read failed");
    ATX_TRY(auto hash, atx::core::sha256_hex(body));
    if (!expected.empty() && hash != expected)
        return Err(ErrorCode::ParseError, "signal cube: manifest does not match external anchor");
    try {
        const json j = json::parse(body);
        if (j.at("schema") != "atx.signal-cube" || j.at("version") != 1 ||
            j.at("layout") != "date-alpha-instrument" || j.at("byte_order") != "little-endian" ||
            j.at("signal_encoding") != "int16" || j.at("missing_code") != kMissing ||
            j.at("stat_layout") != "pearson[4],rank[4],neutral[4],lag_one,coverage" ||
            j.at("statistics_before_quantization") != true || j.at("neutral_available") != false ||
            j.at("coverage_denominator") != "decision-date eligible instruments")
            return Err(ErrorCode::ParseError, "signal cube: unsupported encoding contract");
        auto p = std::make_shared<Impl>(); p->directory = directory; p->cfg = parse_config(j.at("config"));
        if (config_json(p->cfg) != j.at("config"))
            return Err(ErrorCode::ParseError, "signal cube: noncanonical or unknown configuration fields");
        ATX_TRY_VOID(preflight_signal_cube(p->cfg));
        if (p->cfg.maturity_end == 0) return Err(ErrorCode::ParseError, "signal cube: unresolved maturity metadata");
        p->manifest_sha = std::move(hash);
        const auto& entries = j.at("chunks");
        const usize count = (p->cfg.dates - 1U) / p->cfg.chunk_dates + 1U;
        if (!entries.is_array() || entries.size() != count)
            return Err(ErrorCode::ParseError, "signal cube: incomplete chunk index");
        p->chunks.reserve(count);
        for (usize index = 0; index < count; ++index) {
            const auto& e = entries.at(index); Chunk c;
            c.begin = e.at("begin").get<usize>(); c.end = e.at("end").get<usize>();
            c.signals = e.at("signals").get<std::string>(); c.stats = e.at("stats").get<std::string>();
            c.signal_sha = e.at("signal_sha256").get<std::string>(); c.stat_sha = e.at("stat_sha256").get<std::string>();
            c.signal_bytes = e.at("signal_bytes").get<u64>(); c.stat_bytes = e.at("stat_bytes").get<u64>();
            const usize begin = index * p->cfg.chunk_dates;
            const usize end = begin + std::min(p->cfg.chunk_dates, p->cfg.dates - begin);
            const u64 rows = static_cast<u64>(end - begin) * p->cfg.alphas;
            if (c.begin != begin || c.end != end || c.signals != chunk_name(index, false) ||
                c.stats != chunk_name(index, true) || !hash_text(c.signal_sha) || !hash_text(c.stat_sha) ||
                c.signal_bytes != rows * p->cfg.instruments * 2U || c.stat_bytes != rows * kStats * stat_width(p->cfg))
                return Err(ErrorCode::ParseError, "signal cube: invalid chunk geometry, name or hash");
            for (const bool stats : {false, true}) {
                const auto chunk_path = directory / (stats ? c.stats : c.signals);
                const auto status = fs::symlink_status(chunk_path, ec);
                if (ec || !fs::is_regular_file(status)) return Err(ErrorCode::IoError, "signal cube: chunk is not a regular file");
                const auto size = fs::file_size(chunk_path, ec);
                if (ec || size != (stats ? c.stat_bytes : c.signal_bytes))
                    return Err(ErrorCode::ParseError, "signal cube: chunk byte count mismatch");
                ATX_TRY(auto actual, atx::core::sha256_file(chunk_path.string()));
                if (actual != (stats ? c.stat_sha : c.signal_sha))
                    return Err(ErrorCode::ParseError, "signal cube: chunk checksum mismatch");
            }
            p->chunks.push_back(std::move(c));
        }
        return Ok(SignalCube{std::move(p)});
    } catch (const json::exception& e) {
        return Err(ErrorCode::ParseError, "signal cube: invalid manifest: " + std::string{e.what()});
    }
}

Status SignalCube::read_signal_row(usize date, usize alpha, std::span<f64> output) const {
    const auto& p = *impl_; const auto& c = p.cfg;
    if (date >= c.dates || alpha >= c.alphas || output.size() != c.instruments)
        return Err(ErrorCode::InvalidArgument, "signal cube: signal read bounds");
    const auto& chunk = p.chunks[date / c.chunk_dates];
    const u64 row = static_cast<u64>(date - chunk.begin) * c.alphas + alpha;
    std::ifstream file(p.directory / chunk.signals, std::ios::binary);
    ATX_TRY_VOID(seek(file, row * c.instruments * 2U));
    // Bounded stack decode block; no hidden whole-alpha or cube allocation.
    std::array<unsigned char, 8192> buffer{};
    for (usize first = 0; first < c.instruments;) {
        const usize count = std::min(c.instruments - first, buffer.size() / 2U);
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count * 2U));
        if (!file) return Err(ErrorCode::IoError, "signal cube: truncated signal row");
        for (usize i = 0; i < count; ++i) {
            const auto q = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(get_word(buffer.data() + 2U*i, 2)));
            output[first + i] = q == kMissing ? kNan : static_cast<f64>(q) * c.quantization_step;
        }
        first += count;
    }
    return Ok();
}
Result<SignalCubeStats> SignalCube::read_stats(usize date, usize alpha) const {
    const auto& p = *impl_; const auto& c = p.cfg;
    if (date >= c.dates || alpha >= c.alphas) return Err(ErrorCode::InvalidArgument, "signal cube: stat read bounds");
    const auto& chunk = p.chunks[date / c.chunk_dates];
    const u64 row = static_cast<u64>(date - chunk.begin) * c.alphas + alpha;
    std::ifstream file(p.directory / chunk.stats, std::ios::binary);
    ATX_TRY_VOID(seek(file, row * kStats * stat_width(c)));
    ATX_TRY(auto values, read_stat_values(file, stat_width(c)));
    return Ok(expand(values));
}
Status SignalCube::read_stat_matrix(usize begin, usize end, CubeStat stat, usize horizon,
                                    std::span<f64> output) const {
    const auto& p = *impl_; const auto& c = p.cfg;
    ATX_TRY(const usize slot, stat_slot(stat, horizon));
    if (begin > end || end > c.dates || output.size() != static_cast<u64>(end - begin) * c.alphas)
        return Err(ErrorCode::InvalidArgument, "signal cube: stat matrix bounds");
    usize position = 0;
    for (usize date = begin; date < end;) {
        const auto& chunk = p.chunks[date / c.chunk_dates];
        const usize stop = std::min(end, chunk.end);
        std::ifstream file(p.directory / chunk.stats, std::ios::binary);
        const u64 first_row = static_cast<u64>(date - chunk.begin) * c.alphas;
        ATX_TRY_VOID(seek(file, first_row * kStats * stat_width(c)));
        for (; date < stop; ++date) for (usize alpha = 0; alpha < c.alphas; ++alpha) {
            ATX_TRY(auto values, read_stat_values(file, stat_width(c)));
            output[position++] = values[slot];
        }
    }
    return Ok();
}
Result<std::vector<f64>> SignalCube::read_stat_matrix(usize begin, usize end, CubeStat stat, usize horizon) const {
    const auto& c = impl_->cfg;
    if (begin > end || end > c.dates ||
        static_cast<u64>(end - begin) * c.alphas > c.max_working_bytes / sizeof(f64))
        return Err(ErrorCode::OutOfRange, "signal cube: requested statistic matrix exceeds read budget");
    std::vector<f64> output((end - begin) * c.alphas);
    ATX_TRY_VOID(read_stat_matrix(begin, end, stat, horizon, output));
    return Ok(std::move(output));
}

Result<std::string> write_signal_cube(const SignalStore& store, const fs::path& directory,
    SignalCubeConfig config, const std::array<std::span<const f64>, 4>& labels) {
    if (config.normalization != CubeNormalize::AlreadyNormalizedV1 || config.dates != store.n_dates() ||
        config.alphas != store.n_alphas() || config.instruments != store.n_instruments())
        return Err(ErrorCode::InvalidArgument, "signal cube: batch adapter needs matching already-normalized store geometry");
    for (const auto h : labels) if (!h.empty() && h.size() != store.cells())
        return Err(ErrorCode::InvalidArgument, "signal cube: batch adapter label panel shape");
    ATX_TRY(auto writer, SignalCubeWriter::create(directory, std::move(config)));
    for (usize t = 0; t < store.n_dates(); ++t) {
        std::array<std::span<const f64>, 4> row_labels{};
        for (usize h = 0; h < 4; ++h) if (!labels[h].empty())
            row_labels[h] = labels[h].subspan(t * store.n_instruments(), store.n_instruments());
        for (usize a = 0; a < store.n_alphas(); ++a)
            ATX_TRY_VOID(writer.append_row(t, a, store.signal_row(a, t), row_labels));
    }
    return writer.finish();
}
} // namespace atx::engine::combine
