import numpy as np, struct, zlib, sys
def readpng(p):
    d=open(p,'rb').read(); pos=8; idat=b''; w=h=0
    while pos<len(d):
        ln=struct.unpack('>I',d[pos:pos+4])[0]; t=d[pos+4:pos+8]; c=d[pos+8:pos+8+ln]; pos+=12+ln
        if t==b'IHDR': w,h,bd,ct=struct.unpack('>IIBB',c[:10])
        elif t==b'IDAT': idat+=c
    raw=zlib.decompress(idat); bpp=4 if ct==6 else 3; stride=w*bpp; out=np.zeros((h,stride),np.uint8); prev=np.zeros(stride,np.int32); i=0
    for y in range(h):
        f=raw[i]; i+=1; line=np.frombuffer(raw[i:i+stride],np.uint8).astype(np.int32); i+=stride
        if f==0: cur=line
        elif f==1:
            cur=line.copy()
            for x in range(bpp,stride): cur[x]=(cur[x]+cur[x-bpp])&255
        elif f==2: cur=(line+prev)&255
        elif f==3:
            cur=line.copy()
            for x in range(stride): a=cur[x-bpp] if x>=bpp else 0; cur[x]=(cur[x]+((a+prev[x])>>1))&255
        else:
            cur=line.copy()
            for x in range(stride):
                a=cur[x-bpp] if x>=bpp else 0; b=prev[x]; c=prev[x-bpp] if x>=bpp else 0
                pa=abs(b-c); pb=abs(a-c); pc=abs(a+b-2*c); pr=a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                cur[x]=(cur[x]+pr)&255
        out[y]=cur; prev=cur
    return out.reshape(h,w,bpp)[:,:,:3].mean(axis=2)
tag=sys.argv[1]
a=readpng(f'{tag}_eye0.png'); b=readpng(f'{tag}_eye1.png'); h,w=a.shape
for name,(y0,y1,x0,x1) in {'near-pistol':(int(h*0.62),int(h*0.95),int(w*0.12),int(w*0.42)),'far-building':(int(h*0.05),int(h*0.35),int(w*0.25),int(w*0.6))}.items():
    ra=a[y0:y1,x0:x1]; best=None
    for s in range(-80,81,2):
        if x0+s<0 or x1+s>w: continue
        err=np.mean((ra-b[y0:y1,x0+s:x1+s])**2)
        if best is None or err<best[1]: best=(s,err)
    print(f'{tag} {name}: eye1 shift vs eye0 = {best[0]} px (mse {best[1]:.1f})')
