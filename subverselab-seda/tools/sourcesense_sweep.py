"""Grid sweep of the SourceSense decision constants (kTemper, kFitFull, kFitNone, sigma-floor scale) on a labelled set: counts correct clips per setting.
   usage: sourcesense_sweep.py DIR [DIR2 ...]   (the class table is re-measured from the same clips, so this is an in-sample sweep)"""
import sys, itertools, numpy as np
sys.path.insert(0,'tools'); import sourcesense_eval as ev, sourcesense_fit as fit
import sys as _s
rows=[r for d in (_s.argv[1:] or ['/tmp/ss-A']) for r in ev.run_dir(d)]
C7=ev.CLASSES[:7]
X={r['name']:np.array(fit.transform(r['f'])) for r in rows}
mu=np.array([[np.mean([X[r['name']][k] for r in rows if r['truth']==c]) for k in range(8)] for c in C7])
sd=np.array([[np.std([X[r['name']][k] for r in rows if r['truth']==c],ddof=1) for k in range(8)] for c in C7])
floor=np.array([0.35,0.30,0.06,0.45,0.9,0.05,0.25,0.04])
def acc(T,ff,fn,sf=1.0):
    ok=0; per={}
    for r in rows:
        x=X[r['name']]; s=np.maximum(sd,floor*sf); z2=(((x-mu)/s)**2).sum(1)/8; ll=-0.5*z2*8/T
        b=int(np.argmax(ll)); p=np.exp(ll-ll[b]); p=p/p.sum(); fitf=1 if z2[b]<=ff else (0 if z2[b]>=fn else (fn-z2[b])/(fn-ff))
        conf=p[b]*fitf; pred=C7[b] if (conf>=0.7 and r['f']['activeSec']>=2) else 'unsure'
        ok+= pred==r['truth']
    return ok
best=[]
for T in (1,2,3,4,6):
  for ff in (2,3,4,5,6):
    for fn in (6,9,12,16,25):
      for sf in (1.0,1.5):
        if fn>ff: best.append((acc(T,ff,fn,sf),T,ff,fn,sf))
best.sort(reverse=True); print(best[:15])
