# TRAIN-only construction prototype (diagnostic; not the evaluation engine).
import numpy as np, json, math, sys
rd, bd, pref = sys.argv[1], sys.argv[2], sys.argv[3]
m=json.load(open(rd+'/manifest.json')); D,N=m['dates'],m['instruments']
c=np.fromfile(rd+'/close.f64').reshape(D,N); pr=np.fromfile(rd+'/present.u8',dtype=np.uint8).reshape(D,N).astype(bool)
rc=np.fromfile(rd+'/raw_close.f64').reshape(D,N); vol=np.fromfile(rd+'/volume.f64').reshape(D,N)
bl=np.fromfile(f'{bd}/{pref}_combined.f64').reshape(D,N); fin=np.fromfile(f'{bd}/{pref}_combined_finite.u8',dtype=np.uint8).reshape(D,N).astype(bool)
cj=json.load(open(f'{bd}/{pref}_combined.json')); sb,se=cj.get('score_begin',None),cj.get('score_end',None)
if sb is None: sb=int(sys.argv[4]); se=D
ok=pr&np.isfinite(c)&(c>0)
r=np.full((D,N),np.nan); r[1:]=np.where(ok[1:]&ok[:-1],c[1:]/np.where(ok[:-1],c[:-1],1)-1,np.nan)
r=np.where(np.abs(r)>0.8,np.nan,r)  # crude guard for prototype only
mkt=np.nanmean(np.where(fin,1,np.nan)*np.nan_to_num(r,nan=0)*0+r,axis=1)
dv=np.where(ok,rc*vol,np.nan)
def trail(fn,L,d): return fn(d,L)
def expo(d):
    lo=max(0,d-252)
    R=r[lo+1:d+1]; M=np.nanmean(R,axis=1)
    Rz=np.nan_to_num(R-np.nanmean(R,axis=0)); Mz=(M-M.mean())[:,None]
    cnt=np.sum(np.isfinite(R),axis=0)
    beta=(Rz*Mz).sum(0)/max((Mz**2).sum(),1e-12)
    v63=np.nanstd(r[max(0,d-62):d+1],axis=0)
    adv=np.log(np.nanmean(dv[max(0,d-62):d+1],axis=0))
    E=np.stack([beta,v63,adv],1); E[cnt<126,:]=np.nan
    return E
def ranks(x):
    o=np.argsort(x,kind='stable'); rk=np.empty(len(x)); rk[o]=np.arange(len(x)); return rk/(len(x)-1)-0.5
def run(mode,cad=5,frac=0.25,cost=6e-4,borrow=0.03,hl=0,label=''):
    w=np.zeros(N); pnl=[]; to=0.0; gross=[]
    for d in range(sb,se-2):
        if (d-sb)%cad==0:
            s=fin[d]; t=np.zeros(N)
            if mode!='raw':
                E=expo(d); s=s&np.all(np.isfinite(E),1)
            sig=S[d] if hl else bl[d]
            idx=np.where(s&np.isfinite(sig))[0]; q=ranks(sig[idx])
            if mode!='raw':
                X=E[idx]; X=(X-X.mean(0))/X.std(0); X=np.c_[np.ones(len(idx)),X]
                q=q-X@np.linalg.lstsq(X,q,rcond=None)[0]
            t[idx]=q/np.abs(q).sum()
            nw=np.where(fin[d],w+frac*(t-w),0.0)
            to+=np.abs(nw-w).sum(); tc=np.abs(nw-w).sum()*cost; w=nw
        else: tc=0.0
        rr=np.nan_to_num(r[d+2],nan=0.0)  # entry d+1 -> endpoint d+2 ; missing=0 (prototype)
        g=(w*rr).sum(); pnl.append((g, g-tc-borrow/252*np.abs(np.minimum(w,0)).sum())); gross.append(np.abs(w).sum())
    p=np.array(pnl); mk=np.array([np.nanmean(r[d+2]) for d in range(sb,se-2)])
    sr=lambda v: v.mean()/v.std()*math.sqrt(252)
    b=np.cov(p[:,0],mk)[0,1]/np.var(mk)
    months=(se-2-sb)/21
    print(f'{label:18s} grossSR {sr(p[:,0]):.2f} netSR {sr(p[:,1]):.2f} ann {p[:,0].mean()*252:.4f} vol {p[:,0].std()*math.sqrt(252):.4f} beta {b:.3f} gross {np.mean(gross):.3f} turnover/mo(one-way,L1/2?) {to/months:.3f}')

def smooth(hl):
    a=1-0.5**(1/hl); S=np.full((D,N),np.nan); cur=np.full(N,np.nan)
    for d in range(D):
        x=np.where(fin[d],bl[d],np.nan)
        # rank-normalize daily before smoothing so scale is stable
        v=np.isfinite(x); z=np.full(N,np.nan)
        if v.sum()>10: z[v]=ranks(x[v])
        cur=np.where(np.isfinite(z),np.where(np.isfinite(cur),(1-a)*cur+a*z,z),np.nan)
        S[d]=cur
    return S
S=None
run('neutral',label='neut c5 f.25')
run('neutral',cad=5,frac=0.15,label='neut c5 f.15')
run('neutral',cad=10,frac=0.25,label='neut c10 f.25')
S=smooth(10); run('neutral',hl=10,label='neut ewm10 c5 f.25')
S=smooth(21); run('neutral',hl=21,label='neut ewm21 c5 f.25')
run('neutral',hl=21,borrow=0.005,label='neut ewm21 b50')

