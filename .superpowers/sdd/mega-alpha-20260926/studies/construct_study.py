import numpy as np, json, math, glob, sys, time
exec(open(sys.argv[4]).read().split('cands=lib')[0])  # reuse data prep + P/IDX from compose_study
cands=lib['candidates']; F=np.load(sys.argv[3])
yr=np.array([int(np.fromfile(rd+'/sessions.i64',dtype=np.int64)[d]//86400_000_000_000//365+1970) for d in dates]); fit=yr<=2021; hold=yr==2022
sg=np.sign(F[:,fit].mean(1)); sg[sg==0]=1; Fo=F*sg[:,None]
S=np.cov(Fo[:,fit]); mu=Fo[:,fit].mean(1); Sh=0.1*S+0.9*np.diag(np.diag(S)); w=np.linalg.solve(Sh,mu); w/=np.abs(w).sum()
blend=np.zeros((D,N)); cnt=np.zeros((D,N))
for k,cand in enumerate(cands):
    sig=np.fromfile(glob.glob(f"{cache}/*/{cand['id']}.f64")[0]).reshape(D,N)
    for d in dates:
        x=sig[d]; f=np.isfinite(x)&mem[d]
        if f.sum()>1: q=np.zeros(N); q[f]=ranks(x[f]); blend[d]+=w[k]*sg[k]*q
def target(d):
    idx=IDX[d]; q=ranks(blend[d,idx]); Pd,X=P[d]; q=q-X@(Pd@q); t=np.zeros(N); t[idx]=q/np.abs(q).sum(); return t
T={d:target(d) for d in dates}
sr=lambda v: v.mean()/v.std()*math.sqrt(252)
def sim(cad,frac,band=0.0,label=''):
    h=np.zeros(N); pnl=[]; to=[]
    for j,d in enumerate(dates):
        h=np.where(mem[d],h,0.0); tr=0.0
        if j%cad==0:
            t=T[d]; dlt=t-h
            if band>0: dlt=np.where(np.abs(dlt)>band/ max(1,mem[d].sum()),dlt,0.0)
            nh=h+frac*dlt; tr=np.abs(nh-h).sum(); h=nh
        g=np.nansum(h*np.nan_to_num(fwd[d])); c=tr*6e-4+np.abs(np.minimum(h,0)).sum()*0.03/252
        pnl.append((g,g-c)); to.append(tr)
    p=np.array(pnl); mo=np.sum(to)/(len(dates)/21)
    print(f'{label:20s} gross SR all {sr(p[:,0]):5.2f} hold22 {sr(p[hold,0]):5.2f} | net SR all {sr(p[:,1]):5.2f} hold22 {sr(p[hold,1]):5.2f} | turnover/mo {mo:.3f} gross {np.mean([np.abs(h).sum()]):.2f}')
sim(1,1.0,label='daily full')
sim(5,1.0,label='c5 full')
sim(5,0.25,label='c5 f.25')
sim(1,0.05,label='c1 f.05')
sim(1,0.1,label='c1 f.10')
sim(5,0.25,band=1.0,label='c5 f.25 band1/N')
sim(21,1.0,label='c21 full')
print('w top:',sorted(zip(np.round(w,3),[c['id'] for c in cands]))[-5:])
