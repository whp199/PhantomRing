#!/usr/bin/env python3
# usage: xref.py exe VA [VA...]  -> find RIP-relative references (disp32) to the given VAs in executable sections
import sys,struct,mmap,re
import numpy as np
exe=sys.argv[1]
f=open(exe,'rb'); data=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
pe=struct.unpack_from('<I',data,0x3c)[0]
nsec=struct.unpack_from('<H',data,pe+6)[0]; opt=struct.unpack_from('<H',data,pe+20)[0]
imagebase=struct.unpack_from('<Q',data,pe+24+24)[0]
secs=[]
for i in range(nsec):
    name,vs,va,rs,ra,_,_,_,_,ch=struct.unpack_from('<8sIIIIIIHHI',data,pe+24+opt+i*40)
    secs.append((name.rstrip(b'\0').decode(),va,vs,ra,rs,ch))
targets=[int(x,16) for x in sys.argv[2:]]
for n,va,vs,ra,rs,ch in secs:
    if not (ch & 0x20000000): continue  # executable
    buf=np.frombuffer(data,dtype=np.uint8,count=rs-4,offset=ra)
    # disp32 at position p; next-instruction address unknown -> assume disp is last 4 bytes of insn: target = va_of(p)+4+disp
    d=np.frombuffer(data,dtype=np.int32,count=0,offset=ra) # placeholder
    raw=bytes(data[ra:ra+rs])
    arr=np.frombuffer(raw[:len(raw)-(len(raw)%1)],dtype=np.uint8)
    # build int32 view at every offset via stride trick
    a=np.lib.stride_tricks.as_strided(np.frombuffer(raw,dtype=np.uint8),shape=(len(raw)-3,4),strides=(1,1))
    disp=(a[:,0].astype(np.int64)|(a[:,1].astype(np.int64)<<8)|(a[:,2].astype(np.int64)<<16)|(a[:,3].astype(np.int64)<<24))
    disp=np.where(disp>=2**31,disp-2**32,disp)
    pos=np.arange(len(raw)-3,dtype=np.int64)
    for t in targets:
        # disp located at insn offset p; RIP = va_p+4 (+imm size, ignored for LEA/MOV)
        hit=np.nonzero(disp==(t-(imagebase+va)-pos-4))[0]
        for h in hit[:40]:
            print('target 0x%x <- %s:0x%x  bytes %s'%(t,n,imagebase+va+h-3, raw[max(0,h-3):h+4].hex()))
        # also with 1-byte imm following (cmp byte [rip+x],imm8) and 4-byte imm
        for extra in (1,4):
            hit=np.nonzero(disp==(t-(imagebase+va)-pos-4-extra))[0]
            for h in hit[:10]:
                print('target 0x%x <- %s:0x%x (+imm%d) bytes %s'%(t,n,imagebase+va+h-3,extra, raw[max(0,h-3):h+4+extra].hex()))
