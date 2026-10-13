#!/usr/bin/env python3
# usage: pdata.py exe [VA...] ; prints containing RUNTIME_FUNCTION; with no VA prints stats
import sys,struct,mmap,bisect
exe=sys.argv[1]
f=open(exe,'rb'); data=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
pe=struct.unpack_from('<I',data,0x3c)[0]
nsec=struct.unpack_from('<H',data,pe+6)[0]; opt=struct.unpack_from('<H',data,pe+20)[0]
o=pe+24
imagebase=struct.unpack_from('<Q',data,o+24)[0]
exc_rva,exc_size=struct.unpack_from('<II',data,o+112+3*8)
secs=[]
for i in range(nsec):
    name,vs,va,rs,ra=struct.unpack_from('<8sIIII',data,o+opt+i*40)
    secs.append((name.rstrip(b'\0').decode(),va,vs,ra,rs))
def rva2off(r):
    for n,va,vs,ra,rs in secs:
        if va<=r<va+rs: return ra+(r-va)
def secname(r):
    for n,va,vs,ra,rs in secs:
        if va<=r<va+max(vs,rs): return n
    return '?'
po=rva2off(exc_rva)
cnt=exc_size//12
rf=[struct.unpack_from('<III',data,po+i*12) for i in range(cnt)]
rf.sort()
starts=[r[0] for r in rf]
if len(sys.argv)==2:
    from collections import Counter
    c=Counter(secname(r[0]) for r in rf)
    print('RUNTIME_FUNCTION count',cnt,dict(c))
    sys.exit()
for a in sys.argv[2:]:
    v=int(a,16)-imagebase
    i=bisect.bisect_right(starts,v)-1
    if i>=0 and rf[i][0]<=v<rf[i][1]:
        print('0x%x in func 0x%x-0x%x (unwind 0x%x)'%(v+imagebase,rf[i][0]+imagebase,rf[i][1]+imagebase,rf[i][2]+imagebase))
    else:
        print('0x%x not in pdata func (prev 0x%x-0x%x)'%(v+imagebase,rf[i][0]+imagebase,rf[i][1]+imagebase))
