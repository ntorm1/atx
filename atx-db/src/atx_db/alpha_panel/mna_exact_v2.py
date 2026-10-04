"""Conditional exact terminal return from original independently pinned replay.

External immutable reviewed pins bind every candidate/document/vendor/economic
input before compute. Reader-generated typed values are reconstructed here;
caller-manufactured Terms/Price/Evidence objects are not accepted as proof.
Value-only policy never divides by target close. Return policy is frozen in the
external admission, and all terminal/settlement dates must remain pre2023.
"""
from __future__ import annotations

import dataclasses
import datetime as dt
import hashlib
import json
import math
from pathlib import Path

from . import mna_financial_v2 as F
from . import mna_native_v2 as N
from . import mna_price_native_v2 as P
from . import terminal_mna_v2 as T


def exact_code_pins() -> dict[str, str]:
    return {**F.financial_code_pins(), "mna_price_native_v2": hashlib.sha256(Path(P.__file__).read_bytes()).hexdigest(),
            "mna_exact_v2": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}


def _canonical(value):
    if dataclasses.is_dataclass(value):
        value = dataclasses.asdict(value)
    return json.loads(json.dumps(value, default=str, sort_keys=True))


def replay_exact(candidate: T.Candidate, document: bytes, document_pin: N.NativePin,
                 vendor: bytes | Path, target_pin: P.VendorPricePin, target_economic: bytes,
                 target_economic_pin: P.EconomicPin, *, expected: dict,
                 acquirer_pin: P.VendorPricePin | None = None, acquirer_economic: bytes | None = None,
                 acquirer_economic_pin: P.EconomicPin | None = None, bulk_clock=None) -> T.TerminalValue:
    """Native replay only; no ID/boolean/hash-only route around source semantics."""
    before = exact_code_pins()
    actual = {"candidate": _canonical(candidate), "document_pin": _canonical(document_pin),
              "target_pin": _canonical(target_pin), "target_economic_pin": _canonical(target_economic_pin),
              "acquirer_pin": _canonical(acquirer_pin), "acquirer_economic_pin": _canonical(acquirer_economic_pin),
              "expected_code": before, "return_policy": expected.get("return_policy")}
    if expected != actual or expected["return_policy"] not in ("value-only", "pre2023-native-qualified"):
        raise ValueError("complete independently pinned exact-native admission mismatch")
    # Pin all reader/code identities before first monetary/price computation.
    clocks = {document_pin.source_id: bulk_clock} if bulk_clock is not None else None
    financial = F.replay_financial({document_pin.source_id: document}, (document_pin,),
                                   admission_id="independently-pinned-exact-native-contract",
                                   expected_code=F.financial_code_pins(), bulk_clocks=clocks)[0]
    target = P.read_vendor_price(vendor, target_pin, target_economic, target_economic_pin, "native-exact-replay")
    if (financial.target_name != target_economic_pin.issuer_name or financial.target_share_class != target.share_class
        or target.security_id != candidate.security_id or target.session_date != candidate.last_session):
        raise ValueError("native target party/class/security/date binding mismatch")
    acquirer = None
    if financial.stock_ratio is not None:
        if acquirer_pin is None or acquirer_economic is None or acquirer_economic_pin is None:
            raise ValueError("complete original acquirer native price/economic proof required")
        acquirer = P.read_vendor_price(vendor, acquirer_pin, acquirer_economic, acquirer_economic_pin, "native-exact-replay")
        if (financial.acquirer_name != acquirer_economic_pin.issuer_name
            or financial.acquirer_share_class != acquirer.share_class
            or acquirer.session_date != financial.completion_date):
            raise ValueError("native acquirer party/class/date binding mismatch")
    if financial.reasons or financial.completion_date is None or financial.settlement_date is None:
        raise ValueError("native terminal financial completion/settlement/clock unqualified")
    ev = T.Evidence(document_pin.source_id, document_pin.sha256, document_pin.available_at, "native-exact-replay")
    terms = T.Terms(document_pin.accession, candidate.security_id, target.share_class, target.unit, target.basis,
                    financial.cash_currency or target.currency, financial.consideration_type,
                    financial.cash_per_share, financial.stock_ratio, acquirer.security_id if acquirer else None,
                    acquirer.share_class if acquirer else None, acquirer.unit if acquirer else None,
                    acquirer.basis if acquirer else None, financial.completion_date, financial.settlement_date,
                    financial.status, ev, ev, fractional_treatment="exact_per_target_share" if acquirer else "not_applicable")
    # Explicit completed common-share conversion from native source independently
    # resolves the legacy heuristic continuation flag; original flag is retained
    # in the full publisher outcome, never used to shrink the population.
    qualified = T.value_terminal(dataclasses.replace(candidate, continued=False), terms, target, acquirer)
    remaining = set(qualified.reason) - {"native_replay_admission_unavailable"}
    dlret = None
    if expected["return_policy"] == "pre2023-native-qualified" and not remaining:
        dlret = qualified.proceeds_per_share / target.close - 1
    elif expected["return_policy"] == "value-only":
        remaining.add("return_not_requested_value_only")
    if exact_code_pins() != before:
        raise ValueError("native exact readers/code changed during computation")
    if any(value is not None and not math.isfinite(float(value)) for value in (qualified.proceeds_per_share, dlret)):
        raise ValueError("native monetary value exceeds finite output storage range")
    return dataclasses.replace(qualified, dlret=dlret, dlret_exact=dlret is not None,
                               reason=tuple(sorted(remaining)),
                               source_ids=tuple(sorted(set(qualified.source_ids + (target_economic_pin.source_id,) +
                                                ((acquirer_economic_pin.source_id,) if acquirer else ())))))
