"""ABS: capability absent-members-v1 in the --list-rules --json envelope (sorted)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from abs_edit import File  # noqa: E402

r = File("C:/atx-wt/pool-28/atx-impl/src/strategy_ic_rules.cpp")
r.once("""// of the --help probe, research_cycle.py exe_capabilities). Sorted. P9 S2 (ruling S2-2) adds the
// IC method options ic-hac-rule, label-terminal and min-coverage.
constexpr std::array<std::string_view,8> exe_capabilities{"eval-mode-audit-exact","ic-hac-rule",
    "label-terminal","list-rules","marginal","min-coverage","no-composition","theme-registry"};
""", """// of the --help probe, research_cycle.py exe_capabilities). Sorted. P9 S2 (ruling S2-2) adds the
// IC method options ic-hac-rule, label-terminal and min-coverage; P9 lane ABS (ruling OD3P-1)
// absent-members-v1 (--absent-members, strategy_ic_absent.hpp).
constexpr std::array<std::string_view,9> exe_capabilities{"absent-members-v1",
    "eval-mode-audit-exact","ic-hac-rule","label-terminal","list-rules","marginal","min-coverage",
    "no-composition","theme-registry"};
""")
r.save()

t = File("C:/atx-wt/pool-28/atx-impl/tests/strategy_ic_rules_test.cpp")
t.once("""  for (const auto* cap : {"list-rules", "marginal", "no-composition", "theme-registry",
                          "ic-hac-rule", "label-terminal", "min-coverage"}) // + P9 S2 (S2-2)
""", """  for (const auto* cap : {"list-rules", "marginal", "no-composition", "theme-registry",
                          "ic-hac-rule", "label-terminal", "min-coverage", // + P9 S2 (S2-2)
                          "absent-members-v1"})                            // + P9 ABS (OD3P-1)
""")
t.save()
print("ok")
