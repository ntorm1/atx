"""Build-forward (task P5): redundancy, spanning and composites over the R4 ledger.

Input is a sealed R4 qualification ledger (``research_qualification_*``), the sealed R3b
run it graded, the R2b feature version of the composite basis and P4 monthly factor
returns. Everything here is research-only and lives in the research store. Policy
``p5-composite-v2`` (``seeds/research_composite_policy.json``; v1 is history only).

Selection: train + validation evidence only (C1)
------------------------------------------------
R4's ``qualified_*`` statuses also gate on the holdout (holdout sign, holdout z), so a
composite of qualified signals would be selected on the very holdout that later
"confirms" it. A signal is therefore a candidate constituent when its R4 row on the
composite basis passes the **selection-sample gates only**: ``gate_coverage``,
``gate_significance`` (family-corrected EWC: HLZ, or BH q and |z|), ``gate_sign``,
``gate_subperiods`` (the subperiods are truncated to selection formations) and
``gate_size_buckets``; a basis on which the signal is significant with the wrong sign
vetoes it. The holdout gates are never read, and the R4 status is only reported. The
composite policy must be registered in the research store (``research_composite.py
freeze``) before the source R3b run was created (RX7, as R4); otherwise the build is
refused unless explicitly stamped post hoc.

Redundancy annotation (never alters an R4 status)
-------------------------------------------------
Over the selected signals:

* **Rank correlation.** Per fit formation, the Pearson correlation of two signals'
  within-formation average ranks (each signal ranked over its own valued names, the
  policy's sign-oriented ``constituent_variant``) on the names valued on both (at least
  ``min_names_per_formation``), averaged over the fit formations (at least
  ``min_formations``); with full coverage this is the Spearman correlation. Signed: two
  oriented signals that disagree are not duplicates. Computed for all pairs at once per
  formation (masked matrix products).
* **Clusters.** Complete linkage at the declared ``rank_correlation_threshold``;
  deterministic tie-breaks. The representative has the largest ``|ic_z|`` on the
  composite basis (then the feature id).
* **Spanning.** The signal's R3b equal-weighted decile long-short series at 1 month
  (``ls_ew10``, selection formations only, oriented by the realized direction of a
  two-sided hypothesis) on the P4 monthly factors with
  :func:`atx_db.research.factor_returns.span_test` (EWC fixed-b robust alpha), both on
  the full monthly period grid of the series (P4's gap rule: a month without a formation
  is a gap in the HAC sums). A result P4 marks not ``significance_claimable`` is never
  called spanned.
* ``redundancy_status``: ``duplicate_of:<representative>`` for a non-representative
  cluster member; else ``spanned_by_known_factors`` when the oriented alpha z is below
  ``alpha_min_z`` (``span_note='negative_alpha_after_factors'`` when it is significantly
  negative); else ``novel``. An untestable spanning regression is not called spanned
  (``span_status`` says why; the build carries a blocker).

Composites
----------
Constituents are the cluster representatives of the selected signals (a near-copy never
gets a second vote). Per formation every constituent's oriented value is a
cross-sectional z. A **class composite** is ``sum(w_i z_i) / sqrt(w' R_S w)`` over the
constituents ``S`` valued for a name, where ``R`` is the fit-sample correlation of the
members: the combination has unit variance whichever subset is valued, so names with
missing constituents are not over-dispersed (they are not pushed into the extreme
deciles). A name needs at least ``min_constituent_share`` of the constituents; the
result is re-standardized to a z. The **across-class composite** combines the class
composites the same way. Weightings: ``equal`` and ``ic_shrunk`` =
``(1 - s) * max(IC, 0) / sum max(IC, 0) + s / k`` (IC: mean monthly rank IC at
``fit_horizon_months`` on the fit sample). Within-class composites are emitted for classes
with at least ``min_class_constituents`` constituents, across-class composites for at
least ``min_across_inputs`` classes: ``cmp_<class>_eq`` / ``_icw``, ``cmp_classes_eq`` /
``_icw``.

Universe (R2b I1 ruling): composites rank every eligible line with enough constituent
values, unlinked lines included (identity-free price-line constituents are valued there;
fundamentals are linked-only), with ``universe_scope='valid_primary_and_unlinked_lines'``.
Neutral variants that leave ranked unlinked lines out carry R2b's
``sample_conditioning='survivor_conditioned_linked_only'`` label and ``unlinked_excluded``
count; unlinked names with some constituent value but too few for a composite value are
counted per composite (build blocker ``composite_drops_unlinked_lines``).

Fit sample (RX7): formed train/validation formations whose label window at the fit
horizon ends before the holdout. :func:`fit_weights` raises :class:`CompositeLeakError`
on any row in, or reaching into, the holdout.

Memory (I3): constituent values are streamed per batch of formations (at most 24; sized
to ~96 MB of dense blocks), never all histories at once; the store source reads one batch
of all constituents (one variant column) per query.

Grading, storage, one holdout look
----------------------------------
A composite feature version is the constituent R2b version **plus** the composites
(source rows copied, store schema 3, ``anomaly_class='composite'``, constituents and
weights in each composite catalog row), every composite variant produced by R2b's own
:func:`atx_db.research.features.standardize_frame`; it passes R2b's validator. One R3b
run over these versions evaluates the **full family** (every candidate signal plus the
composites: BH, Holm and the DSR ``n_trials`` count all of them), and R4 grades it with
every row stamped ``composite_post_selection`` and the constituent ledger id; no ledger
containing composites or that stamp may seed another build. The registry allows one
holdout evaluation per (frozen split, basis): an identical rerun is a no-op, an
unevaluated build may be replaced (a retry after an abort), and any other build after
the evaluation is refused. The build identity is policy + ledger + inputs + value
digests; code digests are provenance only.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
from collections.abc import Callable, Iterable, Iterator, Mapping, Sequence
from dataclasses import dataclass, field, replace
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import duckdb
import numpy as np
import pandas as pd

from . import evaluation as ev
from . import factor_returns as fr
from . import features as rf
from . import qualification as rq
from .panel import CALENDAR_FORMED
from .store import ResearchStore

COMPOSITE_VERSION = "research-composites-v2"
COMPOSITE_SCHEMA_VERSION = 2
_SEEDS = Path(__file__).resolve().parents[1] / "seeds"
POLICY_PATH = _SEEDS / "research_composite_policy.json"
POLICY_HISTORY_PATHS = {"p5-composite-v1": _SEEDS / "research_composite_policy_v1.json"}
#: Every committed composite policy version and its canonical content hash (RX7).
FROZEN_POLICY_SHA256 = {
    "p5-composite-v1": "1e90aab500ac5e49bec85ab19a82a19f40e1a1cd302fa3c1e303cb04dc45d702",
    "p5-composite-v2": "28f3405dfe3adfbe3e29d976ab6eea026533150c728e8c82e6037cd367b45fee",
}
NOVEL = "novel"
SPANNED = "spanned_by_known_factors"
DUPLICATE_PREFIX = "duplicate_of:"
NEGATIVE_ALPHA = "negative_alpha_after_factors"
SPAN_TESTED, SPAN_TOO_FEW, SPAN_DEGENERATE, SPAN_NOT_CLAIMABLE = (
    "tested", "too_few_periods", "degenerate", "not_significance_claimable")
LEVEL_WITHIN, LEVEL_ACROSS = "within_class", "across_class"
WEIGHT_EQUAL, WEIGHT_IC = "equal", "ic_shrunk"
WEIGHT_TAGS = {WEIGHT_EQUAL: "eq", WEIGHT_IC: "icw"}
COMPOSITE_CLASS = "composite"
POST_SELECTION_STAMP = "composite_post_selection"
COMPOSITE_CAVEATS = (POST_SELECTION_STAMP, "weights_fit_on_selection_sample")
POST_HOC_REASONS = ("composite_policy_not_registered", "composite_policy_registered_after_evaluation")
UNCLASSIFIED = "unclassified"
_ID_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789_"
#: Provenance only (recorded in the manifest, never part of the build identity: I4).
_CODE_FILES = (Path(__file__), Path(rf.__file__), Path(ev.__file__))
_SECTION_KEYS = {
    "source": {"selection", "selection_gates", "basis", "constituent_variant", "fit_horizon_months",
               "min_names_per_formation"},
    "redundancy": {"rank_correlation", "rank_correlation_threshold", "linkage", "min_formations", "representative"},
    "spanning": {"factors", "series", "horizon_months", "calendar", "alpha_min_z", "min_periods"},
    "composite": {"constituents", "universe", "weightings", "ic_shrinkage", "min_class_constituents",
                  "min_across_inputs", "min_constituent_share", "missing_constituents"},
    "holdout": {"evaluations_per_split_and_basis", "grading_family"},
}
_TOP_KEYS = {"composite_policy_version", "supersedes", "title", "frozen_on", "authority", *_SECTION_KEYS,
             "policy_sha256"}
_FIXED = {("source", "selection"): "r4_train_validation_gates",
          ("redundancy", "rank_correlation"): "pearson_of_within_formation_average_ranks_on_names_valued_on_both",
          ("redundancy", "linkage"): "complete", ("redundancy", "representative"): "max_abs_ic_z",
          ("spanning", "calendar"): "monthly_period_grid", ("composite", "constituents"): "cluster_representatives",
          ("composite", "universe"): "all_eligible_lines_price_line_rule",
          ("composite", "missing_constituents"): "variance_adjusted_under_fit_correlation",
          ("holdout", "evaluations_per_split_and_basis"): 1,
          ("holdout", "grading_family"): "source_family_plus_composites"}
#: R4 gates computed on the selection sample only (the holdout gates are never read).
SELECTION_GATES = ("gate_coverage", "gate_significance", "gate_sign", "gate_subperiods", "gate_size_buckets")
_SERIES_COLUMNS = ("ls_ew10", "ls_vw10", "ls_ew5", "ls_vw5")


class CompositeError(ValueError):
    """Build-forward inputs violate the contract (policy, ledger, store)."""


class CompositeLeakError(CompositeError):
    """A holdout row reached a fit (RX7)."""


def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False)


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _clean(value: Any) -> Any:
    if isinstance(value, (bool, np.bool_)):
        return bool(value)
    if isinstance(value, (int, np.integer)):
        return int(value)
    if isinstance(value, (float, np.floating)):
        return float(value) if math.isfinite(float(value)) else None
    if isinstance(value, Mapping):
        return {str(k): _clean(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_clean(v) for v in value]
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    return value


def _true(value: Any) -> bool:
    return value is not None and not (isinstance(value, float) and math.isnan(value)) and bool(value)


def code_sha256() -> str:
    """EOL-normalized digest of this module and the R2b/R3b code it relies on (provenance)."""
    digest = hashlib.sha256()
    for path in _CODE_FILES:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes().replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def _day(value: Any) -> dt.date:
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    return pd.Timestamp(value).date()


def _now() -> dt.datetime:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None)


# ---------------------------------------------------------------------------
# Policy (frozen, pinned, registered before the source run: RX7)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class CompositePolicy:
    version: str
    sha256: str
    content: Mapping[str, Any]
    selection_gates: tuple[str, ...]
    basis: str
    constituent_variant: str
    fit_horizon: int
    min_names: int
    rho_threshold: float
    min_formations: int
    span_factors: tuple[str, ...]
    span_series: str
    span_horizon: int
    alpha_min_z: float
    span_min_periods: int
    weightings: tuple[str, ...]
    ic_shrinkage: float
    min_class_constituents: int
    min_across_inputs: int
    min_constituent_share: float


def policy_sha256(content: Mapping[str, Any]) -> str:
    """Canonical content hash of a composite policy (its own ``policy_sha256`` excluded)."""
    return _sha(_canonical({key: value for key, value in content.items() if key != "policy_sha256"}))


def superseded_policy_versions() -> frozenset[str]:
    """Versions a committed composite policy ``supersedes`` (history: never built with)."""
    names = set()
    for path in (POLICY_PATH, *POLICY_HISTORY_PATHS.values()):
        superseded = json.loads(path.read_bytes().decode("utf-8")).get("supersedes")
        if isinstance(superseded, str):
            names.add(superseded)
    return frozenset(names)


def _number(section: Mapping[str, Any], key: str, low: float, high: float) -> float:
    value = section.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not low <= float(value) <= high:
        raise CompositeError(f"composite policy: {key} must be a number {low}..{high}")
    return float(value)


def _count(section: Mapping[str, Any], key: str, low: int, high: int) -> int:
    value = section.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise CompositeError(f"composite policy: {key} must be an integer {low}..{high}")
    return value


def load_composite_policy(source: Path | str | Mapping[str, Any] = POLICY_PATH, *,
                          allow_unpinned: bool = False) -> CompositePolicy:
    """Load and verify a frozen composite policy (declared hash, pin, exact keys).

    A superseded version (v1: selection on R4 statuses that read the holdout) is refused.
    """
    content: Any = source if isinstance(source, Mapping) else json.loads(Path(source).read_bytes().decode("utf-8"))
    if not isinstance(content, Mapping):
        raise CompositeError("composite policy must be a JSON object")
    version = content.get("composite_policy_version")
    if version in superseded_policy_versions():
        raise CompositeError(f"composite policy {version} is superseded (history only; its constituent selection "
                             "read the holdout)")
    if set(content) != _TOP_KEYS:
        raise CompositeError(f"composite policy keys must be exactly {sorted(_TOP_KEYS)}")
    for name, keys in _SECTION_KEYS.items():
        if not isinstance(content[name], Mapping) or set(content[name]) != keys:
            raise CompositeError(f"composite policy section {name} keys must be exactly {sorted(keys)}")
    actual = policy_sha256(content)
    if content["policy_sha256"] != actual:
        raise CompositeError(f"composite policy {version}: declared sha256 {content['policy_sha256']} != content "
                             f"{actual}")
    pinned = FROZEN_POLICY_SHA256.get(str(version))
    if pinned is None and not allow_unpinned:
        raise CompositeError(f"composite policy {version} is not pinned in FROZEN_POLICY_SHA256")
    if pinned is not None and pinned != actual:
        raise CompositeError(f"composite policy {version} is frozen with sha256 {pinned}; an edit needs a new version")
    for (section, key), expected in _FIXED.items():
        if content[section][key] != expected:
            raise CompositeError(f"composite policy {section}.{key} must be {expected!r} in this code version")
    source_s, redundancy, spanning, composite = (content[k] for k in ("source", "redundancy", "spanning", "composite"))
    gates = tuple(source_s["selection_gates"])
    if not gates or set(gates) - set(SELECTION_GATES):
        raise CompositeError(f"selection_gates must be selection-sample R4 gates from {SELECTION_GATES}")
    if source_s["basis"] not in ev.BASES:
        raise CompositeError(f"basis must be one of {ev.BASES}")
    if source_s["constituent_variant"] not in ev.FEATURE_VARIANTS:
        raise CompositeError(f"constituent_variant must be one of {ev.FEATURE_VARIANTS}")
    fit_horizon = _count(source_s, "fit_horizon_months", 1, 12)
    if fit_horizon not in ev.HORIZON_SESSIONS:
        raise CompositeError(f"fit_horizon_months must be one of {sorted(ev.HORIZON_SESSIONS)}")
    factors = tuple(spanning["factors"])
    if not factors or len(set(factors)) != len(factors):
        raise CompositeError("spanning factors must be unique and non-empty")
    if spanning["series"] not in _SERIES_COLUMNS:
        raise CompositeError(f"spanning series must be one of {_SERIES_COLUMNS}")
    weightings = tuple(composite["weightings"])
    if not weightings or set(weightings) - set(WEIGHT_TAGS) or len(set(weightings)) != len(weightings):
        raise CompositeError(f"weightings must be unique values of {sorted(WEIGHT_TAGS)}")
    return CompositePolicy(
        version=str(version), sha256=actual, content=dict(content), selection_gates=gates,
        basis=str(source_s["basis"]), constituent_variant=str(source_s["constituent_variant"]),
        fit_horizon=fit_horizon, min_names=_count(source_s, "min_names_per_formation", 3, 100_000),
        rho_threshold=_number(redundancy, "rank_correlation_threshold", 0.0, 1.0),
        min_formations=_count(redundancy, "min_formations", 2, 10_000), span_factors=factors,
        span_series=str(spanning["series"]), span_horizon=_count(spanning, "horizon_months", 1, 1),
        alpha_min_z=_number(spanning, "alpha_min_z", 0.0, 10.0),
        span_min_periods=_count(spanning, "min_periods", 3, 10_000), weightings=weightings,
        ic_shrinkage=_number(composite, "ic_shrinkage", 0.0, 1.0),
        min_class_constituents=_count(composite, "min_class_constituents", 2, 1_000),
        min_across_inputs=_count(composite, "min_across_inputs", 2, 1_000),
        min_constituent_share=_number(composite, "min_constituent_share", 0.0, 1.0))


# ---------------------------------------------------------------------------
# Research store tables
# ---------------------------------------------------------------------------

REDUNDANCY_COLUMNS: tuple[tuple[str, str], ...] = (
    ("feature_id", "VARCHAR"), ("r4_status", "VARCHAR"), ("anomaly_class", "VARCHAR"), ("ic_z", "DOUBLE"),
    ("redundancy_status", "VARCHAR"), ("cluster_id", "VARCHAR"), ("cluster_size", "INTEGER"),
    ("representative", "VARCHAR"), ("rho_to_representative", "DOUBLE"), ("span_status", "VARCHAR"),
    ("span_note", "VARCHAR"), ("span_alpha", "DOUBLE"), ("span_alpha_z", "DOUBLE"), ("span_alpha_p", "DOUBLE"),
    ("span_r2", "DOUBLE"), ("span_n_obs", "INTEGER"),
)
MEMBER_COLUMNS: tuple[tuple[str, str], ...] = (
    ("composite_id", "VARCHAR"), ("level", "VARCHAR"), ("weighting", "VARCHAR"), ("anomaly_class", "VARCHAR"),
    ("emitted", "BOOLEAN"), ("member_id", "VARCHAR"), ("weight", "DOUBLE"), ("fit_ic", "DOUBLE"),
    ("fit_formations", "INTEGER"),
)


def ensure_composite_schema(con: duckdb.DuckDBPyConnection) -> None:
    """Create the ``research_composite_*`` tables (kept here until the store owner registers them)."""
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_composite_schema (
            version INTEGER PRIMARY KEY, name VARCHAR NOT NULL, applied_at TIMESTAMP NOT NULL)""")
    versions = {int(r[0]) for r in con.execute("SELECT version FROM research_composite_schema").fetchall()}
    if versions and COMPOSITE_SCHEMA_VERSION not in versions:
        raise CompositeError(f"research_composite_* tables are schema {sorted(versions)} (v1 was fixture-only); "
                             "drop them before using schema v2")
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_composite_policies (
            policy_version VARCHAR PRIMARY KEY, policy_sha256 VARCHAR NOT NULL, policy_json VARCHAR NOT NULL,
            registered_at TIMESTAMP NOT NULL)""")
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_composite_builds (
            holdout_key VARCHAR PRIMARY KEY, split_sha256 VARCHAR NOT NULL, basis VARCHAR NOT NULL,
            ledger_id VARCHAR NOT NULL, ledger_sha256 VARCHAR NOT NULL, evaluation_run_id VARCHAR NOT NULL,
            policy_version VARCHAR NOT NULL, policy_sha256 VARCHAR NOT NULL, build_sha256 VARCHAR NOT NULL,
            manifest_json VARCHAR NOT NULL, superseded_json VARCHAR NOT NULL, composite_versions_json VARCHAR,
            holdout_run_id VARCHAR, created_at TIMESTAMP NOT NULL, evaluated_at TIMESTAMP)""")
    con.execute("CREATE TABLE IF NOT EXISTS research_composite_redundancy (build_sha256 VARCHAR NOT NULL, "
                + ", ".join(f"{n} {k}" for n, k in REDUNDANCY_COLUMNS) + ", PRIMARY KEY (build_sha256, feature_id))")
    con.execute("CREATE TABLE IF NOT EXISTS research_composite_members (build_sha256 VARCHAR NOT NULL, "
                + ", ".join(f"{n} {k}" for n, k in MEMBER_COLUMNS)
                + ", PRIMARY KEY (build_sha256, composite_id, member_id))")
    if not versions:
        con.execute("INSERT INTO research_composite_schema VALUES (?, ?, ?)",
                    [COMPOSITE_SCHEMA_VERSION, "holdout_window_registry_policies", _now()])


def register_composite_policy(con: duckdb.DuckDBPyConnection, policy: CompositePolicy) -> str:
    """Freeze the policy in the research store (RX7): ``'registered'`` or ``'exists'``.

    Run it before the RR4 real-data evaluation; a registered version never changes hash.
    """
    ensure_composite_schema(con)
    row = con.execute("SELECT policy_sha256 FROM research_composite_policies WHERE policy_version = ?",
                      [policy.version]).fetchone()
    if row is not None:
        if row[0] != policy.sha256:
            raise CompositeError(f"composite policy {policy.version} is registered with sha256 {row[0]}")
        return "exists"
    con.execute("INSERT INTO research_composite_policies VALUES (?, ?, ?, ?)",
                [policy.version, policy.sha256, _canonical(dict(policy.content)), _now()])
    return "registered"


def composite_policy_registered_at(con: duckdb.DuckDBPyConnection, policy: CompositePolicy) -> str | None:
    """When this composite policy was frozen in the store (ISO, naive UTC), or None."""
    if not con.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name = 'research_composite_policies'"
                       ).fetchone()[0]:
        return None
    row = con.execute("SELECT policy_sha256, registered_at FROM research_composite_policies WHERE policy_version = ?",
                      [policy.version]).fetchone()
    if row is None:
        return None
    if row[0] != policy.sha256:
        raise CompositeError(f"composite policy {policy.version} is registered with sha256 {row[0]}")
    return pd.Timestamp(row[1]).isoformat()


# ---------------------------------------------------------------------------
# The R4 ledger (read-only) and train/validation selection (C1)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class LedgerSource:
    ledger_id: str
    ledger_sha256: str
    run_id: str
    r4_policy_version: str
    split_sha256: str | None
    post_hoc_policy: bool
    #: The ledger contains composites or carries the post-selection stamp: never a seed (I1).
    post_selection: bool
    features: tuple[Mapping[str, Any], ...]
    bases: tuple[Mapping[str, Any], ...]


def _ledger(ledger_id: str, sha: str, manifest: Mapping[str, Any], features: Sequence[Mapping[str, Any]],
            bases: Sequence[Mapping[str, Any]]) -> LedgerSource:
    stamped = any(row.get("anomaly_class") == COMPOSITE_CLASS or POST_SELECTION_STAMP in str(row.get("caveat_codes"))
                  for row in features) or any(POST_SELECTION_STAMP in str(b) for b in manifest.get("blockers", []))
    return LedgerSource(ledger_id, sha, str(manifest["evaluation"]["run_id"]), str(manifest["policy"]["version"]),
                        manifest["policy"].get("split_sha256"), bool(manifest.get("post_hoc_policy")), stamped,
                        tuple(sorted(features, key=lambda r: r["feature_id"])),
                        tuple(sorted(bases, key=lambda r: (r["feature_id"], r["basis"]))))


def ledger_source(ledger: rq.QualificationLedger) -> LedgerSource:
    """The build-forward view of an in-memory R4 ledger."""
    return _ledger(ledger.ledger_id, ledger.sha256, ledger.manifest, ledger.features, ledger.bases)


def _records(frame: pd.DataFrame) -> list[dict[str, Any]]:
    return frame.astype(object).where(pd.notna(frame), None).to_dict("records")


def load_ledger(con: duckdb.DuckDBPyConnection, ledger_id: str) -> LedgerSource:
    """A sealed R4 ledger from the research store (feature and per-basis rows)."""
    row = con.execute("SELECT ledger_sha256, manifest_json FROM research_qualification_ledgers WHERE ledger_id = ?",
                      [ledger_id]).fetchone()
    if row is None:
        raise CompositeError(f"no stored R4 ledger {ledger_id}")
    features = _records(con.execute("SELECT * EXCLUDE (ledger_id) FROM research_qualification_features "
                                     "WHERE ledger_id = ? ORDER BY feature_id", [ledger_id]).df())
    bases = _records(con.execute("SELECT * EXCLUDE (ledger_id) FROM research_qualification_bases "
                                 "WHERE ledger_id = ? ORDER BY feature_id, basis", [ledger_id]).df())
    return _ledger(ledger_id, str(row[0]), json.loads(row[1]), features, bases)


@dataclass(frozen=True)
class SelectedSignal:
    feature_id: str
    anomaly_class: str
    expected_sign: int
    raw_direction: int | None
    ic_z: float | None
    ic_mean: float | None
    r4_status: str
    selected: bool
    reasons: str | None

    @property
    def orientation(self) -> int:
        """+1 for a signed hypothesis (values are oriented); the realized sign for a two-sided one."""
        if self.expected_sign != 0:
            return 1
        return -1 if (self.raw_direction or 1) < 0 else 1


def select_signals(ledger: LedgerSource, policy: CompositePolicy) -> tuple[SelectedSignal, ...]:
    """Every ledger feature with its train/validation selection verdict (C1).

    Selected: the composite-basis row passes every ``selection_gates`` gate, and no basis
    shows the signal significant with the wrong sign. R4's holdout gates and its final
    status are not read (the status is only reported).
    """
    by_feature: dict[str, dict[str, Mapping[str, Any]]] = {}
    for row in ledger.bases:
        by_feature.setdefault(str(row["feature_id"]), {})[str(row["basis"])] = row
    out = []
    for feature in ledger.features:
        fid = str(feature["feature_id"])
        rows = by_feature.get(fid, {})
        row = rows.get(policy.basis)
        reasons = []
        if row is None or row.get("status") == rq.UNTESTABLE_STRICT:
            reasons.append(f"no_{policy.basis}_evidence")
        else:
            reasons.extend(f"{gate}={row.get(gate)}" for gate in policy.selection_gates if not _true(row.get(gate)))
        vetoed = sorted(b for b, other in rows.items()
                        if _true(other.get("gate_significance")) and other.get("gate_sign") is not None
                        and not _true(other.get("gate_sign")))
        if vetoed:
            reasons.append("sign_reversed_on:" + ",".join(vetoed))
        sign = feature.get("expected_sign")
        chosen = row or {}
        out.append(SelectedSignal(
            fid, str(feature.get("anomaly_class") or UNCLASSIFIED), 1 if sign is None else int(sign),
            None if chosen.get("raw_direction") is None else int(chosen["raw_direction"]),
            None if chosen.get("ic_z") is None else float(chosen["ic_z"]),
            None if chosen.get("ic_mean") is None else float(chosen["ic_mean"]), str(feature.get("status")),
            not reasons, "; ".join(reasons) or None))
    return tuple(out)


def load_selection_series(con: duckdb.DuckDBPyConnection, run_id: str, policy: CompositePolicy,
                          feature_ids: Sequence[str]) -> pd.DataFrame:
    """R3b per-formation long-short series of the spanning test: selection formations only.

    Columns ``feature_id``, ``formation_date``, ``value``. A holdout row is refused.
    """
    frame = con.execute(f"""
        SELECT feature_id, formation_date, segment, {policy.span_series} AS value
        FROM research_eval_series
        WHERE run_id = ? AND basis = ? AND variant = ? AND horizon_months = ? AND list_contains(?::VARCHAR[], feature_id)
          AND in_selection
        ORDER BY feature_id, formation_date
    """, [run_id, policy.basis, policy.constituent_variant, policy.span_horizon, list(feature_ids)]).df()
    if (frame["segment"] == "holdout").any():
        raise CompositeLeakError("the R3b selection series carries holdout rows")
    return frame[["feature_id", "formation_date", "value"]]


# ---------------------------------------------------------------------------
# Fit sample and the holdout guard
# ---------------------------------------------------------------------------

def fit_sample(inputs: ev.BasisInputs, split: ev.FrozenSplit, horizon: int) -> pd.DataFrame:
    """Formed train/validation formations whose ``horizon`` label window ends before the holdout.

    Columns ``month_index``, ``formation_date``, ``label_end``.
    """
    calendar = inputs.calendar
    formed = calendar[calendar["status"] == CALENDAR_FORMED]
    maturity = inputs.maturity
    ends = maturity[maturity["horizon_months"] == horizon].set_index("month_index")["expected_end"]
    segments = [(start, end) for name, start, end in split.segments if name != "holdout"]
    rows = []
    for month, formation in zip(formed["month_index"].tolist(), formed["formation_date"].tolist(), strict=True):
        day = _day(formation)
        end = ends.get(int(month))
        if not any(start <= day <= stop for start, stop in segments) or end is None or pd.isna(end) \
                or _day(end) >= split.holdout_start:
            continue
        rows.append({"month_index": int(month), "formation_date": day, "label_end": _day(end)})
    return pd.DataFrame(rows, columns=["month_index", "formation_date", "label_end"])


def fit_weights(ic_rows: pd.DataFrame, members: Sequence[str], split: ev.FrozenSplit, *, weighting: str,
                shrinkage: float) -> dict[str, dict[str, Any]]:
    """Weights of ``members`` from their monthly ICs on the fit sample (train + validation only).

    ``ic_rows``: ``member_id``, ``formation_date``, ``label_end``, ``ic``. Any row dated in
    the holdout, or whose label window ends on/after the holdout start (or is unknown),
    raises :class:`CompositeLeakError`: the holdout is evaluated once, never fit.
    """
    if not members:
        return {}
    if len(ic_rows):
        formed = pd.to_datetime(ic_rows["formation_date"])
        ends = pd.to_datetime(ic_rows["label_end"])
        holdout = pd.Timestamp(split.holdout_start)
        leak = (formed >= holdout) | ends.isna() | (ends >= holdout)
        if bool(leak.any()):
            first = ic_rows.loc[leak.to_numpy(), "formation_date"].min()
            raise CompositeLeakError(f"fit refuses {int(leak.sum())} holdout row(s) (first {first}; holdout starts "
                                     f"{split.holdout_start}): composites are fit on train + validation only")
    stats: dict[str, tuple[float | None, int]] = {}
    for member in members:
        values = ic_rows.loc[ic_rows["member_id"] == member, "ic"].to_numpy(dtype=float) if len(ic_rows) else \
            np.array([])
        values = values[np.isfinite(values)]
        stats[member] = (float(values.mean()) if len(values) else None, len(values))
    k = len(members)
    equal = 1.0 / k
    if weighting == WEIGHT_EQUAL:
        weights = {member: equal for member in members}
    elif weighting == WEIGHT_IC:
        positive = {member: max(stats[member][0] or 0.0, 0.0) for member in members}
        total = sum(positive.values())
        weights = {member: (1.0 - shrinkage) * (positive[member] / total if total > 0 else equal) + shrinkage * equal
                   for member in members}
    else:
        raise CompositeError(f"unknown weighting {weighting!r}")
    return {member: {"weight": weights[member], "fit_ic": stats[member][0], "fit_formations": stats[member][1]}
            for member in members}


# ---------------------------------------------------------------------------
# Constituent sources: one batch of formations at a time (I3)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Rows:
    """Valued rows of one constituent, sorted by (month, security); raw (unoriented) values."""

    month: np.ndarray
    security: np.ndarray
    value: np.ndarray
    clock: np.ndarray  # epoch microseconds, -1 unknown
    unlinked: np.ndarray  # R2b owner_basis='unlinked_line'


def _sorted_rows(month: np.ndarray, security: np.ndarray, value: np.ndarray, clock: np.ndarray,
                 unlinked: np.ndarray) -> Rows:
    order = np.lexsort((security, month))
    return Rows(month[order].astype(np.int32), security[order].astype(np.int32), value[order].astype(np.float64),
                clock[order].astype(np.int64), unlinked[order].astype(bool))


class FeatureDataSource:
    """In-memory R3b :class:`~atx_db.research.evaluation.FeatureData` (fixtures): loaded once per feature."""

    def __init__(self, inputs: ev.BasisInputs, variant: str) -> None:
        self.inputs, self.variant = inputs, variant
        self._rows: dict[str, Rows] = {}

    def _load(self, feature_id: str) -> Rows:
        if feature_id not in self._rows:
            if self.variant not in self.inputs.variants_by_feature.get(feature_id, ()):
                raise CompositeError(f"{feature_id}: the {self.inputs.basis} basis has no {self.variant} values")
            frame = self.inputs.load_feature(feature_id).values
            value = frame["value"].to_numpy(dtype=float)
            keep = (frame["variant"].astype(str) == self.variant).to_numpy() & np.isfinite(value)
            if "available_at" in frame:
                stamps = pd.to_datetime(frame["available_at"])
                clock = np.where(stamps.isna(), -1, stamps.to_numpy().astype("datetime64[us]").astype(np.int64))
            else:
                clock = np.full(len(frame), -1, dtype=np.int64)
            unlinked = frame["unlinked_line"].to_numpy(dtype=bool) if "unlinked_line" in frame else \
                np.zeros(len(frame), dtype=bool)
            self._rows[feature_id] = _sorted_rows(frame["month_index"].to_numpy()[keep],
                                                  frame["security"].to_numpy()[keep], value[keep], clock[keep],
                                                  unlinked[keep])
        return self._rows[feature_id]

    def load(self, months: np.ndarray, feature_ids: Sequence[str]) -> dict[str, Rows]:
        return {fid: self._load(fid) for fid in feature_ids}


class StoreSource:
    """One R2b version's values of one variant, one batch of formations per query.

    Reads the variant's column of ``research_feature_matrix`` (the table behind R2b's
    ``research_feature_values`` view; the unpivoted view would materialize all six
    variants per batch). Canonical keys come from the R3b basis calendar and security codes
    (registered here under private temp-table names, independent of R3b's own).
    """

    def __init__(self, con: duckdb.DuckDBPyConnection, feature_version: str, variant: str,
                 calendar: pd.DataFrame, security_ids: Sequence[str]) -> None:
        if variant not in rf.VARIANTS:
            raise CompositeError(f"unknown R2b variant {variant!r}")
        self.con, self.version, self.variant = con, feature_version, variant
        formed = calendar[calendar["status"] == CALENDAR_FORMED]
        self.date_of = {int(m): _day(d) for m, d in zip(formed["month_index"], formed["formation_date"], strict=True)}
        suffix = _sha(feature_version)[:12]
        self.months_table, self.securities_table = f"_cmp_months_{suffix}", f"_cmp_securities_{suffix}"
        con.execute(f"CREATE OR REPLACE TEMP TABLE {self.months_table} (formation_date DATE, month_index BIGINT)")
        con.executemany(f"INSERT INTO {self.months_table} VALUES (?, ?)", [(d, m) for m, d in self.date_of.items()])
        con.execute(f"CREATE OR REPLACE TEMP TABLE {self.securities_table} (security_id VARCHAR, code BIGINT)")
        con.register("_cmp_codes", pd.DataFrame({"security_id": pd.Series(list(security_ids), dtype=object),
                                                 "code": np.arange(len(security_ids), dtype=np.int64)}))
        try:
            con.execute(f"INSERT INTO {self.securities_table} SELECT security_id, code FROM _cmp_codes")
        finally:
            con.unregister("_cmp_codes")

    def load(self, months: np.ndarray, feature_ids: Sequence[str]) -> dict[str, Rows]:
        ids = list(feature_ids)
        result = self.con.execute(f"""
            SELECT list_position(?::VARCHAR[], v.feature_id) - 1 AS f, m.month_index, s.code,
                   v.{self.variant} AS value, coalesce(epoch_us(v.available_at), -1) AS clock,
                   coalesce(v.owner_basis = '{rf.OWNER_BASIS_UNLINKED}', false) AS unlinked
            FROM research_feature_matrix v
            JOIN {self.months_table} m ON m.formation_date = v.formation_date
            JOIN {self.securities_table} s ON s.security_id = v.security_id
            WHERE v.feature_version = ? AND v.{self.variant} IS NOT NULL
              AND list_contains(?::VARCHAR[], v.feature_id) AND v.formation_date BETWEEN ? AND ?
        """, [ids, self.version, ids, self.date_of[int(months[0])], self.date_of[int(months[-1])]]).fetchnumpy()
        feature = np.asarray(result["f"], dtype=np.int64)
        out = {}
        for position, fid in enumerate(ids):
            chosen = feature == position
            out[fid] = _sorted_rows(np.asarray(result["month_index"])[chosen], np.asarray(result["code"])[chosen],
                                    np.asarray(result["value"], dtype=float)[chosen],
                                    np.asarray(result["clock"])[chosen], np.asarray(result["unlinked"])[chosen])
        return out


# ---------------------------------------------------------------------------
# Dense month blocks
# ---------------------------------------------------------------------------

def _scatter(month: np.ndarray, security: np.ndarray, values: np.ndarray, batch: np.ndarray, width: int,
             fill: Any = np.nan, dtype: Any = float) -> np.ndarray:
    out = np.full((len(batch), width), fill, dtype=dtype)
    lo, hi = np.searchsorted(month, batch[0], "left"), np.searchsorted(month, batch[-1], "right")
    part = month[lo:hi]
    if not len(part):
        return out
    pos = np.minimum(np.searchsorted(batch, part), len(batch) - 1)
    keep = batch[pos] == part
    out[pos[keep], security[lo:hi][keep]] = values[lo:hi][keep]
    return out


def _zscore_rows(block: np.ndarray) -> np.ndarray:
    finite = np.isfinite(block)
    n = finite.sum(axis=1)
    with np.errstate(invalid="ignore", divide="ignore"):
        mean = np.where(finite, block, 0.0).sum(axis=1) / n
        centered = np.where(finite, block - mean[:, None], 0.0)
        sd = np.sqrt((centered * centered).sum(axis=1) / n)
        out = (block - mean[:, None]) / sd[:, None]
    out[(n < 3) | ~(sd > 0)] = np.nan
    return out


def _rank_rows(block: np.ndarray) -> np.ndarray:
    """Average ranks within each row over its finite entries (NaN elsewhere)."""
    finite = np.isfinite(block)
    out = np.full(block.shape, np.nan)
    rows = np.nonzero(finite)[0]
    if len(rows):
        out[finite] = ev.grouped_average_ranks(rows, block[finite], block.shape[0])
    return out


def _pair_correlations(ranks: np.ndarray, min_names: int) -> tuple[np.ndarray, np.ndarray]:
    """Per month (axis 1) Pearson of ranks on names valued on both: summed rho and month counts (k x k)."""
    k, months, _ = ranks.shape
    total, count = np.zeros((k, k)), np.zeros((k, k), dtype=np.int64)
    for month in range(months):
        x = ranks[:, month, :]
        valued = np.isfinite(x).astype(float)
        filled = np.where(valued > 0, x, 0.0)
        n = valued @ valued.T
        sums = filled @ valued.T  # sums[i, j]: sum of x_i over names valued on both
        squares = (filled * filled) @ valued.T
        cross = filled @ filled.T
        with np.errstate(invalid="ignore", divide="ignore"):
            cov = cross - sums * sums.T / n
            var = squares - sums * sums / n
            rho = cov / np.sqrt(var * var.T)
        good = (n >= min_names) & np.isfinite(rho)
        total += np.where(good, rho, 0.0)
        count += good
    return total, count


def _combine(blocks: Sequence[np.ndarray], clocks: Sequence[np.ndarray], weights: Sequence[float],
             correlation: Sequence[Sequence[float]], min_share: float
             ) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Variance-adjusted weighted combination, re-standardized: (z, clock, constituents valued)."""
    k = len(blocks)
    valued = [np.isfinite(block) for block in blocks]
    count = np.sum(valued, axis=0)
    numerator = np.zeros(blocks[0].shape)
    variance = np.zeros(blocks[0].shape)
    clock = np.full(blocks[0].shape, -1, dtype=np.int64)
    for i in range(k):
        numerator += np.where(valued[i], weights[i] * np.where(valued[i], blocks[i], 0.0), 0.0)
        clock = np.where(valued[i], np.maximum(clock, clocks[i]), clock)
        for j in range(k):
            rho = 1.0 if i == j else float(correlation[i][j])
            if rho:
                variance += weights[i] * weights[j] * rho * (valued[i] & valued[j])
    need = max(1, math.ceil(min_share * k - 1e-9))
    ok = (count >= need) & (variance > 0)
    with np.errstate(invalid="ignore", divide="ignore"):
        raw = np.where(ok, numerator / np.sqrt(np.where(ok, variance, 1.0)), np.nan)
    z = _zscore_rows(raw)
    return z, np.where(np.isfinite(z), clock, -1), np.where(np.isfinite(z), count, 0)


def _batches(months: np.ndarray, width: int, depth: int) -> Iterator[np.ndarray]:
    # ~96 MB of float64/int64 blocks per batch (depth blocks x months x securities), at most 24 months.
    size = int(max(1, min(24, 96 * 2**20 // (8 * max(width, 1) * max(depth, 1) * 3))))
    for start in range(0, len(months), size):
        yield months[start:start + size]


def _monthly_ic(block: np.ndarray, labels: np.ndarray, min_names: int) -> np.ndarray:
    batch = block.shape[0]
    group = np.repeat(np.arange(batch), block.shape[1])
    rho, counts = ev.grouped_rank_correlation(group, block.ravel(), labels.ravel(), batch)
    return np.where(counts >= min_names, rho, np.nan)


# ---------------------------------------------------------------------------
# Definitions, result
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class CompositeDefinition:
    composite_id: str
    level: str
    weighting: str
    anomaly_class: str | None
    #: (member id, weight, fit IC, fit formations): signal ids within a class, class
    #: composite ids across classes.
    members: tuple[tuple[str, float, float | None, int], ...]
    #: Fit-sample correlation of the members (variance adjustment for missing members).
    correlation: tuple[tuple[float, ...], ...]
    emitted: bool


@dataclass(frozen=True)
class BuildForwardResult:
    ledger_id: str
    holdout_key: str
    basis: str
    manifest: Mapping[str, Any]
    selection: tuple[Mapping[str, Any], ...]
    redundancy: tuple[Mapping[str, Any], ...]
    definitions: tuple[CompositeDefinition, ...]
    orientation: Mapping[str, int]
    sha256: str
    width: int = field(default=0, compare=False)

    def to_json_bytes(self) -> bytes:
        payload = {"manifest": self.manifest, "selection": list(self.selection), "redundancy": list(self.redundancy),
                   "definitions": [_definition_json(d) for d in self.definitions], "sha256": self.sha256}
        return (json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode()

    @property
    def emitted(self) -> tuple[CompositeDefinition, ...]:
        return tuple(d for d in self.definitions if d.emitted)


def _definition_json(definition: CompositeDefinition) -> dict[str, Any]:
    return _clean({"composite_id": definition.composite_id, "level": definition.level,
                   "weighting": definition.weighting, "anomaly_class": definition.anomaly_class,
                   "emitted": definition.emitted, "correlation": [list(r) for r in definition.correlation],
                   "members": [{"member_id": m, "weight": w, "fit_ic": ic, "fit_formations": n}
                               for m, w, ic, n in definition.members]})


def _slug(text: str) -> str:
    slug = "".join(ch if ch in _ID_CHARS else "_" for ch in text.lower()).strip("_") or UNCLASSIFIED
    return slug[:40]


def holdout_key(split: ev.FrozenSplit, basis: str) -> str:
    """One holdout evaluation per (frozen split, basis) (M1)."""
    return f"{split.sha256}:{basis}"


# ---------------------------------------------------------------------------
# Build forward (pure: no store access)
# ---------------------------------------------------------------------------

def _complete_linkage(ids: Sequence[str], rho: Mapping[tuple[str, str], float], threshold: float
                      ) -> list[list[str]]:
    clusters = [[feature] for feature in sorted(ids)]

    def link(a: list[str], b: list[str]) -> float:
        values = [rho.get((min(x, y), max(x, y)), math.nan) for x in a for y in b]
        return math.nan if any(math.isnan(v) for v in values) else min(values)

    while True:
        best: tuple[float, str, str, int, int] | None = None
        for i in range(len(clusters)):
            for j in range(i + 1, len(clusters)):
                value = link(clusters[i], clusters[j])
                if math.isnan(value) or value < threshold:
                    continue
                key = (value, clusters[i][0], clusters[j][0], i, j)
                if best is None or value > best[0] or (value == best[0] and key[1:3] < best[1:3]):
                    best = key
        if best is None:
            break
        _, _, _, i, j = best
        clusters[i] = sorted(clusters[i] + clusters[j])
        del clusters[j]
        clusters.sort(key=lambda c: c[0])
    return sorted(clusters, key=lambda c: (-len(c), c[0]))


def _periods(index: Any) -> pd.PeriodIndex:
    return pd.PeriodIndex(pd.to_datetime(pd.Index(index)).to_period("M"))


def _span(signal: SelectedSignal, series: pd.DataFrame, factors: pd.DataFrame, policy: CompositePolicy
          ) -> dict[str, Any]:
    empty = {"span_status": SPAN_TOO_FEW, "span_note": None, "span_alpha": None, "span_alpha_z": None,
             "span_alpha_p": None, "span_r2": None, "span_n_obs": 0, "span_betas": None}
    rows = series[series["feature_id"] == signal.feature_id]
    if not len(rows):
        return empty
    y = pd.Series(signal.orientation * rows["value"].to_numpy(dtype=float), index=_periods(rows["formation_date"]))
    frame = factors[list(policy.span_factors)].copy()
    frame.index = _periods(frame.index)
    if y.index.has_duplicates or frame.index.has_duplicates:
        raise CompositeError(f"{signal.feature_id}: more than one formation in a calendar month")
    grid = pd.period_range(y.index.min(), y.index.max(), freq="M")  # P4's gap rule, selection span only
    try:
        result = fr.span_test(y.reindex(grid), frame.reindex(grid), horizon_periods=policy.span_horizon,
                              factor_window_basis=fr.WINDOW_EXACT)
    except fr.FactorInputError:
        return empty
    if result.n_obs < policy.span_min_periods:
        return {**empty, "span_n_obs": result.n_obs}
    z = result.alpha_z
    if not result.significance_claimable:
        status = SPAN_NOT_CLAIMABLE
    else:
        status = SPAN_TESTED if z is not None and math.isfinite(z) else SPAN_DEGENERATE
    note = NEGATIVE_ALPHA if status == SPAN_TESTED and z <= -policy.alpha_min_z else None
    return {"span_status": status, "span_note": note, "span_alpha": result.alpha, "span_alpha_z": z,
            "span_alpha_p": result.alpha_robust_p, "span_r2": result.r2, "span_n_obs": result.n_obs,
            "span_betas": dict(sorted(result.betas.items()))}


def _factor_digest(factors: pd.DataFrame, names: Sequence[str]) -> str:
    frame = factors[list(names)].copy()
    frame.index = pd.to_datetime(frame.index)
    rows = [[day.date().isoformat(), *(_clean(float(v)) for v in values)]
            for day, values in zip(frame.index, frame.to_numpy(dtype=float), strict=True)]
    return _sha(_canonical(rows))


def _post_hoc(policy_registered_at: str | None, source_created_at: str | None) -> list[str]:
    if policy_registered_at is None:
        return ["composite_policy_not_registered"]
    if source_created_at is None or \
            dt.datetime.fromisoformat(policy_registered_at) > dt.datetime.fromisoformat(source_created_at):
        return ["composite_policy_registered_after_evaluation"]
    return []


def build_forward(ledger: LedgerSource, inputs: ev.BasisInputs, split: ev.FrozenSplit, series: pd.DataFrame,
                  factors: pd.DataFrame, policy: CompositePolicy, *, source: Any = None,
                  factor_source: Mapping[str, Any] | None = None, policy_registered_at: str | None = None,
                  source_created_at: str | None = None, allow_post_hoc_policy: bool = False) -> BuildForwardResult:
    """Select on train/validation evidence, annotate, fit and digest the composites (pure).

    ``inputs`` supplies the composite basis' calendar, maturity and R3b labels (and, without
    ``source``, the constituent values); ``source`` streams constituent values per batch
    (:class:`StoreSource`). ``series`` is the R3b selection long-short series
    (:func:`load_selection_series`), ``factors`` the monthly factor returns indexed by
    formation date. ``policy_registered_at`` / ``source_created_at`` enforce RX7: the
    composite policy must be frozen before the source run was created, else the build is
    refused unless ``allow_post_hoc_policy`` (stamped). Byte-identical for identical inputs.
    """
    if ledger.post_selection:
        raise CompositeError(f"ledger {ledger.ledger_id} contains composites or is stamped {POST_SELECTION_STAMP}: "
                             "a post-selection ledger never seeds another composite build")
    if inputs.basis != policy.basis:
        raise CompositeError(f"the composite basis is {policy.basis}; inputs are {inputs.basis}")
    if ledger.split_sha256 is not None and ledger.split_sha256 != split.sha256:
        raise CompositeError(f"the ledger was graded under split {ledger.split_sha256}, not {split.sha256}")
    post_hoc = _post_hoc(policy_registered_at, source_created_at)
    if post_hoc and not allow_post_hoc_policy:
        raise CompositeError(f"RX7: {', '.join(post_hoc)} (freeze the composite policy before the RR4 run, or "
                             "stamp an explicit post-hoc build)")
    missing = sorted(set(policy.span_factors) - {str(c) for c in factors.columns})
    if missing:
        raise CompositeError(f"factor returns lack {missing}")
    source = source or FeatureDataSource(inputs, policy.constituent_variant)
    signals = select_signals(ledger, policy)
    selected = [s for s in signals if s.selected]
    ids = [s.feature_id for s in selected]
    by_id = {s.feature_id: s for s in selected}
    fit = fit_sample(inputs, split, policy.fit_horizon)
    fit_months = fit["month_index"].to_numpy(dtype=np.int64)
    fit_by_month = fit.set_index("month_index")
    width = int(inputs.security_count)
    labels = inputs.labels
    chosen = (labels["horizon_months"] == policy.fit_horizon) & (labels["status"] == 0) \
        & labels["month_index"].isin(fit_months)
    label_rows = _sorted_rows(labels.loc[chosen, "month_index"].to_numpy(), labels.loc[chosen, "security"].to_numpy(),
                              labels.loc[chosen, "forward_return"].to_numpy(dtype=float),
                              np.zeros(int(chosen.sum()), dtype=np.int64), np.zeros(int(chosen.sum()), dtype=bool))

    def ic_rows(member_id: str, months: np.ndarray, rho: np.ndarray) -> list[dict[str, Any]]:
        return [{"member_id": member_id, "formation_date": fit_by_month.at[int(m), "formation_date"],
                 "label_end": fit_by_month.at[int(m), "label_end"], "ic": float(r)}
                for m, r in zip(months.tolist(), rho.tolist(), strict=True) if math.isfinite(r)]

    def blocks(batch: np.ndarray, members: Sequence[str]) -> dict[str, np.ndarray]:
        data = source.load(batch, members)
        return {fid: _scatter(data[fid].month, data[fid].security, data[fid].value, batch, width)
                * by_id[fid].orientation for fid in members}

    # Pass A (fit sample): pairwise rank correlation of all selected signals, and each signal's IC.
    total = np.zeros((len(ids), len(ids)))
    months_used = np.zeros((len(ids), len(ids)), dtype=np.int64)
    signal_ics: list[dict[str, Any]] = []
    if len(fit_months) and ids:
        for batch in _batches(fit_months, width, 2 * len(ids) + 1):
            values = blocks(batch, ids)
            label_block = _scatter(label_rows.month, label_rows.security, label_rows.value, batch, width)
            for fid in ids:
                signal_ics.extend(ic_rows(fid, batch, _monthly_ic(values[fid], label_block, policy.min_names)))
            ranks = np.stack([_rank_rows(values[fid]) for fid in ids])
            del values
            part, used = _pair_correlations(ranks, policy.min_names)
            total += part
            months_used += used
            del ranks
    rho_mean: dict[tuple[str, str], float] = {}
    formations: dict[tuple[str, str], int] = {}
    for i, a in enumerate(ids):
        for j in range(i + 1, len(ids)):
            b = ids[j]
            pair = (min(a, b), max(a, b))
            formations[pair] = int(months_used[i, j])
            rho_mean[pair] = total[i, j] / months_used[i, j] if months_used[i, j] >= policy.min_formations else math.nan
    clusters = _complete_linkage(ids, rho_mean, policy.rho_threshold)
    representative, cluster_of = {}, {}
    for number, cluster in enumerate(clusters, start=1):
        head = min(cluster, key=lambda f: (-abs(by_id[f].ic_z or 0.0), f))
        for feature in cluster:
            representative[feature] = head
            cluster_of[feature] = (f"c{number:03d}", len(cluster))
    spans = {fid: _span(by_id[fid], series, factors, policy) for fid in ids}
    redundancy = []
    for fid in ids:
        signal, head, span = by_id[fid], representative[fid], spans[fid]
        if head != fid:
            status = f"{DUPLICATE_PREFIX}{head}"
        elif span["span_status"] == SPAN_TESTED and span["span_alpha_z"] < policy.alpha_min_z:
            status = SPANNED
        else:
            status = NOVEL
        redundancy.append(_clean({
            "feature_id": fid, "r4_status": signal.r4_status, "anomaly_class": signal.anomaly_class,
            "ic_z": signal.ic_z, "redundancy_status": status, "cluster_id": cluster_of[fid][0],
            "cluster_size": cluster_of[fid][1], "representative": head,
            "rho_to_representative": 1.0 if head == fid else rho_mean.get((min(fid, head), max(fid, head))), **span}))

    # Constituents: cluster representatives, grouped by class; within-class weights.
    constituents = [fid for fid in ids if representative[fid] == fid]
    classes: dict[str, list[str]] = {}
    for fid in constituents:
        classes.setdefault(by_id[fid].anomaly_class, []).append(fid)
    class_ids = {cls: f"cmp_{_slug(cls)}" for cls in classes}
    if len(set(class_ids.values())) != len(class_ids) or "cmp_classes" in class_ids.values():
        raise CompositeError(f"anomaly classes do not give distinct composite ids: {sorted(classes)}")
    signal_rows = pd.DataFrame(signal_ics, columns=["member_id", "formation_date", "label_end", "ic"])

    def correlation(members: Sequence[str], lookup: Mapping[tuple[str, str], float]) -> tuple[tuple[float, ...], ...]:
        return tuple(tuple(1.0 if a == b else float(np.clip(np.nan_to_num(
            lookup.get((min(a, b), max(a, b)), math.nan), nan=0.0), -0.99, 0.99)) for b in members) for a in members)

    definitions: list[CompositeDefinition] = []
    within: dict[str, dict[str, CompositeDefinition]] = {}
    for weighting in policy.weightings:
        within[weighting] = {}
        for cls in sorted(classes):
            members = classes[cls]
            fitted = fit_weights(signal_rows[signal_rows["member_id"].isin(members)], members, split,
                                 weighting=weighting, shrinkage=policy.ic_shrinkage)
            definition = CompositeDefinition(
                f"{class_ids[cls]}_{WEIGHT_TAGS[weighting]}", LEVEL_WITHIN, weighting, cls,
                tuple((m, fitted[m]["weight"], fitted[m]["fit_ic"], fitted[m]["fit_formations"]) for m in members),
                correlation(members, rho_mean), len(members) >= policy.min_class_constituents)
            within[weighting][cls] = definition
            definitions.append(definition)

    # Pass B (fit sample): IC and pairwise correlation of the class composites (across-class inputs).
    orientation = {fid: by_id[fid].orientation for fid in constituents}
    class_ics: list[dict[str, Any]] = []
    class_rho: dict[tuple[str, str], float] = {}
    if len(fit_months) and constituents and len(classes) >= policy.min_across_inputs:
        inputs_ids = {w: [within[w][cls].composite_id for cls in sorted(classes)] for w in policy.weightings}
        sums = {w: np.zeros((len(classes), len(classes))) for w in policy.weightings}
        used = {w: np.zeros((len(classes), len(classes)), dtype=np.int64) for w in policy.weightings}
        depth = len(constituents) + len(classes) * len(policy.weightings) + 1
        for batch in _batches(fit_months, width, depth):
            label_block = _scatter(label_rows.month, label_rows.security, label_rows.value, batch, width)
            values = blocks(batch, constituents)
            z = {fid: _zscore_rows(values[fid]) for fid in constituents}
            del values
            none = np.full((len(batch), width), -1, dtype=np.int64)
            for weighting in policy.weightings:
                made = []
                for cls in sorted(classes):
                    definition = within[weighting][cls]
                    block, _, _ = _combine([z[m] for m, *_ in definition.members], [none] * len(definition.members),
                                           [w for _, w, *_ in definition.members], definition.correlation,
                                           policy.min_constituent_share)
                    class_ics.extend(ic_rows(definition.composite_id, batch,
                                             _monthly_ic(block, label_block, policy.min_names)))
                    made.append(_rank_rows(block))
                part, count = _pair_correlations(np.stack(made), policy.min_names)
                sums[weighting] += part
                used[weighting] += count
        for weighting in policy.weightings:
            names = inputs_ids[weighting]
            for i, a in enumerate(names):
                for j in range(i + 1, len(names)):
                    n = used[weighting][i, j]
                    class_rho[(min(a, names[j]), max(a, names[j]))] = \
                        sums[weighting][i, j] / n if n >= policy.min_formations else math.nan
    class_rows = pd.DataFrame(class_ics, columns=["member_id", "formation_date", "label_end", "ic"])
    if len(classes) >= policy.min_across_inputs:
        for weighting in policy.weightings:
            names = [within[weighting][cls].composite_id for cls in sorted(classes)]
            fitted = fit_weights(class_rows[class_rows["member_id"].isin(names)], names, split,
                                 weighting=weighting, shrinkage=policy.ic_shrinkage)
            definitions.append(CompositeDefinition(
                f"cmp_classes_{WEIGHT_TAGS[weighting]}", LEVEL_ACROSS, weighting, None,
                tuple((m, fitted[m]["weight"], fitted[m]["fit_ic"], fitted[m]["fit_formations"]) for m in names),
                correlation(names, class_rho), True))

    # Pass C (every formed formation): stream the composites once for their digests and coverage.
    digests: dict[str, Any] = {d.composite_id: hashlib.sha256() for d in definitions if d.emitted}
    coverage: dict[str, dict[str, Any]] = {cid: {"rows": 0, "unlinked_rows": 0, "unlinked_dropped": 0,
                                                 "rows_by_constituents": {}} for cid in digests}
    for _, frames, stats in materialize(source, inputs.calendar, definitions, orientation, policy, width):
        for cid, frame in frames.items():
            for column in ("month_index", "security", "value", "clock_us", "unlinked"):
                digests[cid].update(np.ascontiguousarray(frame[column].to_numpy()).tobytes())
            item = coverage[cid]
            item["rows"] += len(frame)
            item["unlinked_rows"] += int(frame["unlinked"].sum())
            item["unlinked_dropped"] += stats[cid]
            for n, rows in zip(*np.unique(frame["constituents"].to_numpy(), return_counts=True), strict=True):
                item["rows_by_constituents"][str(int(n))] = item["rows_by_constituents"].get(str(int(n)), 0) + int(rows)
    blockers = ["research_only_not_release_eligible"]
    blockers += [f"post_hoc_composite_policy:{reason}" for reason in post_hoc]
    if ledger.post_hoc_policy:
        blockers.append("ledger_post_hoc_policy")
    untested = sum(1 for fid in ids if spans[fid]["span_status"] != SPAN_TESTED)
    if untested:
        blockers.append(f"spanning_untested:{untested}")
    blockers += [f"composite_drops_unlinked_lines:{cid}:{item['unlinked_dropped']}"
                 for cid, item in sorted(coverage.items()) if item["unlinked_dropped"]]
    source_info = dict(factor_source or {"kind": "injected"})
    if source_info.get("kind") != "p4_run":
        blockers.append("factors_not_a_p4_run")
    if not digests:
        blockers.append("no_composite_emitted")
    key = holdout_key(split, policy.basis)
    manifest = _clean({
        "composite_version": COMPOSITE_VERSION, "code_sha256": code_sha256(),
        "policy": {"version": policy.version, "sha256": policy.sha256, "registered_at": policy_registered_at},
        "post_hoc_policy": bool(post_hoc), "post_hoc_reasons": post_hoc,
        "ledger": {"ledger_id": ledger.ledger_id, "ledger_sha256": ledger.ledger_sha256,
                   "evaluation_run_id": ledger.run_id, "evaluation_created_at": source_created_at,
                   "r4_policy_version": ledger.r4_policy_version},
        "basis": policy.basis, "split_sha256": split.sha256, "holdout_key": key,
        "constituent_variant": policy.constituent_variant,
        "fit_sample": {"horizon_months": policy.fit_horizon, "formations": len(fit),
                       "first": fit["formation_date"].min() if len(fit) else None,
                       "last": fit["formation_date"].max() if len(fit) else None,
                       "holdout_start": split.holdout_start},
        "selection": {"rule": "r4_train_validation_gates", "gates": list(policy.selection_gates),
                      "selected": ids, "candidates": len(signals)},
        "factors": {**source_info, "factors": list(policy.span_factors),
                    "sha256": _factor_digest(factors, policy.span_factors)},
        "clusters": clusters, "constituents": constituents,
        "rank_correlations": [[a, b, rho_mean[(a, b)], formations[(a, b)]] for a, b in sorted(rho_mean)],
        "composites": [{"composite_id": cid, "values_sha256": digests[cid].hexdigest(), **coverage[cid]}
                       for cid in sorted(digests)],
        "blockers": sorted(blockers),
    })
    selection = tuple(_clean({"feature_id": s.feature_id, "anomaly_class": s.anomaly_class, "r4_status": s.r4_status,
                              "selected": s.selected, "reasons": s.reasons}) for s in signals)
    identity = {"manifest": {k: v for k, v in manifest.items() if k != "code_sha256"}, "selection": list(selection),
                "redundancy": redundancy, "definitions": [_definition_json(d) for d in definitions]}
    return BuildForwardResult(ledger.ledger_id, key, policy.basis, manifest, selection, tuple(redundancy),
                              tuple(definitions), orientation, _sha(_canonical(identity)), width)


def materialize(source: Any, calendar: pd.DataFrame, definitions: Sequence[CompositeDefinition],
                orientation: Mapping[str, int], policy: CompositePolicy, width: int
                ) -> Iterator[tuple[np.ndarray, dict[str, pd.DataFrame], dict[str, int]]]:
    """Composite values on every formed formation, one batch at a time.

    Yields ``(months, {composite_id: frame}, {composite_id: unlinked_dropped})`` for the
    emitted definitions; a frame has ``month_index``, ``security``, ``value``, ``clock_us``,
    ``unlinked`` and ``constituents`` (members valued). A constituent value without a clock,
    or with a clock after its formation cutoff, raises :class:`CompositeError`.
    """
    formed = calendar[calendar["status"] == CALENDAR_FORMED]
    months = formed["month_index"].to_numpy(dtype=np.int64)
    cutoff = pd.to_datetime(formed["cutoff"]).to_numpy().astype("datetime64[us]").astype(np.int64)
    cutoff_of = dict(zip(months.tolist(), cutoff.tolist(), strict=True))
    fids = sorted(orientation)
    within = [d for d in definitions if d.level == LEVEL_WITHIN]
    across = [d for d in definitions if d.level == LEVEL_ACROSS]
    emitted = [d.composite_id for d in definitions if d.emitted]
    if not fids or not len(months) or not emitted:
        return
    depth = 3 * len(fids) + 2 * len(definitions) + 2
    for batch in _batches(months, width, depth):
        limit = np.array([cutoff_of[int(m)] for m in batch], dtype=np.int64)[:, None]
        data = source.load(batch, fids)
        z, clocks = {}, {}
        unlinked = np.zeros((len(batch), width), dtype=bool)
        for fid in fids:
            rows = data[fid]
            values = _scatter(rows.month, rows.security, rows.value, batch, width) * orientation[fid]
            clock = _scatter(rows.month, rows.security, rows.clock, batch, width, fill=-1, dtype=np.int64)
            if bool((np.isfinite(values) & (clock < 0)).any()):
                raise CompositeError(f"{fid}: a constituent value has no availability clock")
            if bool((clock > limit).any()):
                raise CompositeError(f"{fid}: a constituent value is not visible at its formation cutoff")
            unlinked |= _scatter(rows.month, rows.security, rows.unlinked, batch, width, fill=False, dtype=bool)
            z[fid], clocks[fid] = _zscore_rows(values), clock
        del data
        made: dict[str, tuple[np.ndarray, np.ndarray, np.ndarray]] = {}
        seen: dict[str, np.ndarray] = {}  # some member valued (for the unlinked-drop count)
        for definition in within:
            members = [m for m, *_ in definition.members]
            made[definition.composite_id] = _combine(
                [z[m] for m in members], [clocks[m] for m in members], [w for _, w, *_ in definition.members],
                definition.correlation, policy.min_constituent_share)
            seen[definition.composite_id] = np.logical_or.reduce([np.isfinite(z[m]) for m in members])
        for definition in across:
            members = [m for m, *_ in definition.members]
            made[definition.composite_id] = _combine(
                [made[m][0] for m in members], [made[m][1] for m in members], [w for _, w, *_ in definition.members],
                definition.correlation, policy.min_constituent_share)
            seen[definition.composite_id] = np.logical_or.reduce([seen[m] for m in members])
        frames, dropped = {}, {}
        for cid in emitted:
            block, clock, count = made[cid]
            valued = np.isfinite(block)
            rows_i, cols = np.nonzero(valued)
            frames[cid] = pd.DataFrame({"month_index": batch[rows_i].astype(np.int64), "security": cols.astype(np.int64),
                                        "value": block[rows_i, cols], "clock_us": clock[rows_i, cols],
                                        "unlinked": unlinked[rows_i, cols], "constituents": count[rows_i, cols]})
            dropped[cid] = int((unlinked & seen[cid] & ~valued).sum())
        yield batch, frames, dropped


# ---------------------------------------------------------------------------
# Composite features: R2b variants, catalog, in-memory R3b evaluation (full family)
# ---------------------------------------------------------------------------

def composite_catalog(result: BuildForwardResult) -> tuple[ev.CatalogFeature, ...]:
    """The composites as R3b catalog rows (class ``composite``, every expected variant)."""
    return tuple(ev.CatalogFeature(d.composite_id, 1, COMPOSITE_CLASS, ev.expected_variants(COMPOSITE_CLASS),
                                   COMPOSITE_CLASS) for d in sorted(result.emitted, key=lambda d: d.composite_id))


def stamped_catalog_entries(catalog: Iterable[Any], definitions: Iterable[CompositeDefinition], ledger_id: str
                            ) -> list[SimpleNamespace]:
    """R4 catalog entries of a composite run: the source catalog plus the composites, every row
    stamped ``composite_post_selection`` and ``constituent_ledger:<id>`` (I1)."""
    stamp = (POST_SELECTION_STAMP, f"constituent_ledger:{ledger_id}")
    out = [SimpleNamespace(feature_id=e.feature_id, expected_sign=int(e.expected_sign), anomaly_class=e.anomaly_class,
                           hypothesis_family=getattr(e, "hypothesis_family", None),
                           prior_evidence=getattr(e, "prior_evidence", None),
                           admission=getattr(e, "admission", None),
                           caveat_codes=(*tuple(getattr(e, "caveat_codes", ()) or ()), *stamp),
                           is_research_eligible=bool(getattr(e, "is_research_eligible", True))) for e in catalog]
    out += [SimpleNamespace(feature_id=d.composite_id, expected_sign=1, anomaly_class=COMPOSITE_CLASS,
                            hypothesis_family=COMPOSITE_CLASS, prior_evidence=POST_SELECTION_STAMP,
                            admission="eligible", caveat_codes=(*COMPOSITE_CAVEATS, stamp[1]),
                            is_research_eligible=True)
            for d in sorted(definitions, key=lambda d: d.composite_id) if d.emitted]
    return out


def standardize_composite(frame: pd.DataFrame, composite_id: str, standardization: rf.StandardizationPolicy
                          ) -> tuple[pd.DataFrame, dict[tuple[dt.date, str], dict[str, Any]]]:
    """Every R2b variant of one composite (R2b's own :func:`~atx_db.research.features.standardize_frame`)."""
    work = frame.assign(domain_status=rf.IN_DOMAIN)
    standardized, stats = rf.standardize_frame(work, feature_id=composite_id, orientation=1, log_base=False,
                                               variants=rf.VARIANTS, policy=standardization)
    return standardized, {(pd.Timestamp(day).date(), variant): item for (day, variant), item in stats.items()}


def composite_feature_data(result: BuildForwardResult, inputs: ev.BasisInputs, policy: CompositePolicy,
                           standardization: rf.StandardizationPolicy, *, source: Any = None,
                           covariates: pd.DataFrame | None = None) -> dict[str, ev.FeatureData]:
    """R3b :class:`~atx_db.research.evaluation.FeatureData` of each emitted composite (all variants).

    ``covariates`` (``month_index``, ``security``, ``log_size``, ``industry_group``) feed the
    neutral variants (default: log verified cap from the basis context, no industries);
    unlinked lines have no covariates, as in R2b.
    """
    source = source or FeatureDataSource(inputs, policy.constituent_variant)
    parts: dict[str, list[pd.DataFrame]] = {d.composite_id: [] for d in result.emitted}
    for _, frames, _ in materialize(source, inputs.calendar, result.definitions, result.orientation, policy,
                                    int(inputs.security_count)):
        for cid, frame in frames.items():
            parts[cid].append(frame)
    calendar = inputs.calendar.set_index("month_index")
    if covariates is None:
        context = inputs.context
        cap = context["market_cap"].to_numpy(dtype=float) if len(context) and "market_cap" in context else \
            np.array([])
        ok = np.isfinite(cap) & (cap > 0)
        covariates = pd.DataFrame({"month_index": context["month_index"].to_numpy()[ok] if len(cap) else [],
                                   "security": context["security"].to_numpy()[ok] if len(cap) else [],
                                   "log_size": np.log(cap[ok]), "industry_group": None})
    keyed = covariates.set_index(["month_index", "security"])[["log_size", "industry_group"]]
    out = {}
    for cid, frames in sorted(parts.items()):
        frame = pd.concat(frames, ignore_index=True)
        months = frame["month_index"].to_numpy(dtype=np.int64)
        securities = frame["security"].to_numpy(dtype=np.int64)
        unlinked = frame["unlinked"].to_numpy(dtype=bool)
        joined = keyed.reindex(pd.MultiIndex.from_arrays([months, securities]))
        work = pd.DataFrame({
            "formation_date": calendar["formation_date"].reindex(months).to_numpy(), "security_id": securities,
            "raw_value": frame["value"].to_numpy(dtype=float),
            "industry_group": np.where(unlinked, None, joined["industry_group"].to_numpy(dtype=object)),
            "log_size": np.where(unlinked, np.nan, pd.to_numeric(joined["log_size"], errors="coerce").to_numpy(float)),
            "unlinked": unlinked, "clock_us": frame["clock_us"].to_numpy(dtype=np.int64)})
        standardized, stats = standardize_composite(work, cid, standardization)
        month_of = {_day(calendar.at[int(m), "formation_date"]): int(m) for m in np.unique(months)}
        standardized_month = np.array([month_of[_day(d)] for d in standardized["formation_date"]], dtype=np.int64)
        values = []
        for variant in rf.VARIANTS:
            column = standardized[variant].to_numpy(dtype=float)
            keep = np.isfinite(column)
            values.append(pd.DataFrame({
                "month_index": standardized_month[keep],
                "security": standardized["security_id"].to_numpy(dtype=np.int64)[keep], "variant": variant,
                "value": column[keep], "unlinked_line": standardized["unlinked"].to_numpy(dtype=bool)[keep],
                "available_at": standardized["clock_us"].to_numpy()[keep].astype("datetime64[us]")}))
        dates = pd.DataFrame([{"month_index": month_of[day], "variant": variant, "date_status": item["date_status"],
                               "coverage_fraction": np.nan} for (day, variant), item in sorted(stats.items())])
        out[cid] = ev.FeatureData(cid, 1, pd.concat(values, ignore_index=True), dates, COMPOSITE_CLASS,
                                  COMPOSITE_CLASS)
    return out


def evaluate_composites(result: BuildForwardResult, inputs: ev.BasisInputs, spec: ev.EvaluationSpec,
                        policy: CompositePolicy, *, standardization: rf.StandardizationPolicy,
                        source_catalog: Iterable[ev.CatalogFeature], covariates: pd.DataFrame | None = None,
                        source: Any = None) -> ev.EvaluationTables:
    """The composites' one evaluation through the R3b engine, in memory, **within the full family**:
    every candidate signal of ``inputs`` plus the composites (BH, Holm and DSR ``n_trials``
    count them all; holdout slices included). Fixture path; the store path is one R3b run
    over the composite feature versions (:func:`run_build_forward`)."""
    data = composite_feature_data(result, inputs, policy, standardization, source=source, covariates=covariates)
    variants = dict(inputs.variants_by_feature)
    variants.update({cid: tuple(sorted(set(item.values["variant"].astype(str)), key=rf.VARIANTS.index))
                     for cid, item in data.items()})
    load: Callable[[str], ev.FeatureData] = inputs.load_feature

    def loader(feature_id: str) -> ev.FeatureData:
        return data[feature_id] if feature_id in data else load(feature_id)

    basis = replace(inputs, variants_by_feature=variants, load_feature=loader)
    return ev.evaluate_bases([basis, ev.empty_basis("strict")], replace(spec, features=None, variants=None),
                             catalog=(*tuple(source_catalog), *composite_catalog(result)))


# ---------------------------------------------------------------------------
# Registry: one holdout evaluation per (split, basis) (M1, I4)
# ---------------------------------------------------------------------------

def _insert_rows(con: duckdb.DuckDBPyConnection, table: str, build_sha: str, rows: Sequence[Mapping[str, Any]],
                 columns: Sequence[tuple[str, str]]) -> None:
    if not rows:
        return
    data: dict[str, Any] = {"build_sha256": [build_sha] * len(rows)}
    for name, kind in columns:
        values = [row.get(name) for row in rows]
        if kind == "DOUBLE":
            data[name] = pd.array([None if v is None else float(v) for v in values], dtype="Float64")
        elif kind == "INTEGER":
            data[name] = pd.array([None if v is None else int(v) for v in values], dtype="Int64")
        elif kind == "BOOLEAN":
            data[name] = pd.array(values, dtype="boolean")
        else:
            data[name] = pd.array(values, dtype=object)
    con.register("_cmp_rows", pd.DataFrame(data))
    try:
        names = ", ".join(["build_sha256", *(n for n, _ in columns)])
        con.execute(f"INSERT INTO {table} ({names}) SELECT {names} FROM _cmp_rows")
    finally:
        con.unregister("_cmp_rows")


def persist_build(con: duckdb.DuckDBPyConnection, result: BuildForwardResult) -> str:
    """Store the build of its holdout window: ``'stored'``, ``'exists'`` or ``'replaced'``.

    One holdout evaluation per (split, basis): once a holdout evaluation is recorded, any
    other build is refused. Before that, a different build replaces the stored one (a
    retry after an aborted run, even after code changed; the superseded sha is kept).
    """
    ensure_composite_schema(con)
    row = con.execute("SELECT build_sha256, holdout_run_id, superseded_json, ledger_id FROM research_composite_builds "
                      "WHERE holdout_key = ?", [result.holdout_key]).fetchone()
    if row is not None and row[0] == result.sha256:
        return "exists"
    if row is not None and row[1] is not None:
        raise CompositeError(f"holdout window {result.holdout_key} was already evaluated (run {row[1]}, build {row[0]} "
                             f"from ledger {row[3]}): its holdout is spent; one evaluation per split and basis")
    manifest = result.manifest
    members = [{"composite_id": d.composite_id, "level": d.level, "weighting": d.weighting,
                "anomaly_class": d.anomaly_class, "emitted": d.emitted, "member_id": m, "weight": w, "fit_ic": ic,
                "fit_formations": n} for d in result.definitions for m, w, ic, n in d.members]
    superseded = [] if row is None else [*json.loads(row[2]), row[0]]
    con.execute("BEGIN TRANSACTION")
    try:
        if row is not None:
            for table in ("research_composite_redundancy", "research_composite_members"):
                con.execute(f"DELETE FROM {table} WHERE build_sha256 = ?", [row[0]])
            con.execute("DELETE FROM research_composite_builds WHERE holdout_key = ?", [result.holdout_key])
        con.execute("""
            INSERT INTO research_composite_builds (holdout_key, split_sha256, basis, ledger_id, ledger_sha256,
                evaluation_run_id, policy_version, policy_sha256, build_sha256, manifest_json, superseded_json,
                created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, [result.holdout_key, manifest["split_sha256"], result.basis, result.ledger_id,
              manifest["ledger"]["ledger_sha256"], manifest["ledger"]["evaluation_run_id"],
              manifest["policy"]["version"], manifest["policy"]["sha256"], result.sha256, _canonical(dict(manifest)),
              _canonical(superseded), _now()])
        _insert_rows(con, "research_composite_redundancy", result.sha256, result.redundancy, REDUNDANCY_COLUMNS)
        _insert_rows(con, "research_composite_members", result.sha256, members, MEMBER_COLUMNS)
        con.execute("COMMIT")
    except Exception:
        con.execute("ROLLBACK")
        raise
    return "stored" if row is None else "replaced"


def record_holdout_evaluation(con: duckdb.DuckDBPyConnection, key: str, versions: Sequence[str], run_id: str) -> None:
    """Record the one R3b run that evaluated the window's composites (a second one is refused)."""
    row = con.execute("SELECT holdout_run_id FROM research_composite_builds WHERE holdout_key = ?", [key]).fetchone()
    if row is None:
        raise CompositeError(f"no composite build for holdout window {key}")
    if row[0] is not None and row[0] != run_id:
        raise CompositeError(f"holdout window {key}: the composites were already evaluated once (run {row[0]})")
    con.execute("UPDATE research_composite_builds SET composite_versions_json = ?, holdout_run_id = ?, "
                "evaluated_at = coalesce(evaluated_at, ?) WHERE holdout_key = ?",
                [_canonical(list(versions)), run_id, _now(), key])


# ---------------------------------------------------------------------------
# Composite feature versions: the source version plus the composites (R2b schema v3)
# ---------------------------------------------------------------------------

def _constituent_version(con: duckdb.DuckDBPyConnection, version: str) -> dict[str, Any]:
    row = con.execute("""
        SELECT status, basis, panel_run_id, panel_sha256, classification_basis, catalog_sha256, spec_json,
               values_sha256, blockers_json
        FROM research_feature_versions WHERE feature_version = ?
    """, [version]).fetchone()
    if row is None or row[0] not in rf.SEALED_STATUSES:
        raise CompositeError(f"constituent feature version {version!r} is absent or not sealed")
    spec = json.loads(row[6])
    if spec.get("store_schema") != rf.FEATURE_SCHEMA_VERSION or spec.get("query_version") != rf.QUERY_VERSION:
        raise CompositeError(f"constituent version {version} predates feature store schema "
                             f"{rf.FEATURE_SCHEMA_VERSION} / {rf.QUERY_VERSION}")
    if spec.get("kind") == COMPOSITE_CLASS:
        raise CompositeError(f"{version} is already a composite version: composites never seed composites")
    return {"status": row[0], "basis": row[1], "panel_run_id": row[2], "panel_sha256": row[3],
            "classification_basis": row[4], "catalog_sha256": row[5], "spec": spec, "values_sha256": row[7],
            "blockers": json.loads(row[8] or "[]")}


def _standardization(spec: Mapping[str, Any]) -> rf.StandardizationPolicy:
    return rf.StandardizationPolicy(tuple(float(v) for v in spec["winsor_limits"]), int(spec["min_names"]),
                                    int(spec["industry_min_group_size"]), float(spec["neutral_min_coverage"]),
                                    str(spec["taxonomy_code"]))


def _columns(con: duckdb.DuckDBPyConnection, table: str) -> list[str]:
    return [r[0] for r in con.execute("SELECT column_name FROM duckdb_columns() WHERE database_name = "
                                      "current_database() AND schema_name = 'main' AND table_name = ? "
                                      "ORDER BY column_index", [table]).fetchall()]


def _composite_inputs(definition: CompositeDefinition) -> dict[str, Any]:
    return _clean({"universe_scope": rf.UNIVERSE_SCOPE_ALL_LINES, "level": definition.level,
                   "weighting": definition.weighting, "anomaly_class": definition.anomaly_class,
                   "members": [[m, w] for m, w, _, _ in definition.members]})


def write_composite_version(store: ResearchStore, result: BuildForwardResult, constituent_version: str, *,
                            batches: Iterable[tuple[np.ndarray, Mapping[str, pd.DataFrame], Any]] | None,
                            security_ids: Sequence[str], formation_dates: Mapping[int, dt.date]) -> str:
    """The constituent version plus the emitted composites, as one new R2b feature version.

    Every row of ``constituent_version`` is copied (the full candidate family, so one R3b
    run grades composites within it: I1); ``batches`` are :func:`materialize` output on that
    version's basis (None for an ``untestable_strict`` version: the composites are then
    untestable too). Composites rank every eligible line with enough constituents
    (``valid_primary_and_unlinked_lines``); each variant comes from R2b's standardization
    under the version's policy and covariates (unlinked lines have none, so neutral
    variants leave them out and are labeled ``survivor_conditioned_linked_only``). The
    version passes :func:`atx_db.research.features.validate_feature_version`; its id is a
    content hash and an identical sealed version is reused. The version's blockers carry
    ``composite_post_selection:<ledger id>``.
    """
    con = store.con
    with store.transaction():
        rf.ensure_feature_schema(con)
    base = _constituent_version(con, constituent_version)
    untestable = base["status"] == rf.STATUS_UNTESTABLE
    if untestable != (batches is None):
        raise CompositeError("composite batches are required exactly when the constituent version is sealed")
    standardization = _standardization(base["spec"])
    panel = base["panel_run_id"]
    calendar = con.execute("SELECT formation_date, cutoff, eligible_members FROM research_panel_calendar "
                           "WHERE run_id = ? AND status = ? ORDER BY formation_date", [panel, CALENDAR_FORMED]).fetchall()
    formations = [row[0] for row in calendar]
    eligible = {row[0]: int(row[2] or 0) for row in calendar}
    lines: dict[dt.date, dict[str, int]] = {}
    for day, basis_name, n in con.execute(f"""
        SELECT k.formation_date, {rf._expected_owner_basis_sql("k")} AS owner, count(*) FROM research_panel_cohort k
        WHERE k.run_id = ? GROUP BY ALL
    """, [panel]).fetchall():
        if basis_name is not None:
            lines.setdefault(day, {})[str(basis_name)] = int(n)
    emitted = sorted(result.emitted, key=lambda d: d.composite_id)
    existing_ids = {r[0] for r in con.execute("SELECT feature_id FROM research_feature_catalog WHERE feature_version = ?",
                                              [constituent_version]).fetchall()}
    if existing_ids & {d.composite_id for d in emitted}:
        raise CompositeError(f"{constituent_version} already has features named like the composites")
    features = [*base["spec"].get("features", []),
                *([d.composite_id, "planned", None, _composite_inputs(d), list(rf.VARIANTS)] for d in emitted)]
    ledger_stamp = f"{POST_SELECTION_STAMP}:{result.ledger_id}"
    spec = _clean({**base["spec"], "kind": COMPOSITE_CLASS, "composite_version": COMPOSITE_VERSION,
                   "constituent_feature_version": constituent_version, "constituent_values_sha256": base["values_sha256"],
                   "composite_policy": result.manifest["policy"], "build_sha256": result.sha256,
                   "holdout_key": result.holdout_key, "post_selection": ledger_stamp,
                   "orientation": dict(sorted(result.orientation.items())), "features": features})
    spec_json = _canonical(spec)
    inputs = {"constituent_version": constituent_version, "constituent_values_sha256": base["values_sha256"],
              "build_sha256": result.sha256,
              "composites": {c["composite_id"]: c["values_sha256"] for c in result.manifest["composites"]}}
    spec_sha, code_sha, inputs_json = _sha(spec_json), code_sha256(), _canonical(inputs)
    version = "cmp_" + _sha(_canonical({"spec_sha256": spec_sha, "inputs_sha256": _sha(inputs_json)}))[:40]
    existing = con.execute("SELECT status FROM research_feature_versions WHERE feature_version = ?",
                           [version]).fetchone()
    if existing is not None and existing[0] in rf.SEALED_STATUSES:
        return version
    common = {"source_kind": COMPOSITE_CLASS, "anomaly_class": COMPOSITE_CLASS, "hypothesis_family": COMPOSITE_CLASS,
              "expected_sign": 1, "orientation_sign": 1, "preferred_transform": "rank_normal",
              "preferred_variant": "rank_normal", "domain": "none",
              "caveat_codes": _canonical([*COMPOSITE_CAVEATS, ledger_stamp]), "admission": "eligible",
              "is_control": False, "variants_json": _canonical(list(rf.VARIANTS)),
              "universe_scope": rf.UNIVERSE_SCOPE_ALL_LINES}
    digests = {c["composite_id"]: c["values_sha256"] for c in result.manifest["composites"]}
    with store.transaction():
        for table in ("research_feature_matrix", "research_feature_dates", "research_feature_owner_basis",
                      "research_feature_context", "research_feature_catalog", "research_feature_versions"):
            con.execute(f"DELETE FROM {table} WHERE feature_version = ?", [version])  # a failed earlier attempt
        con.execute("""
            INSERT INTO research_feature_versions (feature_version, status, basis, panel_run_id, panel_sha256,
                classification_basis, query_version, universe_rule, spec_json, spec_sha256, code_sha256,
                catalog_sha256, inputs_json, inputs_sha256, blockers_json, created_at)
            VALUES (?, 'building', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '[]', ?)
        """, [version, base["basis"], panel, base["panel_sha256"], base["classification_basis"], rf.QUERY_VERSION,
              rf.UNIVERSE_RULE, spec_json, spec_sha, code_sha, base["catalog_sha256"], inputs_json, _sha(inputs_json),
              _now()])
        for table in ("research_feature_matrix", "research_feature_dates", "research_feature_owner_basis",
                      "research_feature_context", "research_feature_catalog"):
            names = ", ".join(c for c in _columns(con, table) if c != "feature_version")
            con.execute(f"INSERT INTO {table} (feature_version, {names}) SELECT ?, {names} FROM {table} "
                        "WHERE feature_version = ?", [version, constituent_version])
        if untestable:
            rows = [{"feature_version": version, "formation_date": day, "feature_id": d.composite_id,
                     "variant": variant, "date_status": rf.DATE_EMPTY, "eligible_members": eligible[day],
                     "universe_names": 0, "valid_names": 0, "coverage_fraction": None, "candidate_names": 0,
                     "in_domain_names": 0, "covariate_names": None, "covariate_coverage": None,
                     "reasons_json": _canonical({}), "unlinked_excluded": None, "sample_conditioning": None}
                    for d in emitted for day in formations for variant in rf.VARIANTS]
            rf._insert_frame(con, "research_feature_dates", pd.DataFrame(rows, columns=list(rf._DATE_COLUMNS)),
                             rf._DATE_CASTS)
            _insert_catalog(con, version, emitted, common, rf.FEATURE_UNTESTABLE, "constituent_untestable_strict", {})
            status = rf.STATUS_UNTESTABLE
        else:
            _write_composites(con, version, emitted, batches, security_ids, formation_dates, formations, eligible,
                              lines, standardization, common, digests)
            status = rf.STATUS_SEALED
        conditioned = int(con.execute("SELECT count(*) FROM research_feature_dates WHERE feature_version = ? AND "
                                      "sample_conditioning IS NOT NULL", [version]).fetchone()[0])
        blockers = [b for b in base["blockers"] if not b.startswith(f"{rf.NEUTRAL_CONDITIONING_BLOCKER}:")]
        blockers += ["composite_feature_version", ledger_stamp]
        if conditioned:
            blockers.append(f"{rf.NEUTRAL_CONDITIONING_BLOCKER}:{conditioned}")
        values_sha, _, _ = rf._combined_digest(con, version, rf._context_digest(con, version, formations,
                                                                                 rf._DIGEST_CHUNK))
        con.execute("UPDATE research_feature_versions SET status = ?, values_sha256 = ?, blockers_json = ?, "
                    "diagnostic_json = ?, finished_at = ? WHERE feature_version = ?",
                    [status, values_sha, _canonical(sorted(set(blockers))),
                     _canonical({"composites": [d.composite_id for d in emitted], "formations": len(formations),
                                 "survivor_conditioned_date_rows": conditioned}), _now(), version])
    return version


def _insert_catalog(con: duckdb.DuckDBPyConnection, version: str, definitions: Sequence[CompositeDefinition],
                    common: Mapping[str, Any], status: str, reason: str | None,
                    extra: Mapping[str, Mapping[str, Any]]) -> None:
    rows = []
    for d in definitions:
        item = {"feature_version": version, "feature_id": d.composite_id, **common,
                "inputs_json": _canonical(_composite_inputs(d)), "status": status, "status_reason": reason,
                "value_rows": None, "in_domain_rows": None, "reasons_json": None, "values_sha256": None,
                **extra.get(d.composite_id, {})}
        rows.append([item[name] for name in rf._CATALOG_COLUMNS])
    con.executemany(f"INSERT INTO research_feature_catalog ({','.join(rf._CATALOG_COLUMNS)}) "
                    f"VALUES ({','.join('?' * len(rf._CATALOG_COLUMNS))})", rows)


def _write_composites(con: duckdb.DuckDBPyConnection, version: str, emitted: Sequence[CompositeDefinition],
                      batches: Iterable[tuple[np.ndarray, Mapping[str, pd.DataFrame], Any]],
                      security_ids: Sequence[str], formation_dates: Mapping[int, dt.date],
                      formations: Sequence[dt.date], eligible: Mapping[dt.date, int],
                      lines: Mapping[dt.date, Mapping[str, int]], standardization: rf.StandardizationPolicy,
                      common: Mapping[str, Any], digests: Mapping[str, str]) -> None:
    ids = [d.composite_id for d in emitted]
    stats: dict[str, dict[tuple[dt.date, str], dict[str, Any]]] = {cid: {} for cid in ids}
    candidates: dict[str, dict[dt.date, int]] = {cid: {} for cid in ids}
    owners: dict[str, dict[tuple[dt.date, str], tuple[Any, ...]]] = {cid: {} for cid in ids}
    excluded: dict[str, dict[tuple[dt.date, str], int]] = {cid: {} for cid in ids}
    totals = dict.fromkeys(ids, 0)
    check = {cid: hashlib.sha256() for cid in ids}
    for _, frames, _ in batches:
        for cid in ids:
            frame = frames[cid]
            for column in ("month_index", "security", "value", "clock_us", "unlinked"):
                check[cid].update(np.ascontiguousarray(frame[column].to_numpy()).tobytes())
            if not len(frame):
                continue
            days = [formation_dates[int(m)] for m in frame["month_index"].to_numpy()]
            rows = pd.DataFrame({
                "formation_date": days,
                "security_id": [security_ids[int(code)] for code in frame["security"].to_numpy()],
                "owner_basis": np.where(frame["unlinked"].to_numpy(dtype=bool), rf.OWNER_BASIS_UNLINKED,
                                        rf.OWNER_BASIS_LINKED),
                "raw_value": frame["value"].to_numpy(dtype=float),
                "available_at": [pd.Timestamp(int(c), unit="us").to_pydatetime() for c in frame["clock_us"].to_numpy()]})
            con.register("_cmp_chunk", rows)
            try:
                staged = con.execute("""
                    SELECT r.formation_date, r.security_id, r.owner_basis, r.raw_value, r.available_at,
                           CASE WHEN r.owner_basis = ? THEN x.industry_group END AS industry_group,
                           CASE WHEN r.owner_basis = ? THEN x.log_size END AS log_size
                    FROM _cmp_chunk r
                    LEFT JOIN research_feature_context x
                      ON x.feature_version = ? AND x.formation_date = r.formation_date AND x.security_id = r.security_id
                    ORDER BY r.formation_date, r.security_id
                """, [rf.OWNER_BASIS_LINKED, rf.OWNER_BASIS_LINKED, version]).df()
            finally:
                con.unregister("_cmp_chunk")
            standardized, part = standardize_composite(staged, cid, standardization)
            stats[cid].update(part)
            for day, n in standardized.groupby(pd.to_datetime(standardized["formation_date"]).dt.date).size().items():
                candidates[cid][day] = int(n)
            owners[cid].update(rf._owner_basis_stats(standardized))
            excluded[cid].update(rf._unlinked_excluded(standardized, rf.VARIANTS))
            totals[cid] += len(standardized)
            matrix = pd.DataFrame({
                "feature_version": version, "formation_date": standardized["formation_date"],
                "security_id": standardized["security_id"], "feature_id": cid,
                "owner_basis": standardized["owner_basis"], "expected_sign": 1,
                "available_at": standardized["available_at"],
                "raw_value": standardized["raw_value"].to_numpy(dtype=float), "domain_status": rf.IN_DOMAIN,
                "age_days": None, **{v: standardized[v].to_numpy(dtype=float) for v in rf.VARIANTS}})
            rf._insert_frame(con, "research_feature_matrix", matrix, rf._MATRIX_CASTS)
    for cid in ids:
        if check[cid].hexdigest() != digests.get(cid):
            raise CompositeError(f"{cid}: materialized values differ from the build's digest (inputs changed)")
        date_rows, reasons_total = [], {}
        for day in formations:
            counts = lines.get(day, {})
            names = counts.get(rf.OWNER_BASIS_LINKED, 0) + counts.get(rf.OWNER_BASIS_UNLINKED, 0)
            members, valued = eligible.get(day, 0), candidates[cid].get(day, 0)
            reasons = {key: n for key, n in (("in_domain", valued), ("composite_not_valued", names - valued),
                                             ("not_a_ranked_line", members - names)) if n}
            for key, n in reasons.items():
                reasons_total[key] = reasons_total.get(key, 0) + n
            for variant in rf.VARIANTS:
                item = stats[cid].get((day, variant)) or {"date_status": rf.DATE_THIN, "valid_names": 0,
                                                          "in_domain_names": 0, "covariate_names": None,
                                                          "covariate_coverage": None}
                left_out = excluded[cid].get((day, variant), 0)
                date_rows.append({
                    "feature_version": version, "formation_date": day, "feature_id": cid, "variant": variant,
                    "date_status": item["date_status"], "eligible_members": members, "universe_names": names,
                    "valid_names": int(item["valid_names"]),
                    "coverage_fraction": (item["valid_names"] / names) if names else None, "candidate_names": valued,
                    "in_domain_names": int(item["in_domain_names"]), "covariate_names": item["covariate_names"],
                    "covariate_coverage": item["covariate_coverage"],
                    "reasons_json": _canonical(dict(sorted(reasons.items()))), "unlinked_excluded": left_out,
                    "sample_conditioning": rf.CONDITIONING_LINKED_ONLY
                    if left_out and item["date_status"] == rf.DATE_FORMED else None})
        rf._insert_frame(con, "research_feature_dates", pd.DataFrame(date_rows, columns=list(rf._DATE_COLUMNS)),
                         rf._DATE_CASTS)
        empty = (0, None, None, 0, None, None)
        owner_rows = [[version, day, cid, owner, *owners[cid].get((day, owner), empty)]
                      for day in formations for owner in rf.OWNER_BASES]
        rf._insert_frame(con, "research_feature_owner_basis",
                         pd.DataFrame(owner_rows, columns=list(rf._OWNER_BASIS_COLUMNS)).astype(
                             {name: "float64" for name in rf._OWNER_MOMENTS}), rf._OWNER_BASIS_CASTS)
        digest = rf._feature_digest(con, version, cid, formations, rf._DIGEST_CHUNK)
        _insert_catalog(con, version, [d for d in emitted if d.composite_id == cid], common, rf.FEATURE_BUILT, None,
                        {cid: {"value_rows": totals[cid], "in_domain_rows": totals[cid],
                               "reasons_json": _canonical(dict(sorted(reasons_total.items()))),
                               "values_sha256": digest}})


# ---------------------------------------------------------------------------
# Store orchestration: select, annotate, build, write, evaluate once, grade
# ---------------------------------------------------------------------------

def run_build_forward(store: ResearchStore, ledger_id: str, *, factor_run_id: str,
                      policy: CompositePolicy | None = None, allow_post_hoc_policy: bool = False) -> dict[str, Any]:
    """Build forward from a stored R4 ledger and evaluate the composites once, in the full family.

    Reads the ledger, its R3b run (split, feature versions, label cutoff, creation time),
    the composite basis through R3b's store adapter (calendar, labels) with constituent
    values streamed per batch, the R3b selection series and the P4 monthly factor run;
    stores the build (one evaluation per split and basis), writes one composite version
    per basis (the source version plus the composites) and runs one R3b evaluation of them
    with the source run's spec. :func:`qualify_composites` then grades that run with R4.
    """
    policy = policy or load_composite_policy()
    con = store.con
    ledger = load_ledger(con, ledger_id)
    run = con.execute("SELECT spec_json, status, created_at FROM research_eval_runs WHERE run_id = ?",
                      [ledger.run_id]).fetchone()
    if run is None or run[1] != "complete":
        raise CompositeError(f"R3b run {ledger.run_id} of ledger {ledger_id} is absent or not sealed")
    source_spec = ev.spec_from_payload(json.loads(run[0]), ledger.run_id)
    if source_spec.split is None:
        raise CompositeError("the source run has no frozen split")
    created = pd.Timestamp(run[2]).isoformat() if run[2] is not None else None
    bases = {str(b): str(v) for v, b in con.execute(
        "SELECT feature_version, basis FROM research_feature_versions WHERE list_contains(?::VARCHAR[], feature_version)",
        [list(source_spec.feature_versions)]).fetchall()}
    if policy.basis not in bases:
        raise CompositeError(f"the source run has no {policy.basis} feature version")

    for basis, version in bases.items():
        status = con.execute("SELECT status FROM research_feature_versions WHERE feature_version = ?",
                             [version]).fetchone()[0]
        if basis != policy.basis and status != rf.STATUS_UNTESTABLE:
            raise CompositeError(f"composites on a second testable basis ({basis}) need their own build: v2 builds "
                                 f"on {policy.basis} (the strict basis is untestable today)")
    inputs = ev.open_basis_inputs(store, bases[policy.basis], source_spec)
    # R3b's security codes, read right away (the next basis open re-registers R3b's key tables).
    security_ids = [str(r[0]) for r in con.execute("SELECT security_id FROM _ev_securities ORDER BY code").fetchall()]
    source = StoreSource(con, bases[policy.basis], policy.constituent_variant, inputs.calendar, security_ids)
    series = load_selection_series(con, ledger.run_id, policy,
                                   [s.feature_id for s in select_signals(ledger, policy) if s.selected])
    factors = fr.load_factor_returns(store, factor_run_id, factors=policy.span_factors)
    digest = con.execute("SELECT results_sha256 FROM research_factor_runs WHERE run_id = ?", [factor_run_id]).fetchone()
    result = build_forward(ledger, inputs, source_spec.split, series, factors, policy, source=source,
                           factor_source={"kind": "p4_run", "run_id": factor_run_id,
                                          "results_sha256": None if digest is None else digest[0]},
                           policy_registered_at=composite_policy_registered_at(con, policy),
                           source_created_at=created, allow_post_hoc_policy=allow_post_hoc_policy)
    outcome = persist_build(con, result)
    stored = con.execute("SELECT holdout_run_id, composite_versions_json FROM research_composite_builds "
                         "WHERE holdout_key = ?", [result.holdout_key]).fetchone()
    if stored[0] is not None:
        return {"ledger_id": ledger_id, "holdout_key": result.holdout_key, "build": outcome,
                "build_sha256": result.sha256, "composite_versions": json.loads(stored[1]),
                "holdout_run_id": stored[0], "evaluated": False}
    versions = []
    for basis, version in sorted(bases.items()):
        if basis == policy.basis:
            batches = materialize(source, inputs.calendar, result.definitions, result.orientation, policy,
                                  int(inputs.security_count))
            versions.append(write_composite_version(store, result, version, batches=batches,
                                                    security_ids=security_ids, formation_dates=source.date_of))
        else:  # untestable_strict (checked above): the composites are untestable there too
            versions.append(write_composite_version(store, result, version, batches=None, security_ids=[],
                                                    formation_dates={}))
    run_id = f"p5_{result.sha256[:16]}"
    spec = replace(source_spec, run_id=run_id, feature_versions=tuple(versions), features=None, variants=None)
    evaluation = ev.run_evaluation(store, spec)
    with store.transaction():
        record_holdout_evaluation(con, result.holdout_key, versions, run_id)
    return {"ledger_id": ledger_id, "holdout_key": result.holdout_key, "build": outcome,
            "build_sha256": result.sha256, "composite_versions": versions, "holdout_run_id": run_id,
            "evaluated": True, "family_complete": evaluation.family_complete,
            "n_trials": evaluation.family.get("n_trials"), "blockers": list(evaluation.blockers)}


def qualify_composites(con: duckdb.DuckDBPyConnection, key: str, r4_policy: rq.QualificationPolicy,
                       catalog: Iterable[Any]) -> rq.QualificationLedger:
    """Grade the window's one composite run under R4, within the full family (source catalog +
    composites), every row stamped ``composite_post_selection`` and the constituent ledger id."""
    row = con.execute("SELECT holdout_run_id, ledger_id, build_sha256 FROM research_composite_builds "
                      "WHERE holdout_key = ?", [key]).fetchone()
    if row is None or row[0] is None:
        raise CompositeError(f"holdout window {key} has no evaluated composite build")
    definitions = [CompositeDefinition(cid, level, weighting, cls, (), (), bool(emitted))
                   for cid, level, weighting, cls, emitted in con.execute(
                       "SELECT DISTINCT composite_id, level, weighting, anomaly_class, emitted "
                       "FROM research_composite_members WHERE build_sha256 = ? ORDER BY composite_id",
                       [row[2]]).fetchall()]
    return rq.qualify_run(con, row[0], r4_policy, catalog=stamped_catalog_entries(catalog, definitions, row[1]))


# ---------------------------------------------------------------------------
# Generated documentation
# ---------------------------------------------------------------------------

def _fmt(value: Any, digits: int = 3) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}" if math.isfinite(value) else "-"
    return str(value)


def render_composites_markdown(policy: CompositePolicy, result: BuildForwardResult | None = None) -> str:
    """``docs/research/COMPOSITES.md``: the frozen policy, and a build when given."""
    content = policy.content
    lines = [
        "# Build-forward composites (P5)",
        "",
        "Generated by `scripts/research_composite.py render-doc` from the frozen composite policy"
        + (" and a stored build." if result is not None else "; no real-data build exists yet (heavy run after RR5)."),
        "Research only: nothing here is release eligible.",
        "",
        "## Frozen policy",
        "",
        f"- Version `{policy.version}` (supersedes `{content['supersedes']}`), content sha256 `{policy.sha256}`. "
        "Register it (`research_composite.py freeze`) before the RR4 real-data run: a build whose source run was "
        "created before the registration is refused (RX7).",
        f"- Authority: {content['authority']}.",
        f"- Selection (train + validation only): the R4 row on the `{policy.basis}` basis passes "
        f"{', '.join(f'`{g}`' for g in policy.selection_gates)}; a basis where the signal is significant with the "
        "wrong sign vetoes it. R4's holdout gates and final status are never read (a composite of holdout-qualified "
        "signals would be confirmed by the holdout that selected it).",
        f"- Constituent variant `{policy.constituent_variant}` (sign-oriented); fit sample: train + validation "
        f"formations whose {policy.fit_horizon}-month label window ends before the holdout; any holdout row refuses "
        "the fit.",
        "",
        "## Redundancy annotation (never alters an R4 status)",
        "",
        "- Rank correlation: Pearson of within-formation average ranks on names valued on both (at least "
        f"{policy.min_names} per formation), mean over at least {policy.min_formations} fit formations.",
        f"- Clusters: complete linkage at the declared threshold rho >= {policy.rho_threshold}; representative = "
        "largest |ic_z| on the composite basis.",
        f"- Spanning: R3b `{policy.span_series}` at {policy.span_horizon} month (selection formations) on P4 monthly "
        f"{', '.join(f'`{f}`' for f in policy.span_factors)} over the monthly period grid (a missing month is a HAC "
        f"gap); EWC fixed-b robust alpha z below {policy.alpha_min_z} (at least {policy.span_min_periods} periods) "
        "is `spanned_by_known_factors` (a significantly negative alpha is noted `negative_alpha_after_factors`); a "
        "result P4 marks not significance-claimable is never called spanned.",
        "- `redundancy_status`: `duplicate_of:<representative>` > `spanned_by_known_factors` > `novel`.",
        "",
        "## Composites",
        "",
        "- Constituents: the cluster representatives of the selected signals (a near-copy never votes twice).",
        "- Per formation each constituent is a cross-sectional z; a class composite is sum(w z) / sqrt(w' R w) over "
        "the constituents valued for a name (R: their fit-sample correlation), so a name with missing constituents "
        f"is not over-dispersed; at least {policy.min_constituent_share:.0%} of the constituents are required; "
        "re-standardized. The across-class composite combines class composites the same way.",
        f"- Weightings: {', '.join(f'`{w}`' for w in policy.weightings)}; `ic_shrunk` = (1 - s) x positive-IC share + "
        f"s / k with s = {policy.ic_shrinkage} (IC: mean monthly rank IC at {policy.fit_horizon} months on the fit "
        "sample).",
        f"- Emitted: within-class composites of classes with >= {policy.min_class_constituents} constituents "
        f"(`cmp_<class>_eq` / `_icw`), across classes when >= {policy.min_across_inputs} classes "
        "(`cmp_classes_eq` / `_icw`).",
        "- Universe (R2b I1): every eligible line with enough constituent values, unlinked price lines included "
        "(`valid_primary_and_unlinked_lines`); neutral variants that leave unlinked lines out carry "
        "`survivor_conditioned_linked_only` and `unlinked_excluded`; unlinked names dropped for too few "
        "constituents are counted (`composite_drops_unlinked_lines`).",
        "- Storage and grading: a composite feature version is the source R2b version plus the composites (store "
        "schema 3, class `composite`, constituents and weights in the catalog row). One R3b run evaluates the full "
        "family (every candidate signal plus the composites: BH, Holm and DSR `n_trials` count them all); R4 grades "
        "it with every row stamped `composite_post_selection` and the constituent ledger id. Such a ledger never "
        "seeds another build. The FM size/value/momentum controls come from the copied source rows.",
        "- One holdout evaluation per (frozen split, basis): an unevaluated build may be replaced (retry after an "
        "abort); after the evaluation any other build is refused.",
        "- Memory: constituents are streamed per batch of formations, never whole histories. Measured bound of "
        "the build (selection, correlations, ICs, fits, materialization of 18 composites): 420 MB peak at 40 "
        "signals x 5,000 names x 170 formations (20,000 security codes) with the CLI's 128 MB DuckDB limit. The "
        "full-family R3b evaluation that follows is R3b's own RR-scale run.",
    ]
    if result is not None:
        manifest = result.manifest
        lines += ["", "## Build", "",
                  f"- Ledger `{result.ledger_id}`, holdout window `{result.holdout_key}`, build sha256 "
                  f"`{result.sha256}`, fit formations {manifest['fit_sample']['formations']}.",
                  f"- Blockers: {', '.join(f'`{b}`' for b in manifest['blockers']) or 'none'}.", "",
                  "| signal | R4 status | class | redundancy | cluster | rho to rep | span alpha z | span R2 |",
                  "|---|---|---|---|---|---|---|---|"]
        for row in result.redundancy:
            lines.append(f"| `{row['feature_id']}` | `{row['r4_status']}` | {row['anomaly_class']} | "
                         f"`{row['redundancy_status']}` | {row['cluster_id']} ({row['cluster_size']}) | "
                         f"{_fmt(row['rho_to_representative'])} | {_fmt(row['span_alpha_z'], 2)} | "
                         f"{_fmt(row['span_r2'])} |")
        lines += ["", "| composite | level | weighting | members (weight, fit IC) |", "|---|---|---|---|"]
        for definition in result.emitted:
            members = ", ".join(f"`{m}` {w:.3f} ({_fmt(ic)})" for m, w, ic, _ in definition.members)
            lines.append(f"| `{definition.composite_id}` | {definition.level} | {definition.weighting} | {members} |")
    return "\n".join(lines) + "\n"


__all__ = [
    "COMPOSITE_CLASS",
    "COMPOSITE_VERSION",
    "DUPLICATE_PREFIX",
    "FROZEN_POLICY_SHA256",
    "NOVEL",
    "POLICY_HISTORY_PATHS",
    "POLICY_PATH",
    "POST_SELECTION_STAMP",
    "SELECTION_GATES",
    "SPANNED",
    "BuildForwardResult",
    "CompositeDefinition",
    "CompositeError",
    "CompositeLeakError",
    "CompositePolicy",
    "FeatureDataSource",
    "LedgerSource",
    "Rows",
    "SelectedSignal",
    "StoreSource",
    "build_forward",
    "code_sha256",
    "composite_catalog",
    "composite_feature_data",
    "composite_policy_registered_at",
    "ensure_composite_schema",
    "evaluate_composites",
    "fit_sample",
    "fit_weights",
    "holdout_key",
    "ledger_source",
    "load_composite_policy",
    "load_ledger",
    "load_selection_series",
    "materialize",
    "persist_build",
    "policy_sha256",
    "qualify_composites",
    "record_holdout_evaluation",
    "register_composite_policy",
    "render_composites_markdown",
    "run_build_forward",
    "select_signals",
    "stamped_catalog_entries",
    "standardize_composite",
    "superseded_policy_versions",
    "write_composite_version",
]
