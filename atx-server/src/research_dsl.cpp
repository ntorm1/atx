#include "research_dsl.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

namespace atx::server::research_detail {
namespace {
namespace alpha = atx::engine::alpha;
using Op = alpha::OpCode;
constexpr std::uint64_t max_work_units = 250'000'000;

struct Operator {
    Op code;
    std::string_view name;
    std::string_view category;
    std::string_view example;
    std::string_view description;
    unsigned window_argument{};
};

// The API catalog and compiler deliberately share this table. Engine aliases resolve by opcode.
constexpr Operator operators[]{
    {Op::Abs, "abs", "Arithmetic", "abs(returns)", "Absolute value"},
    {Op::Sign, "sign", "Arithmetic", "sign(returns)", "Sign of a numeric value"},
    {Op::Log, "log", "Arithmetic", "log(close)", "Natural logarithm"},
    {Op::Neg, "reverse", "Arithmetic", "reverse(returns)", "Negate a signal"},
    {Op::Sigmoid, "sigmoid", "Arithmetic", "sigmoid(returns)", "Logistic transform"},
    {Op::Tanh, "tanh", "Arithmetic", "tanh(returns)", "Hyperbolic tangent"},
    {Op::Pow, "power", "Arithmetic", "power(returns,2)", "Raise to a power"},
    {Op::Spow, "signedpower", "Arithmetic", "signedpower(returns,2)",
        "Power of magnitude, preserving sign"},
    {Op::MinP, "min", "Arithmetic", "min(close,volume)", "Elementwise minimum"},
    {Op::MaxP, "max", "Arithmetic", "max(close,volume)", "Elementwise maximum"},
    {Op::CsRank, "rank", "Cross section", "rank(close)",
        "Rank within current eligible instruments; average ties"},
    {Op::CsZscore, "zscore", "Cross section", "zscore(returns)",
        "Center and standardize within the current cross section"},
    {Op::CsScale, "scale", "Cross section", "scale(returns,1)",
        "Rescale signal to a fixed sum of absolute values"},
    {Op::CsNormalize, "normalize", "Cross section", "normalize(returns)",
        "Subtract the cross-sectional mean"},
    {Op::CsWinsorize, "winsorize", "Cross section", "winsorize(returns,4)",
        "Clamp at mean plus or minus a fixed number of sample standard deviations"},
    {Op::TsDelay, "delay", "Time series", "delay(close,5)", "Value n sessions earlier", 2},
    {Op::TsDelta, "delta", "Time series", "delta(close,5)",
        "Current value minus the value n sessions earlier", 2},
    {Op::TsSum, "ts_sum", "Time series", "ts_sum(returns,20)", "Trailing sum", 2},
    {Op::TsMean, "ts_mean", "Time series", "ts_mean(volume,20)", "Trailing mean", 2},
    {Op::TsStd, "ts_std", "Time series", "ts_std(returns,20)",
        "Trailing sample standard deviation", 2},
    {Op::TsVar, "ts_var", "Time series", "ts_var(returns,20)",
        "Trailing sample variance", 2},
    {Op::TsMin, "ts_min", "Time series", "ts_min(close,20)", "Trailing minimum", 2},
    {Op::TsMax, "ts_max", "Time series", "ts_max(close,20)", "Trailing maximum", 2},
    {Op::TsArgMin, "ts_argmin", "Time series", "ts_argmin(close,20)",
        "Position of trailing minimum", 2},
    {Op::TsArgMax, "ts_argmax", "Time series", "ts_argmax(close,20)",
        "Position of trailing maximum", 2},
    {Op::TsRank, "ts_rank", "Time series", "ts_rank(close,20)",
        "Rank of current value within a trailing window", 2},
    {Op::TsCorr, "ts_corr", "Time series", "ts_corr(returns,volume,20)",
        "Trailing Pearson correlation", 3},
    {Op::TsCov, "covariance", "Time series", "covariance(returns,volume,20)",
        "Trailing sample covariance", 3},
    {Op::TsProduct, "product", "Time series", "product(1+returns,20)",
        "Trailing product", 2},
    {Op::TsDecayLinear, "decay_linear", "Time series", "decay_linear(returns,10)",
        "Trailing weighted mean with greater weight on recent observations", 2},
    {Op::TsZscore, "ts_zscore", "Time series", "ts_zscore(returns,20)",
        "Standardize current value against a trailing window", 2},
    {Op::TsBackfill, "ts_backfill", "Time series", "ts_backfill(close,5)",
        "Most recent available value within a trailing window", 2},
    {Op::TsAvDiff, "ts_av_diff", "Time series", "ts_av_diff(close,20)",
        "Current value minus the trailing mean", 2},
    {Op::TsScale, "ts_scale", "Time series", "ts_scale(close,20)",
        "Scale current value within trailing minimum and maximum", 2},
    {Op::TsCountNans, "ts_count_nans", "Time series", "ts_count_nans(close,20)",
        "Count missing observations within a trailing window", 2},
};

const Operator* operator_policy(Op code) noexcept {
    const auto found = std::find_if(std::begin(operators), std::end(operators),
        [code](const auto& item) { return item.code == code; });
    return found == std::end(operators) ? nullptr : found;
}

void validate_text(std::string_view text) {
    if (text.empty() || text.size() > 1024)
        throw std::invalid_argument("strategy_dsl must contain 1 to 1024 bytes");
    int nesting = 0;
    for (const char byte : text) {
        if (byte == '(' && ++nesting > 16)
            throw std::invalid_argument("strategy_dsl exceeds nesting limit");
        if (byte == ')' && --nesting < 0)
            throw std::invalid_argument("strategy_dsl unmatched parentheses");
    }
    if (nesting != 0) throw std::invalid_argument("strategy_dsl unmatched parentheses");
    const auto tokens = alpha::lex(text);
    if (!tokens || tokens->size() > 256)
        throw std::invalid_argument("strategy_dsl invalid or exceeds token limit");
}

double literal_argument(const alpha::Ast& ast, alpha::ExprId id,
                        double lower, double upper, bool integral) {
    if (id == alpha::kNoExpr) throw std::invalid_argument("strategy_dsl missing argument");
    const auto& node = ast.node(id);
    if (node.kind != alpha::Expr::Kind::Literal || !std::isfinite(node.value) ||
        node.value < lower || node.value > upper ||
        (integral && std::floor(node.value) != node.value)) {
        throw std::invalid_argument("strategy_dsl requires a bounded literal argument");
    }
    return node.value;
}

std::uint64_t validate_call(const alpha::Ast& ast, const alpha::Expr& node) {
    const auto* policy = operator_policy(node.op->opcode);
    if (policy == nullptr || !node.op->lookahead_safe)
        throw std::invalid_argument("strategy_dsl operator is not enabled for server execution");
    if (policy->window_argument != 0) {
        const auto id = policy->window_argument == 2 ? node.b : node.c;
        const auto window = literal_argument(ast, id, 1, 252, true);
        if (policy->code == Op::TsDelay || policy->code == Op::TsDelta) return 2;
        // Conservative per-cell work accounting, including sorting and paired rolling moments.
        return 8 * static_cast<std::uint64_t>(window);
    }
    if (policy->code == Op::CsScale || policy->code == Op::CsWinsorize) {
        static_cast<void>(literal_argument(ast, node.b, 0,
            policy->code == Op::CsScale ? 1e6 : 100, false));
    }
    return policy->category == "Cross section" ? 64 : 1;
}

struct Compilation {
    alpha::Program program;
    std::uint64_t work_per_cell{1};
};

Compilation compile_checked(std::string_view text, std::span<const std::string> fields) {
    validate_text(text);
    if (fields.size() > 64) throw std::invalid_argument("research field limit exceeded");
    static const alpha::Library library;
    const auto ast = alpha::parse_expr(text, library);
    if (!ast) throw std::invalid_argument(ast.error().message());
    if (ast->nodes().size() > 256 || ast->roots().size() != 1)
        throw std::invalid_argument("strategy_dsl must contain one bounded expression");
    std::uint64_t work = 1;
    for (const auto& node : ast->nodes()) {
        if (node.kind == alpha::Expr::Kind::Literal &&
            (!std::isfinite(node.value) || std::abs(node.value) > 1e6))
            throw std::invalid_argument("strategy_dsl literal outside supported range");
        if (node.kind == alpha::Expr::Kind::Field &&
            std::find(fields.begin(), fields.end(), ast->field_name(node.name_id)) == fields.end())
            throw std::invalid_argument("strategy_dsl unknown input field: " +
                                        std::string(ast->field_name(node.name_id)));
        if (node.kind == alpha::Expr::Kind::Member)
            throw std::invalid_argument("strategy_dsl record operators are not enabled");
        work += node.kind == alpha::Expr::Kind::Call ? validate_call(*ast, node) : 1;
    }
    const auto analysis = alpha::analyze(*ast);
    if (!analysis) throw std::invalid_argument(analysis.error().message());
    const auto& root = analysis->info(ast->roots().front().root);
    if (root.dtype != alpha::DType::F64 || root.is_record)
        throw std::invalid_argument("strategy_dsl must return one numeric signal");
    if (analysis->required_lookback() > 756)
        throw std::invalid_argument("strategy_dsl cumulative lookback exceeds 756 sessions");
    auto program = alpha::compile(*ast, *analysis);
    if (!program) throw std::invalid_argument(program.error().message());
    return {std::move(*program), work};
}

void check_cancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("research cancelled");
}
} // namespace

alpha::Program compile_research_dsl(std::string_view text,
                                   std::span<const std::string> allowed_fields) {
    return compile_checked(text, allowed_fields).program;
}

std::uint64_t research_dsl_work_per_cell(std::string_view text,
                                      std::span<const std::string> allowed_fields) {
    return compile_checked(text, allowed_fields).work_per_cell;
}

alpha::SignalSet evaluate_research_signal(std::string_view text, const alpha::Panel& panel,
                                          std::uint64_t max_working_bytes,
                                          std::stop_token stop) {
    check_cancelled(stop);
    if (panel.dates() == 0 || panel.dates() > 2000 || panel.instruments() == 0 ||
        panel.instruments() > 512 || panel.num_fields() > 64)
        throw std::invalid_argument("research signal panel dimensions exceed execution bounds");
    std::vector<std::string> fields;
    fields.reserve(panel.num_fields());
    for (std::size_t i = 0; i < panel.num_fields(); ++i) fields.push_back(panel.field_name(i));
    const auto compiled = compile_checked(text, fields);
    const auto cells = static_cast<std::uint64_t>(panel.cells());
    // Include source columns, universe, output and replay targets alongside the oracle slots.
    // Dimensions and AST bounds above prove both products fit uint64_t before allocation.
    const auto arrays = panel.num_fields() + compiled.program.num_slots + 4ULL;
    if (cells * (arrays * sizeof(double) + 1) > max_working_bytes)
        throw std::invalid_argument("strategy_dsl exceeds research working memory budget");
    if (cells * compiled.work_per_cell > max_work_units)
        throw std::invalid_argument("strategy_dsl exceeds research work budget; reduce scope");
    auto result = alpha::evaluate_reference(compiled.program, panel);
    check_cancelled(stop);
    if (!result) throw std::invalid_argument(result.error().message());
    return std::move(*result);
}

nlohmann::json research_operator_catalog() {
    auto items = nlohmann::json::array();
    for (const auto& item : operators) {
        items.push_back({{"name", item.name}, {"category", item.category},
            {"example", item.example}, {"description", item.description},
            {"window_argument", item.window_argument}, {"causal", true}});
    }
    return {{"operators", std::move(items)}, {"limits", {{"max_bytes", 1024},
        {"max_tokens", 256}, {"max_nodes", 256}, {"max_nesting", 16},
        {"max_window", 252}, {"max_lookback", 756}, {"max_work_units", max_work_units}}},
        {"arithmetic", "+ - * / ^; comparisons and logical operators inside numeric conditionals"},
        {"missing_values", "Engine missing-value semantics apply; no implicit zero fill"}};
}
} // namespace atx::server::research_detail
