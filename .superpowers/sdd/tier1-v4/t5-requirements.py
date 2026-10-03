"""Generate the Task5 primitive/formula contract; no data reads or return evaluation."""
import json
from pathlib import Path
from atx_db.alpha_panel import fund_items as F

groups = {
 'sales': ('sale_ttm','sale_q'), 'cogs': ('cogs_ttm','cogs_q'),
 'gross_profit': ('gp_ttm','gp_q'), 'operating_income': ('oi_ttm','oi_q'),
 'ebitda': ('ebitda_ttm','ebitda_q'), 'net_income': ('ni_ttm','ni_q'),
 'income_before_extraordinary': ('ib_ttm',), 'operating_cash_flow': ('cfo_ttm',),
 'assets': ('at',), 'equity': ('seq','ceq','teq'),
 'working_capital': ('act','lct'), 'inventory': ('invt',), 'receivables': ('rect',),
 'debt': ('dlc','dltt'), 'interest': ('xint_ttm',), 'tax': ('txt_ttm','txditc'),
 'capex': ('capx_ttm',), 'research': ('xrd_ttm',), 'dividends': ('dvc_ttm',),
 'issuance': ('sstk_ttm',), 'buybacks': ('prstkc_ttm',),
 'basic_shares': ('cshpri',), 'diluted_shares': ('cshfd',),
 'basic_eps': ('epspx_ttm',), 'diluted_eps': ('epsfx_ttm',),
 'investment': ('ppe',), 'free_cash_flow': ('cfo_ttm','capx_ttm'),
}
formulas = {
 'gross_margin': ('gp_ttm/sale_ttm',['gross_profit','sales']),
 'operating_margin': ('oi_ttm/sale_ttm',['operating_income','sales']),
 'net_margin': ('ni_ttm/sale_ttm',['net_income','sales']),
 'cash_margin': ('cfo_ttm/sale_ttm',['operating_cash_flow','sales']),
 'gp_assets': ('gp_ttm/at',['gross_profit','assets']),
 'roa': ('ni_ttm/mean(at,prior_at)',['net_income','assets']),
 'roe': ('ni_ttm/mean(seq+txditc,prior_seq+prior_txditc)',['net_income','equity','tax']),
 'roic': ('source_EBIT*(1-source_effective_tax)/mean(source_invested_capital,prior_source_invested_capital)',
          ['operating_income','equity','debt','tax']),
 'roce': ('source_EBIT/mean(source_capital_employed,prior_source_capital_employed)',
          ['operating_income','equity','debt']),
 'cash_conversion': ('cfo_ttm/ni_ttm',['operating_cash_flow','net_income']),
 'accrual_quality': ('(ni_ttm-cfo_ttm)/mean(at,prior_at)',['net_income','operating_cash_flow','assets']),
 'free_cash_flow': ('cfo_ttm-capx_ttm',['operating_cash_flow','capex']),
 'working_capital_growth': ('((act-lct)-(prior_act-prior_lct))/abs(prior_act-prior_lct)', ['working_capital']),
 'dilution': ('basic_shares/prior_basic_shares-1',['basic_shares']),
}
for family in ('sales','net_income','operating_cash_flow','free_cash_flow','assets','equity','capex','research',
               'basic_eps','diluted_eps','inventory','receivables','basic_shares','diluted_shares','investment'):
 for period in ('quarterly_yoy','annual','ttm'):
  formulas[f'{family}_{period}_growth'] = ('(current-prior)/abs(prior)',[family])
for family in ('gross_margin','operating_margin','net_margin','cash_margin','gp_assets','roa','roe'):
 formulas[family+'_change'] = ('current-prior', formulas[family][1])
cols=set(F.ITEM_COLUMNS)|set(F.ITEM_COLUMNS_V2)|{it.col for it in F.fcat.CATALOG}
inputs={
 'sales':('sale_q','sale_ttm'), 'net_income':('ni_q','ni_ttm'),
 'operating_cash_flow':('cfo_q','cfo_ttm'), 'free_cash_flow':('cfo_q-capx_q','cfo_ttm-capx_ttm'),
 'capex':('capx_q','capx_ttm'), 'research':('xrd_q','xrd_ttm'),
 'basic_eps':('epspxq','epspx_ttm'), 'diluted_eps':('epsfxq','epsfx_ttm'),
 'basic_shares':('cshprq','cshpri'), 'diluted_shares':('cshfdq','cshfd'),
 'assets':('at','at'), 'equity':('seq','seq'), 'inventory':('invt','invt'),
 'receivables':('rect','rect'), 'investment':('ppe','ppe'),
}
contracts={}
for name,(formula,families) in formulas.items():
 row={'formula':formula,'primitive_families':families,'gold_column':name,
      'status':'Task7/16 registration and independent qualification pending',
      'current_input_columns':sorted({c for family in families for c in groups[family]}),
      'prior_input_columns':[], 'input_clock':'latest source vintage strictly before feature cutoff'}
 if name.endswith('_growth') and name!='working_capital_growth':
  family,period=name.removesuffix('_growth').rsplit('_',1)
  if period=='yoy':family=family.removesuffix('_quarterly');period='quarterly_yoy'
  q,annual=inputs[family];current=q if period=='quarterly_yoy' else annual
  row.update(current_input_columns=current.split('-'),prior_input_columns=current.split('-'),
             formula=f'({current}-prior({current}))/abs(prior({current}))',
             prior_period='same fiscal quarter four quarters earlier' if period in ('quarterly_yoy','ttm') else 'previous comparable full fiscal year',
             source_table='compustat_std.fundq' if current in ('epspxq','epsfxq','cshprq','cshfdq') else 'fundamentals_v11.events/catalog/quarterly_history',
             duration='TTM four contiguous quarters; annual direct source full-period total; stocks use period-end values without summation',
             source_gap='cshfdq is not currently published; diluted quarterly share growth remains unsupported' if current=='cshfdq' else 'missing source/history/basis remains NULL')
  if family=='investment':row['definition']='Net physical capital investment growth: PPENT/ppe period-end change; not gross investment or capex.'
  stock=family in ('assets','equity','inventory','receivables','investment')
  row['input_bindings']={'current':
    {'table':'fundq','columns':[current]} if current in ('epspxq','epsfxq','cshprq','cshfdq') else
    {'table':'quarterly_history','columns':current.split('-'),'item_keys':current.split('-')} if period=='quarterly_yoy' and not stock else
    {'table':'quarterly_history','item_keys':['annual_'+c for c in current.split('-')]} if period=='annual' and not stock and family not in ('basic_eps','diluted_eps','basic_shares','diluted_shares') else
    {'table':'events/catalog','columns':current.split('-')},
    'prior':'same exact keys/columns at declared prior_period and visible restatement vintage'}
  if family in ('basic_eps','diluted_eps') and period=='quarterly_yoy':
   row['source_gap']='Quarterly standardized EPS excludes extraordinary/discontinued operations; annual total-EPS counterparts require matched definition qualification, otherwise NULL.'
 if name=='working_capital_growth':row.update(current_input_columns=['act','lct'],prior_input_columns=['act','lct'],prior_period='previous comparable fiscal year')
 if name=='dilution':row.update(formula='cshpri/prior(cshpri)-1',current_input_columns=['cshpri'],prior_input_columns=['cshpri'],prior_period='previous comparable full fiscal year')
 contracts[name]=row
for family,col in (('basic','cshpri'),('diluted','cshfd')):
 contracts[family+'_share_dilution']={'formula':f'{col}/prior({col})-1','primitive_families':[family+'_shares'],
  'current_input_columns':[col],'prior_input_columns':[col],'prior_period':'previous comparable source full fiscal year',
  'gold_column':family+'_share_dilution','source_table':'fundamentals_v11.catalog',
  'status':'Requires certified class/split/share basis; missing history is NULL'}
doc={
 'version':'fundamentals-v11-requirements-v1', 'status':'SOURCE_QUALIFICATION_AND_COVERAGE_PENDING',
 'primitives':{k:{'columns':list(v),'implemented_columns':[c for c in v if c in cols],
                   'source':'retained SEC CompanyFacts / FSDS exact taxonomy, concept, unit and scope',
                   'clock':'actual filing/component available_at; FX clock if later'} for k,v in groups.items()},
 'formula_to_gold':contracts,
 'policy':{
  'history':'Quarterly YoY requires same fiscal quarter; annual requires source 350-380-day periods and matching fiscal calendars; TTM requires four contiguous source quarters. Prior values use the visible restatement vintage.',
  'denominators':'Actual averages of current/prior source stocks; missing prior is NULL. Zero growth base is NULL; negative base uses abs(base) with explicit negative-base quality flag.',
  'currency':'Same economically comparable reporting currency or verified source FX conversion; unsupported currency/basis is NULL.',
  'sector':'Existing bank/insurer/REIT structural masks; no observed-missingness structural inference.',
  'null_reasons':['missing_source','missing_history','invalid_source_duration','fiscal_transition','currency_mismatch','uncertified_economic_scope','uncertified_share_basis','structural_not_applicable','stale','zero_denominator'],
  'coverage':'Every year2020-2025 in member_equity, univ_all_common, univ_r3000p; ordinary missing history remains eligible denominator. Measurements pending final producer and coordinated universe snapshot.',
  'returns':'All return statistics and complete label windows end before2023-01-01.',
 },
 'explicit_dependencies': ['Task7 gold formula registration/comparison','Task13 indicated dividend rate',
                         'market producer class/ADR/EPS/split/date alignment','independent composite original-model qualification'],
}
Path('C:/atx/atx-db/docs/datasets/fundamentals_v11.requirements.json').write_text(
 json.dumps(doc,indent=2)+'\n',encoding='utf8')
print(len(groups),'primitive families;',len(contracts),'formula contracts')
