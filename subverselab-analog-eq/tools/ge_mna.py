"""GE-1100 EQ stage, traced wiring: pot ends on N (IC201-2/2 inverting input) and MID (after R229, IC201-1/2 + input),
centre taps and gyrator returns to ground. Ideal op-amps. Output = V(MID) * stage-2 gain."""
import numpy as np
bands=[(10e-6,0.22e-6,100e3,470),(4.7e-6,0.12e-6,91e3,470),(2.2e-6,0.068e-6,91e3,470),(1.22e-6,0.033e-6,82e3,470),
 (0.57e-6,0.018e-6,82e3,470),(0.25e-6,0.012e-6,68e3,470),(0.15e-6,0.0068e-6,51e3,470),(0.068e-6,0.0047e-6,43e3,470),
 (0.039e-6,0.0022e-6,39e3,470),(0.018e-6,0.0022e-6,24e3,430),(0.01e-6,0.0012e-6,20e3,430),(0.0056e-6,680e-12,16e3,390)]
F=[16,32,64,125,250,500,1000,2000,4000,8000,16000,32000]
def H(f,pos,endA='N',endB='MID'):
    w=2j*np.pi*f; names=['src','P','N','out1','MID']; ix={n:i for i,n in enumerate(names)}
    n=len(names)+len(bands); Y=np.zeros((n+1,n+1),complex)
    def add(a,b,y):
        if a is not None: Y[a,a]+=y
        if b is not None: Y[b,b]+=y
        if a is not None and b is not None: Y[a,b]-=y; Y[b,a]-=y
    add(ix['src'],ix['P'],1/(30e3+1/(w*6.8e-6))); add(ix['P'],None,1/56e3)
    add(ix['out1'],ix['N'],1/2.7e3+w*470e-12); add(ix['out1'],ix['MID'],1/2.7e3)
    Rp=100e3; A,B=ix[endA],ix[endB]
    for k,((Cs,Cg,Rb,Rs),x) in enumerate(zip(bands,pos)):
        wn=len(names)+k; Z=Rs+w*(Rb*Rs*Cg)+1/(w*Cs); add(wn,None,1/Z)
        x=min(max(x,1e-6),1-1e-6)
        if x<=0.5: add(A,wn,1/(x*Rp)); add(wn,None,1/((0.5-x)*Rp+1e-3)); add(None,B,1/(0.5*Rp))
        else: add(A,None,1/(0.5*Rp)); add(None,wn,1/((x-0.5)*Rp+1e-3)); add(wn,B,1/((1-x)*Rp))
    G=1e6; Y[ix['src'],ix['src']]+=G; I=np.zeros(n+1,complex); I[ix['src']]=G
    o=ix['out1']; Y[o,n]+=1; Y[n,ix['P']]=1; Y[n,ix['N']]=-1
    V=np.linalg.solve(Y,I)
    return V[ix['MID']]*(1+(1/(1/20e3+w*68e-12))/36e3)
if __name__=='__main__':
    db=lambda z:20*np.log10(abs(z))
    flat=[db(H(f,[0.5]*12)) for f in F]; print("flat at centres:", " ".join(f"{v:+.2f}" for v in flat))
    print("flat 20 Hz..20 kHz:", " ".join(f"{db(H(f,[0.5]*12)):+.2f}" for f in [20,50,100,1000,10000,20000]))
    for end,x in (("wiper at N",0.0),("wiper at MID",1.0)):
        vals=[]
        for k,f in enumerate(F):
            p=[0.5]*12; p[k]=x; vals.append(db(H(f,p)))
        print(f"{end:13s}", " ".join(f"{v:+.1f}" for v in vals))
