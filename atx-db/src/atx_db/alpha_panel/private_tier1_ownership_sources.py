"""Bounded original-source phases for the R26 ownership bridge.

The frozen native diagnostic producer remains unqualified. These phases derive
additional documentary facts from originals without altering that producer or
turning inventory/receipt assertions into ownership economics.
"""
import datetime as dt
import hashlib
import re
from urllib.parse import urlparse
import xml.etree.ElementTree as ET
from zoneinfo import ZoneInfo

from . import thirteenf_quality as Q, filing_text_native as FT
from . import private_tier1_admission as A


def original_unit_phase(source_root,rule,indexed_clock,pins):
    """Read a bounded original SEC instruction's explicit Column4 value unit.

    This is an internal documentary interpretation, not rule-bundle admission.
    The public source contract must independently bind the actual rule version
    and its application to the filing. Retrieval is a conservative evidence
    floor; a newly fetched instruction cannot acquire an earlier clock merely
    from its filename or a claimed effective date.
    """
    A.check_pins(pins)
    path=FT.member(source_root,rule['path'])
    receipt_path=FT.member(source_root,rule['receipt'])
    for actual in (path,receipt_path):
        if str(actual) not in pins:raise ValueError('complete independent unit-rule source pins required')
    receipt=FT.receipt_json(source_root,rule['receipt'])
    identity=pins[str(path)]
    url=urlparse(str(receipt.get('url','')))
    if (url.scheme!='https' or url.hostname not in {'sec.gov','www.sec.gov'}
            or not url.path.startswith(('/files/','/info/edgar/','/about/forms/'))
            or receipt.get('http_status')!=200 or receipt.get('error')
            or any(receipt.get(key)!=identity[key] for key in ('sha256','bytes'))):
        raise ValueError('original SEC unit instruction receipt differs')
    raw=FT.bounded_bytes(source_root,rule['path'],8*1024**2)
    if hashlib.sha256(raw).hexdigest()!=identity['sha256']:
        raise ValueError('consumed original unit instruction bytes differ')
    text=FT.FT.html_to_text(raw)
    # Only the explicit value-column instruction is supported. A dollar amount
    # elsewhere in the document cannot establish the information-table unit.
    sections=re.findall(r'\bColumn\s*4\b(.{0,1600}?)(?=\bColumn\s*5\b|$)',text,re.I|re.S)
    units=set()
    for section in sections:
        if not re.search(r'\bvalue\b',section,re.I):continue
        for unit in re.findall(r'\b(?:in|to\s+the\s+nearest)\s+(thousands?\s+of\s+dollars|dollars)\b',section,re.I):
            units.add('USD_THOUSANDS' if unit.lower().startswith('thousand') else 'USD')
    fetched=dt.datetime.fromisoformat(receipt['fetched_at'].replace('Z','+00:00'))
    if fetched.tzinfo is None:raise ValueError('original unit instruction retrieval clock requires timezone')
    A.check_pins(pins)
    selected=next(iter(units)) if len(units)==1 and re.search(r'\bForm\s+13F\b',text,re.I) else None
    return {'value_unit':selected,'value_multiplier':1000.0 if selected=='USD_THOUSANDS' else 1.0 if selected=='USD' else None,
            'available_at':max(fetched,indexed_clock).isoformat() if indexed_clock is not None and selected else None,
            'reason':None if selected else 'original_13F_value_column_unit_unsupported_or_ambiguous',
            'source_sha256':identity['sha256'],'source_quality_certified':False}


def dashed_accession(value):
    key=Q.accession_key(value)
    return key[:10]+'-'+key[10:12]+'-'+key[12:]


def original_position_phase(source_root,expected,retained,original,index_plan,unit_rule,pins):
    """Join authenticated original clock/unit/manager facts without a gate waiver.

    Emits every original position, including principal/options/failed cells.
    Full rule-version application, amendment/cohort completeness and dated
    security/share-denominator admission remain separate required phases.
    """
    import math
    A.check_pins(pins)
    try:
        clock=original_clock_phase(source_root,expected,retained,original,index_plan,pins)
        accepted=dt.datetime.fromisoformat(clock['available_at']) if clock['qualified_clock'] else None
        unit=original_unit_phase(source_root,unit_rule,accepted,pins)
        manager=original_manager_phase(original,clock,expected)
        rows=[]
        for position in clock['positions']:
            raw=position['value_raw'];multiplier=unit['value_multiplier']
            value=raw*multiplier if raw is not None and multiplier is not None else None
            if value is not None and (not math.isfinite(value) or value<0):
                raise ValueError('original normalized ownership value is not finite/nonnegative')
            rows.append({**position,'accession':clock['accession'],'filer_cik':int(expected['filer_cik']),
                'report_date':clock['report_date'],'source_sha256':clock['original_sha256'],
                'value_usd':value,'value_unit':unit['value_unit'],
                'available_at':unit['available_at'] if clock['qualified_clock'] else None,
                'qualified_clock':clock['qualified_clock'],
                'manager_chain_resolved':manager['manager_chain_resolved'],
                'qualified':False,'qualification_reason':'complete_rule_cohort_identity_denominator_admission_pending'})
        return {'original':clock,'unit':unit,'manager':manager,'positions':rows,
            'population':len(rows),'source_quality_certified':False,'gate_status':'UNQUALIFIED',
            'required_phases':['actual_rule_version_application','complete_amendment_manager_cohort',
                'dated_security_identity','admitted_market_share_denominator']}
    finally:A.check_pins(pins)


def original_manager_phase(original,parsed,expected):
    """Reachable single-manager/no-confidential-omission original cover route.

    Shared-manager and notice arrangements require their separate complete
    original chains. Missing source fields never imply no omission or zero.
    """
    if not isinstance(original,bytes) or len(original)>8*1024**2:
        raise ValueError('bounded original manager cover required')
    if hashlib.sha256(original).hexdigest()!=parsed['original_sha256']:
        raise ValueError('original manager cover identity differs')
    text=original.decode('utf-8',errors='replace')
    roots=[]
    for xml in re.findall(r'<XML>(.*?)</XML>',text,re.S|re.I):
        if re.search(r'<!DOCTYPE|<!ENTITY',xml,re.I):
            return {'manager_chain_resolved':False,'table_absence_qualified':False,'reason':'unsupported_original_XML_entity'}
        try:roots.append(ET.fromstring(xml.strip()))
        except ET.ParseError:
            return {'manager_chain_resolved':False,'table_absence_qualified':False,'reason':'original_manager_cover_parse_failed'}
    def values(name):
        return [node.text.strip() for root in roots for node in root.iter()
                if node.tag.rsplit('}',1)[-1]==name and node.text and node.text.strip()]
    counts=values('otherIncludedManagersCount')
    omitted=values('isConfidentialOmitted')
    ciks=[]
    for root in roots:
        for parent in root.iter():
            if parent.tag.rsplit('}',1)[-1]!='credentials':continue
            ciks.extend(child.text.strip() for child in parent if child.tag.rsplit('}',1)[-1]=='cik' and child.text)
    identity=(len(ciks)==1 and ciks[0].isdigit() and int(ciks[0])==int(expected['filer_cik']))
    no_other=(counts==['0'] and not any(row.get('other_manager') for row in parsed['positions']))
    no_confidential=(len(omitted)==1 and omitted[0].lower() in {'false','0'})
    supported=(expected['form'] in {'13F-HR','13F-HR/A'} and identity and no_other and no_confidential
               and parsed.get('cover_reconciled') is True)
    return {'manager_chain_resolved':supported,
            'table_absence_qualified':supported and parsed['table_entry_total']==0 and parsed['table_value_total_raw']==0,
            'reason':None if supported else 'original_manager_identity_shared_or_confidential_chain_unresolved',
            'source_sha256':parsed['original_sha256']}


def original_clock_phase(source_root,expected,retained,original,index_plan,pins):
    """Derive one original Accepted clock, retaining all other source obligations.

    ``index_plan`` contains exactly one native acceptance_proofs entry and its
    independently pinned original index/receipt files. Full caller source-plan
    admission remains mandatory before using this internal phase. No raw bulk Z
    label, filing date or caller Boolean is used as a public clock.
    """
    A.check_pins(pins)
    if not isinstance(original,bytes) or len(original)>8*1024**2:
        raise ValueError('bounded complete ownership original bytes required')
    if hashlib.sha256(original).hexdigest()!=retained['sha256']:
        raise ValueError('actual ownership original identity differs')
    proofs=index_plan.get('acceptance_proofs',[])
    accession=dashed_accession(expected['accession'])
    if len(proofs)!=1 or proofs[0]['accession']!=accession or int(proofs[0]['cik'])!=int(expected['filer_cik']):
        raise ValueError('exact original ownership index/filer proof required')
    for relative,identity in index_plan['files'].items():
        path=FT.member(source_root,relative)
        if pins.get(str(path))!=identity:
            raise ValueError('independent ownership index dependency omitted')
    parsed=Q.original_submission_evidence(original,expected,retained)
    accepted=FT.acceptance_proofs(source_root,index_plan).get((int(expected['filer_cik']),accession))
    reasons=set(parsed['exceptions'])
    header=parsed.get('acceptance_header_raw')
    verified=False
    if accepted is not None and header:
        identity_blockers={'original_digest_mismatch','original_sec_url_accession_unproven',
            'retrieval_clock_missing_or_invalid','original_header_accession_mismatch',
            'original_header_accession_invalid','original_header_form_mismatch',
            'complete_submission_container_missing'}
        try:
            local=dt.datetime.strptime(header,'%Y%m%d%H%M%S')
            actual_local=accepted.astimezone(ZoneInfo('America/New_York')).replace(tzinfo=None)
            fetched=dt.datetime.fromisoformat(retained['fetched_at'].replace('Z','+00:00'))
            verified=(not reasons.intersection(identity_blockers) and local==actual_local
                      and fetched.tzinfo is not None and accepted<=fetched)
        except (ValueError,TypeError,KeyError):
            reasons.add('original_header_or_retrieval_clock_invalid')
    if verified:
        reasons.discard('original_acceptance_timezone_unverified')
    else:
        reasons.add('original_header_and_accepted_index_clock_unresolved')
    A.check_pins(pins)
    return {**parsed,'available_at':accepted.isoformat() if verified else None,
            'qualified_clock':verified,'clock_basis':'original_EDGAR_index_Accepted_and_header' if verified else None,
            'exceptions':sorted(reasons),'source_qualified':False,'qualified_original':False,
            'gate_status':'UNQUALIFIED','source_quality_certified':False}
