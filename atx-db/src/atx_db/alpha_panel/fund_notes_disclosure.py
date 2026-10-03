"""Positive reportability claims with explicit temporal uncertainty, never global certification."""
from __future__ import annotations
import re

CLAIM=re.compile(r'(?i)\b(?:we|the company|our company)\s+(?:currently\s+)?(?:operate(?:s)?(?:\s+(?:in|through|with))?|report(?:s)?|have|has|are organized (?:into|in))\s+(?:only\s+)?(one|single|two|three|four|five|six|seven|eight|nine|ten|[1-9])\s+(?:distinct\s+)?(?:operating (?:and reportable )?|reportable (?:business |operating )?)(?:business )?segments?\b')
COUNTS={'one':1,'single':1,'two':2,'three':3,'four':4,'five':5,'six':6,'seven':7,'eight':8,'nine':9,'ten':10}
UNCERTAIN=re.compile(r'(?i)\b(?:if|when|would|could|previously|formerly|historically|prior to|until|reorganiz\w*|restructur\w*|realign\w*|will|expect\w*|propos\w*)\b')


def disclosure_claims(text: str, reporting_period: str) -> list[dict]:
    """Offsets reference exact retained text; regex terms alone never certify an economic cut."""
    out=[]
    for match in CLAIM.finditer(text):
        left=max(0,text.rfind('.',0,match.start())+1)
        right=text.find('.',match.end())
        right=len(text) if right<0 else right+1
        # Adjacent temporal/reorganization statements can qualify a present-tense sentence.
        scope=text[max(0,left-180):min(len(text),right+180)]
        years=re.findall(r'\b(?:19|20)\d{2}\b',scope)
        kind='temporal_or_reorganization_ambiguous' if UNCERTAIN.search(scope) or any(y!=reporting_period[:4] for y in years) else 'explicit_claim_period_uncorroborated'
        if 'reportable' not in match.group(0).lower():kind='operating_only_reportable_unverified'
        word=match.group(1).lower()
        out.append({'declared_count':COUNTS.get(word,int(word) if word.isdigit() else 0),
                    'start_offset':match.start(),'end_offset':match.end(),'claim':match.group(0),
                    'qualifier_start':max(0,left-180),'qualifier_end':min(len(text),right+180),
                    'reporting_period':reporting_period,'claim_period_years':sorted(set(years)),
                    'quality':kind,'certified':False})
    return out


def named_current_disclosures(text: str, reporting_period: str) -> list[dict]:
    """Independent positive denominator evidence from an explicit current named list.

    No NUM/DIM members, revenue values or reconciliation outcomes enter this parser.
    Absent/ambiguous lists remain unknown; a positive partial frame is not a census.
    Offsets address the retained section's exact decoded text.
    """
    out=[]
    for claim in disclosure_claims(text,reporting_period):
        if claim['quality']!='explicit_claim_period_uncorroborated':continue
        start=claim['end_offset'];end=text.find('.',start)
        if end<0 or end-start>300:continue
        tail=text[start:end].strip()
        tail=re.sub(r'^[\s,:-]*(?:namely\s+|consisting of\s+|which are\s+)?','',tail,flags=re.I)
        # Pure list only. Descriptions and revenue/product prose need richer semantics.
        tail=re.sub(r'\([^)]{0,40}\)','',tail)
        names=[re.sub(r'^\s*\(?\d+\)?[.)]?\s*','',s).strip(' \t\r\n\"\u201c\u201d')
               for s in re.split(r'\s*(?:;|,|\band\b)\s*',tail,flags=re.I) if s.strip()]
        if len(names)!=claim['declared_count'] or len(set(n.lower() for n in names))!=len(names):continue
        if any(not n or len(n)>80 or len(n.split())>8 or re.search(r'(?i)\b(?:includes?|provides?|sells?|products?|revenues?|which|that|will|formerly)\b',n) for n in names):continue
        out.append(claim|{'names':names,'list_end_offset':end,'source_text':text[claim['start_offset']:end],
                          'certified_current_named_disclosure':True,
                          'certified':False,'denominator_scope':'positive retained disclosure only; not all-filer certification'})
    return out
