"""Conservative filing/role-bound XBRL business hierarchy evidence.

Graphs establish identity, never additivity from values. Conflicting roles, multi-parent
overlap, prohibited arcs, targetRole traversal and incomplete fact partitions refuse selection.
Original source facts are never rewritten. Context IDs and source hashes travel with selections.
"""
from __future__ import annotations

import gzip
import hashlib
import json
import re
import datetime as dt
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

XL = '{http://www.w3.org/1999/xlink}'
DT = '{http://xbrl.org/2005/xbrldt}'
AXIS = 'us-gaap_StatementBusinessSegmentsAxis'
DOMAIN = 'us-gaap_SegmentDomain'


def local_member(qname: str) -> str:
    """DERA DIM exact normalization, not a semantic name match."""
    return re.sub(r'Member$', '', qname.split(':')[-1].split('_', 1)[-1])


def linkbase_roles(payload: bytes) -> list[dict[str, Any]]:
    out = []
    for link in ET.fromstring(payload):
        kind = link.tag.rsplit('}', 1)[-1]
        if kind not in ('definitionLink', 'presentationLink'):
            continue
        loc = {e.get(XL+'label'): e.get(XL+'href', '').rsplit('#', 1)[-1] for e in link if e.tag.endswith('}loc')}
        edges, invalid = [], []
        for arc in link:
            if not arc.tag.endswith('Arc'):
                continue
            relation = arc.get(XL+'arcrole', '').rsplit('/', 1)[-1]
            if relation not in ('parent-child', 'domain-member', 'dimension-domain'):
                continue
            parent, child = loc.get(arc.get(XL+'from')), loc.get(arc.get(XL+'to'))
            if not parent or not child or arc.get('use') == 'prohibited' or arc.get(DT+'targetRole'):
                invalid.append(dict(arc.attrib))
                continue
            edges.append((parent, child, relation))
        out.append({'role': link.get(XL+'role'), 'kind': kind, 'edges': edges, 'invalid': invalid})
    return out


def hierarchy(role: dict[str, Any], concept: str) -> dict[str, Any]:
    edges = role['edges']
    if concept not in {n for a, b, _ in edges for n in (a, b)} or not any(a == AXIS and b == DOMAIN for a,b,_ in edges):
        return {'status': 'not_applicable'}
    children: dict[str, set[str]] = {}
    for a,b,relation in edges:
        if relation in ('parent-child','domain-member'):
            children.setdefault(a,set()).add(b)
    visiting, visited, parents = set(), set(), {}
    def visit(node: str) -> None:
        if node in visiting:
            raise ValueError('cycle')
        visiting.add(node)
        for child in children.get(node,()):
            parents.setdefault(child,set()).add(node)
            if len(parents[child])>1:
                raise ValueError('overlapping_members')
            visit(child)
        visiting.remove(node); visited.add(node)
    try:
        visit(DOMAIN)
    except ValueError as exc:
        return {'status':str(exc)}
    if role['invalid']:
        return {'status':'unsupported_or_prohibited_arcs'}
    members={n for n in visited if n != DOMAIN}
    if any(not n.endswith('Member') for n in members):
        return {'status':'ambiguous_domain_node'}
    leaves=sorted(n for n in members if not children.get(n))
    internal=sorted(members-set(leaves))
    return {'status':'nested' if internal else 'flat_ambiguous','leaves':leaves,'internal':internal,
            'roots':sorted(children.get(DOMAIN,())),'role':role['role'],'kind':role['kind']}


def select_hierarchy(roles: list[dict[str, Any]], concept: str, fact_members: set[str]) -> dict[str, Any]:
    applicable=[hierarchy(role,concept) for role in roles]
    applicable=[r for r in applicable if r['status']!='not_applicable']
    if not applicable:
        return {'status':'missing_links'}
    if any(r['status'] not in ('nested','flat_ambiguous') for r in applicable):
        return {'status':'invalid_role_graph','roles':applicable}
    partitions={tuple(r['leaves']) for r in applicable}
    if len(partitions)!=1:
        return {'status':'conflicting_roles','roles':applicable}
    chosen=applicable[0]
    if not any(r['status']=='nested' for r in applicable):
        return {'status':'flat_ambiguous','roles':applicable}
    leaves={local_member(m) for m in chosen['leaves']}
    internal={local_member(m) for r in applicable for m in r['internal']}
    if len(leaves)!=len(chosen['leaves']):
        return {'status':'normalization_collision','roles':applicable}
    if len(leaves)<2 or not leaves <= fact_members or not fact_members <= leaves | internal:
        return {'status':'incomplete_or_unmapped_partition','roles':applicable}
    return {'status':'verified_structure','members':sorted(leaves),'roles':applicable}


def primary_contexts(payload: bytes, concept: str, period: str, axis: str = 'us-gaap:StatementBusinessSegmentsAxis') -> dict[str,list[str]]:
    """Exact primary QName and fiscal period; preserve all original context identifiers."""
    text=payload.decode('utf-8','replace')
    contexts={}
    for match in re.finditer(r'<(?:\w+:)?context\b[^>]*\bid=["\']([^"\']+)["\'][^>]*>(.*?)</(?:\w+:)?context>',text,re.I|re.S):
        ident,body=match.groups()
        if not re.search(r'<(?:\w+:)?endDate>\s*'+re.escape(period)+r'\s*</',body,re.I):
            continue
        start=re.search(r'<(?:\w+:)?startDate>\s*([^<]+)\s*</',body,re.I)
        if not start or not 300 <= (dt.date.fromisoformat(period)-dt.date.fromisoformat(start.group(1).strip())).days <= 380:
            continue
        dims=re.findall(r'<(?:\w+:)?explicitMember\b[^>]*dimension=["\']([^"\']+)["\'][^>]*>\s*([^<]+)\s*</',body,re.I)
        extra=[(dimension,member.strip()) for dimension,member in dims if dimension!=axis]
        if extra and (axis!='us-gaap:StatementBusinessSegmentsAxis' or extra!=[('srt:ConsolidationItemsAxis','us-gaap:OperatingSegmentsMember')]):
            continue
        members=[member.strip() for dimension,member in dims if dimension==axis]
        if len(members)==1:
            contexts[ident]=members[0]
    result={}
    qname=concept.replace('_',':',1)
    for match in re.finditer(r'<ix:nonFraction\b([^>]+)>',text,re.I):
        attrs=dict((k.lower(),v) for k,v in re.findall(r'([\w:]+)=["\']([^"\']*)["\']',match.group(1)))
        context=attrs.get('contextref')
        if attrs.get('name')==qname and context in contexts:
            result.setdefault(local_member(contexts[context]),[]).append(context)
    return {k:sorted(set(v)) for k,v in result.items()}


# Primary table plus narrative explicitly declares the reportable level. XML flattens it.
# The exact primary SHA and relevant role are required; not an issuer-wide name rule.
CORROBORATED = {
 '0000950170-23-004361': {
  'primary_sha256':'213017d672c48f545f72b1dc979ddb603ccf33b76caf5e4d0dd8504f56060127',
  'role':'http://martinmarietta.com/20221231/taxonomy/role/Role_DisclosureSegmentsFinancialDataForContinuingOperationsForCompanySReportableSegmentsDetail',
  'concept':'us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax',
  'members':['BuildingMaterialsBusinessEastGroup','BuildingMaterialsBusinessWestGroup','MagnesiaSpecialties'],
  'subtotal':'BuildingMaterialsBusiness',
  'basis':'primary Note 9 reportable-segment table and narrative; East/West nested in Building Materials subtotal',
 }
}

CORROBORATED.update({
 '0001558370-25-001617': {
  'primary_sha256':'5d139e8b95301bc2d3c4586ffb438868a1b84ba1ebb6ec6503c136bd5e309d44',
  'role':'http://www.helixesg.com/role/DisclosureBusinessSegmentInformationMeasureOfSegmentProfitOrLossDetails',
  'concept':'us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax',
  'members':['ProductionFacilities','Robotics','ShallowWaterAbandonment','WellIntervention'],
  'basis':'primary reportable segment narrative aggregates US/UK/Brazil Well Intervention units into one financial-reporting segment; four reported strategic units'},
 '0001558370-25-001102': {
  'primary_sha256':'6506263989373cd6466617f2da12de34f825338647aa3f833e3ccd7da79644a2',
  'role':'http://www.wattswater.com/role/DisclosureSegmentInformationSegmentReconciledToItsConsolidatedFinancialStatementsDetails',
  'concept':'us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax',
  'members':['AmericasSegment','AsiaPacificMiddleEastAndAfricaSegment','EuropeSegment'],
  'basis':'primary segment note explicitly states three geographic and reportable segments Americas, Europe and APMEA'},
 '0001558370-25-001888': {
  'primary_sha256':'3133bdff9589402322059c6d077dc369507b4af4d0db087d632af7c3d1f0239e',
  'role':'http://riotblockchain.com/role/DisclosureSegmentInformationSegmentRevenueAndSegmentGrossProfitLossDetails',
  'concept':'us-gaap_Revenues','members':['BitcoinMiningSegment','EngineeringSegment'],
  'basis':'primary note explicitly identifies Bitcoin Mining and Engineering reportable segments; prior Data Center Hosting was discontinued in 2024'},
 '0001628280-25-015336': {
  'primary_sha256':'20df588d2dacd6fa7be0dfcd71be70a8bf5a19f3a7e6f925b1964c16e95c52a7',
  'role':'http://www.fathomrealty.com/role/SegmentReportingScheduleofKeyOperatingDatafortheReportableSegmentsDetails',
  'concept':'us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax',
  'members':['MortgageSegment','RealEstateBrokerage','Technology'],
  'bridge_members':['CorporateNonSegment','IntersegmentElimination'],
  'basis':'primary table explicitly presents Real Estate Brokerage, Mortgage, Technology reportable columns, separately corporate services and intersegment elimination bridge'},
 '0001609253-25-000027': {
  'primary_sha256':'527b195df15110807dd991153c789efece5f735dff219fc11bf023b133a506ca',
  'role':'http://www.oxy.com/role/SEGMENTINFORMATIONReconciliationofRevenuefromSegmentstoConsolidatedDetails',
  'concept':'us-gaap_Revenues','members':['CarbonManagementSegment','OilAndNaturalGasSegment'],
  'basis':'California Resources primary segment table explicitly separates Oil and Natural Gas, Carbon Management, Total Reportable Segments and reconciliation of operating revenues'},
 '0001060391-25-000091': {
  'primary_sha256':'3560da64dd58fd3fd2696e385abf3cd29839bd728cc7e8f6bbf13977c6414588',
  'role':'http://www.republicservices.com/role/SegmentReportingSummarizedFinancialInformationDetails',
  'concept':'us-gaap_Revenues','members':['Group1Segment','Group2Segment','Group3Segment'],
  'basis':'primary segment note explicitly identifies Group 1/2/3 and states Recycling & Waste Subtotal combines Group 1 and Group 2'},
 '0000886206-25-000085': {
  'primary_sha256':'2807c9b8adbe821d43fa563f79fc8055189b0120f28d03357d712a05a891b0fe',
  'role':'http://www.franklincovey.com/role/DisclosureSegmentInformationScheduleOfSegmentInformationDetails',
  'concept':'us-gaap_Revenues','members':['EducationPractice','InternationalDirectOfficeSegment','InternationalLicenseesSegment','NorthAmericaSegment'],
  'basis':'primary Note 17 explicitly identifies four operating/reportable segments; AllOther is outside that declared four-segment set'},
 '0000929351-22-000017': {
  'primary_sha256':'11dd7437166a1d19c38745c5c5a9323bcd1aa1eb4983cd365d323a89d85bb439',
  'role':'http://www.lionsgate.com/role/SegmentInformationSegmentInformationDetails',
  'concept':'us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax',
  'members':['MediaNetworks','MotionPicture','TelevisionProduction'],
  'bridge_members':['IntersegmentElimination'],
  'basis':'primary Note 16 explicitly declares these three reportable business segments; Studio Business groups Motion Picture and Television Production, not a fourth segment'},
})


def cached_accessions(rawroot: Path) -> set[str]:
    receipt=rawroot/'receipts.jsonl'
    return {json.loads(s)['adsh'] for s in receipt.read_text(encoding='utf-8').splitlines()} if receipt.exists() else set()


def cached_structure(rawroot: Path, accession: str, tag: str, period: str, fact_members: set[str]) -> dict[str,Any]:
    receipt=rawroot/'receipts.jsonl'
    rows=[json.loads(s) for s in receipt.read_text(encoding='utf-8').splitlines()] if receipt.exists() else []
    rows=[r for r in rows if r['adsh']==accession and r.get('http_status')==200 and r['kind'] in ('primary','linkbase')]
    roles=[]; primary=None; sources=[]
    for row in rows:
        payload=gzip.decompress((rawroot/row['path']).read_bytes())
        if hashlib.sha256(payload).hexdigest()!=row['sha256']:
            return {'status':'source_hash_mismatch'}
        sources.append({k:row[k] for k in ('url','sha256','kind','path')})
        if row['kind']=='primary':primary=payload
        else:roles.extend(linkbase_roles(payload))
    concept='us-gaap_'+tag
    contexts=primary_contexts(primary,concept,period) if primary else {}
    result=select_hierarchy(roles,concept,fact_members)
    if result['status']=='verified_structure':
        # A taxonomy hierarchy can describe products or units below reportable segments.
        # Identity of the economic cut requires independent primary disclosure corroboration.
        result['status']='validated_graph_requires_reportable_cut'
    if primary:
        from .fund_notes_semantic import semantic_cut
        semantic=semantic_cut(primary,concept,period,fact_members,roles,contexts)
        if semantic['status']=='verified_structure':result=semantic
        else:result['semantic_diagnostic']=semantic
    corroboration=CORROBORATED.get(accession)
    if corroboration and primary and hashlib.sha256(primary).hexdigest()==corroboration['primary_sha256'] and concept==corroboration['concept']:
        matches=[r for r in roles if r['role']==corroboration['role'] and r['kind']=='presentationLink']
        expected=set(corroboration['members'])
        source_nodes={local_member(n) for r in matches for a,b,_ in r['edges'] for n in (a,b)}
        graphs=[hierarchy(r,concept) for r in matches]
        sound=graphs and all(g['status'] in ('nested','flat_ambiguous') for g in graphs)
        # A corroborated cut may be above leaves, but cannot overlap itself.
        for r in matches:
            children={}
            for parent,child,relation in r['edges']:
                if relation=='parent-child':children.setdefault(parent,set()).add(child)
            for member in expected:
                # Use qualified XML nodes for traversal, exact DERA names for identity.
                todo=[b for a,b,_ in r['edges'] if local_member(a)==member]
                seen=set()
                while todo:
                    node=todo.pop()
                    if node in seen:continue
                    seen.add(node)
                    if local_member(node) in expected:sound=False
                    todo.extend(children.get(node,()))
        if matches and sound and expected<=source_nodes and expected<=fact_members and expected<=set(contexts):
            result={'status':'verified_structure','members':sorted(expected),'roles':[hierarchy(r,concept) for r in matches],
                    'corroboration':corroboration}
    if result['status']=='verified_structure' and not set(result['members'])<=set(contexts):
        result['status']='missing_primary_context_identity'
    if result['status']=='verified_structure' and corroboration and corroboration.get('bridge_members'):
        bridge=primary_contexts(primary,concept,period,'srt:ConsolidationItemsAxis')
        graph_members={local_member(n) for r in roles if r['role']==corroboration['role'] for a,b,_ in r['edges'] for n in (a,b)}
        result['bridge_context_ids']={m:bridge[m] for m in corroboration['bridge_members'] if m in bridge and m in graph_members}
    if result['status']=='verified_structure' and primary:
        from .fund_notes_semantic import source_bridge_contexts
        result['source_bridge_contexts']=source_bridge_contexts(primary,concept,period,set(result['members']),roles)
    return result | {'sources':sources,'concept':concept,'period':period,'context_ids':contexts}
