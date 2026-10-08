"""Native source admission for golden research; no import-time warehouse IO.

Plans are independently reviewed pins, never generated from an active manifest.
An arbitrary Parquet/manifest pair is deliberately not an admission interface.
"""
from __future__ import annotations

import hashlib
import importlib
import json
from pathlib import Path
from . import common as C


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"), default=str).encode()).hexdigest()


def pin(path):
    path = Path(path)
    before = path.stat()
    result = {"sha256": C.sha256_file(path), "bytes": before.st_size}
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("changed input during hash")
    return result


def member(root, relative):
    root = Path(root).resolve()
    path = (root / relative).resolve()
    if Path(relative).is_absolute() or ".." in Path(relative).parts or not path.is_relative_to(root):
        raise ValueError("invalid immutable member")
    return path


def check_pins(pins):
    if not pins:
        raise ValueError("independent complete input/code pins required")
    for name, expected in pins.items():
        if not Path(name).is_absolute() or pin(name) != expected:
            raise ValueError("stale independent input/code pin: " + name)


UNSUPPORTED = {
    "standardized_v11": "final qualified fundamental leaf dependency and public full reader replay pending",
    "ratios_v11": "final qualified fundamental leaf dependency and public full reader replay pending",
    "factors": "R21 construction reviewed; actual native source/economic gates open",
    "characteristics": "R21 construction reviewed; qualified complete native generation admission pending",
    "earnings_calendar_v2": "full immutable source/public validation reader interface pending",
    "ea_actuals": "phase-aware publication exists; complete native public reader replay pending",
    "thirteenf_features": "native originals explicitly unqualified; cohort/source/link/denominator admission pending",
    "delisting_v2": "native economic proceeds/complete holding convention admission pending",
    "short_interest": "complete independently admitted native reader generation pending",
    "short_volume_ext": "complete independently admitted native reader generation pending",
    "events": "complete independently admitted native reader generation pending",
    "text_features": "Task15 native reader implemented; independent review, actual source qualification and exact golden field mapping pending",
    "fundamentals_notes": "R12 period/proof/context inventory independent admission pending",
}


def admit(name, spec, con):
    """Run actual complete native validation, with independent identities on both sides.

expected_validation_sha256 authenticates the full native result; dependency_pins
must include every native frozen input, documentary source, code and metadata.
The native validator additionally establishes exact inventories and replays.
    """
    if name in {'reference_french','reference_fred'}:
        from . import private_tier1_reference_sources as N
        version=N.VERSION if name=='reference_french' else N.FRED_VERSION
        if spec.get('interface_version')!=version:
            raise ValueError('reviewed fixed complete reference source interface required')
        return N.admit(spec,con)
    if name=='fundamentals_notes':
        from . import private_tier1_notes_sources as N
        if spec.get('interface_version')!=N.VERSION:
            return {'status':'excluded','reason':'reviewed fixed R12 notes interface required','products':{}}
        return N.admit(spec,con)
    if name in {'short_interest','short_volume_ext'}:
        from . import private_tier1_short_sources as N
        if spec.get('interface_version')!=N.VERSION:
            return {'status':'excluded','reason':'reviewed fixed original short-source interface required','products':{}}
        return N.admit(name,spec,con)
    if name in {'text_features','classification_tnic','delisting_v2','distributions','events_buyback'}:
        from . import private_tier1_family_sources as F
        if spec.get('interface_version')!=F.VERSION:
            return {'status':'excluded','reason':'reviewed fixed native family interface required','products':{}}
        return F.admit(name,spec,con)
    if name in {"standardized_v11", "ratios_v11", "earnings_calendar_v2", "ea_actuals"} and spec.get("interface_version") == "tier1-native-readers-v1":
        if not spec.get("independent_review"):
            raise ValueError("Task17 native reader independent review prerequisite")
        from . import tier1_native_readers as I
        return I.admit(name, spec, con)
    if name in UNSUPPORTED:
        return {"status": "excluded", "reason": UNSUPPORTED[name], "products": {}}
    if name not in {"universe_release_v1", "fundamentals_release_v1", "events_guidance_v2", "char_monthly"}:
        raise ValueError("unsupported native source adapter")
    check_pins(spec["dependency_pins"])
    root, generation = Path(spec["root"]).resolve(), Path(spec["generation"]).resolve()
    if generation.parent != root / "generations":
        raise ValueError("native immutable generation required")
    manifest_path = generation / "manifest.json"
    if pin(manifest_path) != spec["manifest_pin"]:
        raise ValueError("changed independently expected manifest")
    if name == "universe_release_v1":
        from . import universe_release as U
        result = U.validate_generation(Path(spec["lake_root"]), generation, con)
        snapshot = C.read_json(generation / "snapshot.json")
        dependencies = {str(member(Path(spec["lake_root"]), p)): v for p, v in snapshot["files"].items()}
        dependencies.update({str(member(Path(spec["lake_root"]), p)): v for p, v in snapshot["manifests"].items()})
        dependencies.update(snapshot["code"])
        dependencies.update({v["path"]: {k: v[k] for k in ("sha256", "bytes")} for v in snapshot["external"].values()})
        products = {f"universe_{year}": generation / f"year={year}/universe.parquet" for year in U.REQUIRED}
        products.update({name: [member(Path(spec["lake_root"]), p) for p in item["files"]]
                         for name, item in snapshot["products"].items()})
    elif name == "fundamentals_release_v1":
        from . import fund_batch_release as F
        from . import fund_release_runtime as RT
        if not spec.get("extract_admission") or not spec.get("expected_admission_sha256"):
            raise ValueError("qualified all85 extraction admission required")
        runtime = RT.load_runtime(Path(spec["runtime_contract"]), spec["runtime_contract_sha256"], spec["runtime_code_sha256"])
        with RT.connection(root, runtime) as (native_con, resources),F.writer_lock(root):
            proof = RT.check_proof(root, generation, runtime, spec["snapshot_sha256"])
            result = F.validate_generation(root, generation, native_con, extract_admission=Path(spec["extract_admission"]),
                expected_admission_sha256=spec["expected_admission_sha256"], force_full_replay=True)
            RT._recheck_proof(root, generation, runtime, spec["snapshot_sha256"], proof)
        # Full native frozen dependency traversal belongs to the native validator;
        # additionally bind its independently supplied complete snapshot content.
        snapshot = C.read_json(generation / "snapshot.json")
        if digest(snapshot) != spec["snapshot_sha256"]:
            raise ValueError("independently expected financial snapshot mismatch")
        from . import private_tier1_accounting as accounting
        dependencies=accounting.financial_dependencies(root,generation,result,snapshot,runtime,proof)
        products = {"qualified_fundamental_leaf": generation / F.SEMANTIC_STAGE}
    elif name == "char_monthly":
        from . import char_monthly as M, char_monthly_sources as N, fund_release_runtime as RT, fund_batch_release as F
        lake = Path(spec["lake_root"])
        financial=spec.get('financial')
        if not financial:raise ValueError('complete underlying R23 financial admission/proof required')
        admitted_financial=admit('fundamentals_release_v1',financial,con)
        frozen_binding=C.read_json(generation/'snapshot.json')['bindings']
        if (Path(financial['generation']).resolve()!=member(lake,frozen_binding['fundamentals_generation']) or
            financial['manifest_pin']!=frozen_binding['fundamentals_manifest'] or
            any(financial[k]!=spec[k] for k in ('runtime_contract','runtime_contract_sha256','runtime_code_sha256'))):
            raise ValueError('monthly runtime proof differs from exact underlying financial generation')
        runtime = RT.load_runtime(Path(spec["runtime_contract"]), spec["runtime_contract_sha256"], spec["runtime_code_sha256"])
        # Its reader invokes the full financial validator: R23 requires the
        # separately pinned 256MB native protocol, never the golden128MB reader.
        with RT.connection(lake/"fundamentals_release_v1", runtime) as (native_con, resources),F.writer_lock(lake/F.STAGE):
            proof=RT.check_proof(lake/'fundamentals_release_v1',Path(financial['generation']),runtime,financial['snapshot_sha256'])
            result = M.validate_generation(lake, generation, native_con)
            frozen = C.read_json(generation / "snapshot.json")
            snapshot = N.admit(lake, frozen["bindings"], native_con)
            if digest(snapshot) != spec["snapshot_sha256"]:
                raise ValueError("independent monthly complete source identity mismatch")
            dependencies = {str(member(lake, p)): v for p, v in snapshot["files"].items()}
            dependencies.update({str(member(lake, p)): v for p, v in snapshot["manifests"].items()})
            dependencies.update(snapshot["code"])
            products = {relative: generation / relative for relative in result["files"]}
            for product_name in ("cap_quality","price_quality","daily_factors","prices"):
                products["_"+product_name]=[member(lake,p) for p in snapshot["products"][product_name]["files"]]
            import datetime as dt
            for date in result["months"]:
                date = dt.date.fromisoformat(date)
                M.native_month(native_con, date)
                path = generation / f"month={date:%Y-%m}/char_monthly.parquet"
                native_con.execute(f"CREATE OR REPLACE TEMP VIEW gm_published AS SELECT * FROM {N.sql([path])}")
                if native_con.execute("SELECT count(*) FROM ((SELECT * FROM gm_published EXCEPT ALL SELECT * FROM cm_output) UNION ALL (SELECT * FROM cm_output EXCEPT ALL SELECT * FROM gm_published))").fetchone()[0]:
                    raise ValueError("monthly full native source replay differs")
            RT._recheck_proof(lake/'fundamentals_release_v1',Path(financial['generation']),runtime,financial['snapshot_sha256'],proof)
            dependencies.update(financial['dependency_pins'])
    else:
        from . import guidance_native as G, guidance_publication as P
        result = G.validate(spec["native_bindings"], root, manifest_path)
        manifest = P.validate(root, manifest_path)
        if result["phase"] not in ("features", "validated"):
            raise ValueError("guidance feature phase required")
        from . import guidance_sources as S
        native_identity = S.bind(spec["native_bindings"])
        if digest(native_identity) != spec["native_identity_sha256"]:
            raise ValueError("independent complete guidance identity mismatch")
        dependencies = {str(manifest_path): spec["manifest_pin"]}
        dependencies.update({str(Path(p).resolve()):pin(p) for p in native_identity['files'].values()})
        dependencies.update({str(Path(r['path']).resolve()):pin(r['path']) for r in native_identity['bindings'].values()})
        for module in ('guidance','guidance_contract','guidance_kernels','guidance_sources','guidance_publication','guidance_native','ea_publication','ea_actuals','events_sources','common'):
            path=Path(importlib.import_module('.'+module,__package__).__file__).resolve()
            dependencies[str(path)]=pin(path)
        # Retained original documents and their independently selected receipts
        # are dependencies, rather than just the catalog that names them.
        from . import events_sources as ES
        catalog=Path(native_identity['files']['catalog'])
        import pyarrow.parquet as pq
        receipt_inventory=spec['document_receipts']
        for batch in pq.ParquetFile(catalog).iter_batches(batch_size=64,columns=['doc_sha256','doc_source']):
            for document in batch.to_pylist():
                sha=document['doc_sha256'];source=document['doc_source']
                docroot=ES.EARNINGS_RELEASE_DIR if source=='landed_v2' else ES.RAW_DIR if source=='sec_events' else None
                if docroot is None:raise ValueError('unsupported original guidance document family')
                paths=[docroot/'objects'/sha[:2]/(sha+suffix) for suffix in ('.gz','')]
                retained=next((p for p in paths if p.is_file()),None)
                if retained is None:continue # native full population retains missing-document exclusions
                blob=G.read_document(document)
                if blob is None or hashlib.sha256(blob).hexdigest()!=sha:raise ValueError('original guidance content identity mismatch')
                dependencies[str(retained.resolve())]=pin(retained)
                receipts=receipt_inventory.get(sha)
                if not receipts:raise ValueError('original guidance receipt dependency pending')
                for path,expected in receipts.items():
                    actual=pin(path)
                    if actual!=expected:raise ValueError('original guidance receipt changed')
                    dependencies[str(Path(path).resolve())]=actual
        products = {p: member(root, relative) for p, relative in manifest["products"].items()}
    if any(spec["dependency_pins"].get(p) != v for p, v in dependencies.items()):
        raise ValueError("native dependency omitted from independent complete pins")
    if digest(result) != spec["expected_validation_sha256"]:
        raise ValueError("independent native validation identity mismatch")
    check_pins(spec["dependency_pins"])
    if pin(manifest_path) != spec["manifest_pin"]:
        raise ValueError("native generation changed during admission")
    complete=dict(spec['dependency_pins'])
    if name in {'universe_release_v1','char_monthly','events_guidance_v2'}:
        # These fixed original validators already authenticate complete files
        # against the independently expected manifest. Preserve their expected
        # identities instead of adopting current bytes after admission.
        current_manifest=C.read_json(manifest_path)
        base=root if name=='events_guidance_v2' else generation
        for relative,expected in current_manifest['files'].items():
            path=member(base,relative)
            if pin(path)!=expected:raise ValueError('native published product differs from admitted manifest')
            if str(path) in complete and complete[str(path)]!=expected:
                raise ValueError('independent native product identity conflicts with manifest')
            complete[str(path)]=expected
    if pin(manifest_path)!=spec['manifest_pin']:
        raise ValueError('native manifest changed during consumed-product binding')
    return {"status": "admitted_diagnostic", "products": products, "validation": result,
            "source_quality_certified": False, "spec": spec,"dependency_pins":complete}


def admitted_pins(admitted):
    """Actual complete consumed native products, distinct from source assertions.

    Only fixed native admissions produce these product maps. Their full native
    replay authenticates the output inventory against independently expected
    sources and manifest; this inventory is then sealed in the golden snapshot.
    """
    result={}
    for row in admitted.values():
        if row['status']=='excluded':continue
        for inventory in (row.get('spec',{}).get('dependency_pins',{}),row.get('dependency_pins',{})):
            for path,expected in inventory.items():
                if path in result and result[path]!=expected:
                    raise ValueError('conflicting independently admitted source identity')
                result[path]=expected
        for key,value in row['products'].items():
            for product in value if isinstance(value,(tuple,list)) else [value]:
                path=Path(product).resolve(strict=True)
                if path.is_file():
                    actual=pin(path)
                    if result.get(str(path))!=actual:
                        raise ValueError('consumed native product missing or differs from admitted identity')
                    continue
                if key!='qualified_fundamental_leaf':raise ValueError('unsupported native product directory')
                # The complete R23 dependency traversal supplies all leaf bytes.
                manifest=path/'manifest.json'
                if str(manifest) not in result or pin(manifest)!=result[str(manifest)]:
                    raise ValueError('full qualified financial leaf dependency inventory missing')
    check_pins(result)
    return result


def code_pins():
    names = ("golden_sources", "golden_panel", "golden_labels", "golden_evaluation", "golden_export", "golden_registry", "golden_returns")
    pins = {str(Path(importlib.import_module("." + n, __package__).__file__).resolve()):
            pin(importlib.import_module("." + n, __package__).__file__) for n in names}
    docs = Path(__file__).parents[3]/"docs"/"datasets"
    for name in ("evaluation-config.json", "golden_field_registry.json", "golden_notes_inventory.json",'golden_profitability_requirements.json'):
        pins[str((docs/name).resolve())] = pin(docs/name)
    return pins


def generation_pins(generation):
    """Complete actual products, metadata, code and native reader dependencies.

Used on both sides of every public read. The immutable native snapshot supplies
the inventory; an unchanged adjacent manifest alone never authenticates a read.
    """
    generation=Path(generation).resolve()
    manifest=C.read_json(generation/'manifest.json')
    result={str(generation/'manifest.json'):pin(generation/'manifest.json')}
    for group in ('files','receipts'):
        for relative,identity in manifest.get(group,{}).items():
            path=member(generation,relative)
            if pin(path)!=identity:raise ValueError('changed immutable golden payload/receipt')
            result[str(path)]=identity
    snapshot=generation/'snapshot.json'
    if snapshot.exists():
        result[str(snapshot)]=pin(snapshot); frozen=C.read_json(snapshot)
        if manifest.get('snapshot')!=result[str(snapshot)]:raise ValueError('changed immutable golden snapshot')
        if 'plan' in frozen:
            result.update(frozen.get('source_pins',{}))
            result[frozen['plan_path']]=frozen['plan_pin']
            result.update(frozen['plan']['code'])
            for spec in frozen['plan']['sources'].values():result.update(spec.get('dependency_pins',{}))
            for spec in frozen['plan']['sources'].values():
                if spec.get('financial'):result.update(spec['financial']['dependency_pins'])
                if spec.get('interface_version') in ('tier1-notes-sources-v1','tier1-short-sources-v1'):
                    mapped=Path(spec['mapped_generation']).resolve()
                    result[str(mapped/'manifest.json')]=spec['mapped_manifest_pin']
                    for relative,identity in spec['mapped_product_pins'].items():
                        result[str(member(mapped,relative))]=identity
        else:
            result.update(frozen['code'])
            if frozen.get('returns_binding'):result.update(frozen['returns_binding']['dependency_pins'])
            source=member(Path(frozen['golden_stage'])/'generations',frozen['golden_generation'])
            result.update(generation_pins(source))
    result.update(code_pins());check_pins(result)
    return result


def curated_notes():
    path = Path(__file__).parents[3]/"docs"/"datasets"/"golden_notes_inventory.json"
    doc = C.read_json(path)
    if doc.get("reviewed_snapshot") != "c23def1a" or len(doc["inventory"]) != 9:
        raise ValueError("unsupported initial curated proof inventory")
    return doc["inventory"]


def notes_admission(row, proof, inventory, cutoff):
    """R12: curated reportable identity is distinct from revenue/geography proof."""
    accession = row.get("bseg_partition_source_adsh")
    expected = inventory.get(accession)
    original = curated_notes().get(accession)
    if not original or not expected or any(expected.get(k) != original.get(k) for k in ("primary_sha256", "role", "concept", "members", "basis")):
        return None, "outside_nine_reviewed_curated_proofs"
    if row.get("field") in {"tax_dtl", "foreign_sales_share", "major_cust_share"}:
        return None, "ambiguous_note_or_unverified_geography"
    if not expected or not expected.get("period") or not expected.get("proof_sha256"):
        return None, "missing_independent_curated_period_proof_pin"
    # The unchanged native structural reader records an empty/proposed selection
    # on every verified primary. Only recon_overlay's applied-selection marker
    # changes the measure; retain that marker and its five R12 exclusions.
    if "selected_external_revenue" in proof:
        return None, "automatic_external_selection_withheld"
    if digest(proof) != expected["proof_sha256"] or row.get("period") != expected["period"]:
        return None, "proof_or_period_mismatch"
    corroboration = proof.get("corroboration", {})
    for key in ("primary_sha256", "role", "concept", "members", "basis"):
        if corroboration.get(key) != expected.get(key):
            return None, "curated_source_identity_mismatch"
    if row.get("source_sha256") != expected["primary_sha256"] or row.get("source_accession") != accession:
        return None, "source_accession_sha_mismatch"
    if row.get("available_at") is None or row["available_at"] >= cutoff:
        return None, "not_available"
    if row.get("field") == "bseg_n":
        return len(expected["members"]), "curated_reportable_count"
    if row.get("field") != "bseg_rev_hhi":
        return None, "unsupported_certified_note"
    contexts = expected.get("revenue_context_admission")
    if not contexts or row.get("revenue_context_admission") != contexts:
        return None, "independent_revenue_context_bridge_basis_pending"
    values = row.get("member_values", [])
    total, bridge = row.get("total"), row.get("bridge")
    if (len(values) != len(expected["members"]) or any(v is None or v <= 0 for v in values)
        or total is None or total <= 0 or bridge is None or abs(sum(values) + bridge - total) > 1e-8 * total):
        return None, "revenue_arithmetic_or_context_mismatch"
    return sum(v*v for v in values) / sum(values)**2, "reported_segment_measure_hhi"
