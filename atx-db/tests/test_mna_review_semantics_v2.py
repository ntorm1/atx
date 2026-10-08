from dataclasses import replace
import hashlib
import pytest
from atx_db.alpha_panel import mna_financial_v2 as F
from atx_db.alpha_panel import mna_price_native_v2 as P
from tests.test_mna_financial_v2 import original
from tests.test_mna_exact_v2 import fixture


@pytest.mark.parametrize('qualifier', ['only if the holder satisfies future performance conditions',
                                      'provided that future conditions are met', 'unless the merger is reversed',
                                      'subject to approval', 'only when a later milestone is achieved'])
def test_i1_qualified_cash_is_not_unconditional(qualifier):
    blob, pin = original('On January 3, 2020, Acme completed the merger with Buyer. '
                         'each share of Acme common stock was converted into the right to receive USD12 in cash '
                         + qualifier + '. On January 3, 2020, the cash consideration was paid.')
    row = F.parse_financial(blob.decode(), pin)
    assert row.cash_per_share is None and row.consideration_type is None
    assert set(row.reasons) & {'conditional_consideration', 'unsupported_qualified_consideration_tail'}


@pytest.mark.parametrize('kind', ['raw', 'conversion', 'contradiction'])
def test_i2_negation_and_contradictory_basis_refuse(kind):
    args, _, _ = fixture()
    blob, pin = args[5], args[6]
    if kind == 'raw':
        quote = 'close is not the unadjusted raw traded close.'
        blob = blob.replace(pin.raw_basis_quote.encode(), quote.encode()); pin = replace(pin, raw_basis_quote=quote)
    elif kind == 'conversion':
        quote = 'conversion terms are not unchanged with no split or FX conversion.'
        blob = blob.replace(pin.conversion_basis_quote.encode(), quote.encode()); pin = replace(pin, conversion_basis_quote=quote)
    else:
        blob += b' conversion terms may change due to a split. close is adjusted.'
    pin = replace(pin, sha256=hashlib.sha256(blob).hexdigest())
    with pytest.raises(ValueError, match='unsupported'):
        P.read_economic(blob, pin)
