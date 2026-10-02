"""The research field builder with the lane-YDATA field modules registered (platform v8, draft).

The draft fields are registered here, never in ``prepare_research_fields.py``: the plain builder, every v8 field list
and every existing field's formula and producer fingerprint stay exactly as they are, so no v8 reuse count can move.
Run this file with the builder's own arguments (same argv, same manifest layout, the builder's own code identity in the
manifest; each draft field's entry names its module as ``producer``):

  python atx-engine/tools/prepare_research_fields_ydata.py <the base build's argv> --price-source <the role's
      TickerHistory3> --mgr13f-stage <thirteenf/> --mgr13f-stage-sha256 <pin>
      --fields <the base list>,iv_skew_21,stio_chg_q,div_init_omit,deal_pending --reuse <the base fields directory> ...

``FIELDS_IVSHAPE_DRAFT``: the option-smile slope field of research_fields_ivshape.py (``--price-source``).
``FIELDS_MGR13F_DRAFT``: the 13F manager-horizon field of research_fields_mgr13f.py (``--mgr13f-stage`` and its pin;
it requires ``shares_out``, which requires ``si_shares``, in the same run).
``FIELDS_DIVEVENT_DRAFT``: the dividend initiation / omission field of research_fields_divevent.py (``--price-source``).
``FIELDS_DEALS_DRAFT``: the pending-merger target field of research_fields_deals.py (the SEC module's ``--sec-stages``,
``--sec-filings-sha256`` and ``--sec-identity-bridge(-sha256)``).
``register`` binds each module into a builder namespace exactly as the builder binds its own modules (``bind`` appends
its ``FIELDS`` to ``ALL_FIELDS``; the module object joins ``FIELD_MODULES``, whose hooks then check, reuse and compute
its fields). At integration it folds into the draft entry of record as further tuple elements.
"""
from __future__ import annotations

import prepare_research_fields as builder  # same directory: the builder (it does not import this module)
import research_fields_deals                # same directory
import research_fields_divevent             # same directory
import research_fields_ivshape              # same directory
import research_fields_mgr13f               # same directory

DRAFT_MODULES = (research_fields_ivshape, research_fields_mgr13f, research_fields_divevent, research_fields_deals)
FIELDS_IVSHAPE_DRAFT = ("iv_skew_21",)
FIELDS_MGR13F_DRAFT = ("stio_chg_q",)
FIELDS_DIVEVENT_DRAFT = ("div_init_omit",)
FIELDS_DEALS_DRAFT = ("deal_pending",)


def register(host_namespace: dict) -> list:
    """Bind every draft module into the builder namespace ``host_namespace`` (``vars(prepare_research_fields)``) and
    append it to its ``FIELD_MODULES``; a module already registered there is left as it is. Returns the bound module
    objects."""
    modules = host_namespace["FIELD_MODULES"]
    bound = []
    for module in DRAFT_MODULES:
        present = [m for m in modules if type(m).__module__ == module.__name__]
        if present:
            bound += present
            continue
        m = module.bind(host_namespace)
        modules.append(m)
        bound.append(m)
    return bound


def main(argv=None):
    register(vars(builder))
    builder.main(argv)


if __name__ == "__main__":
    main()
