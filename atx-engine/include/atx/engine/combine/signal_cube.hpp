#pragma once

// E1 / E-11: bounded, immutable date-major [date][alpha][instrument] storage.
// Signal rows are int16 z scores; -32768 is missing, q*quantization_step is the
// decoded value. Statistics are computed BEFORE quantization. ExactF64V1 keeps
// legacy Pearson arithmetic; Float32V1 is an explicit lossy cache representation.
// A writer owns a fresh directory. Only finish() publishes manifest.json, after
// closing and hashing every chunk. Readers require a complete valid manifest.

#include <array>
#include <filesystem>
#include <memory>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::combine {

class SignalStore;

enum class CubeNormalize : atx::u8 { AlreadyNormalizedV1 = 1, ZScoreClipV2 = 2, ZScoreRestandardizeV1 = 3 };
enum class CubeReturnTreatment : atx::u8 { RawV1 = 1, WinsorizedV2 = 2 };
enum class CubeStatPrecision : atx::u8 { ExactF64V1 = 1, Float32V1 = 2 };
enum class CubeStat : atx::u8 { PearsonIc, RankIc, NeutralIc, LagOne, Coverage };

struct SignalCubeConfig {
    atx::usize dates{}, alphas{}, instruments{};
    atx::usize chunk_dates{32};
    atx::f64 quantization_step{0.0001};
    CubeNormalize normalization{CubeNormalize::ZScoreClipV2};
    CubeReturnTreatment return_treatment{CubeReturnTreatment::WinsorizedV2};
    CubeStatPrecision stat_precision{CubeStatPrecision::ExactF64V1};
    atx::f64 winsor{3.0};
    std::array<atx::usize, 4> horizons{{5, 21, 63, 126}};
    atx::usize execution_delay{1};
    atx::usize maturity_end{}; // exclusive date; zero resolves to dates
    atx::u64 max_working_bytes{256ULL * 1024ULL * 1024ULL};
    // Caller-supplied provenance, carried verbatim in the hash-bound manifest.
    std::string source_sha256;
    std::string transform_identity;
    std::string return_identity;
    std::string membership_identity;
    std::vector<std::string> alpha_identities; // unique, exactly alphas entries
};

struct SignalCubeStats {
    static constexpr atx::f64 missing = std::numeric_limits<atx::f64>::quiet_NaN();
    std::array<atx::f64, 4> pearson{{missing, missing, missing, missing}};
    std::array<atx::f64, 4> rank{{missing, missing, missing, missing}};
    std::array<atx::f64, 4> neutral{{missing, missing, missing, missing}}; // reserved for E2
    atx::f64 lag_one{missing};
    atx::f64 coverage{missing};
};

struct SignalCubeSizing {
    atx::u64 signal_bytes{}, stat_bytes{}, writer_working_bytes{};
};

[[nodiscard]] atx::core::Result<SignalCubeSizing> preflight_signal_cube(const SignalCubeConfig&);

class SignalCubeWriter {
public:
    [[nodiscard]] static atx::core::Result<SignalCubeWriter> create(
        const std::filesystem::path& directory, SignalCubeConfig config);
    SignalCubeWriter(SignalCubeWriter&&) noexcept;
    SignalCubeWriter& operator=(SignalCubeWriter&&) noexcept;
    ~SignalCubeWriter();
    SignalCubeWriter(const SignalCubeWriter&) = delete;
    SignalCubeWriter& operator=(const SignalCubeWriter&) = delete;

    // Strict date-major order: (0,0), (0,1), ... . Signal and each supplied label
    // row have instruments cells. Empty label rows mean unavailable, never zero.
    // Label h is close[d+delay+h]/close[d+delay]-1, with caller's return guard;
    // no label is read if its endpoint reaches maturity_end. Binary membership
    // is decision-date eligibility; empty means all names eligible.
    [[nodiscard]] atx::core::Status append_row(atx::usize date, atx::usize alpha,
        std::span<const atx::f64> signal,
        const std::array<std::span<const atx::f64>, 4>& forward_returns = {},
        std::span<const atx::u8> membership = {});
    [[nodiscard]] atx::core::Result<std::string> finish(); // published manifest SHA256
    [[nodiscard]] const SignalCubeConfig& config() const noexcept;
private:
    struct Impl;
    explicit SignalCubeWriter(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

class SignalCube {
public:
    // Validates geometry, manifest completeness, exact byte sizes and every
    // chunk SHA before exposing data. expected_manifest_sha256 is an optional
    // external provenance anchor. Reading performs no writes or tail repair.
    [[nodiscard]] static atx::core::Result<SignalCube> open(
        const std::filesystem::path& directory, std::string_view expected_manifest_sha256 = {});
    [[nodiscard]] const SignalCubeConfig& config() const noexcept;
    [[nodiscard]] std::string_view manifest_sha256() const noexcept;
    [[nodiscard]] atx::core::Status read_signal_row(atx::usize date, atx::usize alpha,
                                                   std::span<atx::f64> output) const;
    [[nodiscard]] atx::core::Result<SignalCubeStats> read_stats(atx::usize date, atx::usize alpha) const;
    // Output is [date-begin][alpha], exactly (end-begin)*alphas cells. Labels
    // already respect the recorded delay/maturity policy; a consumer must also
    // embargo each walk-forward fit by its decision date.
    [[nodiscard]] atx::core::Status read_stat_matrix(atx::usize begin, atx::usize end,
        CubeStat stat, atx::usize horizon_index, std::span<atx::f64> output) const;
    [[nodiscard]] atx::core::Result<std::vector<atx::f64>> read_stat_matrix(
        atx::usize begin, atx::usize end, CubeStat stat, atx::usize horizon_index = 0) const;
private:
    struct Impl;
    explicit SignalCube(std::shared_ptr<const Impl>);
    std::shared_ptr<const Impl> impl_;
};

// Compatibility adapter for an existing in-memory store. Signals are already
// normalized: config must say AlreadyNormalizedV1, with the original transform
// identity. Each supplied label panel is date-major dates*instruments; empty
// means unavailable. The caller explicitly chooses which horizon (if any) uses
// SignalStore::forward_returns(). No horizon is inferred or relabeled.
[[nodiscard]] atx::core::Result<std::string> write_signal_cube(
    const SignalStore& store, const std::filesystem::path& directory,
    SignalCubeConfig config,
    const std::array<std::span<const atx::f64>, 4>& forward_returns = {});

} // namespace atx::engine::combine
