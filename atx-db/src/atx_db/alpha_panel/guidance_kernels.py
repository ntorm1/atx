"""Strict missing-aware guidance comparability and event-time features."""
from __future__ import annotations
import datetime as dt,math
KEY=("cik","measure","basis","accounting_basis_id","period_type","fiscal_year","fiscal_quarter","period_end","fiscal_calendar_id","currency","unit","share_basis","share_basis_id")
def clock(value):
 if isinstance(value,str):value=dt.datetime.fromisoformat(value)
 if value is None:return None
 return value.replace(tzinfo=dt.UTC) if value.tzinfo is None else value.astimezone(dt.UTC)
def finite(value):return isinstance(value,(float,int)) and not isinstance(value,bool) and math.isfinite(value)
def midpoint(row):
 if row.get("qualification_reason")!="qualified":return None
 if row.get("value_type")=="point" and finite(row.get("point")):return row["point"]
 if row.get("value_type")=="range" and finite(row.get("low")) and finite(row.get("high")) and row["low"]<=row["high"]:return (row["low"]+row["high"])/2
 return None

def comparable(a,b):return all(a.get(k)==b.get(k) for k in KEY) and all(a.get(k) is not None for k in KEY if k!="fiscal_quarter")
def affects(row,observation):
 # NULL denotes an explicitly unknown scope within the source-declared target,
 # not a qualified mapping. Complete conflicting proofs retain their union.
 for identity in observation.get("comparator_identities") or []:
  if all(identity.get(key) is None or row.get(key)==identity[key] for key in KEY):return True
 return False

def prior(row,history):
 tau=clock(row.get("available_at"))
 if tau is None:return None,"issue_clock_unqualified"
 candidates=[r for r in history if (comparable(row,r) or affects(row,r)) and clock(r.get("available_at")) is not None and clock(r["available_at"])<tau]
 if not candidates:return None,"no_comparable_prior_guidance"
 latest=max(clock(r["available_at"]) for r in candidates)
 latest_rows=[r for r in candidates if clock(r["available_at"])==latest]
 # A rejected or ambiguous most-recent original issue must not fall back to an older good row.
 signatures={(midpoint(r),r.get("qualification_reason"),r.get("accession")) for r in latest_rows}
 if len(signatures)!=1 or midpoint(latest_rows[0]) is None:return None,"latest_prior_ambiguous_or_unqualified"
 return latest_rows[0],"qualified"

def revision(row,history):
 previous,reason=prior(row,history);value=midpoint(row)
 return {"guid_revision":value-midpoint(previous) if previous is not None and value is not None else None,
         "prior_accession":previous.get("accession") if previous else None,"feature_reason":reason if value is not None else "current_guidance_no_supported_midpoint"}

def surprise(actual,history,price,previous_session):
 previous,reason=prior(actual,history)
 result={"guid_surprise":None,"prior_accession":previous.get("accession") if previous else None,"feature_reason":reason}
 tau=clock(actual.get("available_at"))
 if previous is None:return result
 if actual.get("qualification_reason")!="qualified" or not finite(actual.get("actual_value")):
  result["feature_reason"]="actual_basis_or_value_unqualified";return result
 if not price or previous_session is None:
  result["feature_reason"]="price_basis_missing";return result
 if clock(price.get("close_at")) is None or clock(price["close_at"])>=tau or price.get("session_date")!=previous_session or clock(price.get("available_at")) is None or clock(price["available_at"])>=tau:
  result["feature_reason"]="price_clock_or_session_unqualified";return result
 if price.get("basis_id")!=actual.get("share_basis_id") or price.get("currency")!=actual.get("currency") or not finite(price.get("close")) or price["close"]<=0 or price.get("qualification_reason")!="qualified":
  result["feature_reason"]="price_basis_unqualified";return result
 denominator=price["close"]
 if actual["measure"]!="EPS":result["feature_reason"]="unit_mismatch_total_revenue";return result
 result.update(guid_surprise=(actual["actual_value"]-midpoint(previous))/denominator,feature_reason="qualified")
 return result
