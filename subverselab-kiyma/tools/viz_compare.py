import numpy as np, wave, sys
from PIL import Image, ImageDraw
def load(p):
    w=wave.open(p); n=w.getnframes(); ch=w.getnchannels(); sw=w.getsampwidth(); fr=w.readframes(n)
    if sw==3:
        b=np.frombuffer(fr,np.uint8).reshape(-1,3); x=(b[:,0].astype(np.int32)|(b[:,1].astype(np.int32)<<8)|(b[:,2].astype(np.int32)<<16)); x=np.where(x>=2**23,x-2**24,x)/2**23
    elif sw==2: x=np.frombuffer(fr,np.int16)/32768
    else: x=np.frombuffer(fr,np.int32)/2**31
    return x.reshape(-1,ch).mean(1), w.getframerate()
def spec(x,sr,W=1200,H=220):
    n=2048; hop=max(1,(len(x)-n)//W); win=np.hanning(n)
    S=np.array([np.abs(np.fft.rfft(x[i*hop:i*hop+n]*win)) for i in range(W)]).T
    S=20*np.log10(S+1e-9); S=np.clip((S+20)/100*255+255-255,0,255)  # -120..-20 -> 0..255? 
    db=20*np.log10(np.abs(np.array([np.fft.rfft(x[i*hop:i*hop+n]*win) for i in range(W)])).T+1e-9)
    db=np.clip((db-np.max(db)+90)/90,0,1)
    f=np.linspace(0,1,db.shape[0]); fl=np.logspace(np.log10(30/(sr/2)),0,H)
    idx=np.clip((fl*(db.shape[0]-1)).astype(int),0,db.shape[0]-1)
    img=(db[idx][::-1]*255).astype(np.uint8)
    rgb=np.stack([img, (img*0.7).astype(np.uint8), (255-img)//3],-1)
    return Image.fromarray(rgb)
def wavimg(x,W=1200,H=90):
    im=Image.new('RGB',(W,H),(15,15,18)); d=ImageDraw.Draw(im); s=len(x)/W
    for i in range(W):
        seg=x[int(i*s):int((i+1)*s)]; 
        if len(seg)==0: continue
        a,b=seg.min(),seg.max(); d.line([i,H/2-b*H/2,i,H/2-a*H/2],fill=(220,170,80) if max(abs(a),abs(b))<0.99 else (255,40,40))
    return im
files=sys.argv[2:]; rows=[]
for f in files:
    x,sr=load(f); rows+= [wavimg(x), spec(x,sr)]
    lab=Image.new('RGB',(1200,16),(0,0,0)); ImageDraw.Draw(lab).text((4,2),f.split('/')[-1]+f"  peak {20*np.log10(np.abs(x).max()):.1f} dBFS  rms {20*np.log10(np.sqrt((x**2).mean())):.1f}",fill=(255,255,255)); rows.insert(len(rows)-2,lab)
H=sum(r.height for r in rows); out=Image.new('RGB',(1200,H)); y=0
for r in rows: out.paste(r,(0,y)); y+=r.height
out.save(sys.argv[1])
