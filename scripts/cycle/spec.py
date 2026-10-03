"""The cycle spec (research_cycle.py, split move-only into scripts/cycle/ by P9 lane E2): the constants, CycleError,
the load of an ``atx.research-cycle-spec/v1`` file (a template resolved first, research_spec.py) and its validation,
and the parsers of the command line's --runner-override and --attempt."""
from __future__ import annotations

import json
from pathlib import Path
import re

import research_ledger
import research_spec
import research_tree

SCRIPTS_DIR = Path(__file__).resolve().parents[1]   # scripts/: a relative SPEC not found is looked up here


SCHEMA = "atx.research-cycle-spec/v1"
PHASES = ("fields", "check", "ref", "u", "fit", "card", "marginal", "gate", "w", "nav", "monitor", "summ")
COMPARE_SUFFIX = "-compare"            # the internal identity step after phase P is "P-compare"
STOP_PHASES = PHASES + tuple(p + COMPARE_SUFFIX for p in PHASES)
COMPARE_MODES = ("file", "csv-rows", "json-rows")
OUT_PHASES = ("fields", "ref", "u", "fit", "card", "w", "nav", "monitor")  # {out:PHASE} operands
ATTEMPT_PHASES = ("u", "fit", "w", "nav")
ERA_PHASES = ("fields", "u", "w", "nav")   # H-1: the phases each era of a roles: cycle runs (research_roles.py)
EXIT_OK, EXIT_USAGE, EXIT_PIN, EXIT_STOP, EXIT_GATE = 0, 2, 3, 4, 10
FIT_INCOMPLETE = 3                     # fit_composition_weights.py: "incomplete (rerun resumes)"
MAX_ATTEMPTS = 9
GATE_REQUIRE = ("all", "any")          # gate.require: every listed candidate admitted, or at least one
REQUIRED = {"schema", "name", "python", "runner", "inputs"}
SEC_STAGE_INPUTS = ("earnings_calendar", "insider", "sec_filings")   # dirs <ROOT>/<key>/ -> --sec-stages ROOT
SEC_INPUTS = ("sec_identity_bridge",) + SEC_STAGE_INPUTS            # all four or none (L9 design)
HOLDINGS_INPUTS = ("thirteenf", "ftd", "regsho_threshold", "security_master", "short_volume_ext")  # --<key> DIR
INPUT_KEYS = ("library", "recipe", "baseline_library", "role", "identity_bridge", "fund_events", "baseline_fields",
              "reference_admission", "reference_cell", "sic_events", "reference_combined", "reference_daily",
              "reference_orientations", "reference_daily_ic", "reference_weights") + SEC_INPUTS + HOLDINGS_INPUTS + \
    ("reuse_fields", "label_role", "reference_resid_parent")
# reference_resid_parent (v8 R-11, finding R6B-O-5; derived by research_spec for the cell whose template adds
# --theme-resid): the parent cell's composition_weights.json; the single-window fit step passes it as
# --theme-resid-parent PATH --theme-resid-parent-sha256 PIN (the fitter refuses a re-fit that is not that file plus the
# theme_residualise block); a pooled (era) fit never gets it.
# label_role {dir, path (the role's manifest.json), sha256} (v8 Ruling E-25, cell B0c): every phase that replays the
# book MARKS it with it (nav and ref: --label-role PATH --label-role-sha256 PIN, the manifest bound in the runner
# receipt; the NAV verb's capacity pass forwards both); signals, fields and every decision input stay on inputs.role.
# Review F-8: a spec with a ref phase and a label role is an add-alpha child of a labelled parent (a template drops its
# parent's ref, research_spec.IDENTITY_SECTIONS), so its ref reproduces a labelled NAV and is compared with that NAV's
# labelled S2 daily CSV on equal footing.
# F-2's marginal IC verb (atx-impl/src/strategy_marginal_ic.cpp, dispatch_marginal_ic / run_marginal_ic): every option
# takes one value and a run without MARGINAL_REQUIRED is refused. The step builds MARGINAL_BUILT from the spec (paths and
# pins of inputs.library, inputs.<marginal.pool>, inputs.role, inputs.<marginal.themes>, this cycle's cache, fields and
# output); marginal.flags may set only MARGINAL_SPEC_FLAGS (unsigned integers; --min-names defaults to the u pass's).
MARGINAL_REQUIRED = ("--candidate-cache", "--library", "--pool", "--role", "--output")
MARGINAL_CANDIDATES = "--candidates"   # P9 ruling P4: marginal.candidates -> the verb's --candidates FILE (lane S1)
# Lane S1's value options --pair-cache DIR and --verified-digests FILE (P9 E1 x S1 merge note) belong to the step, never
# to marginal.flags: no spec sets them until E2 wires them (S1's --workers / --exclude-self are not value pairs here).
MARGINAL_BUILT = MARGINAL_REQUIRED + ("--library-sha256", "--pool-sha256", "--themes", "--fields", MARGINAL_CANDIDATES,
                                      "--pair-cache", "--verified-digests")
MEMBER_ID_RE = re.compile(r"[A-Za-z0-9_.:-]+")
W_BUILT = ("--library", "--library-sha256", "--train", "--train-sha256", "--train-fields", "--train-fields-sha256",
           "--output", "--candidate-cache", "--composition-weights", "--composition-weights-sha256")
MARGINAL_SPEC_FLAGS = {"--min-names": (3, None), "--max-memory-mib": (32, 16384)}   # option: (min, max) as the verb
OPERAND = re.compile(r"\{(input|out):([A-Za-z0-9_]+)\}")
SOURCE_FLAGS = (("finra", "--finra"), ("tickerhistory", "--tickerhistory"), ("lake", "--lake"),
                ("finra_short_volume", "--finra-short-volume"))
DSR_FROM_LEDGER = "ledger+1"           # summ.dsr_n: resolved at scoring time from the trial ledger
SUMM_V8 = "v8"                         # nav_summ --protocol v8: seed 20260929, 4,999 draws, origin + window_id lines
V8_FIXED_FLAGS = ("--seed", "--draws", "--block")   # review F-2: v8-prereg item 4's bootstrap; never in a v8 summ.extra
V5_AIM_RULE, V8_AIM_RULE = "ew-theme-aim-v1", "ew-theme-aim-v2"   # Ruling E-27b: a v8 spec never fits the v5 rule
DEFAULT_OUT_ROOT = "build-equity"      # derived cache / fit roots and the cycle dir when the spec has no out_root
RECEIPT_MODES = ("every-phase",)       # spec "receipts": the direct phases run through the bounded runner too
CAP_KEYS = ("seconds", "max_rss_mib", "min_free_mib")
# spec "build": the exe dir and the DLL dirs (env_path_prepend) of each research build tree (platform review P-4)
BUILDS = {
    "equity": {"bin": "build-equity/bin", "path": ["C:/atx-cache/vcpkg_installed/x64-windows/debug/bin",
                                                   "C:/atx-cache/vcpkg_installed/x64-windows/bin"]},
    "equity-rel": {"bin": "build-equity-rel/bin", "path": ["C:/atx-cache/vcpkg_installed/x64-windows/bin"]},
}
EXE_NAMES = {"ic": "atx-equity-strategy-ic.exe", "nav": "atx-equity-strategy-targets.exe"}
EXES_PIN = "exes_sha256"               # P9 OR-2: spec {exe key: SHA-256}, written by `lock --exes`
SHA256_RE = re.compile(r"[0-9a-f]{64}")
# ruling E1-REUSE-a2: the spec key naming each phase's output (Cycle.inherited: a template keeps its parent's output
# when the name is the parent's)
PHASE_OUTPUTS = {"fields": ("fields", "output"), "u": ("ic", "u_output"), "w": ("ic", "w_output"),
                 "fit": ("fit", "output"), "card": ("card", "output"), "marginal": ("marginal", "output"),
                 "monitor": ("monitor", "output"), "nav": ("nav", "output")}
# P9 NV-4: the NAV verb (strategy_nav_v7.cpp) writes summary.json before the capacity pass and v7_extras.json; a NAV
# whose argv carries CAPACITY_FLAG is done only when CAPACITY_FILES exist beside its summary.json
CAPACITY_FLAG, CAPACITY_FILES = "--capacity-curve", ("capacity_curve.csv", "v7_extras.json")
# Default per-phase runner caps, applied when the spec's runner.phases does not name the phase. OD-2 (owner ruling
# E-1, 2026-09-29): the IC passes get 2,560 MiB and 300 s on a role longer than 1,200 dates (the 3-year role has
# 1,155, the 4-year role about 1,405); every other phase keeps the runner's own caps.
RUNNER_PHASE_RULES = (
    {"phases": ("u", "w"), "when": {"role_dates_over": 1200}, "caps": {"seconds": 300, "max_rss_mib": 2560}},
)
# P9 OR section 5: under a host memory budget (--host-budget-mib) a pending step of the key phase runs side by side with
# the next pending step of the value phase (only its own compare steps between): neither reads what the other writes
PARALLEL = {"ref": "u", "card": "marginal"}


class CycleError(Exception):
    def __init__(self, message: str, code: int = EXIT_STOP):
        super().__init__(message)
        self.code = code


# ------------------------------------------------------------------ spec
def find_spec(arg: str) -> Path:
    p = Path(arg)
    if p.exists():
        return p
    alt = SCRIPTS_DIR / arg
    if alt.exists():
        return alt
    raise CycleError(f"spec not found: {arg}", EXIT_USAGE)


def load_spec(path: Path, _seen: tuple = ()) -> dict:
    """A cycle spec, validated; a template (research_spec.py: a cell as its parent's spec plus the registered change)
    is resolved first, its parent chain loaded the same way (a cycle of parents is refused)."""
    try:
        spec = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise CycleError(f"spec {path}: {exc}", EXIT_USAGE) from exc
    if research_spec.is_template(spec):
        here = Path(path).resolve()
        if here in _seen:
            raise CycleError(f"spec {path}: template parents form a cycle", EXIT_USAGE)
        try:
            spec = research_spec.resolve(spec, Path(path), lambda p: load_spec(p, _seen + (here,)), research_tree.REPO)
        except research_spec.TemplateError as exc:
            raise CycleError(str(exc), EXIT_USAGE) from exc
    validate_spec(spec)
    return spec


def as_built(fields: dict | None) -> bool:
    """A fields section pinning an AS-BUILT dir: manifest_sha256 present (a digest, or null until `lock --write`)."""
    return isinstance(fields, dict) and "manifest_sha256" in fields


def validate_spec(spec: dict) -> None:
    if not isinstance(spec, dict) or spec.get("schema") != SCHEMA:
        raise CycleError(f"spec schema must be {SCHEMA}", EXIT_USAGE)
    missing = sorted(REQUIRED - set(spec))
    if missing:
        raise CycleError(f"spec misses {', '.join(missing)}", EXIT_USAGE)
    unknown = sorted(set(spec["inputs"]) - set(INPUT_KEYS))
    if unknown:
        raise CycleError(f"spec inputs: unknown key(s) {', '.join(unknown)} (known: {', '.join(INPUT_KEYS)})",
                         EXIT_USAGE)
    for key, item in spec["inputs"].items():
        if not isinstance(item, dict) or "path" not in item or "sha256" not in item:
            raise CycleError(f"spec inputs.{key} needs path and sha256 (null until `lock`)", EXIT_USAGE)
    r = spec["runner"]
    if not all(k in r for k in ("script", "seconds", "max_rss_mib", "min_free_mib")):
        raise CycleError("spec runner needs script, seconds, max_rss_mib, min_free_mib", EXIT_USAGE)
    validate_runner_phases(r.get("phases"), r["seconds"])
    need = {"fields": ("output",), "static_check": ("script",), "ic": ("u_output", "flags"),
            "fit": ("script", "output", "flags"), "gate": ("admitted",), "nav": ("output", "rule", "flags"),
            "summ": ("script", "dsr_n"), "card": ("script", "output"), "monitor": ("script", "output"),
            "ref": ("output", "combined"), "marginal": ("output",)}
    for section, keys in need.items():
        if section in spec and not all(k in spec[section] for k in keys):
            raise CycleError(f"spec {section} needs {', '.join(keys)}", EXIT_USAGE)
    validate_v8_keys(spec)
    f = spec.get("fields")
    if f is not None and not as_built(f) and not all(k in f for k in ("builder", "list")):
        raise CycleError("spec fields needs builder and list (a built fields dir) or manifest_sha256 (as built)",
                         EXIT_USAGE)
    if as_built(f) and f["manifest_sha256"] is not None and (not isinstance(f["manifest_sha256"], str) or
                                                             len(f["manifest_sha256"]) != 64):
        raise CycleError("spec fields.manifest_sha256 must be a SHA-256 hex digest (null until `lock --write`)",
                         EXIT_USAGE)
    if "ref" in spec:
        ref = spec["ref"]
        if "nav" not in spec or ref["combined"] not in spec["inputs"] or ref["output"] == spec["nav"]["output"]:
            raise CycleError("spec ref needs nav (its construction), inputs.<ref.combined> and an output of its own",
                             EXIT_USAGE)
    if "nav" in spec and "w_output" not in spec.get("ic", {}):
        raise CycleError("spec nav needs ic.w_output (the weighted pass feeds the NAV)", EXIT_USAGE)
    if ("fit" in spec and "ic" not in spec) or ("gate" in spec and "fit" not in spec) or \
            (("ic" in spec or "static_check" in spec) and "fields" not in spec):
        raise CycleError("spec: fit needs ic; gate needs fit; ic and static_check need fields", EXIT_USAGE)
    exes = effective_exes(spec)
    if ("ic" in spec and "ic" not in exes) or ("nav" in spec and "nav" not in exes):
        raise CycleError("spec exes needs ic (for ic) and nav (for nav)", EXIT_USAGE)
    for key in ("role", "library"):
        if "ic" in spec and key not in spec["inputs"]:
            raise CycleError(f"spec inputs needs {key}", EXIT_USAGE)
    if "summ" in spec and "nav" not in spec:
        raise CycleError("spec summ needs nav", EXIT_USAGE)
    if "card" in spec and "fit" not in spec:
        raise CycleError("spec card needs fit (it reads this cycle's admission.json)", EXIT_USAGE)
    if "monitor" in spec and ("nav" not in spec or "fit" not in spec):
        raise CycleError("spec monitor needs fit and nav", EXIT_USAGE)
    if "static_check" in spec and "baseline_library" not in spec["inputs"]:
        raise CycleError("spec static_check needs inputs.baseline_library", EXIT_USAGE)
    if spec.get("gate", {}).get("require", "all") not in GATE_REQUIRE:
        raise CycleError(f"spec gate.require must be one of {', '.join(GATE_REQUIRE)}", EXIT_USAGE)
    sm = spec.get("summ", {})
    if "cells_from_ledger" in sm and (sm["cells_from_ledger"] is not True or "cells" in sm or not sm.get("ledger")):
        raise CycleError("spec summ.cells_from_ledger must be true, excludes summ.cells and needs summ.ledger",
                         EXIT_USAGE)
    if sm.get("dsr_n") == DSR_FROM_LEDGER and (not sm.get("ledger") or "cells" in sm):
        raise CycleError(f"spec summ.dsr_n \"{DSR_FROM_LEDGER}\" needs summ.ledger and excludes summ.cells",
                         EXIT_USAGE)
    validate_compare(spec)
    cells = sm.get("cells")
    if cells is not None:
        if not isinstance(cells, list) or not cells or not all(isinstance(c, str) and c for c in cells) or \
                len(set(cells)) != len(cells) or spec["nav"]["output"] in cells:
            raise CycleError("spec summ.cells must list distinct prior NAV dirs (not this cycle's nav.output)",
                             EXIT_USAGE)
        if len(cells) + 1 != spec["summ"]["dsr_n"]:
            raise CycleError(f"spec summ.cells lists {len(cells)} prior cells + this one, dsr_n is "
                             f"{spec['summ']['dsr_n']}: the listed grid must be the declared N trials", EXIT_USAGE)


def validate_runner_phases(phases, seconds=None) -> None:
    """runner.phases (and ``seconds``, the runner's own cap): positive caps, a time cap at most the bounded runner's
    maximum (research_tree.RUNNER_MAX_SECONDS, P9 OR-1: refused at load, never as a runner exit 2 mid-cycle)."""
    refusal = research_tree.seconds_cap_refusal("spec runner.seconds", seconds)
    if refusal:
        raise CycleError(refusal, EXIT_USAGE)
    if phases is None:
        return
    if not isinstance(phases, dict) or not all(p in PHASES and isinstance(c, dict) and c and set(c) <= set(CAP_KEYS)
                                               and all(type(v) in (int, float) and v > 0 for v in c.values())
                                               for p, c in phases.items()):
        raise CycleError(f"spec runner.phases must map a phase to caps {{{', '.join(CAP_KEYS)}}} (positive numbers)",
                         EXIT_USAGE)
    for p, c in phases.items():
        refusal = research_tree.seconds_cap_refusal(f"spec runner.phases.{p}.seconds", c.get("seconds"))
        if refusal:
            raise CycleError(refusal, EXIT_USAGE)


def argparse_values(flags: list, option: str) -> list:
    """Every value argparse (``allow_abbrev`` on, its default) reads for the long ``option`` in an argv list, in order:
    ``--option V``, ``--option=V`` and any abbreviation (``--opt V``, ``--opt=V``: argparse takes a unique prefix, and a
    prefix shared with another option fails that run, so every prefix is counted); None for an occurrence with no value.
    The value an occurrence consumes is skipped; a bare ``--`` ends the options (review R6B-C-4)."""
    name, out, i = option[2:], [], 0
    while i < len(flags):
        tok = flags[i]
        i += 1
        if tok == "--":
            break
        if not (isinstance(tok, str) and tok.startswith("--") and len(tok) > 2):
            continue
        head, eq, value = tok[2:].partition("=")
        if not name.startswith(head):
            continue
        if eq:
            out.append(value)
        else:
            out.append(flags[i] if i < len(flags) and isinstance(flags[i], str) else None)
            i += 1
    return out


def validate_e27b(spec: dict) -> None:
    """Ruling E-27b on the argv the tools parse (review R6B-C-4). A spec is v8 when it is a verdict spec or nav_summ
    reads --protocol v8 from summ.extra in any spelling (``--protocol v8``, ``--protocol=v8``, an abbreviation); a v8
    spec never fits the v5 rule ew-theme-aim-v1, whatever the spelling of --composition in fit.flags (``=``, an
    abbreviation, a repeat: argparse keeps the last). The cycle's own readers (summ_protocol, option_value, the template
    maps) read one two-token pair, so --protocol (every spec) and a v8 spec's --composition are written once, that way."""
    extra = (spec.get("summ") or {}).get("extra") or []
    flags = (spec.get("fit") or {}).get("flags") or []
    protocols, compositions = argparse_values(extra, "--protocol"), argparse_values(flags, "--composition")
    v8 = spec.get("verdict") is True or SUMM_V8 in protocols
    if v8 and V5_AIM_RULE in compositions:
        raise CycleError(f"spec fit: {V5_AIM_RULE} is the v5 R4' rule (aim gains normalised globally); a v8 spec fits "
                         f"R-3 on an ew-theme-v1 parent with {V8_AIM_RULE} (Ruling E-27b: gains renormalised inside "
                         "each theme, member cap 1/(2T))", EXIT_USAGE)
    for where, given, option, values in (("summ.extra", extra, "--protocol", protocols),
                                         ("fit.flags", flags, "--composition", compositions if v8 else [])):
        if values and (len(values) > 1 or option_value(given, option) != values[0]):
            raise CycleError(f"spec {where}: write {option} once, as the two tokens '{option} VALUE' (argparse also "
                             f"reads '{option}=VALUE', an abbreviation or a repeat, which the cycle's own checks do not "
                             f"see; review R6B-C-4)", EXIT_USAGE)


def validate_v8_keys(spec: dict) -> None:
    """The platform v8 spec keys: build, out_root, receipts, verdict, marginal, the stage inputs, reuse_fields; Ruling
    E-27b on the parsed argv (``validate_e27b``)."""
    if "build" in spec and spec["build"] not in BUILDS:
        raise CycleError(f"spec build must be one of {', '.join(BUILDS)}", EXIT_USAGE)
    if EXES_PIN in spec:
        pins, exes = spec[EXES_PIN], effective_exes(spec)
        if not (isinstance(pins, dict) and pins and all(k in exes and isinstance(v, str) and SHA256_RE.fullmatch(v)
                                                        for k, v in pins.items())):
            raise CycleError(f"spec {EXES_PIN} must map exes keys ({', '.join(exes) or 'none'}) to SHA-256 digests "
                             "(`lock --exes` writes it)", EXIT_USAGE)
    if "out_root" in spec and (not isinstance(spec["out_root"], str) or not spec["out_root"] or
                               Path(spec["out_root"]).is_absolute()):
        raise CycleError("spec out_root must be a root-relative directory (the bounded runner writes only inside "
                         "its root)", EXIT_USAGE)
    if spec.get("receipts", RECEIPT_MODES[0]) not in RECEIPT_MODES:
        raise CycleError(f"spec receipts must be one of {', '.join(RECEIPT_MODES)}", EXIT_USAGE)
    if "verdict" in spec and type(spec["verdict"]) is not bool:
        raise CycleError("spec verdict must be true or false", EXIT_USAGE)
    if "marginal" in spec:
        validate_marginal(spec)
    w_flags = spec.get("ic", {}).get("w_flags")
    if w_flags is not None and (not isinstance(w_flags, dict) or not all(
            isinstance(k, str) and k.startswith("--") and k not in W_BUILT and (type(v) is bool or isinstance(v, str))
            for k, v in w_flags.items())):
        raise CycleError(f"spec ic.w_flags maps an option of the w pass only to a value, true or false (not one the "
                         f"step builds: {', '.join(W_BUILT)})", EXIT_USAGE)
    if w_flags:
        try:
            research_spec.apply_flags(spec["ic"]["flags"], w_flags, "ic.w_flags")
        except research_spec.TemplateError as exc:
            raise CycleError(f"spec {exc}", EXIT_USAGE) from exc
    if "ledger_copy" in spec.get("summ", {}) and not (isinstance(spec["summ"]["ledger_copy"], str) and
                                                        spec["summ"].get("ledger")):
        raise CycleError("spec summ.ledger_copy (a path) needs summ.ledger", EXIT_USAGE)
    validate_summ_protocol(spec)
    validate_e27b(spec)
    inputs = spec["inputs"]
    sec = [k for k in SEC_INPUTS if k in inputs]
    if sec and len(sec) != len(SEC_INPUTS):
        raise CycleError(f"spec inputs: the SEC stage inputs {', '.join(SEC_INPUTS)} come together (missing "
                         f"{', '.join(k for k in SEC_INPUTS if k not in inputs)})", EXIT_USAGE)
    if sec:
        dirs = [Path(input_dir(inputs[k])) for k in SEC_STAGE_INPUTS]
        if any(d.name != k for d, k in zip(dirs, SEC_STAGE_INPUTS)) or len({d.parent.as_posix() for d in dirs}) != 1:
            raise CycleError("spec inputs: earnings_calendar, insider and sec_filings must be the directories of those "
                             "names under one alpha-panel root (--sec-stages)", EXIT_USAGE)
    if "reuse_fields" in inputs and ("fields" not in spec or as_built(spec["fields"])):
        raise CycleError("spec inputs.reuse_fields needs a built fields section (not an as-built manifest_sha256)",
                         EXIT_USAGE)


def option_value(flags: list, option: str) -> str | None:
    """The value after `option` in a flag list (None when absent or last)."""
    k = flags.index(option) if option in flags else -1
    return flags[k + 1] if 0 <= k < len(flags) - 1 else None


def summ_protocol(spec: dict) -> str | None:
    """"v8" when the spec's scoring step runs under the v8 pre-registration (review C-2): a verdict spec ("verdict":
    true, a v8 key) or "--protocol v8" in summ.extra. None otherwise: the v7 argv, unchanged."""
    extra = spec.get("summ", {}).get("extra", [])
    return SUMM_V8 if spec.get("verdict") is True or option_value(extra, "--protocol") == SUMM_V8 else None


def validate_summ_protocol(spec: dict) -> None:
    """summ.origin (contract K5 class of the cell, nav_summ --origin) and a verdict spec's protocol (review C-2)."""
    sm = spec.get("summ")
    if not isinstance(sm, dict):
        return
    extra = sm.get("extra", [])
    if "origin" in sm:
        origins = research_ledger.backtest_integrity().ORIGINS
        if sm["origin"] not in origins:
            raise CycleError(f"spec summ.origin must be one of {', '.join(origins)} (contract K5)", EXIT_USAGE)
        if "--origin" in extra:
            raise CycleError("spec summ: the origin is summ.origin or --origin in summ.extra, not both", EXIT_USAGE)
    if spec.get("verdict") is True and option_value(extra, "--protocol") not in (None, SUMM_V8):
        raise CycleError("spec verdict: a verdict cell is scored under nav_summ --protocol v8 (the v8 "
                         "pre-registration); summ.extra asks for another protocol", EXIT_USAGE)
    fixed = [x for x in extra if isinstance(x, str) and x.split("=", 1)[0] in V8_FIXED_FLAGS]
    if fixed and summ_protocol(spec) == SUMM_V8:
        raise CycleError(f"spec summ: a v8 scoring step runs the pre-registered paired bootstrap (v8-prereg item 4: "
                         f"seed 20260929, 4,999 draws, block 21); summ.extra may not set {', '.join(fixed)}",
                         EXIT_USAGE)


def validate_marginal(spec: dict) -> None:
    """A marginal section the step can always turn into the verb's full argv (refused when the spec is loaded, so at
    plan time, never half way through a run): the pool and the themes weights are pinned inputs, flags are value pairs
    of MARGINAL_SPEC_FLAGS within the verb's ranges, and the step's own options are never repeated there."""
    m, inputs = spec["marginal"], spec["inputs"]
    if "ic" not in spec or m.get("pool", "reference_combined") not in inputs:
        raise CycleError("spec marginal needs ic and inputs.<marginal.pool> (default reference_combined: the parent's "
                         "combined signal)", EXIT_USAGE)
    themes = m.get("themes")
    if themes is not None and (not isinstance(themes, str) or themes not in inputs):
        raise CycleError("spec marginal.themes must name the pinned input holding the pool's composition weights (e.g. "
                         "reference_weights = the parent's fit output composition_weights.json: the verb's --themes "
                         "takes that file and binds it by the pool's composition_weights_sha256)", EXIT_USAGE)
    flags = m.get("flags", [])
    if not isinstance(flags, list) or len(flags) % 2 or not all(isinstance(x, str) for x in flags):
        raise CycleError("spec marginal.flags must be option/value pairs (strings); the verb's --themes takes the "
                         "weights file: name its input in marginal.themes", EXIT_USAGE)
    ic_names = option_value(spec["ic"].get("flags", []), "--min-names")
    for key, value in zip(flags[::2], flags[1::2]):
        if key in MARGINAL_BUILT:
            raise CycleError(f"spec marginal.flags: {key} is built by the step from the spec (it builds "
                             f"{', '.join(MARGINAL_BUILT)})", EXIT_USAGE)
        low, high = MARGINAL_SPEC_FLAGS.get(key, (None, None))
        if low is None or not value.isdigit() or int(value) < low or (high is not None and int(value) > high):
            raise CycleError(f"spec marginal.flags: {key} {value!r} (allowed: "
                             + ", ".join(f"{k} N >= {lo}" + (f" <= {hi}" if hi else "") for k, (lo, hi) in
                                         MARGINAL_SPEC_FLAGS.items()) + ")", EXIT_USAGE)
    if "--min-names" not in flags[::2] and ic_names is not None and not ic_names.isdigit():
        raise CycleError(f"spec marginal: the u pass's --min-names {ic_names!r} is not a count (set marginal.flags "
                         "--min-names)", EXIT_USAGE)
    ids = m.get("candidates")
    if ids is not None and not (isinstance(ids, list) and ids and len(set(ids)) == len(ids) and
                                all(isinstance(x, str) and MEMBER_ID_RE.fullmatch(x) for x in ids)):
        raise CycleError("spec marginal.candidates must be distinct member ids (P9 ruling P4: the verb's --candidates "
                         "FILE, one id per line)", EXIT_USAGE)


def input_dir(item: dict) -> str:
    """A pinned input's directory: its dir key, else the parent of its path."""
    return item.get("dir") or str(Path(item["path"]).parent).replace("\\", "/")


def effective_exes(spec: dict) -> dict:
    """spec exes with the build key applied: a missing exe defaults to the build's bin dir, a bare name goes in it."""
    exes = dict(spec.get("exes") or {})
    b = BUILDS.get(spec.get("build"))
    if b is None:
        return exes
    for key, name in EXE_NAMES.items():
        value = exes.get(key, name)
        exes[key] = value if "/" in value or "\\" in value else f"{b['bin']}/{value}"
    return exes


def phase_present(spec: dict, phase: str) -> bool:
    """Whether the spec runs `phase` (its section is present)."""
    if phase == "w":
        return "w_output" in spec.get("ic", {})
    return {"check": "static_check", "u": "ic"}.get(phase, phase) in spec


def validate_compare(spec: dict) -> None:
    items = spec.get("compare")
    if items is None:
        return
    if not isinstance(items, list) or not items:
        raise CycleError("spec compare must be a non-empty list", EXIT_USAGE)
    names = set()
    for c in items:
        if not isinstance(c, dict) or not all(isinstance(c.get(k), str) and c[k] for k in ("name", "after", "mode",
                                                                                           "a", "b")):
            raise CycleError("spec compare items need name, after, mode, a and b (strings)", EXIT_USAGE)
        if c["name"] in names:
            raise CycleError(f"spec compare: duplicate name {c['name']}", EXIT_USAGE)
        names.add(c["name"])
        if c["after"] not in PHASES or not phase_present(spec, c["after"]):
            raise CycleError(f"spec compare {c['name']}: after {c['after']!r} is not a phase this spec runs",
                             EXIT_USAGE)
        if c["mode"] not in COMPARE_MODES:
            raise CycleError(f"spec compare {c['name']}: mode must be one of {', '.join(COMPARE_MODES)}", EXIT_USAGE)
        if "keys" in c and (c["mode"] == "file" or not isinstance(c["keys"], str) or not c["keys"]):
            raise CycleError(f"spec compare {c['name']}: keys (a library JSON path) is for the row modes only",
                             EXIT_USAGE)
        if "keys_in" in c and ("keys" not in c or not isinstance(c["keys_in"], str) or not c["keys_in"]):
            raise CycleError(f"spec compare {c['name']}: keys_in (a library JSON path) narrows keys", EXIT_USAGE)
        for side in [s for s in ("a", "b", "keys", "keys_in") if s in c]:
            for kind, key in OPERAND.findall(c[side]):
                ok = key in spec["inputs"] if kind == "input" else key in OUT_PHASES and phase_present(spec, key)
                if not ok:
                    raise CycleError(f"spec compare {c['name']}: operand {{{kind}:{key}}} names no {kind} of this "
                                     "spec", EXIT_USAGE)
            if "{" in OPERAND.sub("", c[side]) or "}" in OPERAND.sub("", c[side]):
                raise CycleError(f"spec compare {c['name']}: malformed operand {c[side]!r}", EXIT_USAGE)


def parse_runner_overrides(items: list[str]) -> dict:
    out = {}
    for item in items or []:
        key, _, value = item.partition("=")
        if key not in ("seconds", "max_rss_mib", "min_free_mib") or not value.replace(".", "", 1).isdigit():
            raise CycleError(f"--runner-override {item}: expected seconds|max_rss_mib|min_free_mib=NUMBER", EXIT_USAGE)
        out[key] = int(value) if value.isdigit() else float(value)
        refusal = research_tree.seconds_cap_refusal("--runner-override seconds", out[key]) if key == "seconds" else None
        if refusal:                                       # P9 OR-1: refused here, not as a runner exit 2 mid-cycle
            raise CycleError(refusal, EXIT_USAGE)
    return out


def parse_attempts(items: list[str]) -> dict:
    out = {}
    for item in items or []:
        phase, _, n = item.partition("=")
        if phase not in ATTEMPT_PHASES or not n.isdigit() or not 1 <= int(n) <= MAX_ATTEMPTS:
            raise CycleError(f"--attempt {item}: expected PHASE=N, PHASE in {', '.join(ATTEMPT_PHASES)}, "
                             f"1 <= N <= {MAX_ATTEMPTS}", EXIT_USAGE)
        out[phase] = int(n)
    return out
