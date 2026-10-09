#!/usr/bin/env python3
"""Execute the atx-vol api/ restructure (Task 2 of the 2026-08-14 plan).

Consumes tmp/api-restructure/placement.csv + rewrite_map.csv (Task 1 output;
regenerate deterministically via api_restructure_measure.py if missing).

Does, in order:
  1. git mv every header in placement.csv to its measured destination.
  2. git mv every atx-vol/src/*.cpp, src/detail/*.cpp, and the 10 pre-existing
     src-local .hpp (al_probe, american_boundary, boundary_interp,
     corpus_board_fit, laned_greek_run, opra_batch_detail,
     slice_payload_padding, step_mark_memo, surface_db_seed, term_carry) into
     src/<module>/. src/simd/ is left untouched: its 15 .cpp are globbed
     flatly and non-recursively by atx-vol/CMakeLists.txt, and its 8
     *_avx2*.hpp kernels are already module-final (coordinator ruling).
  3. Rewrite every #include of an old atx/vol/... spelling, repo-wide, to its
     measured destination (public -> <atx/vol/api/...>, keeping the original
     angle/quote delimiter; private -> always "<module>/<name>.hpp", the
     spelling the new PRIVATE src/ include dir on internal targets resolves).
     Also rewrites same-directory / relative references to the 10 moved
     src-local headers (bare "name.hpp", "../name.hpp", "../src/name.hpp") to
     the same "<module>/name.hpp" form.
  4. Mechanically patches the target_sources() path list in
     atx-vol/CMakeLists.txt (the only one of the three CMakeLists.txt that
     lists literal src/foo.cpp paths) from src/foo.cpp -> src/<module>/foo.cpp.

Idempotent: re-running after a full apply touches nothing (every git_mv sees
dst already in place and skips; every rewrite regex targets only the OLD
spelling, which no longer appears in the tree).

Loud failure: any stem this script cannot place is reported by name and the
script exits 1. Module assignment for such a stem must be added to
CONTENT_DECIDED below (with a one-line rationale) before re-running.
"""
import csv
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(r"C:\atx")
VOL = REPO / "atx-vol"
TMP = REPO / "tmp" / "api-restructure"
CMAKE_LIB_FILE = VOL / "CMakeLists.txt"

sys.path.insert(0, str(Path(__file__).resolve().parent))
import api_restructure_measure as measure  # MODULE, DETAIL_MODULE (Task 1)

# ---- Step 1.2's explicit dict (brief, verbatim) -- cpp-only TUs whose
# primary header lives outside the 144 (tools/include or research/include
# satellite trees) or that have no header at all. ----
EXPLICIT = {
    "instrumentation_abi": "core", "boundary_interp": "pricing",
    "analytics_density": "analytics", "snapshot_cache": "storage",
    "track_gc": "storage", "track_compact_reconcile": "storage",
    "tearsheet": "backtest", "dispersion_run": "backtest",
    "dispersion_backtest": "backtest", "dispersion_workflow": "backtest",
    "listed_dispersion_pipeline": "backtest",
    "listed_dispersion_reconciliation": "backtest",
    "listed_definitions_cache": "marketdata", "run_report": "storage",
    "run_diagnostics": "storage", "run_archive": "storage",
    "backtest_driver": "backtest", "corpus_board_fit": "marketdata",
    "surface_db_seed": "storage", "step_mark_memo": "backtest",
    "laned_greek_run": "backtest", "slice_payload_padding": "storage",
    "american_boundary": "pricing", "surface_db_populate": "storage",
    "surface_db_build": "storage", "surface_db_admin": "storage",
    "convex_recovery": "fitting", "risk_surface_validation": "fitting",
    "prepared_portfolio": "backtest", "prepared_fitting": "fitting",
    "deriv_book": "backtest",
}

# ---- Stems absent from EXPLICIT + MODULE + DETAIL_MODULE, resolved by
# reading the file and following its actual callers -- see
# task-2-report.md for the full trace of each. ----
CONTENT_DECIDED = {
    # al_probe.hpp: env-gated Andersen-Lake hot-path probe. Included by
    # american.cpp, american_iv.cpp, boundary_interp.cpp (all pricing) vs
    # calib.cpp (fitting) and corpus_board_fit.cpp (marketdata) -- pricing
    # wins 3-of-5 and is the algorithm family the probe instruments (AL =
    # Andersen-Lake).
    "al_probe": "pricing",
    # term_carry.hpp: coherent_q_eff/interpolate_positive_log carry-curve
    # helpers. Included by priced_surface.cpp + priced_surface_view.cpp
    # (backtest) vs session.cpp (fitting) -- backtest wins 2-of-3.
    "term_carry": "backtest",
    # opra_batch_detail.hpp: Civil-kernel date parsing shared by
    # opra_batch.cpp and opra_hive.cpp (both marketdata); surface_db_admin.cpp
    # (storage) is the minority consumer. Coordinator ruling: "obviously
    # marketdata alongside opra_batch."
    "opra_batch_detail": "marketdata",
    # analytics_aggregate/io/primitives.cpp: siblings of analytics_density.cpp
    # (already dict'd "analytics" in EXPLICIT above) -- all four implement
    # pieces of the public analytics.hpp surface and lead with
    # #include "atx/vol/analytics.hpp".
    "analytics_aggregate": "analytics",
    "analytics_io": "analytics",
    "analytics_primitives": "analytics",
    # dispersion_strategy.cpp: DispersionStrategy/IStrategy/lifecycle_decide --
    # no header of its own; sibling of listed_dispersion_strategy.cpp
    # (dict'd "backtest") and includes strategy.hpp/backtest.hpp/dispersion.hpp
    # (all backtest).
    "dispersion_strategy": "backtest",
}

STEM_MODULE = {}
STEM_MODULE.update(measure.MODULE)
STEM_MODULE.update(measure.DETAIL_MODULE)
STEM_MODULE.update(EXPLICIT)
STEM_MODULE.update(CONTENT_DECIDED)

# The 10 pre-existing top-level src-local headers that move into src/<module>/.
# (The 8 src/simd/*_avx2*.hpp kernels are NOT here -- they stay in src/simd/.)
SRC_LOCAL_HEADER_STEMS = [
    "al_probe", "american_boundary", "boundary_interp", "corpus_board_fit",
    "laned_greek_run", "opra_batch_detail", "slice_payload_padding",
    "step_mark_memo", "surface_db_seed", "term_carry",
]


def module_for_stem(stem: str):
    if stem in STEM_MODULE:
        return STEM_MODULE[stem]
    if stem.endswith("_avx2"):
        base = stem[: -len("_avx2")]
        if base in STEM_MODULE:
            return STEM_MODULE[base]
    return None


def git_mv(src: Path, dst: Path) -> bool:
    """Returns True if a move happened, False if already-done (idempotent)."""
    if dst.exists() and not src.exists():
        return False
    if not src.exists():
        sys.exit(f"MISSING SOURCE: {src} (neither it nor {dst} exists)")
    if dst.exists():
        sys.exit(f"CONFLICT: both {src} and {dst} exist")
    dst.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "mv", str(src), str(dst)], check=True, cwd=REPO)
    return True


def move_headers() -> int:
    """Step 1.1: git mv every header in placement.csv (144 rows)."""
    n = 0
    with open(TMP / "placement.csv", newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            src = REPO / row["old_path"]
            dst = REPO / row["new_path"]
            if git_mv(src, dst):
                n += 1
    return n


def move_src_files() -> int:
    """Step 1.2: git mv every atx-vol/src/*.cpp, src/detail/*.cpp, and the
    10 src-local .hpp into src/<module>/."""
    unmatched = []
    candidates = (
        sorted((VOL / "src").glob("*.cpp"))
        + sorted((VOL / "src" / "detail").glob("*.cpp"))
        + sorted((VOL / "src").glob("*.hpp"))
    )
    n = 0
    for f in candidates:
        mod = module_for_stem(f.stem)
        if mod is None:
            unmatched.append(f.stem)
            continue
        if git_mv(f, VOL / "src" / mod / f.name):
            n += 1
    if unmatched:
        sys.exit(
            "UNASSIGNED src stem(s) -- add to CONTENT_DECIDED (with a "
            "one-line rationale) and re-run: " + ", ".join(sorted(set(unmatched)))
        )
    return n


# ---- Step 1.3: repo-wide include rewrite -----------------------------------

SKIP_DIR_NAMES = {".git", "atx-db", "atx-factor"}


def should_skip(rel_parts) -> bool:
    for part in rel_parts:
        if part in SKIP_DIR_NAMES or part.startswith("build"):
            return True
    return False


def iter_target_files():
    for ext in ("*.cpp", "*.hpp", "*.h", "*.in"):
        for f in REPO.rglob(ext):
            if should_skip(f.relative_to(REPO).parts[:-1]):
                continue
            yield f


def build_rewrite_patterns():
    patterns = []
    with open(TMP / "rewrite_map.csv", newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            old, new = row["old_include"], row["new_include"]
            is_public = new.startswith("atx/vol/")
            angle_re = re.compile(r"#include[ \t]*<" + re.escape(old) + r">")
            quote_re = re.compile(r'#include[ \t]*"' + re.escape(old) + r'"')
            if is_public:
                angle_repl, quote_repl = f"#include <{new}>", f'#include "{new}"'
            else:
                angle_repl = quote_repl = f'#include "{new}"'
            patterns.append((angle_re, angle_repl))
            patterns.append((quote_re, quote_repl))
    # src-local headers: bare "STEM.hpp", "../STEM.hpp", "src/STEM.hpp",
    # "../src/STEM.hpp" -> "<module>/STEM.hpp" (resolved via the new PRIVATE
    # src/ include dir on internal targets).
    for stem in SRC_LOCAL_HEADER_STEMS:
        mod = module_for_stem(stem)
        assert mod, stem
        pat = re.compile(r'#include[ \t]*"(?:\.\./)*(?:src/)?' + re.escape(stem) + r'\.hpp"')
        patterns.append((pat, f'#include "{mod}/{stem}.hpp"'))
    return patterns


def rewrite_includes() -> int:
    patterns = build_rewrite_patterns()
    changed = 0
    for f in iter_target_files():
        try:
            with open(f, "r", encoding="utf-8", newline="") as fh:
                text = fh.read()
        except (UnicodeDecodeError, OSError):
            continue
        new_text = text
        for pat, repl in patterns:
            new_text = pat.sub(repl, new_text)
        if new_text != text:
            with open(f, "w", encoding="utf-8", newline="") as fh:
                fh.write(new_text)
            changed += 1
    return changed


# ---- Step 3: mechanical CMake src/ path patch (atx-vol/CMakeLists.txt only --
# tests/ and python/ CMakeLists.txt list their OWN dir's files, never src/foo,
# so they need no path rewrite here; see task-2-report.md) ------------------

SRC_CPP_RE = re.compile(r"src/(?:detail/)?([A-Za-z0-9_]+)\.cpp")


def patch_cmake_src_paths() -> bool:
    text = CMAKE_LIB_FILE.read_text(encoding="utf-8")
    unmatched = []

    def repl(m: re.Match) -> str:
        stem = m.group(1)
        mod = module_for_stem(stem)
        if mod is None:
            unmatched.append(stem)
            return m.group(0)
        return f"src/{mod}/{stem}.cpp"

    new_text = SRC_CPP_RE.sub(repl, text)
    if unmatched:
        sys.exit(
            "CMakeLists.txt references unassigned stem(s): "
            + ", ".join(sorted(set(unmatched)))
        )
    if new_text != text:
        CMAKE_LIB_FILE.write_text(new_text, encoding="utf-8")
        return True
    return False


def main():
    n_headers = move_headers()
    print(f"headers moved: {n_headers}")
    n_src = move_src_files()
    print(f"src files moved: {n_src}")
    n_rewritten = rewrite_includes()
    print(f"files with rewritten includes: {n_rewritten}")
    cmake_changed = patch_cmake_src_paths()
    print(f"CMakeLists.txt src paths patched: {cmake_changed}")


if __name__ == "__main__":
    main()
