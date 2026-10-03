"""Bounded primary-document reportable lists, independently of revenue arithmetic.

Only explicit present-period reportable lists whose named members match current annual
inline facts and a sound presentation role can certify a cut. Products, historical,
conditional and reorganization statements remain unresolved. Offsets refer to decoded
primary HTML, with its SHA retained by the caller. No numerical fitting is performed.
"""
from __future__ import annotations

import re
from html.parser import HTMLParser
from typing import Any

COUNTS = {'one':1,'single':1,'two':2,'three':3,'four':4,'five':5,
          'six':6,'seven':7,'eight':8,'nine':9,'ten':10}
CLAIM = re.compile(r'(?i)\b(?:we|the company|our company|[\w &]+?)\s+(?:currently\s+)?'
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
    out=[]
    for i,block in enumerate(primary.blocks):
        for claim in CLAIM.finditer(block['text']):
            count=COUNTS.get(claim.group(1).lower(),int(claim.group(1)) if claim.group(1).isdigit() else 0)
            # A list may use separate consecutive paragraphs; bounded by 1500 chars and
            # the next reportability statement. Exact count plus unique aliases is required.
            selected_blocks=[block]
            # Extend an explicitly introduced multiline list, not arbitrary prose.
            if block['text'].rstrip().endswith(':'):
                for following in primary.blocks[i+1:i+8]:
                    if len(following['text'])>300:break
                    selected_blocks.append(following)
            text=' '.join(b['text'] for b in selected_blocks)[:1500]
            next_claim=CLAIM.search(text,claim.end())
            if next_claim:text=text[:next_claim.start()]
            scope=block['text']
            prior=primary.blocks[i-1]['text'][-180:] if i else ''
            years=re.findall(r'\b(?:19|20)\d{2}\b',scope)
            ambiguous=bool(QUALIFIER.search(prior+' '+scope)) or any(y!=period[:4] for y in years)
            # Only the declared clause and immediately following list identify members.
            tokens=re.findall(r'[a-z0-9]+',text[claim.end():].lower())
            keys={''.join(tokens[a:b]) for a in range(len(tokens)) for b in range(a+1,min(a+13,len(tokens)+1))}
            matched={m for m in members if member_aliases(m)&keys}
            # Shared aliases cannot establish distinct economic names.
            aliases={m:member_aliases(m)&keys for m in matched}
            collision=any(aliases[a]&aliases[b] for a in matched for b in matched if a!=b)
            quality='ambiguous_period' if ambiguous else 'explicit_named_reportable_list' if len(matched)==count and not collision else 'unmapped_reportable_list'
            end=selected_blocks[-1]['end']
            out.append({'declared_count':count,'members':sorted(matched),'quality':quality,
                        'start_offset':block['start'],'end_offset':end,'text':text,
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
