"""Bounded primary-document reportable lists, independently of revenue arithmetic.

Only explicit present-period reportable lists whose named members match current annual
inline facts and a sound presentation role can certify a cut. Products, historical,
conditional and reorganization statements remain unresolved. Offsets refer to decoded
primary HTML, with its SHA retained by the caller. No numerical fitting is performed.
"""
from __future__ import annotations

import re
import datetime as dt
from html.parser import HTMLParser
from typing import Any

COUNTS = {'one':1,'single':1,'two':2,'three':3,'four':4,'five':5,
          'six':6,'seven':7,'eight':8,'nine':9,'ten':10}
CLAIM = re.compile(r'(?i)\b(?:we|the company|our company|[\w &]{1,80}?)\s+(?:currently\s+)?'
                   r'(?:have|has|report|reports|operate|operates|is organized into|are organized into)'
                   r'\s+(?:in\s+|through\s+)?(one|single|two|three|four|five|six|seven|eight|nine|ten|[1-9])'
                   r'\s+(?:operating and |operating |business )?reportable (?:business |operating )?segments?\b')
QUALIFIER = re.compile(r'(?i)\b(?:previously|formerly|historically|prior to|until|if|would|could|'
                       r'reorganiz\w*|restructur\w*|realign\w*|will|expect\w*|propos\w*)\b')


class Primary(HTMLParser):
    """Small streaming HTML index; no DOM or external entity expansion."""
    def __init__(self, text: str):
        super().__init__(convert_charrefs=True)
        self.text=text; self.lines=[0]+[m.end() for m in re.finditer('\n',text)]
        self.hidden=0; self.blocks=[]; self.parts=[]; self.start=0
        self.tables=[]; self.table_stack=[]; self.row=-1; self.facts=[]
        self.feed(text); self.flush(len(text))

    def source_offset(self):
        line,col=self.getpos(); return self.lines[line-1]+col

    def flush(self,end):
        visible=re.sub(r'\s+',' ',''.join(self.parts)).strip()
        if visible:self.blocks.append({'text':visible,'start':self.start,'end':end})
        self.parts=[]; self.start=end

    def handle_starttag(self,tag,attrs):
        attrs=dict(attrs); pos=self.source_offset()
        if tag in ('ix:header','script','style'):self.hidden+=1
        if self.hidden:return
        if tag in ('p','div','li','tr','br'):self.flush(pos);self.start=pos
        if tag=='table':
            self.tables.append({'start':pos,'end':None});self.table_stack.append(len(self.tables)-1)
        if tag=='tr':self.row+=1
        if tag=='ix:nonfraction':
            self.facts.append({'name':attrs.get('name'),'context':attrs.get('contextref'),
                'unit':attrs.get('unitref'),'sign':attrs.get('sign'),'start':pos,
                'table':self.table_stack[-1] if self.table_stack else None,'row':self.row})

    def handle_endtag(self,tag):
        if tag in ('ix:header','script','style'):
            self.hidden=max(0,self.hidden-1);return
        if self.hidden:return
        if tag in ('p','div','li','tr'):self.flush(self.source_offset())
        if tag=='table' and self.table_stack:
            self.tables[self.table_stack.pop()]['end']=self.source_offset()

    def handle_data(self,data):
        if not self.hidden:self.parts.append(data)


def name_key(text: str) -> str:
    return re.sub(r'[^a-z0-9]','',text.lower())


def member_aliases(member: str) -> set[str]:
    """Exact spelling aliases only; no fuzzy issuer or economic equivalence match."""
    words=re.sub(r'([a-z0-9])([A-Z])',r'\1 \2',member)
    aliases={name_key(words)}
    aliases.add(name_key(re.sub(r'(?i)\b(?:segment|segments|business|practice)\b','',words)))
    return {a for a in aliases if len(a)>=3}


def reportable_lists(primary: Primary, period: str, members: set[str]) -> list[dict[str,Any]]:
    from .fund_notes_disclosure import list_partitions
    out=[]
    for i,block in enumerate(primary.blocks):
        for claim in CLAIM.finditer(block['text']):
            count=COUNTS.get(claim.group(1).lower(),int(claim.group(1)) if claim.group(1).isdigit() else 0)
            selected_blocks=[block]
            tail=block['text'][claim.end():]
            introduced=bool(re.match(r'\s*[:,\-]|\s+(?:namely|consisting of|which are)\b',tail,re.I))
            # The first sentence terminates a declaration. Subsequent sentences never
            # supply missing names. Only an unfinished introduced list may cross blocks.
            def clause_end(value):
                terminal=re.search(r'(?<!\d)\.(?=\s|$)',value)
                return value[:terminal.start()] if terminal else value
            clause=clause_end(tail)
            if introduced and not tail.rstrip().endswith('.') and not re.search(r'(?<!\d)\.(?=\s|$)',tail):
                for following in primary.blocks[i+1:i+8]:
                    if len(following['text'])>300 or CLAIM.search(following['text']):break
                    # Stop after a complete syntactic list; no nearby alias harvesting.
                    if list_partitions(clause,count):break
                    selected_blocks.append(following)
                    clause+='\n'+clause_end(following['text'])
                    if len(clause)>1500 or re.search(r'(?<!\d)\.(?=\s|$)',following['text']):break
            text=' '.join(b['text'] for b in selected_blocks)
            scope=text
            prior=primary.blocks[i-1]['text'][-180:] if i else ''
            # An immediately following qualification can govern the completed list.
            after_index=i+len(selected_blocks)
            forward=primary.blocks[after_index]['text'][:180] if after_index<len(primary.blocks) else ''
            forward_reference=re.match(r'(?i)^(?:these|those|the above|our|the)\s+(?:reportable |operating )?segments?\b',forward)
            forward_intro=re.match(r'(?i)^(?:previously|formerly|historically|prior to|until|if\b|following (?:reorganiz|restructur)|(?:as of |in )(?:19|20)\d{2})',forward)
            if forward_intro or (forward_reference and QUALIFIER.search(forward)):scope+=' '+forward
            years=re.findall(r'\b(?:19|20)\d{2}\b',prior+' '+scope)
            ambiguous=bool(QUALIFIER.search(prior+' '+scope)) or any(y!=period[:4] for y in years)
            partitions=list_partitions(clause,count) if introduced else []
            mapped=[]
            for names in partitions:
                matches=[{m for m in members if name_key(n) in member_aliases(m)} for n in names]
                if all(len(ms)==1 for ms in matches) and len(set.union(*matches))==count:
                    mapped.append((names,sorted(set.union(*matches))))
            unique={tuple(ms) for _,ms in mapped}
            matched=mapped[0][1] if len(unique)==1 else []
            quality='ambiguous_period' if ambiguous else 'explicit_named_reportable_list' if len(unique)==1 else 'unmapped_reportable_list'
            end=selected_blocks[-1]['end']
            out.append({'declared_count':count,'members':sorted(matched),'quality':quality,
                        'start_offset':block['start'],'end_offset':end,'text':text,
                        'list_items':mapped[0][0] if len(unique)==1 else [],
                        'list_clause':clause,'qualifier_scope':prior+' '+scope,
                        'contributing_blocks':[{'start_offset':b['start'],'end_offset':b['end']} for b in selected_blocks],
                        'reporting_period':period,'certified':False})
    return out


def semantic_cut(payload: bytes, concept: str, period: str, fact_members: set[str],
                 roles: list[dict], contexts: dict[str,list[str]]) -> dict:
    from . import fund_notes_structure as S
    primary=Primary(payload.decode('utf8','replace'))
    claims=reportable_lists(primary,period,fact_members)
    candidates=[c for c in claims if c['quality']=='explicit_named_reportable_list' and c['declared_count']>=2]
    partitions={tuple(c['members']) for c in candidates}
    if len(partitions)!=1:
        return {'status':'conflicting_primary_lists' if len(partitions)>1 else 'no_certifiable_named_list','claims':claims}
    claim=candidates[0];expected=set(claim['members'])
    if not expected<=set(contexts):return {'status':'missing_primary_context_identity','claims':claims}
    # Units must exist and match across all current annual revenue facts; year identity
    # comes from context dates, never a nearby visual year column.
    ids={c for m in expected for c in contexts[m]}
    facts=[f for f in primary.facts if f['name']==concept.replace('_',':',1) and f['context'] in ids]
    units={f['unit'] for f in facts}
    unit_defs={m.group(1):m.group(2) for m in re.finditer(r'<(?:\w+:)?unit\b[^>]*id=["\']([^"\']+)["\'][^>]*>(.*?)</(?:\w+:)?unit>',primary.text,re.I|re.S)}
    if len(units)!=1 or None in units or not units<=set(unit_defs):return {'status':'missing_or_conflicting_units','claims':claims}
    if not re.search(r'(?:iso4217:)?[A-Z]{3}\s*</',unit_defs[next(iter(units))]):return {'status':'noncurrency_unit','claims':claims}
    # Primary table identity anchors the chosen role even when unrelated roles differ.
    table_sets={t:{S.local_member(m) for m,cs in contexts.items() if any(f['context'] in cs and f['table']==t for f in facts)} for t in {f['table'] for f in facts} if t is not None}
    table_ids=[t for t,ms in table_sets.items() if expected<=ms]
    if not table_ids:return {'status':'missing_current_revenue_table','claims':claims}
    matching=[]
    for role in roles:
        graph=S.hierarchy(role,concept)
        if role['kind']!='presentationLink' or graph['status'] not in ('nested','flat_ambiguous'):continue
        nodes={S.local_member(n) for a,b,_ in role['edges'] for n in (a,b)}
        if not expected<=nodes:continue
        children={}
        for a,b,_ in role['edges']:children.setdefault(a,set()).add(b)
        overlap=False
        for member in expected:
            todo=[b for a,b,_ in role['edges'] if S.local_member(a)==member];seen=set()
            while todo:
                node=todo.pop()
                if node in seen:continue
                seen.add(node)
                if S.local_member(node) in expected:overlap=True
                todo.extend(children.get(node,()))
        if not overlap:matching.append(graph)
    if not matching:return {'status':'no_sound_primary_matched_role','claims':claims}
    claim=claim|{'certified':True}
    return {'status':'verified_structure','members':sorted(expected),'roles':matching,
            'corroboration':{'basis':'automatic explicit named current-reportable list plus annual inline revenue table and role',
                             'claim':claim,'table_spans':[primary.tables[t] for t in table_ids]},
            'semantic_claims':claims,'unit_refs':sorted(units)}


def source_bridge_contexts(payload: bytes, concept: str, period: str, selected: set[str], roles: list[dict]) -> dict:
    """Same annual revenue table's explicit corporate/elimination/all-other bridge.

    All Other is accepted only with a source declaration that it is outside reportable
    segments. The signed NUM value remains authoritative, and never affects HHI/count.
    """
    from . import fund_notes_structure as S
    primary=Primary(payload.decode('utf8','replace'))
    business=S.primary_contexts(payload,concept,period)
    consolidation=S.primary_contexts(payload,concept,period,'srt:ConsolidationItemsAxis')
    # Production prefers plain business facts to an OperatingSegments consolidation
    # qualifier. Bind bridges to that same source context family, not both gross and
    # external-sales rows sharing a revenue concept.
    plain_ids={m.group(1) for m in re.finditer(r'<(?:\w+:)?context\b[^>]*id=["\']([^"\']+)["\'][^>]*>(.*?)</(?:\w+:)?context>',primary.text,re.I|re.S)
               if len(re.findall(r'<(?:\w+:)?explicitMember\b',m.group(2),re.I))==1}
    business={m:sorted(set(ids)&plain_ids) if set(ids)&plain_ids else ids for m,ids in business.items()}
    selected_ids={c for m in selected for c in business.get(m,[])}
    selected_units={f['unit'] for f in primary.facts if f['name']==concept.replace('_',':',1) and f['context'] in selected_ids and f['unit']}
    tables={f['table'] for f in primary.facts if f['name']==concept.replace('_',':',1) and f['context'] in selected_ids and f['table'] is not None}
    selected_rows={(f['table'],f['row']) for f in primary.facts if f['name']==concept.replace('_',':',1) and f['context'] in selected_ids and f['table'] is not None}
    nodes={S.local_member(n) for r in roles for a,b,_ in r['edges'] for n in (a,b)}
    out={}
    for axis,contexts in [('ConsolidationItems',consolidation),('BusinessSegments',business)]:
        for member,ids in contexts.items():
            if member in selected or member not in nodes:continue
            if not re.fullmatch(r'(?i)(?:Corporate(?:EntitiesAndOther|NonSegment)?|IntersegmentEliminations?|IntercompanyEliminations?|Eliminations?|AllOther)',member):continue
            if member.lower()=='allother':
                declarations=[b for b in primary.blocks if re.search(r'(?i)all\s+other',b['text']) and re.search(r'(?i)(?:not (?:separately )?reportable|non.?reportable|not included in (?:our |the )?(?:four |three |two )?(?:operating and )?reportable)',b['text'])]
                if not declarations:continue
            matching=[f for f in primary.facts if f['name']==concept.replace('_',':',1) and f['context'] in ids and (f['table'],f['row']) in selected_rows and f['unit'] in selected_units]
            if matching:out[member]={'context_ids':sorted({f['context'] for f in matching}),'segments':f'{axis}={member};','source_spans':[{'start_offset':f['start'],'table':primary.tables[f['table']]} for f in matching]}
    return out


def source_external_revenue(payload: bytes, period: str, selected: set[str], roles: list[dict]) -> dict:
    """Existing full-revenue facts in a source-declared customer/external-sales section.

    The heading and section boundary select the measure before any NUM values are
    read. Gross sales and equity-method-inclusive segment measures remain separate.
    A customer-revenue row must contain every selected member; an external-sales
    section ends at its explicitly labeled total. No intersegment row is bridged.
    """
    from . import fund_notes_structure as S
    primary=Primary(payload.decode('utf8','replace'))
    concepts=('us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax',
              'us-gaap_RevenueFromContractWithCustomerIncludingAssessedTax')
    bodies={m.group(1):m.group(2) for m in re.finditer(r'<(?:\w+:)?context\b[^>]*id=["\']([^"\']+)["\'][^>]*>(.*?)</(?:\w+:)?context>',primary.text,re.I|re.S)}
    unit_defs={m.group(1):m.group(2) for m in re.finditer(r'<(?:\w+:)?unit\b[^>]*id=["\']([^"\']+)["\'][^>]*>(.*?)</(?:\w+:)?unit>',primary.text,re.I|re.S)}
    company_contexts=set()
    for ident,body in bodies.items():
        if re.search(r'<(?:\w+:)?(?:explicitMember|typedMember)\b',body,re.I):continue
        end_date=re.search(r'<(?:\w+:)?endDate>\s*([^<]+)\s*</',body,re.I)
        start_date=re.search(r'<(?:\w+:)?startDate>\s*([^<]+)\s*</',body,re.I)
        if not end_date or not start_date or end_date.group(1).strip()!=period:continue
        if 300 <= (dt.date.fromisoformat(period)-dt.date.fromisoformat(start_date.group(1).strip())).days <= 380:company_contexts.add(ident)
    choices=[]
    for concept in concepts:
        contexts=S.primary_contexts(payload,concept,period)
        if not selected<=set(contexts):continue
        id_member={ident:member for member in selected for ident in contexts[member]}
        for i,block in enumerate(primary.blocks):
            label=block['text'].strip()
            row_heading=bool(re.match(r'(?i)^Revenues from customers(?=\s|\$|\d|$)',label))
            section_heading=label.lower()=='sales to external customers'
            if not row_heading and not section_heading:continue
            table_ids=[j for j,t in enumerate(primary.tables) if t['end'] is not None and t['start']<=block['start']<t['end']]
            if len(table_ids)!=1:continue
            table=table_ids[0];end=block['end']
            if section_heading:
                following=[b for b in primary.blocks[i+1:] if primary.tables[table]['start']<=b['start']<primary.tables[table]['end']]
                # Exact total label closes this section. Other section headings cannot
                # be skipped to harvest later contexts from the same table.
                boundary=next((b for b in following if re.match(r'(?i)^(?:Total sales|Intersegment sales|Sales to external customers|Income (?:from|\(loss\) from) operations|Interest expense|Depreciation and amortization|Capital expenditures)(?=\s|\$|\d|$)',b['text'])),None)
                if boundary is None or not re.match(r'(?i)^Total sales(?=\s|\$|\d|$)',boundary['text']):continue
                end=boundary['start']
            facts=[f for f in primary.facts if f['name']==concept.replace('_',':',1) and
                   f['context'] in id_member and f['table']==table and block['start']<=f['start']<end]
            by_member={member:[f for f in facts if id_member[f['context']]==member] for member in selected}
            # One fact per member in this exact annual section; duplicated or missing
            # current contexts require further source review.
            if any(len(fs)!=1 for fs in by_member.values()):continue
            proof=semantic_cut(payload,concept,period,selected,roles,contexts)
            if proof['status']!='verified_structure' or set(proof['members'])!=selected:continue
            units={f['unit'] for f in facts}
            if len(units)!=1 or sorted(units)!=proof['unit_refs']:continue
            if row_heading:
                total_row=facts[0]['row']
            else:
                following_fact=next((f for f in primary.facts if f['table']==table and f['start']>=boundary['start']),None)
                if not following_fact:continue
                total_row=following_fact['row']
            company_facts=[f for f in primary.facts if f['name']==concept.replace('_',':',1) and
                           f['context'] in company_contexts and f['table']==table and f['row']==total_row and f['unit'] in units]
            # A customer component cannot borrow a company total elsewhere in the
            # filing. This exact source row/section must supply its annual company total.
            if len(company_facts)!=1:continue
            measure=re.search(r'(?:iso4217:)?([A-Z]{3})\s*</',unit_defs[next(iter(units))])
            if not measure:continue
            source_segments={}
            for member,fs in by_member.items():
                dims=re.findall(r'<(?:\w+:)?explicitMember\b[^>]*dimension=["\']([^"\']+)["\'][^>]*>\s*([^<]+)\s*</',bodies[fs[0]['context']],re.I)
                source_segments[member]=f'BusinessSegments={member};'+('ConsolidationItems=OperatingSegments;' if ('srt:ConsolidationItemsAxis','us-gaap:OperatingSegmentsMember') in dims else '')
            choices.append({'concept':concept,'members':sorted(selected),
                'context_ids':{m:[fs[0]['context']] for m,fs in by_member.items()},
                'segments':source_segments,'currency':measure.group(1),
                'company_total_context_ids':[company_facts[0]['context']],
                'company_total_source_offset':company_facts[0]['start'],'company_total_row':total_row,
                'unit_refs':sorted(units),'source_label':label,'start_offset':block['start'],
                'end_offset':end,'table':primary.tables[table],
                'source_rows':sorted({f['row'] for f in facts}),
                'basis':'explicit customer/external-sales section; full-revenue concept; annual member contexts; sound reportable role'})
    unique={(c['concept'],tuple((m,tuple(ids)) for m,ids in sorted(c['context_ids'].items()))) for c in choices}
    return choices[0] if len(unique)==1 else {}
