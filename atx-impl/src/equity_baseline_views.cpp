#include "equity_baseline_views.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atx::impl {
namespace {
namespace alpha = atx::engine::alpha;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

constexpr std::array<std::string_view, 3> kFields{"close", "raw_close", "volume"};
// Checkpoint 20: optional context fields exposed to the family DSLs when the
// context carries them (history-panel canonical names). Borrowed spans only, so
// they add no bytes; a missing optional field is simply not registered and a
// DSL that names it fails to compile, loudly. `sector` is a DSL Group by name.
constexpr std::array<std::string_view, 11> kOptionalFields{
    "market_cap", "sector", "earnFlag", "nEarnCnt_5d", "atmCenI_21d", "atmCenI_126d",
    "high", "low", "open",
    // R21-3: point-in-time scalars appended by `panel --asof-field` (FINRA short
    // interest). Present only when the context was built with those flags.
    "si_shares", "si_dtc"};

// Registers the required fields and every present optional field as borrowed
// column spans over [offset, offset + cells) of the context.
[[nodiscard]] atx::core::Status append_context_fields(const PanelArtifact &context,
                                                      atx::usize offset, atx::usize cells,
                                                      std::vector<std::string> &names,
                                                      std::vector<std::span<const atx::f64>> &columns) {
    const auto &panel = context.panel;
    for (const auto name : kFields) {
        ATX_TRY(auto id, panel.field_id(name));
        names.emplace_back(name);
        columns.push_back(panel.field_all(id).subspan(offset, cells));
    }
    for (const auto name : kOptionalFields) {
        const auto id = panel.field_id(name);
        if (!id) {
            continue;
        }
        if (panel.field_all(*id).size() != panel.dates() * panel.instruments()) {
            return Err(ErrorCode::InvalidArgument, "equity baseline: ragged optional field");
        }
        names.emplace_back(name);
        columns.push_back(panel.field_all(*id).subspan(offset, cells));
    }
    return Ok();
}

[[nodiscard]] bool hash_text(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

[[nodiscard]] Result<atx::u64> product(atx::u64 left, atx::u64 right) {
    if (right != 0 && left > std::numeric_limits<atx::u64>::max() / right) {
        return Err(ErrorCode::OutOfRange, "equity baseline: allocation product overflow");
    }
    return Ok(left * right);
}

[[nodiscard]] Status add_bytes(atx::u64 &total, atx::u64 count, atx::u64 width) {
    ATX_TRY(auto bytes, product(count, width));
    if (bytes > std::numeric_limits<atx::u64>::max() - total) {
        return Err(ErrorCode::OutOfRange, "equity baseline: allocation sum overflow");
    }
    total += bytes;
    return Ok();
}

[[nodiscard]] Status validate_context(const PanelArtifact &context) {
    const auto &panel = context.panel;
    const auto &identity = context.identity;
    ATX_TRY_VOID(require_same_panel_axes(identity, identity));
    const auto maximum = std::numeric_limits<atx::usize>::max();
    if (panel.dates() != identity.session_keys.size() ||
        panel.instruments() != identity.instrument_ids.size() ||
        panel.instruments() == 0 || panel.dates() > maximum / panel.instruments() ||
        !hash_text(context.artifact_id) || !hash_text(context.payload_sha256)) {
        return Err(ErrorCode::InvalidArgument, "equity baseline: identified context shape/hash");
    }
    std::set<std::string_view> names;
    for (atx::usize field = 0; field < panel.num_fields(); ++field) {
        const auto &name = panel.field_name(field);
        if (name.empty() || !names.insert(name).second) {
            return Err(ErrorCode::InvalidArgument, "equity baseline: duplicate/empty field");
        }
    }
    for (const auto name : kFields) {
        ATX_TRY(auto id, panel.field_id(name));
        if (panel.field_all(id).size() != panel.dates() * panel.instruments()) {
            return Err(ErrorCode::InvalidArgument, "equity baseline: ragged numeric field");
        }
    }
    return Ok();
}

struct Prepared {
    EquityBaselinePlan plan;
    alpha::Program program;
};

[[nodiscard]] Result<Prepared> prepare(const PanelArtifact &context,
                                       const EquityBaselineConfig &config) {
    ATX_TRY_VOID(validate_context(context));
    const auto &window = config.evaluation;
    if (config.observation_basis !=
            EquityBaselineObservationBasis::ArchiveRawVolumeAndPointwiseAdjustedCloseV1 ||
        window.begin_session_key >= window.end_exclusive_session_key ||
        config.max_additional_bytes == 0) {
        return Err(ErrorCode::InvalidArgument, "equity baseline: source basis/window/budget");
    }
    const auto &keys = context.identity.session_keys;
    const auto first = std::lower_bound(keys.begin(), keys.end(), window.begin_session_key);
    const auto last = std::lower_bound(keys.begin(), keys.end(), window.end_exclusive_session_key);
    if (first == keys.end() || *first != window.begin_session_key || first == last ||
        static_cast<atx::usize>(first - keys.begin()) < kEquityBaselineWarmup) {
        return Err(ErrorCode::InvalidArgument, "equity baseline: exact start/256-row warmup required");
    }
    EquityBaselinePlan plan;
    plan.evaluation_begin = static_cast<atx::usize>(first - keys.begin());
    plan.evaluation_end = static_cast<atx::usize>(last - keys.begin());
    plan.first_evaluation_session_key = *first;
    plan.last_evaluation_session_key = *(last - 1);
    plan.feature_begin = plan.evaluation_begin - kEquityBaselineWarmup;
    const auto feature_rows = plan.evaluation_end - plan.feature_begin;
    const auto evaluation_rows = plan.evaluation_end - plan.evaluation_begin;
    // Both products are bounded by the validated full context shape.
    plan.feature_cells = feature_rows * context.panel.instruments();
    plan.evaluation_cells = evaluation_rows * context.panel.instruments();
    const alpha::Library library;
    ATX_TRY(auto program, alpha::compile_batch(kEquityBaselineDsl, library));
    if (program.roots.size() != kEquityBaselineDsl.size() || program.num_slots == 0) {
        return Err(ErrorCode::Internal, "equity baseline: fixed program shape changed");
    }
    plan.vm_slots = program.num_slots;
    ATX_TRY(auto slot_cells, product(plan.vm_slots, plan.feature_cells));
    ATX_TRY(plan.vm_slot_bytes, product(slot_cells, sizeof(atx::f64)));
    plan.additional_array_bytes = plan.vm_slot_bytes;
    // Conservative overlap: VM/context signals and the final cropped signals,
    // both masks, returned row labels/counts and fixed-program rolling scratch.
    ATX_TRY_VOID(add_bytes(plan.additional_array_bytes, plan.feature_cells, 2 * sizeof(atx::f64)));
    ATX_TRY_VOID(add_bytes(plan.additional_array_bytes, plan.evaluation_cells, 2 * sizeof(atx::f64)));
    ATX_TRY_VOID(add_bytes(plan.additional_array_bytes, plan.feature_cells, sizeof(atx::u8)));
    ATX_TRY_VOID(add_bytes(plan.additional_array_bytes, plan.evaluation_cells, sizeof(atx::u8)));
    ATX_TRY_VOID(add_bytes(plan.additional_array_bytes, evaluation_rows,
                           sizeof(atx::i64) + 3 * sizeof(atx::usize)));
    ATX_TRY_VOID(add_bytes(plan.additional_array_bytes, feature_rows, 2 * sizeof(atx::f64)));
    ATX_TRY_VOID(add_bytes(plan.additional_array_bytes, 252, 2 * sizeof(atx::f64)));
    ATX_TRY_VOID(add_bytes(plan.additional_array_bytes, program.fields.size(), sizeof(alpha::FieldId)));
    if (config.membership) {
        // D-12 as-of flags (one byte per evaluation cell) and the parsed id column.
        ATX_TRY_VOID(add_bytes(plan.additional_array_bytes, plan.evaluation_cells,
                               sizeof(atx::u8)));
        ATX_TRY_VOID(add_bytes(plan.additional_array_bytes, context.panel.instruments(),
                               sizeof(atx::i64)));
    }
    if (plan.additional_array_bytes > config.max_additional_bytes ||
        slot_cells > std::numeric_limits<atx::usize>::max() ||
        plan.vm_slot_bytes > std::numeric_limits<atx::usize>::max()) {
        return Err(ErrorCode::OutOfRange, "equity baseline: additional array budget exceeded");
    }
    return Ok(Prepared{plan, std::move(program)});
}

[[nodiscard]] bool observed_numeric(atx::f64 close, atx::f64 raw, atx::f64 volume) {
    if (!std::isfinite(close) || close <= 0.0 || !std::isfinite(raw) || raw <= 0.0 ||
        !std::isfinite(volume) || volume < 0.0) return false;
    const auto dollars = raw * volume;
    return std::isfinite(dollars) && (volume == 0.0 || dollars > 0.0);
}

struct FeatureView {
    alpha::Panel panel;
    atx::usize observed_cells{};
};

[[nodiscard]] Result<FeatureView> feature_view(const PanelArtifact &context,
                                              const EquityBaselinePlan &plan) {
    const auto &panel = context.panel;
    const auto instruments = panel.instruments();
    const auto offset = plan.feature_begin * instruments;
    std::vector<std::string> names;
    std::vector<std::span<const atx::f64>> columns;
    ATX_TRY_VOID(append_context_fields(context, offset, plan.feature_cells, names, columns));
    std::span<const atx::f64> source_observed;
    if (const auto id = panel.field_id("observed"); id) {
        const auto full = panel.field_all(*id);
        if (full.size() != panel.dates() * instruments) {
            return Err(ErrorCode::InvalidArgument, "equity baseline: ragged observation mask");
        }
        source_observed = full.subspan(offset, plan.feature_cells);
    }
    std::vector<atx::u8> mask(plan.feature_cells, 0);
    atx::usize observed_count = 0;
    for (atx::usize cell = 0; cell < plan.feature_cells; ++cell) {
        const bool valid = observed_numeric(columns[0][cell], columns[1][cell], columns[2][cell]);
        bool observed = valid;
        if (!source_observed.empty()) {
            const auto value = source_observed[cell];
            if ((value != 0.0 && value != 1.0) || (value == 1.0 && !valid)) {
                return Err(ErrorCode::InvalidArgument, "equity baseline: invalid observed cell");
            }
            observed = value == 1.0;
        }
        const auto original_row = plan.feature_begin + cell / instruments;
        if (original_row >= plan.evaluation_begin &&
            panel.in_universe(original_row, cell % instruments) && !observed) {
            return Err(ErrorCode::InvalidArgument,
                       "equity baseline: decision eligibility lacks valid observation");
        }
        mask[cell] = observed ? 1U : 0U;
        observed_count += observed ? 1U : 0U;
    }
    ATX_TRY(auto view, alpha::Panel::create_borrowed(plan.evaluation_end - plan.feature_begin,
        instruments, std::move(names), std::move(columns), std::move(mask)));
    return Ok(FeatureView{std::move(view), observed_count});
}

// D-12: one as-of membership flag per (row, instrument), date-major. The family
// VM needs the feature window too: rolling CS features must use the membership
// that was effective at each historical observation, not today's member list.
// Empty when the rule does not apply (no membership, or ContextYearUnionV1).
[[nodiscard]] Result<std::vector<atx::u8>>
asof_member_cells(const PanelArtifact &context, const EquityBaselineConfig &config,
                  atx::usize begin, atx::usize end) {
    if (!config.membership) return Ok(std::vector<atx::u8>{});
    if (config.membership_rule != EquityMembershipRule::AsOfV2) {
        return Err(ErrorCode::InvalidArgument,
                   "equity baseline: a membership image requires the as-of rule");
    }
    const auto instruments = context.panel.instruments();
    std::vector<atx::i64> ids(instruments, 0);
    for (atx::usize i = 0; i < instruments; ++i) {
        const auto &text = context.identity.instrument_ids[i];
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), ids[i]);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            return Err(ErrorCode::InvalidArgument,
                       "equity baseline: as-of membership needs integer instrument ids");
        }
    }
    const auto rows = end - begin;
    std::vector<atx::u8> member(rows * instruments, 0);
    for (atx::usize row = 0; row < rows; ++row) {
        const auto key = context.identity.session_keys[begin + row];
        for (atx::usize i = 0; i < instruments; ++i) {
            member[row * instruments + i] = config.membership->member(key, ids[i]) ? 1U : 0U;
        }
    }
    return Ok(std::move(member));
}

} // namespace

std::string_view equity_membership_rule_label(EquityMembershipRule rule) noexcept {
    switch (rule) {
    case EquityMembershipRule::ContextYearUnionV1: return "context-year-union-v1";
    case EquityMembershipRule::AsOfV2: return "as-of-pit-membership-v2";
    }
    return "unknown";
}

Result<EquityMembershipRule> parse_equity_membership_rule(std::string_view text) {
    if (text == "as-of-v2") return Ok(EquityMembershipRule::AsOfV2);
    if (text == "year-union-v1") return Ok(EquityMembershipRule::ContextYearUnionV1);
    return Err(ErrorCode::InvalidArgument,
               "equity membership rule must be as-of-v2 or year-union-v1: " + std::string(text));
}

bool EquityAsOfMembership::member(atx::i64 session_key, atx::i64 security_id) const noexcept {
    // The last rebalance effective on or before the session governs it.
    const auto after = std::upper_bound(effective_session_keys.begin(),
                                        effective_session_keys.end(), session_key);
    if (after == effective_session_keys.begin()) return false;
    const auto index = static_cast<atx::usize>(after - effective_session_keys.begin()) - 1U;
    const auto &ids = security_ids[index];
    return std::binary_search(ids.begin(), ids.end(), security_id);
}

Result<EquityAsOfMembership> equity_asof_membership(
    const atx::engine::data::PitMembershipImage &image, atx::usize cut) {
    if (image.rebalances.empty()) {
        return Err(ErrorCode::InvalidArgument, "equity membership: image has no rebalances");
    }
    std::vector<atx::usize> order(image.rebalances.size());
    for (atx::usize r = 0; r < order.size(); ++r) {
        if (cut >= image.rebalances[r].cuts.size()) {
            return Err(ErrorCode::InvalidArgument, "equity membership: cut index out of range");
        }
        order[r] = r;
    }
    std::stable_sort(order.begin(), order.end(), [&](atx::usize a, atx::usize b) {
        return image.rebalances[a].effective_session_key <
               image.rebalances[b].effective_session_key;
    });
    EquityAsOfMembership out;
    out.effective_session_keys.reserve(order.size());
    out.security_ids.reserve(order.size());
    for (const auto r : order) {
        const auto &rebalance = image.rebalances[r];
        if (!out.effective_session_keys.empty() &&
            out.effective_session_keys.back() == rebalance.effective_session_key) {
            return Err(ErrorCode::InvalidArgument,
                       "equity membership: two rebalances share an effective session");
        }
        auto ids = rebalance.cuts[cut].security_ids;
        if (!std::is_sorted(ids.begin(), ids.end()) ||
            std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
            return Err(ErrorCode::InvalidArgument,
                       "equity membership: cut ids are not strictly ascending");
        }
        out.effective_session_keys.push_back(rebalance.effective_session_key);
        out.security_ids.push_back(std::move(ids));
    }
    return Ok(std::move(out));
}

Result<atx::usize> equity_membership_cut_index(const atx::engine::data::PitMembershipImage &image,
                                               std::string_view cut_text) {
    // Same spelling as `panel --universe-cut`: "<top_n>:<units>.<hundredths>", the band
    // carried as basis points ("0.10" -> 1000), parsed digit-wise (no double rounding).
    const auto colon = cut_text.find(':');
    const auto dot = cut_text.find('.', colon == std::string_view::npos ? 0 : colon);
    if (colon == std::string_view::npos || colon == 0 || dot == std::string_view::npos ||
        dot == colon + 1 || cut_text.size() - dot != 3) {
        return Err(ErrorCode::InvalidArgument,
                   "equity membership: cut must be <top_n>:<units>.<hh>: " + std::string(cut_text));
    }
    const auto parse_u32 = [](std::string_view text, atx::u32 &out) {
        const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
        return !text.empty() && r.ec == std::errc{} && r.ptr == text.data() + text.size();
    };
    atx::u32 top_n = 0;
    atx::u32 units = 0;
    atx::u32 hundredths = 0;
    if (!parse_u32(cut_text.substr(0, colon), top_n) || top_n == 0 ||
        !parse_u32(cut_text.substr(colon + 1, dot - colon - 1), units) || units > 100U ||
        !parse_u32(cut_text.substr(dot + 1), hundredths)) {
        return Err(ErrorCode::InvalidArgument,
                   "equity membership: malformed cut: " + std::string(cut_text));
    }
    const atx::u32 band_bp = (units * 100U + hundredths) * 100U;
    const auto ti = std::find(image.top_n.begin(), image.top_n.end(), top_n);
    const auto bi = std::find(image.band_bp.begin(), image.band_bp.end(), band_bp);
    if (ti == image.top_n.end() || bi == image.band_bp.end()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity membership: the image carries no cut " + std::string(cut_text));
    }
    return Ok(static_cast<atx::usize>(ti - image.top_n.begin()) * image.band_bp.size() +
              static_cast<atx::usize>(bi - image.band_bp.begin()));
}

Result<LoadedEquityMembership> load_equity_membership(const std::string &path,
                                                      std::string_view cut_text) {
    constexpr std::streamoff kMaxBytes = 512LL * 1024LL * 1024LL;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return Err(ErrorCode::IoError, "equity membership: cannot open " + path);
    }
    const std::streamoff size = in.tellg();
    if (size <= 0 || size > kMaxBytes) {
        return Err(ErrorCode::IoError, "equity membership: empty or oversized image " + path);
    }
    std::string bytes(static_cast<atx::usize>(size), '\0');
    in.seekg(0, std::ios::beg);
    if (!in.read(bytes.data(), static_cast<std::streamsize>(size))) {
        return Err(ErrorCode::IoError, "equity membership: cannot read " + path);
    }
    LoadedEquityMembership out;
    ATX_TRY(out.sha256, atx::core::sha256_hex(std::string_view{bytes}));
    ATX_TRY(auto image, atx::engine::data::decode_membership_bin(bytes));
    ATX_TRY(out.cut_index, equity_membership_cut_index(image, cut_text));
    ATX_TRY(out.asof, equity_asof_membership(image, out.cut_index));
    out.rebalances = image.rebalances.size();
    return Ok(std::move(out));
}

Status publish_manifest_then_release_pending(const std::filesystem::path &directory,
                                             const std::function<Status()> &write_manifest) {
    std::error_code ec;
    if (!std::filesystem::exists(directory / ".pending", ec) || ec) {
        return Err(ErrorCode::IoError, "equity publication: no pending marker to release");
    }
    ATX_TRY_VOID(write_manifest());
    if (!std::filesystem::remove(directory / ".pending", ec) || ec) {
        return Err(ErrorCode::IoError, "equity publication: cannot release pending marker");
    }
    return Ok();
}

Result<std::optional<LoadedEquityMembership>>
resolve_equity_membership(bool context_has_membership, std::string_view recipe_sha256,
                          std::string_view recipe_cut, EquityMembershipRule rule,
                          const std::string &membership_path) {
    using Loaded = std::optional<LoadedEquityMembership>;
    if (!context_has_membership) {
        if (!membership_path.empty()) {
            return Err(ErrorCode::InvalidArgument,
                       "equity membership: the context declares no membership restriction; "
                       "--membership does not apply");
        }
        return Ok(Loaded{});
    }
    if (rule == EquityMembershipRule::ContextYearUnionV1) {
        if (!membership_path.empty()) {
            return Err(ErrorCode::InvalidArgument,
                       "equity membership: --membership-rule year-union-v1 takes no --membership");
        }
        return Ok(Loaded{});
    }
    if (membership_path.empty()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity membership: the context carries a year-union membership allow-list "
                   "(a within-window look-ahead); pass --membership <membership.bin> for the "
                   "as-of mask, or --membership-rule year-union-v1 to reproduce pre-W0 output");
    }
    ATX_TRY(auto loaded, load_equity_membership(membership_path, recipe_cut));
    if (loaded.sha256 != recipe_sha256) {
        return Err(ErrorCode::InvalidArgument,
                   "equity membership: --membership hashes to " + loaded.sha256 +
                       " but the context was built from " + std::string(recipe_sha256));
    }
    return Ok(Loaded{std::move(loaded)});
}

Result<EquityBaselinePlan> plan_equity_baseline(const PanelArtifact &context,
                                              const EquityBaselineConfig &config) {
    ATX_TRY(auto prepared, prepare(context, config));
    return Ok(prepared.plan);
}

Result<EquityBaselineEvaluation> evaluate_equity_baseline(const PanelArtifact &context,
                                                        const EquityBaselineConfig &config) {
    ATX_TRY(auto prepared, prepare(context, config));
    const auto &plan = prepared.plan;
    ATX_TRY(const auto asof_member, asof_member_cells(context, config,
        plan.evaluation_begin, plan.evaluation_end));
    ATX_TRY(auto feature, feature_view(context, plan));
    alpha::SignalSet full_signals;
    {
        alpha::Engine engine(feature.panel);
        ATX_TRY(full_signals, engine.evaluate(prepared.program));
    } // Release VM slots before allocating the returned evaluation signals.
    if (full_signals.dates != feature.panel.dates() ||
        full_signals.instruments != feature.panel.instruments() ||
        full_signals.alphas.size() != kEquityBaselineDsl.size() ||
        std::any_of(full_signals.alphas.begin(), full_signals.alphas.end(),
                    [&](const auto &value) { return value.values.size() != plan.feature_cells; })) {
        return Err(ErrorCode::Internal, "equity baseline: fixed VM output shape changed");
    }
    const auto instruments = context.panel.instruments();
    const auto evaluation_rows = plan.evaluation_end - plan.evaluation_begin;
    const auto warmup_cells = kEquityBaselineWarmup * instruments;
    alpha::SignalSet signals;
    signals.dates = evaluation_rows;
    signals.instruments = instruments;
    signals.alphas.resize(kEquityBaselineDsl.size());
    for (atx::usize a = 0; a < signals.alphas.size(); ++a) {
        signals.alphas[a].name = kEquityBaselineSignalNames[a];
        signals.alphas[a].values.assign(plan.evaluation_cells,
                                        std::numeric_limits<atx::f64>::quiet_NaN());
    }
    std::vector<atx::u8> mask(plan.evaluation_cells, 0);
    std::vector<atx::usize> ready_by_observation(evaluation_rows, 0);
    std::vector<atx::usize> admitted_by_observation(evaluation_rows, 0);
    atx::usize eligible_cells = 0;
    atx::usize ready_cells = 0;
    atx::usize floor_rejected = 0;
    atx::usize membership_rejected = 0;
    atx::usize admitted_cells = 0;
    std::span<const atx::f64> raw_close_feature;
    std::span<const atx::f64> volume_feature;
    if (config.min_dollar_adv > 0.0) {
        if (config.dollar_adv_window < 1 || config.dollar_adv_window > kEquityBaselineWarmup + 1) {
            return Err(ErrorCode::InvalidArgument,
                       "equity baseline: dollar_adv_window exceeds the feature warmup");
        }
        const auto feature_offset = plan.feature_begin * instruments;
        ATX_TRY(auto raw_close_id, context.panel.field_id("raw_close"));
        ATX_TRY(auto volume_id, context.panel.field_id("volume"));
        raw_close_feature = context.panel.field_all(raw_close_id).subspan(feature_offset,
                                                                          plan.feature_cells);
        volume_feature = context.panel.field_all(volume_id).subspan(feature_offset,
                                                                    plan.feature_cells);
    }
    const auto dollar_adv_ok = [&](atx::usize feature_row, atx::usize instrument) {
        const auto window = config.dollar_adv_window;
        atx::f64 total = 0.0;
        for (atx::usize back = 0; back < window; ++back) {
            const auto index = (feature_row - back) * instruments + instrument;
            const auto dollars = raw_close_feature[index] * volume_feature[index];
            if (!std::isfinite(dollars) || dollars <= 0.0) return false;
            total += dollars;
        }
        return total / static_cast<atx::f64>(window) >= config.min_dollar_adv;
    };
    for (atx::usize cell = 0; cell < plan.evaluation_cells; ++cell) {
        const auto row = cell / instruments;
        const auto instrument = cell % instruments;
        const bool context_eligible =
            context.panel.in_universe(plan.evaluation_begin + row, instrument);
        // D-12: the year-union context mask is intersected with as-of membership, so a
        // mid-window joiner is invisible before the session its rebalance takes effect.
        const bool member = asof_member.empty() || asof_member[cell] != 0U;
        membership_rejected += (context_eligible && !member) ? 1U : 0U;
        const bool eligible = context_eligible && member;
        const auto first = full_signals.alphas[0].values[warmup_cells + cell];
        const auto second = full_signals.alphas[1].values[warmup_cells + cell];
        const bool ready = std::isfinite(first) && std::isfinite(second);
        eligible_cells += eligible ? 1U : 0U;
        ready_cells += ready ? 1U : 0U;
        ready_by_observation[row] += ready ? 1U : 0U;
        if (!eligible || !ready) continue;
        if (config.min_dollar_adv > 0.0 && !dollar_adv_ok(kEquityBaselineWarmup + row, instrument)) {
            ++floor_rejected;
            continue;
        }
        mask[cell] = 1;
        ++admitted_cells;
        ++admitted_by_observation[row];
        signals.alphas[0].values[cell] = first;
        signals.alphas[1].values[cell] = second;
    }
    std::vector<std::string> names;
    std::vector<std::span<const atx::f64>> columns;
    const auto evaluation_offset = plan.evaluation_begin * instruments;
    ATX_TRY_VOID(append_context_fields(context, evaluation_offset, plan.evaluation_cells, names,
                                       columns));
    ATX_TRY(auto panel, alpha::Panel::create_borrowed(evaluation_rows, instruments,
        std::move(names), std::move(columns), std::move(mask)));
    std::vector<atx::i64> keys;
    std::vector<atx::usize> context_rows;
    keys.reserve(evaluation_rows);
    context_rows.reserve(evaluation_rows);
    for (atx::usize row = plan.evaluation_begin; row < plan.evaluation_end; ++row) {
        keys.push_back(context.identity.session_keys[row]);
        context_rows.push_back(row);
    }
    return Ok(EquityBaselineEvaluation{std::move(panel), std::move(signals), std::move(keys),
        std::move(context_rows), std::move(ready_by_observation), std::move(admitted_by_observation),
        plan, feature.observed_cells, eligible_cells, floor_rejected, ready_cells, admitted_cells,
        membership_rejected});
}

Result<EquityFamilyEvaluation> evaluate_equity_families(const PanelArtifact &context,
                                                        const EquityBaselineConfig &config,
                                                        const EquityBaselineEvaluation &baseline,
                                                        std::span<const std::string_view> dsl,
                                                        std::span<const std::string_view> names) {
    if (dsl.empty() || dsl.size() != names.size() || config.max_additional_bytes == 0) {
        return Err(ErrorCode::InvalidArgument, "equity families: dsl/name list shape");
    }
    ATX_TRY_VOID(validate_context(context));
    const auto &base = baseline.plan;
    const auto instruments = context.panel.instruments();
    const auto evaluation_rows = base.evaluation_end - base.evaluation_begin;
    if (base.evaluation_begin >= base.evaluation_end ||
        base.evaluation_end > context.panel.dates() ||
        baseline.panel.dates() != evaluation_rows || baseline.panel.instruments() != instruments) {
        return Err(ErrorCode::InvalidArgument, "equity families: baseline plan/axes mismatch");
    }
    const alpha::Library library;
    ATX_TRY(auto program, alpha::compile_batch(dsl, library));
    if (program.roots.size() != dsl.size() || program.num_slots == 0) {
        return Err(ErrorCode::Internal, "equity families: program shape differs from the list");
    }
    // The warmup is derived, never hard-coded: the type checker's causality rail.
    const atx::usize warmup = program.required_lookback;
    if (base.evaluation_begin < warmup) {
        return Err(ErrorCode::InvalidArgument,
                   "equity families: context lacks the program's required lookback before "
                   "the evaluation window");
    }
    EquityBaselinePlan plan = base;
    plan.feature_begin = base.evaluation_begin - warmup;
    plan.feature_cells = (base.evaluation_end - plan.feature_begin) * instruments;
    plan.vm_slots = program.num_slots;
    ATX_TRY(auto slot_cells, product(plan.vm_slots, plan.feature_cells));
    ATX_TRY(plan.vm_slot_bytes, product(slot_cells, sizeof(atx::f64)));
    atx::u64 budget = plan.vm_slot_bytes;
    ATX_TRY_VOID(add_bytes(budget, plan.feature_cells, dsl.size() * sizeof(atx::f64)));
    ATX_TRY_VOID(add_bytes(budget, plan.evaluation_cells, dsl.size() * sizeof(atx::f64)));
    ATX_TRY_VOID(add_bytes(budget, plan.feature_cells, sizeof(atx::u8)));
    ATX_TRY_VOID(add_bytes(budget, warmup + 1, 2 * sizeof(atx::f64)));
    if (config.membership) {
        ATX_TRY_VOID(add_bytes(budget, plan.feature_cells, sizeof(atx::u8)));
        ATX_TRY_VOID(add_bytes(budget, instruments, sizeof(atx::i64)));
    }
    if (budget > config.max_additional_bytes || slot_cells > std::numeric_limits<atx::usize>::max()) {
        return Err(ErrorCode::OutOfRange, "equity families: additional array budget exceeded");
    }
    ATX_TRY(auto feature, feature_view(context, plan));
    alpha::SignalSet full_signals;
    {
        alpha::Engine engine(feature.panel);
        ATX_TRY(auto cs_members, asof_member_cells(context, config,
            plan.feature_begin, plan.evaluation_end));
        ATX_TRY_VOID(engine.set_cross_section_mask(std::move(cs_members)));
        ATX_TRY(full_signals, engine.evaluate(program));
    }
    if (full_signals.dates != feature.panel.dates() ||
        full_signals.instruments != instruments || full_signals.alphas.size() != dsl.size() ||
        std::any_of(full_signals.alphas.begin(), full_signals.alphas.end(),
                    [&](const auto &value) { return value.values.size() != plan.feature_cells; })) {
        return Err(ErrorCode::Internal, "equity families: VM output shape changed");
    }
    const auto warmup_cells = warmup * instruments;
    EquityFamilyEvaluation out;
    out.warmup = warmup;
    out.vm_slots = plan.vm_slots;
    out.additional_array_bytes = budget;
    out.signals.dates = evaluation_rows;
    out.signals.instruments = instruments;
    out.signals.alphas.resize(dsl.size());
    out.finite_admitted_cells.assign(dsl.size(), 0);
    for (atx::usize a = 0; a < dsl.size(); ++a) {
        auto &alpha_out = out.signals.alphas[a];
        alpha_out.name = names[a];
        alpha_out.values.assign(plan.evaluation_cells, std::numeric_limits<atx::f64>::quiet_NaN());
        const auto &full = full_signals.alphas[a].values;
        for (atx::usize cell = 0; cell < plan.evaluation_cells; ++cell) {
            if (!baseline.panel.in_universe(cell / instruments, cell % instruments)) continue;
            const auto value = full[warmup_cells + cell];
            if (!std::isfinite(value)) continue;
            alpha_out.values[cell] = value;
            ++out.finite_admitted_cells[a];
        }
    }
    return Ok(std::move(out));
}

} // namespace atx::impl
