# TRAIN-only nested composition study on cached candidate signals (diagnostic, not evaluator).
import numpy as np, json, math, glob, os, sys, time
t0=time.time()
rd='build-equity/recent-fast-train-2020-2022-v1'; lib=json.load(open(sys.argv[1])); cache=sys.argv[2]
m=json.load(open(rd+'/manifest.json')); D,N=m['dates'],m['instruments']; sb=399
c=np.fromfile(rd+'/close.f64').reshape(D,N); pr=np.fromfile(rd+'/present.u8',dtype=np.uint8).reshape(D,N).astype(bool)
mem=np.fromfile(rd+'/member.u8',dtype=np.uint8).reshape(D,N).astype(bool)
rc=np.fromfile(rd+'/raw_close.f64').reshape(D,N); vo=np.fromfile(rd+'/volume.f64').reshape(D,N)
ok=pr&np.isfinite(c)&(c>0)
r=np.full((D,N),np.nan); r[1:]=np.where(ok[1:]&ok[:-1],c[1:]/np.where(ok[:-1],c[:-1],1)-1,np.nan)
rr=np.where(ok[1:]&ok[:-1]&(rc[1:]>0)&(rc[:-1]>0),np.log(np.where(ok[1:],rc[1:],1)/np.where(ok[:-1],rc[:-1],1)),np.nan)
la=np.log1p(r[1:]); g=(np.abs(la)>1.5)|(np.abs(la)>np.abs(rr)+0.10); r[1:][g]=np.nan
mk=np.nanmean(np.where(mem[:-1],r[1:],np.nan),axis=1); mk=np.r_[np.nan,mk]
dv=np.where(ok,rc*vo,0.0)
def roll(x,L):
    cs=np.nancumsum(np.nan_to_num(x),axis=0); cs=np.vstack([np.zeros((1,)+x.shape[1:]),cs]); return cs[L:]-cs[:-L]
# exposures at each date d (window ending d): beta252, vol63, logadv63
R=np.nan_to_num(r); V=np.isfinite(r)&np.isfinite(mk)[:,None]; M=np.where(V,mk[:,None],0.0); R=np.where(V,R,0.0)
L=252; n=roll(V.astype(float),L); sR=roll(R,L); sM=roll(M,L); sRM=roll(R*M,L); sMM=roll(M*M,L)
beta=np.full((D,N),np.nan); cov=sRM-sR*sM/np.maximum(n,1); var=sMM-sM*sM/np.maximum(n,1)
beta[L-1:]=np.where(n>=126,cov/np.where(var>0,var,np.nan),np.nan)
L2=63; V2=np.isfinite(r); n2=roll(V2.astype(float),L2); s1=roll(np.where(V2,r,0),L2); s2=roll(np.where(V2,r*r,0),L2)
vol=np.full((D,N),np.nan); vol[L2-1:]=np.where(n2>=32,np.sqrt(np.maximum(s2-s1*s1/np.maximum(n2,1),0)/np.maximum(n2-1,1)),np.nan)
adv=np.full((D,N),np.nan); adv[L2-1:]=roll(dv,L2)/L2; ladv=np.where(adv>0,np.log(np.where(adv>0,adv,1)),np.nan)
fwd=np.full((D,N),np.nan); fwd[:-2]=r[2:]   # decision d -> entry d+1 -> endpoint d+2
dates=range(sb,D-2)
P={}; IDX={}
for d in dates:
    s=mem[d]&np.isfinite(beta[d])&np.isfinite(vol[d])&np.isfinite(ladv[d]); idx=np.where(s)[0]
    X=np.c_[beta[d,idx],vol[d,idx],ladv[d,idx]]; X=(X-X.mean(0))/X.std(0); X=np.clip(X,-5,5); X=np.c_[np.ones(len(idx)),X]
    P[d]=(np.linalg.solve(X.T@X,X.T),X); IDX[d]=idx
def ranks(x):
    o=np.argsort(x,kind='stable'); k=np.empty(len(x)); k[o]=np.arange(len(x)); return k/(len(x)-1)-0.5
cands=lib['candidates']; F=np.zeros((len(cands),len(dates))); cov_=np.zeros(len(cands))
for k,cand in enumerate(cands):
    fs=glob.glob(f"{cache}/*/{cand['id']}.f64"); sig=np.fromfile(fs[0]).reshape(D,N)
    for j,d in enumerate(dates):
        idx=IDX[d]; x=sig[d,idx]; f=np.isfinite(x)
        q=np.zeros(len(idx)); q[f]=ranks(x[f]); Pd,X=P[d]; q=q-X@(Pd@q); gs=np.abs(q).sum()
        if gs>0: q/=gs
        F[k,j]=np.nansum(q*np.nan_to_num(fwd[d,idx]))
    cov_[k]=np.mean(np.isfinite(sig[sb:D-2][mem[sb:D-2]]))
yr=np.array([int(str(np.fromfile(rd+'/sessions.i64',dtype=np.int64)[d]//86400_000_000_000//365+1970)) for d in dates])
fit=yr<=2021; hold=yr==2022
sr=lambda v: v.mean()/v.std()*math.sqrt(252) if v.std()>0 else 0
sg=np.sign(F[:,fit].mean(1)); sg[sg==0]=1; Fo=F*sg[:,None]
fam=[c_['family'] for c_ in cands]; fams=sorted(set(fam))
def report(name,w):
    w=np.asarray(w,float); p=w@Fo; print(f'{name:22s} fitSR {sr(p[fit]):5.2f}  hold2022 SR {sr(p[hold]):5.2f}  all {sr(p):5.2f}')
report('equal',np.ones(len(cands))/len(cands))
fw=np.array([1/len(fams)/fam.count(f) for f in fam]); report('family-equal',fw)
iv=1/Fo[:,fit].std(1); report('inv-vol',iv/iv.sum())
ic=np.maximum(Fo[:,fit].mean(1),0); report('pos-mean (fit)',ic/ic.sum() if ic.sum()>0 else ic)
t=Fo[:,fit].mean(1)/Fo[:,fit].std(1); tw=np.maximum(t,0); report('pos-sharpe (fit)',tw/tw.sum())
S=np.cov(Fo[:,fit]); mu=Fo[:,fit].mean(1)
for lam in [0.5,0.9]:
    Sh=(1-lam)*S+lam*np.diag(np.diag(S)); w=np.linalg.solve(Sh,mu); report(f'MV shrink{lam}',w/np.abs(w).sum())
print('per-candidate hold2022 SR top/bottom:')
h=[(sr(Fo[k,hold]),sr(Fo[k,fit]),cands[k]['id'],round(cov_[k],3)) for k in range(len(cands))]
for x in sorted(h)[-6:]+sorted(h)[:4]: print('  %-34s fit %.2f hold %.2f cov %.3f'%(x[2],x[1],x[0],x[3]))
C=np.corrcoef(Fo[:,fit]); print('mean pairwise corr (fit) %.2f'%((C.sum()-len(C))/(len(C)**2-len(C))), 'secs %.1f'%(time.time()-t0))
np.save(sys.argv[3],F)
