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
