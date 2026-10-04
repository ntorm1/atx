import datetime as dt
import hashlib
from dataclasses import asdict, replace

import pytest

from atx_db.alpha_panel import mna_native_v2 as N
from atx_db.alpha_panel.terminal_mna_v2 import UTC


def native():
    blob = b'<html><script>converted into the right to receive $99 in cash</script><p>Each share was converted into the right to receive $12 in cash.</p></html>'
    clock = dt.datetime(2020, 1, 3, tzinfo=UTC)
    pin = N.NativePin("synthetic-original", hashlib.sha256(blob).hexdigest(), "0000000001-20-000001", 1,
                      clock, clock, "retrieval_floor")
    return blob, pin


def test_native_replay_full_schema_keys_and_semantics():
    blob, pin = native()
    objects, pins, code = {pin.source_id: blob}, (pin,), N.code_pins()
    rows = N.replay_native(objects, pins, admission_id="offline-independent-fixture", expected_code=code)
    assert rows[0].cash_per_share == 12
    N.validate_native_rows([asdict(row) for row in rows], objects, pins,
                           admission_id="offline-independent-fixture", expected_code=code)
    manufactured = asdict(rows[0])
    manufactured["cash_per_share"] = 999
    with pytest.raises(ValueError, match="replay mismatch"):
        N.validate_native_rows([manufactured], objects, pins, admission_id="fixture", expected_code=code)
    with pytest.raises(ValueError, match="schema"):
        N.validate_native_rows([{"accession": pin.accession}], objects, pins, admission_id="fixture", expected_code=code)


def test_no_self_receipt_or_abbreviated_code_proof():
    blob, pin = native()
    with pytest.raises(ValueError, match="code identity"):
        N.replay_native({pin.source_id: blob}, (pin,), admission_id="fixture", expected_code={})
    with pytest.raises(ValueError, match="identity mismatch"):
        N.read_native(blob + b"corruption", pin, "fixture")
    with pytest.raises(ValueError, match="inventory"):
        N.replay_native({}, (pin,), admission_id="fixture", expected_code=N.code_pins())


def test_html_clock_floor_never_backdated():
    blob, pin = native()
    with pytest.raises(ValueError, match="retrieval floor"):
        N.read_native(blob, replace(pin, available_at=pin.available_at-dt.timedelta(days=1)), "fixture")
    with pytest.raises(ValueError, match="missing"):
        N.read_native(blob, replace(pin, clock_basis="native_sec_acceptance"), "fixture")


def test_original_sec_submission_headers_match_exact_clock_and_filer():
    blob = b'''<ACCEPTANCE-DATETIME>20200103123000
ACCESSION NUMBER: 0000000001-20-000001
FILER:
 COMPANY DATA:
  CENTRAL INDEX KEY: 0000000001
</SEC-HEADER>
<DOCUMENT><TYPE>8-K
<TEXT><p>converted into the right to receive $12 in cash.</p></TEXT></DOCUMENT>'''
    clock = dt.datetime(2020, 1, 3, 17, 30, tzinfo=UTC)
    pin = N.NativePin("submission", hashlib.sha256(blob).hexdigest(), "0000000001-20-000001", 1,
                      clock+dt.timedelta(days=1), clock, "native_sec_acceptance")
    assert N.read_native(blob, pin, "fixture").available_at == clock
    with pytest.raises(ValueError, match="identity/clock"):
        N.read_native(blob, replace(pin, cik=2), "fixture")


def test_duplicate_native_keys_rejected():
    blob, pin = native()
    with pytest.raises(ValueError, match="unique"):
        N.replay_native({pin.source_id: blob}, (pin, pin), admission_id="fixture", expected_code=N.code_pins())
