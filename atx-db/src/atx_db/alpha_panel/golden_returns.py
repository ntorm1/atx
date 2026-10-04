"""Fixed native conditional holding-window reader, extending reviewed Task8 semantics.

Original vendor prices/economic blocks use Task12's native readers; action rows
use Task8's complete independently reviewed price/split/cash source protocol.
Task13 full native distribution replay qualifies any positive cash event.
Task12 exact-native proceeds and separately proved residual-cash observations
are required for a terminal window. No legacy ret or caller boolean is proof.
"""
from __future__ import annotations
import dataclasses
import datetime as dt
import importlib
from pathlib import Path
from . import common as C, golden_sources as S
from . import char_replication_native as N, char_monthly_evidence as E
from . import mna_price_native_v2 as MP, mna_exact_v2 as MX, mna_native_v2 as MN, terminal_mna_v2 as T
from .golden_labels import guard, HORIZONS, label_window

VERSION="golden-native-windows-v1"


def native_code():
    names=("golden_returns","golden_labels","common","release_inputs","char_replication_native",
        "char_monthly_evidence","char_monthly_registry","char_monthly_kernels","mna_price_native_v2",
        "mna_exact_v2","mna_native_v2","mna_financial_v2","mna_vintages_v2","mna_runtime_v2",
        "terminal_mna_v2","distributions","distributions_native","distributions_ancillary","distributions_qualification")
    modules=[importlib.import_module("."+name,__package__) for name in names]
    return {str(Path(module.__file__).resolve()):S.pin(module.__file__) for module in modules}


def typed(kind,record):
    """Exact native dataclasses; unknown/missing fields reject instead of defaulting."""
    if set(record)!={f.name for f in dataclasses.fields(kind)}:
        raise ValueError("complete exact native typed pin required")
    record=dict(record)
    for name in ("valid_from","valid_to","session_date","last_session"):
        if name in record:record[name]=dt.date.fromisoformat(record[name])
    for name in ("available_at","fetched_at"):
        if name in record:
            record[name]=dt.datetime.fromisoformat(record[name].replace("Z","+00:00"))
            if record[name].tzinfo is None:raise ValueError("original native UTC clock required")
    return kind(**record)


class NativeWindows:
    def __init__(self,root,spec,con):
        self.root=Path(root).resolve();self.spec=spec;self.con=con
        if spec.get("version")!=VERSION or not spec.get("independent_review"):
            raise ValueError("independently reviewed native window bindings required")
        if spec.get("native_code")!=native_code():raise ValueError("native reader code changed")
        from .char_replication import CONFIG_SHA
        if spec.get("task8_config_sha256")!=CONFIG_SHA:raise ValueError("frozen original Task8 source conventions required")
        S.check_pins(spec["dependency_pins"])
        self.source=N.reviewed(self.root,spec["source_contract"],"char_replication_label_source_v1",spec["task8_config_sha256"])
        if self.source.get("convention")!="unadjusted_close_split_cash_per_prior_share_v1" or self.source.get("currency")!="USD":
            raise ValueError("unsupported Task8 native action/price convention")
        self.prices=N.product(self.root,self.source["prices"])
        self.actions=N.product(self.root,self.source["actions"])
        # These are the EXACT native source products in a separately reviewed
        # Task8 economic contract, not arbitrarily selected gold input Parquet.
        from .golden_panel import sql
        con.execute(f"CREATE OR REPLACE TEMP VIEW golden_native_actions AS SELECT * FROM {sql(self.actions)}")
        columns=dict((r[0],r[1]) for r in con.execute("DESCRIBE golden_native_actions").fetchall())
        if any(k not in columns for k in ("security_id","session_date","split_multiplier","cash_dividend","terminal_treatment")):
            raise ValueError("Task8 complete native action schema required")
        if con.execute("SELECT 1 FROM golden_native_actions GROUP BY security_id,session_date HAVING count(*)<>1 OR security_id IS NULL OR session_date IS NULL LIMIT 1").fetchone():
            raise ValueError("ambiguous complete native action inventory")
        self.inventory={}
        for record in spec["window_inventories"]:
            path=N.review_file(self.root,record)
            if S.pin(path)!=spec["dependency_pins"].get(str(path.resolve())):
                raise ValueError("window inventory omitted from independent complete pins")
            # Bounded documentary JSONL; SQLite-like keyed storage stays in the
            # file-backed128MB DuckDB connection, never a full Python dict.
            escaped=path.as_posix().replace("'","''")
            table="golden_window_inventory_"+str(len(self.inventory))
            con.execute(f"CREATE OR REPLACE TEMP TABLE {table} AS SELECT CAST(feature_date AS DATE) feature_date,CAST(security_id AS BIGINT) security_id,CAST(horizon AS INTEGER) horizon,proof_binding FROM read_json_auto('{escaped}',format='newline_delimited')")
            if con.execute(f"SELECT 1 FROM {table} GROUP BY feature_date,security_id,horizon HAVING count(*)<>1 LIMIT 1").fetchone():
                raise ValueError("duplicate independent window proof inventory")
            if con.execute(f"SELECT 1 FROM {table} WHERE feature_date>=DATE '2023-01-01' OR horizon NOT IN (5,21,63) LIMIT 1").fetchone():
                raise ValueError("window inventory outside pre2023 contract")
            self.inventory[table]=record
        self.distributions=None
        if spec.get("distributions"):
            from . import distributions as D
            d=spec["distributions"]
            target=N.review_file(self.root,d["product"])
            admission=N.review_file(self.root,d["admission"])
            validation=D.validate_product(target,admission,d["admission"]["expected_sha256"])
            if S.digest(validation)!=d["expected_validation_sha256"]:raise ValueError("independent Task13 full native validation changed")
            con.execute(f"CREATE OR REPLACE TEMP VIEW golden_native_distributions AS SELECT * FROM {sql([target])}")
            self.distributions=d
        self._required_pins()
        self.recheck()

    def _required_pins(self):
        required={str(p.resolve()):S.pin(p) for p in (*self.prices,*self.actions)}
        for record in (self.spec["source_contract"],*self.spec["window_inventories"]):
            path=N.review_file(self.root,record);required[str(path.resolve())]=record["identity"]
        required.update(native_code())
        if any(self.spec["dependency_pins"].get(p)!=identity for p,identity in required.items()):
            raise ValueError("abbreviated independent native window reader dependency inventory")

    def recheck(self):
        S.check_pins(self.spec["dependency_pins"])
        if native_code()!=self.spec["native_code"]:raise ValueError("native loaded source reader changed")

    def proof(self,feature,sid,h):
        found=[]
        for table in self.inventory:
            found.extend(self.con.execute(f"SELECT proof_binding FROM {table} WHERE feature_date=? AND security_id=? AND horizon=?",[feature,sid,h]).fetchall())
        if len(found)>1:raise ValueError("window proof duplicated across inventory units")
        if not found:return None
        binding=found[0][0]
        proof=E.reviewed_metadata(self.root,binding,"golden_complete_window_v1")
        self.require_record(binding)
        return proof

    def require_record(self,record,maximum=8*1024**2):
        path=N.review_file(self.root,record)
        if self.spec["dependency_pins"].get(str(path.resolve()))!=record["identity"]:
            raise ValueError("native original/proof omitted from independent full inventory")
        if path.stat().st_size>maximum:raise ValueError("native documentary byte bound")
        return path

    def price(self,item,sid,date):
        vendor=N.review_file(self.root,item["vendor_original"])
        if vendor not in self.prices:raise ValueError("native vendor outside exact Task8 admitted price products")
        if self.spec["dependency_pins"].get(str(vendor.resolve()))!=item["vendor_original"]["identity"]:
            raise ValueError("native original price omitted from full pins")
        economic=self.require_record(item["economic_original"]).read_bytes()
        price_pin=typed(MP.VendorPricePin,item["vendor_pin"])
        economic_pin=typed(MP.EconomicPin,item["economic_pin"])
        if price_pin.security_id!=sid or price_pin.session_date!=date:
            raise ValueError("native price keyed date/security mismatch")
        result=MP.read_vendor_price(vendor,price_pin,economic,economic_pin,"independently-reviewed-golden-window")
        if result.evidence.available_at.date()>=dt.date(2023,1,1):
            raise ValueError("native historical price/economic clock crosses pre2023 evaluation seal")
        return result

    def terminal(self,item,sid,last_session):
        admission=item.get("terminal_native")
        if not admission:return None,"native_exact_delisting_admission_missing"
        document=self.require_record(admission["document_original"]).read_bytes()
        economic=self.require_record(admission["target_economic_original"]).read_bytes()
        vendor=N.review_file(self.root,admission["vendor_original"])
        if vendor not in self.prices:raise ValueError("terminal price outside native price inventory")
        candidate=typed(T.Candidate,admission["candidate"])
        if candidate.security_id!=sid or candidate.last_session!=last_session:raise ValueError("terminal candidate/date/security mismatch")
        # No generic typed terms/proceeds supplied by callers: the financial,
        # native SEC clock, source-price and economic readers reconstruct them.
        extra={}
        if admission.get("acquirer_pin"):
            extra.update(acquirer_pin=typed(MP.VendorPricePin,admission["acquirer_pin"]),
                         acquirer_economic=self.require_record(admission["acquirer_economic_original"]).read_bytes(),
                         acquirer_economic_pin=typed(MP.EconomicPin,admission["acquirer_economic_pin"]))
        if admission.get("bulk_clock"):
            b=admission["bulk_clock"]
            extra["bulk_clock"]=(self.require_record(b["bulk_original"]).read_bytes(),typed(MN.BulkClockPin,b["bulk_pin"]),
                (self.require_record(b["submission_original"]).read_bytes(),typed(MN.NativePin,b["submission_pin"])))
            extra["clock_receipts"]={key:self.require_record(record).read_bytes() for key,record in admission["clock_receipts"].items()}
        result=MX.replay_exact(candidate,document,typed(MN.NativePin,admission["document_pin"]),vendor,
            typed(MP.VendorPricePin,admission["target_pin"]),economic,typed(MP.EconomicPin,admission["target_economic_pin"]),
            expected=admission["expected_exact"],**extra)
        if not result.dlret_exact or result.dlret is None or result.reason:return None,"native_delisting_economic_proceeds_unqualified"
        if result.settlement_date>=dt.date(2023,1,1) or result.available_at.date()>=dt.date(2023,1,1):
            raise ValueError("native terminal settlement/proceeds observation violates seal")
        return result,None

    def window(self,calendar,index,sid,h):
        feature=calendar[index]
        if index+h+1>=len(calendar):return {"value":None,"reason":"incomplete_calendar_window"}
        entry,exit=calendar[index+1],calendar[index+h+1]
        if exit>=dt.date(2023,1,1):return {"value":None,"reason":"window_crosses_pre2023_seal"}
        guard(feature,entry,exit,calendar[index+2:index+h+2])
        proof=self.proof(feature,sid,h)
        if not proof:return {"value":None,"reason":"missing_independent_complete_window_proof"}
        if (proof.get("feature_date"),proof.get("security_id"),proof.get("horizon"),proof.get("entry_date"),proof.get("exit_date"))!=(str(feature),sid,h,str(entry),str(exit)):
            raise ValueError("independent next-close/horizon/window identity mismatch")
        observations=proof["observations"]
        if [r["session_date"] for r in observations]!=[str(d) for d in calendar[index+2:index+h+2]]:
            raise ValueError("incomplete actual calendar holding observations")
        start=self.price(proof["start_mark"],sid,entry)
        prior=float(start.close);native={};cash_state=False
        availability=max(start.evidence.available_at,dt.datetime.combine(exit,dt.time(22),tzinfo=dt.timezone.utc))
        for item,date in zip(observations,calendar[index+2:index+h+2]):
            if cash_state:
                cash=item.get("residual_cash")
                if not cash:return {"value":None,"reason":"missing_residual_cash_native_observation"}
                cashproof=E.reviewed_metadata(self.root,cash,"golden_residual_cash_observation_v1");self.require_record(cash)
                source=self.require_record(cashproof["original"])
                # Fixed literal native cash account observation: source-backed
                # dated USD factor, no fabricated flat-zero residual proceeds.
                original=C.read_json(source)
                if set(original)!={"session_date","security_id","currency","cash_factor","available_at"} or original["session_date"]!=str(date) or original["security_id"]!=sid or original["currency"]!="USD" or original!=cashproof["observation"]:
                    raise ValueError("native residual cash source/key/currency mismatch")
                clock=dt.datetime.fromisoformat(original["available_at"])
                if clock.tzinfo is None or clock.date()>=dt.date(2023,1,1):raise ValueError("residual cash observation crosses seal")
                availability=max(availability,clock)
                factor=original["cash_factor"]
                if not N.finite(factor) or factor<0:return {"value":None,"reason":"invalid_native_residual_cash_factor"}
                native[date]={"source_qualified":True,"cash_return":factor-1,"cash_return_qualified":True};continue
            action_source=self.require_record(item["corporate_action_source"],maximum=2**63-1)
            if action_source not in self.actions:raise ValueError("action outside Task8 independent native source contract")
            from .golden_panel import sql
            actions=self.con.execute(f"SELECT split_multiplier,cash_dividend,terminal_treatment FROM {sql([action_source])} WHERE security_id=? AND session_date=?",[sid,date]).fetchall()
            if len(actions)!=1:return {"value":None,"reason":"missing_or_ambiguous_native_action_observation"}
            split,cash,terminal=actions[0]
            if not N.finite(split) or split<=0 or not N.finite(cash) or cash<0:return {"value":None,"reason":"native_split_cash_basis_unqualified"}
            if cash>0:
                if not self.distributions:return {"value":None,"reason":"Task13_native_cash_source_admission_pending"}
                matched=self.con.execute("SELECT event_id FROM golden_native_distributions WHERE security_id=? AND amount=? AND currency='USD' AND source_qualification='native_fixed_regular_event' AND event_id=?",[sid,cash,item.get("distribution_event_id")]).fetchall()
                if len(matched)!=1:return {"value":None,"reason":"cash_amount_class_native_distribution_mismatch"}
                exproof=item.get("cash_ex_date_proof")
                if not exproof:return {"value":None,"reason":"independent_cash_ex_date_identity_pending"}
                exdoc=E.reviewed_metadata(self.root,exproof,"golden_cash_ex_date_v1");self.require_record(exproof)
                original=self.require_record(exdoc["original"]).read_text(encoding="utf-8")
                if exdoc.get("security_id")!=sid or exdoc.get("ex_date")!=str(date) or exdoc.get("distribution_event_id")!=item["distribution_event_id"] or exdoc.get("quote") not in original or str(date) not in exdoc.get("quote",""):
                    raise ValueError("original cash ex-date/distribution proof mismatch")
            if terminal=="none":
                current=self.price(item,sid,date)
                availability=max(availability,current.evidence.available_at)
                ret=(float(current.close)*split+cash)/prior-1
                prior=float(current.close)
                native[date]={"ret":ret,"source_qualified":True}
            elif terminal=="cash_conversion":
                result,reason=self.terminal(item,sid,calendar[calendar.index(date)-1])
                if reason:return {"value":None,"reason":reason}
                if result.settlement_date>date:return {"value":None,"reason":"cash_settlement_not_complete_at_observation"}
                availability=max(availability,result.available_at)
                # Proceeds replace the prior price directly; never multiply a
                # last-price return by unrelated proceeds with incompatible basis.
                native[date]={"ret":float(result.proceeds_per_share)/prior-1,"source_qualified":True}
                cash_state=True
            else:return {"value":None,"reason":"unsupported_terminal_stock_or_mixed_residual_convention"}
        result=label_window(calendar,index,h,native)
        result["delisting_adjusted"]=cash_state
        result["proof_sha256"]=S.digest(proof)
        result["source_contract_sha256"]=self.spec["source_contract"]["expected_sha256"]
        result['available_at']=availability
        self.recheck()
        return result
