#!/usr/bin/env python3
"""Presence-based leakage adjudication for the VRP trainer's fold plan.

Independently reconstructs atx-vol-vrp-train's purged/embargoed anchored
walk-forward (tools/vrp_train.hpp build_vrp_observations + make_vrp_plan +
src/backtest/research_validation.cpp make_purged_walk_forward_plan) from a
frozen vrp_panel_v1 TSV, then adjudicates every ADMITTED TRAIN row against
the corpus surface-presence data: the row's TRUE label end is the 21st
forward BAR on the SYMBOL'S OWN bar axis (bar = session where the symbol has
a fitted surface; `surface_presence.txt`: `KEY <date>` session lines each
followed by `surface <SYM> ...` membership lines), and a train row whose
true end lands PAST its fold's earliest test decision has integrated
test-period prices into a training label -- demonstrated leakage (the fix-2
review's blocker; this script is that review's adjudication, kept as a QA
asset).

Semantics reconstructed (--label-end-axis):
  emitted (default, the shipped trainer): label_end = the symbol's own 21st
    emitted successor row's timestamp -- an UPPER bound on the true bar-axis
    end (emitted rows are a subset of bars), with the --max-label-span
    reject-and-count cap (default 42 pooled sessions) bounding the embargo.
  pooled (the reverted 5c0f9504 semantics, kept for regression
    adjudication): label_end = pooled-session-axis t+21, no span cap --
    UNDERSTATES bar-holey symbols' windows; this mode is how the leak class
    is demonstrated.

Rows decided before presence coverage begins are adjudicated by the
STRUCTURAL bound instead: true end <= recorded emitted-axis end (subset
property), so recorded_end <= test_min proves them safe ("bounded-safe";
only meaningful under --label-end-axis emitted -- under pooled the recorded
end is not an upper bound and such rows count as indeterminate).

Optional --metrics <vrp_metrics.tsv> cross-checks the reconstruction against
the trainer's own artifact (per-fold n_train/n_test/purged/embargoed + the
rejection counters) and fails closed (exit 2) on ANY mismatch, so the
adjudication provably ran on the trainer's actual plan.

Adjudicability guards (both exit 2, fix2-review round-3 minors):
  n_bad_spot: the panel builder's bar axis is presence AND finite positive
    spot (load_vrp_series skips bad-spot sessions), so a panel whose
    `# n_bad_spot=` meta counter is NONZERO has a bar axis SPARSER than
    presence -- presence-derived 21st forward bars would come too early and
    the oracle would be optimistic. Refused outright. A panel without the
    meta line (hand-built fixtures) proceeds with a printed note.
  emitted-but-not-present: EVERY panel row date >= coverage_start (decision,
    successor, rejected, and tail rows alike -- recorded label ends rest on
    successor rows' timestamps) must be a presence bar of its symbol.

Exit codes: 0 zero violations; 1 >= 1 train row with true end > test_min
(details printed); 2 bad args, malformed inputs, nonzero panel n_bad_spot
meta, panel/presence inconsistency (any emitted row absent from presence),
or reconstruction mismatch against --metrics.

Run: python -m pytest atx-vol/scripts/vrp_leak_adjudicate_test.py -q
"""

from __future__ import annotations

import argparse
import bisect
import sys
from dataclasses import dataclass, field
from pathlib import Path

from vrp_panel_qa import HORIZON_SESSIONS, parse_tsv_file

DEFAULT_MAX_LABEL_SPAN = 2 * HORIZON_SESSIONS  # trainer default (42)


# ── Panel -> observations (mirrors build_vrp_observations) ────────────────


@dataclass
class Obs:
    symbol: str
    date: str
    ts: int
    label_end_ts: int
    label_end_date: str


@dataclass
class ObsBuild:
    obs: list[Obs] = field(default_factory=list)
    n_labeled: int = 0
    n_rejected_no_t21: int = 0
    n_rejected_span_cap: int = 0
    spans: list[int] = field(default_factory=list)  # pooled-session spans, pre-cap


def is_finite_label(text: str) -> bool:
    if text.lower() in ("nan", "-nan", "+nan"):
        return False
    value = float(text)
    return value == value and abs(value) != float("inf")


def build_observations(
    rows: list[dict[str, str]], axis: str, max_label_span: int | None
) -> ObsBuild:
    """One entry per usable labeled row, in canonical (ts, symbol) order --
    the same admission rule, label_end semantics, and span cap as the
    trainer (axis='pooled' reproduces the reverted 5c0f9504 build)."""
    keyed = sorted(
        ((int(r["entry_ts_ns"]), r["symbol"], r["date"], is_finite_label(r["label"]))
         for r in rows),
        key=lambda t: (t[0], t[1]),
    )
    session_ts = sorted({t[0] for t in keyed})
    sess_idx = {ts: i for i, ts in enumerate(session_ts)}
    ts_date: dict[int, str] = {}
    for ts, _sym, date, _lab in keyed:
        if ts_date.setdefault(ts, date) != date:
            raise ValueError(f"session ts {ts} maps to two dates")
    # Date order must mirror timestamp order: the adjudication compares by
    # date string, which is only sound if the two axes are isomorphic.
    session_dates = [ts_date[ts] for ts in session_ts]
    if session_dates != sorted(session_dates):
        raise ValueError("panel session dates are not ordered like entry_ts_ns")
    per_sym: dict[str, list[tuple[int, str, bool]]] = {}
    for ts, sym, date, labeled in keyed:
        per_sym.setdefault(sym, []).append((ts, date, labeled))

    out = ObsBuild()
    for sym, srows in sorted(per_sym.items()):
        for p, (ts, date, labeled) in enumerate(srows):
            if not labeled:
                continue
            out.n_labeled += 1
            if p + HORIZON_SESSIONS >= len(srows):
                out.n_rejected_no_t21 += 1
                continue
            succ_ts, succ_date, _ = srows[p + HORIZON_SESSIONS]
            span = sess_idx[succ_ts] - sess_idx[ts]
            out.spans.append(span)
            if max_label_span is not None and span > max_label_span:
                out.n_rejected_span_cap += 1
                continue
            if axis == "emitted":
                end_ts = succ_ts
            else:  # pooled: t+21 distinct pooled sessions later
                end_ts = session_ts[sess_idx[ts] + HORIZON_SESSIONS]
            out.obs.append(Obs(sym, date, ts, end_ts, ts_date[end_ts]))
    out.obs.sort(key=lambda o: (o.ts, o.symbol))
    return out


# ── Walk-forward plan (mirrors derive_vrp_walk_forward + make_plan_impl) ──


def derive_walk(n_groups: int) -> tuple[int, int, int]:
    if n_groups >= 252 + 63:
        return 252, 63, 63
    test = min(max(n_groups // 6, HORIZON_SESSIONS), 63)
    train = min(max(n_groups // 3, 84), 252)
    return train, test, test


@dataclass
class Fold:
    fold_id: int
    train: list[int]
    test: list[int]
    purged: list[int]
    embargoed: list[int]
    test_min_ts: int


def build_plan(
    obs: list[Obs], min_train: int, test_groups: int, step: int, embargo_ns: int
) -> list[Fold]:
    """Anchored purged/embargoed folds with GROUP-level purge/embargo,
    exactly research_validation.cpp make_plan_impl."""
    group_ts: list[int] = []
    bounds: list[tuple[int, int]] = []
    begin = 0
    while begin < len(obs):
        end = begin + 1
        while end < len(obs) and obs[end].ts == obs[begin].ts:
            end += 1
        group_ts.append(obs[begin].ts)
        bounds.append((begin, end))
        begin = end
    if len(group_ts) < min_train + test_groups:
        raise ValueError("insufficient decision groups for one complete fold")

    folds: list[Fold] = []
    test_begin = min_train
    fold_id = 0
    while test_begin <= len(group_ts) - test_groups:
        test_idx = [
            i
            for g in range(test_begin, test_begin + test_groups)
            for i in range(bounds[g][0], bounds[g][1])
        ]
        test_min = min(obs[i].ts for i in test_idx)
        embargo_begin = test_min - embargo_ns
        train_idx: list[int] = []
        purged: list[int] = []
        embargoed: list[int] = []
        for g in range(0, test_begin):
            g_rows = range(bounds[g][0], bounds[g][1])
            overlaps = any(obs[i].label_end_ts > test_min for i in g_rows)
            inside_embargo = embargo_ns > 0 and embargo_begin <= group_ts[g] < test_min
            for i in g_rows:
                if overlaps:
                    purged.append(i)
                elif inside_embargo:
                    embargoed.append(i)
                else:
                    train_idx.append(i)
        if not train_idx:
            raise ValueError("purge and embargo removed every training observation")
        folds.append(Fold(fold_id, train_idx, test_idx, purged, embargoed, test_min))
        fold_id += 1
        if step > len(group_ts) - test_begin:
            break
        test_begin += step
    return folds


# ── Panel meta counters ───────────────────────────────────────────────────


def read_panel_meta(path: Path) -> dict[str, int]:
    """Integer `# key=value` meta lines from the panel's comment header
    (stops at the first non-comment line -- the frozen column header)."""
    meta: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("#"):
            break
        key, sep, value = line[1:].strip().partition("=")
        if sep:
            try:
                meta[key.strip()] = int(value.strip())
            except ValueError:
                pass
    return meta


# ── Presence data ─────────────────────────────────────────────────────────


def load_presence(path: Path) -> tuple[list[str], dict[str, list[str]]]:
    """-> (sorted session dates, symbol -> sorted bar dates)."""
    sessions: set[str] = set()
    bars: dict[str, set[str]] = {}
    current: str | None = None
    for line_num, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] == "KEY":
            if len(parts) != 2:
                raise ValueError(f"{path}:{line_num}: malformed KEY line")
            current = parts[1]
            sessions.add(current)
        elif parts[0] == "surface":
            if current is None or len(parts) < 2:
                raise ValueError(f"{path}:{line_num}: surface line outside a KEY block")
            bars.setdefault(parts[1], set()).add(current)
        else:
            raise ValueError(f"{path}:{line_num}: unknown line kind '{parts[0]}'")
    if not sessions:
        raise ValueError(f"{path}: no KEY sessions")
    return sorted(sessions), {sym: sorted(dates) for sym, dates in bars.items()}


# ── Cross-check against the trainer's metrics artifact ────────────────────


def cross_check_metrics(path: Path, folds: list[Fold], built: ObsBuild) -> list[str]:
    """Returns mismatch descriptions (empty == reconstruction confirmed)."""
    meta: dict[str, int] = {}
    table: dict[int, tuple[int, int]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("# ") and "=" in line:
            key, _, value = line[2:].partition("=")
            try:
                meta[key] = int(value)
            except ValueError:
                pass
            continue
        fields = line.split("\t")
        if len(fields) >= 4 and fields[1] == "gbt":
            table[int(fields[0])] = (int(fields[2]), int(fields[3]))
    problems: list[str] = []

    def check(name: str, got: int, want: int) -> None:
        if got != want:
            problems.append(f"{name}: reconstruction {got} != artifact {want}")

    if "n_rows_rejected_no_t21" in meta:
        check("n_rows_rejected_no_t21", built.n_rejected_no_t21, meta["n_rows_rejected_no_t21"])
    if "n_rows_rejected_span_cap" in meta:
        check(
            "n_rows_rejected_span_cap",
            built.n_rejected_span_cap,
            meta["n_rows_rejected_span_cap"],
        )
    if len(table) != len(folds):
        problems.append(f"fold count: reconstruction {len(folds)} != artifact {len(table)}")
        return problems
    for f in folds:
        if f.fold_id not in table:
            problems.append(f"fold {f.fold_id}: missing from artifact")
            continue
        n_train, n_test = table[f.fold_id]
        check(f"fold {f.fold_id} n_train", len(f.train), n_train)
        check(f"fold {f.fold_id} n_test", len(f.test), n_test)
        for kind, got in (("n_train_purged", len(f.purged)), ("n_train_embargoed",
                                                             len(f.embargoed))):
            key = f"fold_{f.fold_id}_{kind}"
            if key not in meta:
                problems.append(f"{key}: missing from artifact")
            else:
                check(key, got, meta[key])
    return problems


# ── Adjudication ──────────────────────────────────────────────────────────


def span_summary(spans: list[int]) -> str:
    if not spans:
        return "spans: none"
    s = sorted(spans)
    n = len(s)
    pick = lambda q: s[int(q * (n - 1))]  # noqa: E731
    return (
        f"spans (pooled sessions, n={n}): min={s[0]} p50={pick(0.5)} p90={pick(0.9)} "
        f"p95={pick(0.95)} p99={pick(0.99)} max={s[-1]}"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Adjudicate the VRP trainer's fold plan against surface presence."
    )
    parser.add_argument("--panel", required=True, type=Path)
    parser.add_argument("--presence", required=True, type=Path)
    parser.add_argument("--metrics", type=Path, default=None,
                        help="trainer vrp_metrics.tsv to cross-check the reconstruction")
    parser.add_argument("--label-end-axis", choices=("emitted", "pooled"), default="emitted")
    parser.add_argument("--max-label-span", type=int, default=None, metavar="N",
                        help="span cap in pooled sessions (default: 42 under emitted, "
                        "OFF under pooled -- each mode's own trainer default)")
    parser.add_argument("--min-train-sessions", type=int, default=None)
    parser.add_argument("--test-sessions", type=int, default=None)
    parser.add_argument("--step-sessions", type=int, default=None)
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    max_span = args.max_label_span
    if max_span is None and args.label_end_axis == "emitted":
        max_span = DEFAULT_MAX_LABEL_SPAN

    try:
        _header, rows = parse_tsv_file(args.panel)
        built = build_observations(rows, args.label_end_axis, max_span)
        sessions, bars = load_presence(args.presence)
        walk_flags = (args.min_train_sessions, args.test_sessions, args.step_sessions)
        if any(v is not None for v in walk_flags):
            if any(v is None for v in walk_flags):
                raise ValueError("pass all three walk flags or none")
            min_train, test_groups, step = walk_flags
        else:
            n_groups = len({o.ts for o in built.obs})
            min_train, test_groups, step = derive_walk(n_groups)
        embargo_ns = max((o.label_end_ts - o.ts for o in built.obs), default=0)
        folds = build_plan(built.obs, min_train, test_groups, step, embargo_ns)
    except (ValueError, OSError, KeyError) as exc:
        print(f"vrp_leak_adjudicate: {exc}", file=sys.stderr)
        return 2

    # Adjudicability gate (fix2-review minor a): the true bar axis is
    # presence AND finite positive spot; n_bad_spot > 0 means presence is
    # DENSER than the bar axis, presence-derived true ends come too early,
    # and a real leak could be missed -- refuse the corpus, never pass it.
    n_bad_spot = read_panel_meta(args.panel).get("n_bad_spot")
    if n_bad_spot is not None and n_bad_spot != 0:
        print(
            f"vrp_leak_adjudicate: panel meta n_bad_spot={n_bad_spot} != 0 -- presence "
            "data is denser than the panel's bar axis (bar = presence AND finite "
            "positive spot), so presence cannot adjudicate this corpus",
            file=sys.stderr,
        )
        return 2
    if n_bad_spot is None:
        print("panel carries no n_bad_spot meta; treating presence as the bar axis")

    print(f"axis={args.label_end_axis} max_label_span={max_span} "
          f"walk={min_train}/{test_groups}/{step} embargo_days={embargo_ns / 86.4e12:.1f}")
    print(f"labeled={built.n_labeled} admitted={len(built.obs)} "
          f"rejected_no_t21={built.n_rejected_no_t21} "
          f"rejected_span_cap={built.n_rejected_span_cap}")
    print(span_summary(built.spans))

    if args.metrics is not None:
        problems = cross_check_metrics(args.metrics, folds, built)
        if problems:
            for p in problems:
                print(f"vrp_leak_adjudicate: METRICS MISMATCH {p}", file=sys.stderr)
            return 2
        print(f"metrics cross-check: reconstruction matches {args.metrics.name} exactly")

    coverage_start = sessions[0]
    # Structural-bound sanity, WIDENED to every panel row (fix2-review minor
    # b): every emitted row inside presence coverage -- decision, successor,
    # rejected, and tail rows alike -- must be a bar of its symbol, because
    # the recorded label ends the bounded_safe path rests on are SUCCESSOR
    # rows' timestamps. Scanning admitted decision dates alone would let a
    # successor-side panel/presence inconsistency slip past this guard.
    bar_sets = {sym: frozenset(dates) for sym, dates in bars.items()}
    anomalies = 0
    examples: list[str] = []
    for r in rows:
        if r["date"] >= coverage_start and r["date"] not in bar_sets.get(r["symbol"], ()):
            anomalies += 1
            if len(examples) < 5:
                examples.append(f"{r['symbol']}/{r['date']}")
    if anomalies:
        print(f"vrp_leak_adjudicate: {anomalies} emitted-but-not-present panel row(s) "
              f"(e.g. {', '.join(examples)}); presence data cannot adjudicate this panel",
              file=sys.stderr)
        return 2

    violations: list[str] = []   # PROVEN leaks: true end (or its bound) past test start
    uncertified: list[str] = []  # no presence coverage and no structural bound
    for f in folds:
        determinate = 0
        bounded = 0
        indeterminate = 0
        test_min_date = min(o.date for o in (built.obs[i] for i in f.test))
        for i in f.train:
            o = built.obs[i]
            sym_bars = bars.get(o.symbol, [])
            if o.date >= coverage_start:
                b = bisect.bisect_left(sym_bars, o.date)
                if b + HORIZON_SESSIONS < len(sym_bars):
                    true_end = sym_bars[b + HORIZON_SESSIONS]
                    determinate += 1
                    if true_end > test_min_date:
                        violations.append(
                            f"fold {f.fold_id}: {o.symbol} decided {o.date} true_end "
                            f"{true_end} > test_min {test_min_date} "
                            f"(recorded {o.label_end_date})"
                        )
                    continue
                # 21st bar past presence coverage: fall through to the bound.
            if args.label_end_axis == "emitted":
                # true_end <= recorded emitted end, so recorded <= test_min
                # proves the row safe; recorded > test_min would be a plan
                # violation -- a proven leak either way.
                if o.label_end_date <= test_min_date:
                    bounded += 1
                else:
                    violations.append(
                        f"fold {f.fold_id}: {o.symbol} decided {o.date} recorded end "
                        f"{o.label_end_date} > test_min {test_min_date} (plan violation)"
                    )
            else:
                indeterminate += 1
                uncertified.append(
                    f"fold {f.fold_id}: {o.symbol} decided {o.date} (recorded "
                    f"{o.label_end_date}, test_min {test_min_date})"
                )
        print(
            f"fold {f.fold_id}: n_train={len(f.train)} n_test={len(f.test)} "
            f"purged={len(f.purged)} embargoed={len(f.embargoed)} "
            f"test_min={test_min_date} adjudicated: determinate={determinate} "
            f"bounded_safe={bounded} indeterminate={indeterminate}"
        )

    if violations:
        print(f"vrp_leak_adjudicate: {len(violations)} train row(s) with true label end "
              "past their fold's test start:", file=sys.stderr)
        for v in violations[:50]:
            print(f"  {v}", file=sys.stderr)
        if len(violations) > 50:
            print(f"  ... and {len(violations) - 50} more", file=sys.stderr)
        return 1
    if uncertified:
        print(f"vrp_leak_adjudicate: 0 proven leaks but {len(uncertified)} row(s) cannot "
              "be certified (no presence coverage, no structural bound)", file=sys.stderr)
        return 1
    print("vrp_leak_adjudicate: PASS -- zero train rows with true label end past test start")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
