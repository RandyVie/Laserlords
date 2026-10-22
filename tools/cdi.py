import struct, sys
import os
B=open(os.environ.get('LL_BIN','data/ll.bin'),'rb')
def sec(n):
    B.seek(n*2352); r=B.read(2352)
    sub=r[16:20]; form2 = bool(sub[2]&0x20)
    return r[24:24+(2324 if form2 else 2048)], sub
lbl,_=sec(16)
pt_size=struct.unpack('>I',lbl[136:140])[0]; pt_addr=struct.unpack('>I',lbl[148:152])[0]
pt=sec(pt_addr)[0][:pt_size]
dirs=[];i=0
while i<len(pt):
    nl=pt[i]; addr=struct.unpack('>I',pt[i+2:i+6])[0]; par=struct.unpack('>H',pt[i+6:i+8])[0]
    name=pt[i+8:i+8+nl]; dirs.append((name,addr,par)); i+=8+nl+(nl&1)
print('dirs',dirs)
files=[]
for dname,addr,par in dirs:
    s=addr; done=False
    while not done:
        d,_=sec(s); j=0
        while j<2048:
            L=d[j]
            if L==0: break
            size=struct.unpack('>I',d[j+14:j+18])[0]; lbn=struct.unpack('>I',d[j+6:j+10])[0]
            flags=d[j+25]; il=d[j+26:j+28]; nl=d[j+32]; name=d[j+33:j+33+nl]
            su=d[j+33+nl+((nl+1)&1):j+L]
            attr=struct.unpack('>H',su[4:6])[0] if len(su)>=6 else 0
            fnum=su[8] if len(su)>8 else 0
            if name not in (b'\x00',b'\x01'):
                files.append((dname,name.decode('latin1'),lbn,size,flags,il.hex(),attr,fnum))
            j+=L
        # dir size from first record
        done=True
for f in files: print(f)

import collections, os
os.makedirs('ext',exist_ok=True)
def read_file(lbn,size):
    out=b''; n=(size+2047)//2048
    for k in range(n): out+=sec(lbn+k)[0][:2048]
    return out[:size]
for d,name,lbn,size,fl,il,attr,fnum in files:
    if not name.endswith('.rtf'):
        open('ext/'+name,'wb').write(read_file(lbn,size))
# survey rtf sector types
for d,name,lbn,size,fl,il,attr,fnum in files:
    if name.endswith('.rtf'):
        n=size//2048 if size%2048==0 else (size+2047)//2048
        c=collections.Counter()
        for k in range(min(n,4000)):
            B.seek((lbn+k)*2352+16); sh=B.read(4)
            sm=sh[2]; kind='A' if sm&4 else 'V' if sm&2 else 'D' if sm&8 else '?'
            c[(kind,sh[1],sh[3])]+=1
        print(name, n, dict(c.most_common(6)))
