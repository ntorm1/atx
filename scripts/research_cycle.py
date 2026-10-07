#!/usr/bin/env python3
"""One research cycle driven by a spec file (platform v7 lane L2): replaces the hand-copied vNN_train.sh ladders.

The entry point and public API of the cycle. P9 lane E2 split its code, move-only, into the package scripts/cycle/
(orchestration review section 1): spec, resolver, phases, gate, compare, lock and cli (see cycle/__init__.py). Every
name callers used here is re-exported unchanged. The usage (``--help``) is cycle/cli.py's docstring.
"""
from __future__ import annotations

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cycle_admission  # noqa: E402,F401
import cycle_resume  # noqa: E402,F401
import research_ledger  # noqa: E402,F401
import research_spec  # noqa: E402,F401
import research_tree  # noqa: E402,F401
from cycle.cli import (check_clean, execute, finish_step, git_clean, git_scoped, header,  # noqa: E402,F401
                       main, make_cycle, parallel_partner, plan_lines, run_cycle, run_processes, start_step,
                       status_lines, step_outcome)
from cycle.compare import _first_diff, _lines, compare, compare_files, compare_keys  # noqa: E402,F401
from cycle.gate import (admission_trials, check_recorded_heads, copy_ledger, fields_check, gate,  # noqa: E402,F401
                        ledger_state, write_verdict)
from cycle.lock import lock, lock_exes, lock_inputs, lock_pin  # noqa: E402,F401
from cycle.phases import (CACHE_PATH_MAX, Cycle, Step, cache_path_problem, deepest_cache_path,  # noqa: E402,F401
                          exe_capabilities, library_ids, window_id)
from cycle.resolver import Resolver, placeholder  # noqa: E402,F401
from cycle.spec import (ATTEMPT_PHASES, BUILDS, CAP_KEYS, CAPACITY_FILES, CAPACITY_FLAG,  # noqa: E402,F401
                        COMPARE_MODES, COMPARE_SUFFIX, DEFAULT_OUT_ROOT, DSR_FROM_LEDGER, ERA_PHASES, EXE_NAMES,
                        EXES_PIN, EXIT_GATE, EXIT_OK, EXIT_PIN, EXIT_STOP, EXIT_USAGE, FIT_INCOMPLETE, GATE_REQUIRE,
                        HOLDINGS_INPUTS, INPUT_KEYS, MARGINAL_BUILT, MARGINAL_CANDIDATES, MARGINAL_REQUIRED,
                        MARGINAL_SPEC_FLAGS, MAX_ATTEMPTS, MEMBER_ID_RE, OPERAND, OUT_PHASES, PARALLEL, PHASE_OUTPUTS,
                        PHASES, RECEIPT_MODES, REQUIRED, RUNNER_PHASE_RULES, SCHEMA, SEC_INPUTS, SEC_STAGE_INPUTS,
                        SHA256_RE, SOURCE_FLAGS, STOP_PHASES, SUMM_V8, V5_AIM_RULE, V8_AIM_RULE, V8_FIXED_FLAGS,
                        W_BUILT, CycleError, argparse_values, as_built, effective_exes, find_spec, input_dir,
                        load_spec, option_value, parse_attempts, parse_runner_overrides, phase_present,
                        summ_protocol, validate_compare, validate_e27b, validate_marginal, validate_runner_phases,
                        validate_spec, validate_summ_protocol, validate_v8_keys)
from cycle.spec import EVAL_EXE  # noqa: E402,F401  (P9 ruling B2-R3: the engine verb's exes key and file name)
from cycle_verdict import PBO_JSON, SUMM_JSON, VERDICT, VerdictError, step_key  # noqa: E402,F401
from research_common import fmt_argv, sha256_file  # noqa: E402,F401

if __name__ == "__main__":
    # the lazily imported research_roles / research_gc / research_add_alpha `import research_cycle`: this module (the
    # cycle's names live in scripts/cycle/, so their CycleError is the one main() catches either way; H-1)
    sys.modules.setdefault("research_cycle", sys.modules[__name__])
    sys.exit(main())
