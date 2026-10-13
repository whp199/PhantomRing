#!/usr/bin/env python3
# usage: funcs.py exe startVA endVA -> list function starts: pdata starts + addresses after int3/ret padding aligned 16
import sys,struct,mmap,bisect
exe=sys.argv[1]
f=open(exe,'rb'); data=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
pe=struct.unpack_from('<I',data,0x3c)[0]
nsec=struct.unpack_from('<H',data,pe+6)[0]; opt=struct.unpack_from('<H',data,pe+20)[0]
o=pe+24; imagebase=struct.unpack_from('<Q',data,o+24)[0]
exc_rva,exc_size=struct.unpack_from('<II',data,o+112+3*8)
secs=[]
for i in range(nsec):
    name,vs,va,rs,ra=struct.unpack_from('<8sIIII',data,o+opt+i*40); secs.append((va,rs,ra))
def rva2off(r):
    for va,rs,ra in secs:
        if va<=r<va+rs: return ra+(r-va)
po=rva2off(exc_rva)
pd={}
for i in range(exc_size//12):
    b,e,u=struct.unpack_from('<III',data,po+i*12); pd[b+imagebase]=e+imagebase
s=int(sys.argv[2],16); e=int(sys.argv[3],16)
starts=set(k for k in pd if s<=k<e)
# padding heuristic: 16-aligned address preceded by 0xcc
for a in range(s,e,16):
    off=rva2off(a-imagebase)
    if data[off-1]==0xcc and data[off]!=0xcc: starts.add(a)
for a in sorted(starts):
    off=rva2off(a-imagebase)
    print('0x%x %s %s'%(a,'pdata' if a in pd else 'pad  ',data[off:off+16].hex()))
