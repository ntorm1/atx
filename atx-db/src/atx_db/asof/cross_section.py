"""Universe-scoped point-in-time cross-section: "field X for every member of U as of T" (task P7).

``cross_section_asof(as_of_ts, fields, db_path, basis=...)`` returns one row per
(member of the tier-1 universe active at ``as_of_ts``, requested field) with the
value, its ``value_status``, lineage status, clocks, staleness and the identity /
universe / availability bases every value was produced under.

Clock
-----
``as_of_ts`` (timezone-aware UTC) is the knowledge cutoff. The screen session is
the latest observed market session whose rows were knowable by the cutoff (the
FQ1/R2a session rule: ``market_daily_metrics`` rows clocked at or before the
session's 22:00 UTC and at or before the cutoff), looked back at most
``session_lookback_days``. Every input is filtered by the cutoff; nothing filed,
restated or listed after it can appear. At a month-end session with the cutoff at
22:00 UTC the screen is exactly one formation of the R2a monthly panel.

Reuse, not a fork
-----------------
Membership, identity, derived and market fields are computed by the R2a panel
builder itself on a one-date calendar (``(screen session, cutoff)``): the shared
FQ1/R2a cohort builder (``fundamental_signal_research.stage_cohort``), the
reconstructed owner bridge, the primary-line rule, the FQ1 state selection, the
set-based lineage prover and the panel's value and market rules. Those rows are
therefore byte-identical to the R2a panel at the same formation (the per-field
``r2a_field_digests`` equal the panel's ``research_panel_coverage.values_sha256``).
The builder runs in a private scratch research store (:class:`ScreenStore`) that
attaches the warehouse ``READ_ONLY``; the scratch file is deleted afterwards.

Fields
------
``derived:<metric_code>`` (q/ttm/avg2 seed metrics) and ``market:<metric_code>``
(daily ``market_daily_metrics`` columns) -- a bare metric code resolves through the
seed registry -- follow R2a exactly, with one integrity guard: a market value is
never revived from an older revision when the newest revision visible at the
cutoff carries NULL (``market_null_revision_guard_rows``). ``item:<code>:<basis>``
reads ``fundamental_standardized``: per owner CIK the latest visible period, then
availability, then source/rule/basis/id (the derived engine's item order), ranked
on the complete state, so a newer NULL state (e.g. ``reported_eps_conflict_v1``)
is reported as ``null_state`` and never revives an earlier value.
``feature:<feature_id>[:<variant>]`` reads a sealed R2b feature version from a
research store (attached ``READ_ONLY``): the latest feature formation whose 22:00
UTC cutoff is at or before ``as_of_ts``; its age is the staleness.

Grain and labels
----------------
One row per (member visible at the session and cutoff, field). Members that the
basis does not admit (``not_common``, ``overlapping_membership``) keep a NULL row
with that status. Owner-scoped fields attach to the issuer's primary line; every
other line carries its exclusion (``missing_owner_link``,
``secondary_issuer_line``, ...). Size rows carry ``shares_source`` /
``size_status`` (only DEI counts are ``verified_dei_shares``). Reconstructed rows
carry ``identity_basis='current_ticker_unverified'`` and
``universe_basis='us_listed_reconstructed_v1'`` and are research/inspection input,
never certifiable history; blockers name owner-link attrition, unverified size
and research-only feature rows. ``staleness_days`` is the distance from each
value's ``reference_date`` (fiscal/period end, market session or feature
formation) to the as-of date; ``stale_current_anchor`` rows keep their value for
diagnostics only.

Bounds
------
At most 64 fields; members x fields may not exceed ``max_rows`` (checked before
any value is computed); DuckDB runs at ``memory_limit`` (default 256MB) and 1-2
threads with external access disabled after the attachments.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import tempfile
import time
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import pandas as pd

from ..connection import DEFAULT_DB_PATH

QUERY_VERSION = "pit-cross-section-v1"
SCREEN_RUN_ID = "pit_screen"
BASIS_STRICT = "strict"
BASIS_RECONSTRUCTED = "reconstructed"
BASES = (BASIS_STRICT, BASIS_RECONSTRUCTED)
FIELD_DERIVED = "derived"
FIELD_MARKET = "market"
FIELD_ITEM = "item"
FIELD_FEATURE = "feature"
FIELD_KINDS = (FIELD_DERIVED, FIELD_MARKET, FIELD_ITEM, FIELD_FEATURE)
ITEM_BASES = ("quarterly", "annual", "ttm", "instant")
FEATURE_VARIANTS = ("signed_raw", "winsor", "zscore", "rank_normal", "industry_neutral", "size_neutral")
DEFAULT_FEATURE_VARIANT = "rank_normal"
FEATURE_SEALED_STATUSES = ("sealed", "untestable_strict")
FEATURE_AVAILABILITY_BASIS = "research_feature_formation_22h"
FEATURE_SCOPE = "research_feature"
ITEM_SCOPE = "owner"
DECISION_HOUR = 22
MAX_FIELDS = 64
DEFAULT_MAX_ROWS = 1_000_000
MAX_ROWS_LIMIT = 5_000_000
DEFAULT_SESSION_LOOKBACK_DAYS = 10
WAREHOUSE_ALIAS = "wh"
RESEARCH_ALIAS = "rs"
# Reasons whose value is point-in-time safe to publish (R2a VALUE_BEARING_REASONS).
VALUE_BEARING_REASONS = ("valid", "stale_current_anchor")
SCREEN_BLOCKERS = (
    "modeled_filing_availability_not_exact_delivery_vintage",
    "source_backfill_and_historical_membership_may_be_incomplete",
    "screen_session_from_observed_market_rows_not_a_certified_calendar",
    "inspection_only_not_release_eligible",
)

_CODE = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_FEATURE_ID = re.compile(r"^[a-z][a-z0-9_]{0,95}$")
_MEMORY = re.compile(r"^[1-9][0-9]{0,5}(MB|GB)$")

# Output grain: R2a's value columns (identical names and meaning) plus the screen's.
_VALUE_TYPES = (
    ("formation_date", "DATE"), ("security_id", "VARCHAR"), ("feature_id", "VARCHAR"),
    ("metric_code", "VARCHAR"), ("metric_window", "VARCHAR"), ("raw_value", "DOUBLE"),
    ("reason", "VARCHAR"), ("lineage_status", "VARCHAR"), ("available_at", "TIMESTAMP"),
    ("latest_input_clock", "TIMESTAMP"), ("period_end", "DATE"), ("fiscal_period_start", "DATE"),
    ("fiscal_period_end", "DATE"), ("value_origin", "VARCHAR"), ("age_days", "INTEGER"),
    ("max_age_days", "INTEGER"), ("owner_cik", "VARCHAR"), ("derived_value_id", "VARCHAR"),
    ("derived_owner_security_id", "VARCHAR"), ("lineage_digest", "VARCHAR"), ("identity_basis", "VARCHAR"),
    ("universe_basis", "VARCHAR"), ("availability_basis", "VARCHAR"), ("feature_scope", "VARCHAR"),
    ("shares_source", "VARCHAR"), ("size_status", "VARCHAR"),
    ("field_kind", "VARCHAR"), ("field_order", "INTEGER"), ("symbol", "VARCHAR"),
    ("security_type", "VARCHAR"), ("exchange_code", "VARCHAR"), ("membership_reason", "VARCHAR"),
    ("eligible", "BOOLEAN"), ("cohort_reason", "VARCHAR"), ("owner_link_reason", "VARCHAR"),
    ("primary_line", "BOOLEAN"), ("reference_date", "DATE"), ("source_ref", "VARCHAR"),
)
_OUTPUT_RENAMES = {"formation_date": "screen_date", "feature_id": "field_id", "raw_value": "value",
                   "reason": "value_status"}
OUTPUT_COLUMNS = (
    "as_of_ts", "screen_date", "field_id", "field_kind", "metric_code", "metric_window", "security_id", "symbol",
    "security_type", "exchange_code", "membership_reason", "eligible", "cohort_reason", "owner_link_reason",
    "primary_line", "owner_cik", "value", "value_status", "lineage_status", "available_at", "latest_input_clock",
    "reference_date", "staleness_days", "period_end", "fiscal_period_start", "fiscal_period_end", "value_origin",
    "age_days", "max_age_days", "identity_basis", "universe_basis", "availability_basis", "feature_scope",
    "shares_source", "size_status", "source_ref", "derived_value_id", "derived_owner_security_id",
    "lineage_digest",
)


class ScreenBoundExceeded(ValueError):
    """The requested screen exceeds its row bound; nothing was computed."""


# ---------------------------------------------------------------------------
# Fields
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class ScreenField:
    """One requested field. ``window`` is the derived window, ``daily`` for market
    fields, the item basis, or the feature variant (``None``: the catalog's
    preferred variant)."""

    kind: str
    code: str
    window: str | None = None

    @property
    def field_id(self) -> str:
        if self.kind in (FIELD_DERIVED, FIELD_MARKET):
            return self.code
        if self.kind == FIELD_ITEM:
            return f"item:{self.code}:{self.window}"
        return f"feature:{self.code}" + (f":{self.window}" if self.window else "")


def _registry() -> dict[str, str]:
    from .. import fundamental_signal_research as fsr
    from ..research import panel

    registry = {row.metric_code: row.window for row in fsr.default_derived_definitions()}
    registry.update({code: spec["metric_window"] for code, spec in panel.NATIVE_FEATURES.items()})
    return registry


def _item_keys() -> frozenset[tuple[str, str]]:
    from ..standardization import default_standardization_rules

    return frozenset((rule.canonical_code, rule.basis) for rule in default_standardization_rules())


def parse_screen_fields(specs: Iterable[str | ScreenField]) -> tuple[ScreenField, ...]:
    """Validate field specifications (text or :class:`ScreenField`) in request order.

    Text forms: ``<metric_code>``, ``derived:<code>``, ``market:<code>``,
    ``item:<canonical_code>:<basis>``, ``feature:<feature_id>[:<variant>]``.
    """
    registry = _registry()
    items: frozenset[tuple[str, str]] | None = None
    parsed: list[ScreenField] = []
    for spec in specs:
        if isinstance(spec, ScreenField):
            candidate = spec
        elif isinstance(spec, str):
            parts = spec.strip().split(":")
            if len(parts) == 1:
                window = registry.get(parts[0])
                if window is None:
                    raise ValueError(f"unknown metric field {spec!r}; use kind:code for items and features")
                candidate = ScreenField(FIELD_MARKET if window == "daily" else FIELD_DERIVED, parts[0], window)
            elif parts[0] in (FIELD_DERIVED, FIELD_MARKET) and len(parts) == 2:
                candidate = ScreenField(parts[0], parts[1], registry.get(parts[1]))
            elif parts[0] == FIELD_ITEM and len(parts) == 3:
                candidate = ScreenField(FIELD_ITEM, parts[1], parts[2])
            elif parts[0] == FIELD_FEATURE and len(parts) in (2, 3):
                candidate = ScreenField(FIELD_FEATURE, parts[1], parts[2] if len(parts) == 3 else None)
            else:
                raise ValueError(f"unsupported field specification {spec!r}")
        else:
            raise ValueError(f"unsupported field specification {spec!r}")
        kind, code, window = candidate.kind, candidate.code, candidate.window
        if kind in (FIELD_DERIVED, FIELD_MARKET):
            if not isinstance(code, str) or not _CODE.fullmatch(code):
                raise ValueError(f"invalid metric code {code!r}")
            expected = registry.get(code)
            if expected is None or (kind == FIELD_MARKET) != (expected == "daily") or window != expected:
                raise ValueError(f"{kind}:{code} is not a {kind} seed metric (registry window {expected!r})")
        elif kind == FIELD_ITEM:
            if not isinstance(code, str) or not _CODE.fullmatch(code) or window not in ITEM_BASES:
                raise ValueError(f"item fields need item:<canonical_code>:<{'|'.join(ITEM_BASES)}>, got {code!r}")
            items = _item_keys() if items is None else items
            if (code, window) not in items:
                raise ValueError(f"item:{code}:{window} has no standardization rule")
        elif kind == FIELD_FEATURE:
            if not isinstance(code, str) or not _FEATURE_ID.fullmatch(code):
                raise ValueError(f"invalid feature id {code!r}")
            if window is not None and window not in FEATURE_VARIANTS:
                raise ValueError(f"feature variant must be one of {FEATURE_VARIANTS}, got {window!r}")
        else:
            raise ValueError(f"field kind must be one of {FIELD_KINDS}")
        parsed.append(ScreenField(kind, code, window))
    ids = [item.field_id for item in parsed]
    codes = [item.code for item in parsed if item.kind in (FIELD_DERIVED, FIELD_MARKET)]
    if len(set(ids)) != len(ids) or len(set(codes)) != len(codes):
        raise ValueError("duplicate screen fields")
    if not 1 <= len(parsed) <= MAX_FIELDS:
        raise ValueError(f"a screen needs 1..{MAX_FIELDS} fields")
    return tuple(parsed)


def resolve_basis(basis: str | None = None, universe_id: str | None = None) -> tuple[str, str]:
    """(basis, universe_id) from either argument; they must agree when both are given."""
    from .. import fundamental_signal_research as fsr

    universes = {BASIS_STRICT: fsr.STRICT_UNIVERSE_ID, BASIS_RECONSTRUCTED: fsr.RECONSTRUCTED_UNIVERSE_ID}
    if basis is None and universe_id is None:
        basis = BASIS_RECONSTRUCTED
    if basis is not None and basis not in BASES:
        raise ValueError(f"basis must be one of {BASES}")
    if universe_id is not None:
        by_universe = {value: key for key, value in universes.items()}
        if universe_id not in by_universe:
            raise ValueError(f"universe_id must be one of {sorted(by_universe)} (tier-1 PIT universes)")
        if basis is not None and by_universe[universe_id] != basis:
            raise ValueError(f"universe {universe_id} is the {by_universe[universe_id]} basis, not {basis}")
        basis = by_universe[universe_id]
    return str(basis), universes[str(basis)]


# ---------------------------------------------------------------------------
# Scratch store
# ---------------------------------------------------------------------------

def _same_file(left: Path, right: Path) -> bool:
    return left == right or (left.exists() and right.exists() and left.samefile(right))


class ScreenStore:
    """Private scratch research store for one screen.

    The warehouse is attached ``READ_ONLY`` (verified) and, when given, a
    research store too (for ``feature:`` fields). Builder writes -- the R2a
    staging tables and lineage proofs -- land in a scratch DuckDB file in a
    private temporary directory that is removed on close. External access is
    disabled once everything is attached; spill is capped by ``spill_limit``.
    """

    def __init__(self, warehouse_path: Path | str = DEFAULT_DB_PATH, *, scratch_dir: Path | str | None = None,
                 memory_limit: str = "256MB", threads: int = 1, spill_limit: str | None = None,
                 research_db_path: Path | str | None = None) -> None:
        if not isinstance(memory_limit, str) or not _MEMORY.fullmatch(memory_limit):
            raise ValueError("memory_limit must look like '256MB' or '1GB'")
        if isinstance(threads, bool) or not isinstance(threads, int) or not 1 <= threads <= 2:
            raise ValueError("threads must be 1 or 2")
        if spill_limit is not None and not re.fullmatch(r"(0|[1-9][0-9]{0,5})(B|MB|GB)", spill_limit):
            raise ValueError("spill_limit must look like '0B', '512MB' or '2GB'")
        self.warehouse_path = Path(warehouse_path).resolve()
        self.research_db_path = None if research_db_path is None else Path(research_db_path).resolve()
        self.scratch_dir = None if scratch_dir is None else Path(scratch_dir)
        self.memory_limit, self.threads, self.spill_limit = memory_limit, threads, spill_limit
        self.research: Any = None
        self._tmp: tempfile.TemporaryDirectory[str] | None = None

    @property
    def con(self) -> Any:
        if self.research is None:
            raise RuntimeError("ScreenStore is not open; use it as a context manager")
        return self.research.con

    @property
    def has_research_store(self) -> bool:
        return self.research_db_path is not None

    def __enter__(self) -> ScreenStore:
        from ..research import ResearchStore

        if not self.warehouse_path.is_file():
            raise FileNotFoundError(f"warehouse not found: {self.warehouse_path}")
        if self.research_db_path is not None:
            if not self.research_db_path.is_file():
                raise FileNotFoundError(f"research store not found: {self.research_db_path}")
            if _same_file(self.research_db_path, self.warehouse_path):
                raise ValueError("the research store must not be the warehouse file")
        self._tmp = tempfile.TemporaryDirectory(prefix="pit-screen-", dir=self.scratch_dir,
                                                ignore_cleanup_errors=True)
        try:
            self.research = ResearchStore(Path(self._tmp.name) / "pit_screen.duckdb",
                                          warehouse_path=self.warehouse_path, memory_limit=self.memory_limit,
                                          threads=self.threads, warehouse_alias=WAREHOUSE_ALIAS)
            self.research.open()
            con = self.research.con
            if self.research_db_path is not None:
                literal = "'" + self.research_db_path.as_posix().replace("'", "''") + "'"
                con.execute(f"ATTACH {literal} AS {RESEARCH_ALIAS} (READ_ONLY)")
            if self.spill_limit is not None:
                con.execute(f"SET max_temp_directory_size='{self.spill_limit}'")
            con.execute("SET enable_external_access=false")
            attached = dict(con.execute(
                "SELECT database_name, readonly FROM duckdb_databases() WHERE database_name IN (?, ?)",
                [WAREHOUSE_ALIAS, RESEARCH_ALIAS]).fetchall())
            expected = {WAREHOUSE_ALIAS} | ({RESEARCH_ALIAS} if self.research_db_path is not None else set())
            if set(attached) != expected or not all(attached.values()):
                raise RuntimeError(f"screen inputs must be attached READ_ONLY, got {attached}")
        except BaseException:
            self.__exit__(None, None, None)
            raise
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        try:
            if self.research is not None:
                self.research.close()
        finally:
            self.research = None
            if self._tmp is not None:
                self._tmp.cleanup()
                self._tmp = None

    def settings(self) -> dict[str, str]:
        """DuckDB's effective settings that bound the screen."""
        return dict(self.con.execute("""
            SELECT name, value FROM duckdb_settings()
            WHERE name IN ('memory_limit','threads','max_temp_directory_size','enable_external_access',
                           'preserve_insertion_order','TimeZone')
        """).fetchall())


# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class CrossSectionResult:
    query_version: str
    status: str
    as_of_ts: dt.datetime
    screen_date: dt.date | None
    basis: str
    universe_id: str
    identity_basis: str
    fields: tuple[ScreenField, ...]
    members: int
    eligible_members: int
    valid_members: int
    rows: pd.DataFrame
    field_digests: dict[str, str]
    r2a_field_digests: dict[str, str]
    screen_sha256: str
    blockers: tuple[str, ...]
    diagnostics: dict[str, Any] = field(default_factory=dict)


def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False, default=str)


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _sql_text(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _utc_naive(value: dt.datetime) -> dt.datetime:
    if not isinstance(value, dt.datetime) or value.tzinfo is None or value.utcoffset() is None:
        raise ValueError("as_of_ts must be a timezone-aware datetime")
    return value.astimezone(dt.UTC).replace(tzinfo=None)


def _bounded_int(label: str, value: int, low: int, high: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise ValueError(f"{label} must be an integer {low}..{high}")
    return value


def _empty_frame() -> pd.DataFrame:
    return pd.DataFrame({column: pd.Series(dtype="object") for column in OUTPUT_COLUMNS})


# ---------------------------------------------------------------------------
# Screen
# ---------------------------------------------------------------------------

def run_cross_section(
    screen: ScreenStore,
    as_of_ts: dt.datetime,
    fields: Sequence[str | ScreenField],
    *,
    basis: str | None = None,
    universe_id: str | None = None,
    feature_version: str | None = None,
    owner_links: Sequence[Any] | None = None,
    max_age_days: int = 200,
    annual_max_age_days: int = 400,
    session_lookback_days: int = DEFAULT_SESSION_LOOKBACK_DAYS,
    max_rows: int = DEFAULT_MAX_ROWS,
    metric_batch_size: int = 16,
    unverified_vendor_shares: bool = False,
    include_unlisted_tail: bool = True,
) -> CrossSectionResult:
    """Screen every member of the tier-1 universe at ``as_of_ts`` inside an open :class:`ScreenStore`.

    ``owner_links`` overrides the reconstructed owner bridge (A5 ``BridgeRow``
    shaped rows, as in the R2a panel); by default it is built from the warehouse.
    """
    from ..market_daily import MARKET_DAILY_SOURCE_NAME
    from ..research import panel

    if not isinstance(screen, ScreenStore) or screen.research is None:
        raise ValueError("run_cross_section needs an open ScreenStore")
    cutoff = _utc_naive(as_of_ts)
    basis, universe = resolve_basis(basis, universe_id)
    parsed = parse_screen_fields(fields)
    _bounded_int("max_rows", max_rows, 1, MAX_ROWS_LIMIT)
    _bounded_int("session_lookback_days", session_lookback_days, 1, 31)
    if not isinstance(unverified_vendor_shares, bool):
        raise ValueError("unverified_vendor_shares must be a bool")
    feature_fields = [f for f in parsed if f.kind == FIELD_FEATURE]
    if feature_fields and (not screen.has_research_store or not isinstance(feature_version, str)
                           or not re.fullmatch(r"[0-9a-f]{64}", feature_version)):
        raise ValueError("feature fields need a research store and a 64-hex feature_version")
    store, con = screen.research, screen.con
    phases: dict[str, float] = {}
    started = time.perf_counter()

    panel_features = tuple(panel.PanelFeature(f.code, f.code, str(f.window)) for f in parsed
                           if f.kind in (FIELD_DERIVED, FIELD_MARKET))
    checked = (panel.canonical_features(panel_features, unverified_vendor_shares=unverified_vendor_shares)
               if panel_features else ())
    panel._require_inputs(store, basis, checked, build_bridge=owner_links is None)
    as_of_date = cutoff.date()
    sessions = panel.observed_sessions(con, market_source=MARKET_DAILY_SOURCE_NAME,
                                       first_day=as_of_date - dt.timedelta(days=session_lookback_days),
                                       as_of_date=as_of_date, run_at=cutoff)
    labels = panel._basis_labels(basis)
    base = {"basis": basis, "universe": universe, "identity_basis": labels["identity_basis"], "fields": parsed,
            "cutoff": cutoff}
    if not sessions:
        return _result(con, base, "no_observed_session", None, {}, {}, {"session_lookback_days":
                                                                        session_lookback_days}, [])
    screen_date = sessions[-1]
    month = screen_date.replace(day=1)
    options = panel.ResearchPanelOptions(
        run_id=SCREEN_RUN_ID, basis=basis, start_month=month, end_month=month, as_of_date=screen_date,
        run_at=cutoff.replace(tzinfo=dt.UTC), features=panel_features or None, metric_batch_size=metric_batch_size,
        max_age_days=max_age_days, annual_max_age_days=annual_max_age_days,
        include_unlisted_tail=include_unlisted_tail, unverified_vendor_shares=unverified_vendor_shares)
    _, features, spec = panel._validate(options)
    features = features if panel_features else ()
    scopes = {item[0]: item[3] for item in spec["features"]}
    work_spec = {**spec, "scopes": scopes}

    bridge: dict[str, Any] | None = None
    if basis == BASIS_RECONSTRUCTED:
        if owner_links is None:
            link_rows, summary = panel._owner_bridge_rows(store)
        else:
            link_rows, summary = tuple(owner_links), None
        bridge = {"staged": panel.stage_owner_links(con, link_rows), "bridge_summary": summary}
    phases["owner_bridge"] = time.perf_counter() - started
    started = time.perf_counter()
    con.execute("CREATE OR REPLACE TEMP TABLE _rp_features (feature_id VARCHAR, metric_code VARCHAR, "
                "metric_window VARCHAR)")
    if features:
        con.executemany("INSERT INTO _rp_features VALUES (?,?,?)",
                        [[f.feature_id, f.metric_code, f.metric_window] for f in features])
    row = panel.CalendarRow(month, screen_date, screen_date, screen_date, cutoff, None, panel.CALENDAR_FORMED)
    panel._stage_run_calendar(con, [row])
    cohort = panel._stage_cohorts(store, spec, labels["market_source"]).get(screen_date)
    members = int(con.execute("SELECT count(*) FROM _rp_cohort_all").fetchone()[0])
    if members * len(parsed) > max_rows:
        raise ScreenBoundExceeded(f"{members} members x {len(parsed)} fields exceeds max_rows={max_rows}")
    phases["cohort"] = time.perf_counter() - started
    _stage_fields(con, parsed, scopes, max_age_days, annual_max_age_days)
    con.execute("CREATE OR REPLACE TEMP TABLE _xs_values ("
                + ", ".join(f"{name} {kind}" for name, kind in _VALUE_TYPES) + ")")

    r2a_digests: dict[str, str] = {}
    diagnostics: dict[str, Any] = {"session_lookback_days": session_lookback_days,
                                   "observed_sessions_in_lookback": len(sessions), "owner_bridge": bridge,
                                   "cohort": cohort, "market_null_revision_guard_rows": 0, "batches": []}
    eligible = int(cohort["eligible"]) if cohort else 0
    started = time.perf_counter()
    if features and eligible:
        _, definitions_sha, hashes = panel._definitions(con, features)
        diagnostics["definitions_sha256"] = definitions_sha
        for batch in panel._batches(features, options.metric_batch_size):
            stats: dict[str, Any] = {"batch": batch.ordinal, "features": [f.feature_id for f in batch.features]}
            if batch.is_market:
                panel._market_formation(store, batch, work_spec, labels["market_source"], row)
                guarded = _guard_market_null_revisions(con, batch, labels["market_source"])
                diagnostics["market_null_revision_guard_rows"] += guarded
            else:
                stats.update(panel._derived_selection(store, SCREEN_RUN_ID, batch, work_spec, hashes, 1,
                                                      panel.DEFAULT_PROOF_CHUNK_ROOTS))
                panel._derived_formations(store, SCREEN_RUN_ID, batch, work_spec, [row], [0])
            leaks = con.execute(f"""
                SELECT count(*) FROM _rp_batch_values v JOIN _rp_calendar c ON c.decision_date=v.formation_date
                WHERE {panel._leak_predicate('c.cutoff', 'v.')}
            """).fetchone()[0]
            if leaks:
                raise RuntimeError(f"{leaks} staged values are not visible at the screen cutoff")
            r2a_digests.update(con.execute(panel._VALUE_DIGEST_SQL.format(
                relation="_rp_batch_values", where="")).fetchall())
            _append_panel_values(con)
            diagnostics["batches"].append(stats)
    phases["panel_fields"] = time.perf_counter() - started
    started = time.perf_counter()
    if any(f.kind == FIELD_ITEM for f in parsed) and members:
        diagnostics["item_states"] = _append_item_values(con, cutoff, screen_date, universe)
    phases["item_fields"] = time.perf_counter() - started
    started = time.perf_counter()
    if feature_fields and members:
        diagnostics["research_features"] = _append_feature_values(con, feature_fields, feature_version,
                                                                  screen_date, cutoff, basis, universe)
    phases["feature_fields"] = time.perf_counter() - started
    if members:
        _append_ineligible_values(con, universe)
    diagnostics["phase_seconds"] = {key: round(value, 3) for key, value in phases.items()}
    return _result(con, base, "screened", screen_date, cohort or {}, r2a_digests, diagnostics, sessions)


def _stage_fields(con: Any, parsed: Sequence[ScreenField], scopes: dict[str, str], max_age_days: int,
                  annual_max_age_days: int) -> None:
    from ..research import panel

    availability = {FIELD_DERIVED: panel.FUNDAMENTAL_AVAILABILITY_BASIS,
                    FIELD_MARKET: panel.MARKET_AVAILABILITY_BASIS,
                    FIELD_ITEM: panel.FUNDAMENTAL_AVAILABILITY_BASIS, FIELD_FEATURE: FEATURE_AVAILABILITY_BASIS}
    rows = []
    for order, item in enumerate(parsed):
        if item.kind in (FIELD_DERIVED, FIELD_MARKET):
            scope = scopes[item.code]
            basis = (panel.VENDOR_SHARES_AVAILABILITY_BASIS if panel.NATIVE_FEATURES.get(item.code, {}).get(
                "requires") == panel.UNVERIFIED_VENDOR_SHARES else availability[item.kind])
        else:
            scope = ITEM_SCOPE if item.kind == FIELD_ITEM else FEATURE_SCOPE
            basis = availability[item.kind]
        age = annual_max_age_days if item.kind == FIELD_ITEM and item.window == "annual" else max_age_days
        rows.append([item.field_id, item.kind, item.code, item.window, order, scope, basis, age])
    con.execute("CREATE OR REPLACE TEMP TABLE _xs_fields (field_id VARCHAR, field_kind VARCHAR, code VARCHAR, "
                "window_code VARCHAR, field_order INTEGER, feature_scope VARCHAR, availability_basis VARCHAR, "
                "max_age_days INTEGER)")
    con.executemany("INSERT INTO _xs_fields VALUES (?,?,?,?,?,?,?,?)", rows)


_COHORT_EXTRAS = ("k.symbol, k.security_type, k.exchange_code, k.membership_reason, k.eligible, k.cohort_reason, "
                  "k.owner_link_reason, k.primary_line")


def _append_panel_values(con: Any) -> None:
    """Copy one R2a batch (``_rp_batch_values``) with the member's as-of attributes."""
    con.execute(f"""
        INSERT INTO _xs_values BY NAME
        SELECT v.*, f.field_kind, f.field_order, {_COHORT_EXTRAS},
               CASE WHEN f.field_kind='{FIELD_MARKET}' THEN
                         CASE WHEN v.available_at IS NOT NULL THEN v.formation_date END
                    ELSE v.fiscal_period_end END AS reference_date,
               v.derived_value_id AS source_ref
        FROM _rp_batch_values v
        JOIN _xs_fields f ON f.field_id=v.feature_id
        JOIN _rp_cohort_all k ON k.decision_date=v.formation_date AND k.security_id=v.security_id
    """)


def _guard_market_null_revisions(con: Any, batch: Any, market_source: str) -> int:
    """Never revive an older market revision: the newest visible revision's NULL wins.

    The panel's market read takes ``arg_max(value, clock)`` per column, which
    skips NULL arguments; when the newest revision visible at the cutoff carries
    NULL for a column, that value is reported as ``invalid_current_state``.
    """
    codes = [f.metric_code for f in batch.features if f.metric_code in _market_columns(con)]
    if not codes:
        return 0
    picks = ",\n".join(
        f'struct_extract(arg_max(struct_pack(v := m."{code}"), (m.available_at, m.market_daily_id)), \'v\') '
        f'AS "{code}"' for code in codes)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _xs_market_newest AS
        SELECT m.security_id, {picks}
        FROM market_daily_metrics m
        JOIN _rp_calendar c ON m.trade_date=c.decision_date AND m.available_at<=c.cutoff
                           AND m.as_of_date<=c.decision_date
        JOIN _rp_cohort_all k ON k.decision_date=c.decision_date AND k.security_id=m.security_id AND k.eligible
        WHERE m.source=?
        GROUP BY m.security_id
    """, [market_source])
    guarded = 0
    for feature in batch.features:
        if feature.metric_code not in codes:
            continue
        flagged = int(con.execute(f"""
            SELECT count(*) FROM _rp_batch_values v JOIN _xs_market_newest n ON n.security_id=v.security_id
            WHERE v.feature_id=? AND v.raw_value IS NOT NULL AND n."{feature.metric_code}" IS NULL
        """, [feature.feature_id]).fetchone()[0])
        if flagged:
            con.execute(f"""
                UPDATE _rp_batch_values SET raw_value=NULL, reason='invalid_current_state'
                WHERE feature_id=? AND raw_value IS NOT NULL AND security_id IN (
                  SELECT security_id FROM _xs_market_newest WHERE "{feature.metric_code}" IS NULL)
            """, [feature.feature_id])
            guarded += flagged
    return guarded


def _market_columns(con: Any) -> frozenset[str]:
    return frozenset(row[0] for row in con.execute("""
        SELECT column_name FROM duckdb_columns()
        WHERE database_name=? AND schema_name='main' AND table_name='market_daily_metrics'
    """, [WAREHOUSE_ALIAS]).fetchall())


ITEM_STATE_SQL = """
    CREATE OR REPLACE TEMP TABLE _xs_item_state AS
    WITH owners AS (
      SELECT DISTINCT owner_cik AS cik FROM _rp_cohort_all WHERE eligible AND leg_reason='valid'
    ), src AS (
      SELECT lpad(coalesce(nullif(regexp_extract(f.security_id, 'CIK-([0-9]{1,10})$', 1), ''),
                           nullif(ltrim(f.cik, '0'), '')), 10, '0') AS cik,
             q.field_id, f.basis, f.period_start, f.period_end, f.value, f.available_at, f.valid_to,
             f.source, f.rule_id, f.standardized_id
      FROM fundamental_standardized f
      JOIN _xs_fields q ON q.field_kind='item' AND q.code=f.canonical_code AND q.window_code=f.basis
      WHERE f.available_at IS NOT NULL AND f.available_at<=? AND f.period_end<=? AND f.as_of_date<=?
    )
    SELECT s.cik, s.field_id,
           arg_max(struct_pack(v := s.value, period_start := s.period_start, period_end := s.period_end,
                               available_at := s.available_at, valid_to := s.valid_to,
                               standardized_id := s.standardized_id),
                   (s.period_end, s.available_at, s.source, s.rule_id, s.basis, s.standardized_id)) AS st
    FROM src s SEMI JOIN owners o ON o.cik=s.cik
    GROUP BY s.cik, s.field_id
"""

ITEM_VALUES_SQL = """
    INSERT INTO _xs_values BY NAME
    WITH grid AS (
      SELECT k.decision_date, k.security_id, k.owner_cik, k.identity_basis, k.leg_reason,
             k.symbol, k.security_type, k.exchange_code, k.membership_reason, k.eligible, k.cohort_reason,
             k.owner_link_reason, k.primary_line,
             q.field_id, q.field_kind, q.field_order, q.code, q.window_code, q.max_age_days, q.feature_scope,
             q.availability_basis, CASE WHEN k.leg_reason='valid' THEN s.st END AS st
      FROM _rp_cohort_all k CROSS JOIN _xs_fields q
      LEFT JOIN _xs_item_state s ON s.cik=k.owner_cik AND s.field_id=q.field_id
      WHERE k.eligible AND q.field_kind='item'
    ), judged AS (
      SELECT *, date_diff('day', struct_extract(st, 'period_end'), decision_date) AS age,
             CASE WHEN leg_reason<>'valid' THEN leg_reason
                  WHEN st IS NULL THEN 'missing_item_state'
                  WHEN struct_extract(st, 'valid_to') IS NOT NULL AND struct_extract(st, 'valid_to')<=?
                       THEN 'expired_state_without_successor'
                  WHEN struct_extract(st, 'v') IS NULL THEN 'null_state'
                  WHEN NOT isfinite(struct_extract(st, 'v')) THEN 'invalid_current_state'
                  WHEN date_diff('day', struct_extract(st, 'period_end'), decision_date)>max_age_days
                       THEN 'stale_current_anchor'
                  ELSE 'valid' END AS reason
      FROM grid
    )
    SELECT decision_date AS formation_date, security_id, field_id AS feature_id, code AS metric_code,
           window_code AS metric_window,
           CASE WHEN reason IN ('valid','stale_current_anchor') THEN struct_extract(st, 'v') END AS raw_value,
           reason, CAST(NULL AS VARCHAR) AS lineage_status,
           struct_extract(st, 'available_at') AS available_at,
           struct_extract(st, 'available_at') AS latest_input_clock,
           struct_extract(st, 'period_end') AS period_end,
           struct_extract(st, 'period_start') AS fiscal_period_start,
           struct_extract(st, 'period_end') AS fiscal_period_end,
           CASE WHEN st IS NOT NULL THEN window_code END AS value_origin,
           CAST(age AS INTEGER) AS age_days,
           CASE WHEN st IS NOT NULL THEN max_age_days END AS max_age_days,
           owner_cik, identity_basis, ? AS universe_basis, availability_basis, feature_scope,
           field_kind, field_order, symbol, security_type, exchange_code, membership_reason, eligible,
           cohort_reason, owner_link_reason, primary_line,
           struct_extract(st, 'period_end') AS reference_date,
           struct_extract(st, 'standardized_id') AS source_ref
    FROM judged
"""


def _append_item_values(con: Any, cutoff: dt.datetime, screen_date: dt.date, universe: str) -> dict[str, Any]:
    con.execute(ITEM_STATE_SQL, [cutoff, screen_date, screen_date])
    con.execute(ITEM_VALUES_SQL, [cutoff, universe])
    return {"owner_states": int(con.execute("SELECT count(*) FROM _xs_item_state").fetchone()[0])}


FEATURE_VALUES_SQL = """
    INSERT INTO _xs_values BY NAME
    WITH grid AS (
      SELECT k.decision_date, k.security_id, k.owner_cik, k.identity_basis,
             k.symbol, k.security_type, k.exchange_code, k.membership_reason, k.eligible, k.cohort_reason,
             k.owner_link_reason, k.primary_line,
             m.security_id IS NOT NULL AS has_row, m.available_at AS row_clock, m.domain_status, m.age_days,
             {owner_basis} AS owner_basis, CAST(m."{variant}" AS DOUBLE) AS value
      FROM _rp_cohort_all k
      LEFT JOIN {alias}.main.research_feature_matrix m
        ON m.feature_version=? AND m.formation_date=? AND m.feature_id=? AND m.security_id=k.security_id
      WHERE k.eligible
    ), judged AS (
      SELECT *,
             CASE WHEN CAST(? AS VARCHAR) IS NOT NULL THEN CAST(? AS VARCHAR)
                  WHEN NOT has_row THEN 'not_in_feature_universe'
                  WHEN row_clock>? THEN 'invalid_input_clock'
                  WHEN domain_status<>'in_domain' THEN domain_status
                  WHEN value IS NULL OR NOT isfinite(value) THEN ?
                  ELSE 'valid' END AS reason
      FROM grid
    )
    SELECT decision_date AS formation_date, security_id, ? AS feature_id, ? AS metric_code, ? AS metric_window,
           CASE WHEN reason='valid' THEN value END AS raw_value, reason, CAST(NULL AS VARCHAR) AS lineage_status,
           CASE WHEN has_row THEN row_clock END AS available_at,
           CASE WHEN has_row THEN row_clock END AS latest_input_clock,
           CASE WHEN has_row THEN 'research_feature_version' END AS value_origin,
           CASE WHEN has_row THEN age_days END AS age_days,
           owner_cik,
           CASE WHEN owner_basis='unlinked_line' THEN 'price_line' ELSE identity_basis END AS identity_basis,
           ? AS universe_basis, '{availability}' AS availability_basis, '{scope}' AS feature_scope,
           '{kind}' AS field_kind, ? AS field_order, symbol, security_type, exchange_code, membership_reason,
           eligible, cohort_reason, owner_link_reason, primary_line,
           CASE WHEN has_row THEN CAST(? AS DATE) END AS reference_date,
           CASE WHEN has_row THEN ? END AS source_ref
    FROM judged
"""


def _append_feature_values(con: Any, fields: Sequence[ScreenField], version: str | None, screen_date: dt.date,
                           cutoff: dt.datetime, basis: str, universe: str) -> dict[str, Any]:
    """Rows of a sealed R2b feature version at its latest formation knowable by the cutoff."""
    row = con.execute(f"SELECT status, basis FROM {RESEARCH_ALIAS}.main.research_feature_versions "
                      "WHERE feature_version=?", [version]).fetchone()
    if row is None:
        raise ValueError(f"feature version {version} is not in the research store")
    if row[0] not in FEATURE_SEALED_STATUSES:
        raise ValueError(f"feature version {version} is {row[0]!r}, not sealed")
    if row[1] != basis:
        raise ValueError(f"feature version {version} was built on the {row[1]} basis, not {basis}")
    formation = con.execute(f"""
        SELECT max(formation_date) FROM {RESEARCH_ALIAS}.main.research_feature_dates
        WHERE feature_version=? AND formation_date<=?
          AND CAST(formation_date AS TIMESTAMP) + INTERVAL {DECISION_HOUR} HOUR<=?
    """, [version, screen_date, cutoff]).fetchone()[0]
    columns = frozenset(item[0] for item in con.execute("""
        SELECT column_name FROM duckdb_columns()
        WHERE database_name=? AND schema_name='main' AND table_name='research_feature_matrix'
    """, [RESEARCH_ALIAS]).fetchall())
    catalog = {item[0]: (item[1], item[2]) for item in con.execute(f"""
        SELECT feature_id, status, preferred_variant FROM {RESEARCH_ALIAS}.main.research_feature_catalog
        WHERE feature_version=?
    """, [version]).fetchall()}
    orders = {item[0]: item[1] for item in con.execute("SELECT field_id, field_order FROM _xs_fields").fetchall()}
    detail: dict[str, Any] = {"feature_version": version, "formation_date": formation, "fields": {}}
    for item in fields:
        status, preferred = catalog.get(item.code, (None, None))
        variant = item.window or preferred or DEFAULT_FEATURE_VARIANT
        if variant not in FEATURE_VARIANTS or variant not in columns:
            raise ValueError(f"feature variant {variant!r} is not a column of research_feature_matrix")
        date_status = None if formation is None else con.execute(f"""
            SELECT date_status FROM {RESEARCH_ALIAS}.main.research_feature_dates
            WHERE feature_version=? AND formation_date=? AND feature_id=? AND variant=?
        """, [version, formation, item.code, variant]).fetchone()
        date_status = None if date_status is None else date_status[0]
        blocked = ("no_feature_formation" if formation is None
                   else None if status == "built" else f"feature_not_built:{status or 'absent'}")
        missing = (f"feature_date:{date_status}" if date_status not in (None, "formed")
                   else "variant_not_produced")
        sql = FEATURE_VALUES_SQL.format(
            alias=RESEARCH_ALIAS, variant=variant, availability=FEATURE_AVAILABILITY_BASIS, scope=FEATURE_SCOPE,
            kind=FIELD_FEATURE, owner_basis="m.owner_basis" if "owner_basis" in columns else "CAST(NULL AS VARCHAR)")
        con.execute(sql, [version, formation, item.code, blocked, blocked, cutoff, missing, item.field_id, item.code,
                          variant, universe, orders[item.field_id], formation, version])
        detail["fields"][item.field_id] = {"variant": variant, "catalog_status": status, "date_status": date_status}
    return detail


INELIGIBLE_VALUES_SQL = """
    INSERT INTO _xs_values BY NAME
    SELECT k.decision_date AS formation_date, k.security_id, q.field_id AS feature_id, q.code AS metric_code,
           q.window_code AS metric_window, CAST(NULL AS DOUBLE) AS raw_value, k.cohort_reason AS reason,
           k.owner_cik, k.identity_basis, ? AS universe_basis, q.availability_basis, q.feature_scope,
           q.field_kind, q.field_order, k.symbol, k.security_type, k.exchange_code, k.membership_reason, k.eligible,
           k.cohort_reason, k.owner_link_reason, k.primary_line
    FROM _rp_cohort_all k CROSS JOIN _xs_fields q
    WHERE NOT k.eligible
"""


def _append_ineligible_values(con: Any, universe: str) -> None:
    """Members the basis does not admit keep one NULL row per field with their status."""
    con.execute(INELIGIBLE_VALUES_SQL, [universe])


def _result(con: Any, base: dict[str, Any], status: str, screen_date: dt.date | None, cohort: dict[str, Any],
            r2a_digests: dict[str, str], diagnostics: dict[str, Any],
            sessions: Sequence[dt.date]) -> CrossSectionResult:
    cutoff: dt.datetime = base["cutoff"]
    parsed: tuple[ScreenField, ...] = base["fields"]
    if screen_date is None:
        frame, digests, counts = _empty_frame(), {item.field_id: _sha("") for item in parsed}, {}
        members = eligible = valid = 0
    else:
        con.execute("""
            CREATE OR REPLACE TEMP TABLE _xs_out AS
            SELECT CAST(? AS TIMESTAMP) AS as_of_ts, *,
                   date_diff('day', reference_date, CAST(? AS DATE)) AS staleness_days
            FROM _xs_values
        """, [cutoff, cutoff.date()])
        grain = con.execute(
            "SELECT count(*), count(DISTINCT feature_id || chr(31) || security_id) FROM _xs_out").fetchone()
        if grain[0] != grain[1]:
            raise RuntimeError("screen grain violated: more than one row per (member, field)")
        leaks = con.execute("""
            SELECT count(*) FROM _xs_out
            WHERE available_at>as_of_ts OR latest_input_clock>as_of_ts
               OR (raw_value IS NOT NULL AND reason NOT IN ('valid','stale_current_anchor'))
        """).fetchone()[0]
        if leaks:
            raise RuntimeError(f"{leaks} screen rows are not visible at the cutoff or carry a rejected value")
        columns = [name for name, _ in _VALUE_TYPES if name not in ("field_order",)] + ["staleness_days"]
        digest_sql = ("SELECT feature_id, sha256(coalesce(string_agg(to_json(struct_pack("
                      + ",".join(f"{c} := {c}" for c in columns)
                      + ")), chr(10) ORDER BY security_id), '')) FROM _xs_out GROUP BY feature_id")
        digests = dict(con.execute(digest_sql).fetchall())
        digests = {item.field_id: digests.get(item.field_id, _sha("")) for item in parsed}
        counts = {}
        for field_id, reason, n in con.execute(
                "SELECT feature_id, reason, count(*) FROM _xs_out GROUP BY ALL ORDER BY ALL").fetchall():
            counts.setdefault(field_id, {})[reason] = int(n)
        frame = con.execute("SELECT * FROM _xs_out ORDER BY field_order, security_id").df()
        frame = frame.rename(columns=_OUTPUT_RENAMES)[list(OUTPUT_COLUMNS)]
        members = int(con.execute("SELECT count(*) FROM _rp_cohort_all").fetchone()[0])
        eligible = int(cohort.get("eligible", 0))
        valid = int(cohort.get("valid", 0))
    blockers = list(SCREEN_BLOCKERS)
    if base["basis"] == BASIS_RECONSTRUCTED:
        blockers.append("reconstructed_identity_universe_and_availability_not_certifiable")
    if cohort.get("unlinked"):
        blockers.append(f"owner_link_attrition:{cohort['unlinked']}/{cohort['eligible']}")
    if screen_date is not None:
        unverified = int(con.execute("SELECT count(*) FROM _xs_out WHERE size_status=?",
                                     ["unverified_vendor_shares"]).fetchone()[0])
        if unverified:
            blockers.append(f"unverified_vendor_share_size_rows:{unverified}")
        lag = (cutoff.date() - screen_date).days
        if lag > 4:
            blockers.append(f"screen_session_lag_days:{lag}")
        if diagnostics.get("market_null_revision_guard_rows"):
            blockers.append(f"market_null_revision_guard_rows:{diagnostics['market_null_revision_guard_rows']}")
    if any(item.kind == FIELD_FEATURE for item in parsed):
        blockers.append("research_feature_values_research_only")
    valid_values = sum(field_counts.get("valid", 0) for field_counts in counts.values())
    if status == "screened":
        if not members:
            status = "empty_universe"
        elif base["basis"] == BASIS_STRICT and not valid:
            status = "untestable_strict"
        elif not valid_values:
            status = "blocked_empty"
        else:
            status = "complete"
    if screen_date is not None and not valid_values:
        blockers.append("no_valid_values")
    diagnostics = {**diagnostics, "value_status_counts": counts,
                   "screen_session": None if screen_date is None else screen_date.isoformat(),
                   "last_observed_sessions": [day.isoformat() for day in sessions[-3:]]}
    manifest = [QUERY_VERSION, cutoff.isoformat(), None if screen_date is None else screen_date.isoformat(),
                base["basis"], base["universe"], [item.field_id for item in parsed], digests]
    return CrossSectionResult(
        query_version=QUERY_VERSION, status=status, as_of_ts=cutoff, screen_date=screen_date, basis=base["basis"],
        universe_id=base["universe"], identity_basis=base["identity_basis"], fields=parsed, members=members,
        eligible_members=eligible, valid_members=valid, rows=frame, field_digests=digests,
        r2a_field_digests=dict(sorted(r2a_digests.items())), screen_sha256=_sha(_canonical(manifest)),
        blockers=tuple(blockers), diagnostics=diagnostics)


def cross_section_asof(
    as_of_ts: dt.datetime,
    fields: Sequence[str | ScreenField],
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    basis: str | None = None,
    universe_id: str | None = None,
    research_db_path: Path | str | None = None,
    feature_version: str | None = None,
    scratch_dir: Path | str | None = None,
    memory_limit: str = "256MB",
    threads: int = 1,
    **options: Any,
) -> CrossSectionResult:
    """One row per (tier-1 universe member active at ``as_of_ts``, field); see the module docstring.

    Opens a private :class:`ScreenStore` over ``db_path`` (attached read-only),
    runs :func:`run_cross_section` and removes the scratch store.
    """
    with ScreenStore(db_path, scratch_dir=scratch_dir, memory_limit=memory_limit, threads=threads,
                     research_db_path=research_db_path) as screen:
        return run_cross_section(screen, as_of_ts, fields, basis=basis, universe_id=universe_id,
                                 feature_version=feature_version, **options)


__all__ = [
    "BASES",
    "BASIS_RECONSTRUCTED",
    "BASIS_STRICT",
    "FEATURE_VARIANTS",
    "FIELD_KINDS",
    "ITEM_BASES",
    "OUTPUT_COLUMNS",
    "QUERY_VERSION",
    "CrossSectionResult",
    "ScreenBoundExceeded",
    "ScreenField",
    "ScreenStore",
    "cross_section_asof",
    "parse_screen_fields",
    "resolve_basis",
    "run_cross_section",
]
