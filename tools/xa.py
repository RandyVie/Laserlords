import struct, wave, sys, collections, numpy as np
import os
B=open(os.environ.get('LL_BIN','data/ll.bin'),'rb')
K0=[0,60,115,98]; K1=[0,0,-52,-55]
def decode(lbn, nsec, chan_filter=None):
    chans=collections.defaultdict(list); fmt={}
    for k in range(nsec):
        B.seek((lbn+k)*2352); r=B.read(2352)
        fn,ch,sm,ci=r[16:20]
        if not sm&4: continue
        chans[ch].append(r[24:24+2304]); fmt[ch]=ci
    return chans, fmt
def xa_to_pcm(sectors, ci):
    stereo=ci&1; bits8=(ci>>4)&1; rate=18900 if (ci>>2)&1 else 37800
    nunits = 4 if bits8 else 8
    out=[]; st=[[0,0],[0,0]]
    for s in sectors:
        for g in range(18):
            grp=s[g*128:(g+1)*128]; hdr=grp[:16]; data=grp[16:]
            for u in range(nunits):
                p=hdr[4+u]; sh=p&15; f=(p>>4)&3; c=(u&1) if stereo else 0
                p1,p2=st[c]; buf=[]
                for i in range(28):
                    if bits8: n=data[i*4+u]; n = n-256 if n>127 else n; v=(n<<8)>>sh
                    else:
                        b=data[i*4+(u>>1)]; n=(b>>4) if u&1 else (b&15); n = n-16 if n>7 else n; v=(n<<12)>>sh
                    v += (p1*K0[f]+p2*K1[f]+32)>>6
                    v=max(-32768,min(32767,v)); p2,p1=p1,v; buf.append(v)
                st[c]=[p1,p2]; out.append((c,buf))
    if stereo:
        L=[];R=[]
        for c,b in out: (R if c else L).extend(b)
        n=min(len(L),len(R)); a=np.empty(n*2,np.int16); a[0::2]=L[:n]; a[1::2]=R[:n]
    else:
        a=np.array([x for _,b in out for x in b],np.int16)
    return a, rate, 2 if stereo else 1
lbn,nsec,prefix=int(sys.argv[1]),int(sys.argv[2]),sys.argv[3]
chans,fmt=decode(lbn,nsec)
import os; os.makedirs('audio',exist_ok=True)
for ch in sorted(chans):
    a,rate,nc=xa_to_pcm(chans[ch],fmt[ch])
    w=wave.open(f'audio/{prefix}_ch{ch:02d}.wav','wb'); w.setnchannels(nc); w.setsampwidth(2); w.setframerate(rate); w.writeframes(a.tobytes()); w.close()
    print(prefix, ch, f'{len(a)/nc/rate:.1f}s', rate, 'stereo' if nc==2 else 'mono', 'peak',int(np.abs(a.astype(np.int32)).max()))
