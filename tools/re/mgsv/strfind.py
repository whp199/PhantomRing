#!/usr/bin/env python3
# usage: strfind.py exe word... ; finds NUL-terminated exact ASCII strings and substrings; prints VA + section
import sys,struct,mmap,re
exe=sys.argv[1]
f=open(exe,'rb'); data=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
pe=struct.unpack_from('<I',data,0x3c)[0]
nsec=struct.unpack_from('<H',data,pe+6)[0]; opt=struct.unpack_from('<H',data,pe+20)[0]
imagebase=struct.unpack_from('<Q',data,pe+24+24)[0]
secs=[]
for i in range(nsec):
    name,vs,va,rs,ra=struct.unpack_from('<8sIIII',data,pe+24+opt+i*40)
    secs.append((name.rstrip(b'\0').decode(),va,vs,ra,rs))
def off2va(o):
    for n,va,vs,ra,rs in secs:
        if ra<=o<ra+rs: return imagebase+va+(o-ra),n
    return 0,'?'
for w in sys.argv[2:]:
    wb=w.encode()
    exact=[];sub=0
    for m in re.finditer(re.escape(wb),data):
        o=m.start()
        pre=data[o-1] if o>0 else 0
        post=data[o+len(wb)] if o+len(wb)<len(data) else 0
        if pre==0 and post==0: exact.append(o)
        else: sub+=1
    wide=len(list(re.finditer(re.escape(w.encode('utf-16le')),data)))
    s=' '.join('%s:0x%x'%(off2va(o)[1],off2va(o)[0]) for o in exact[:4])
    print('%-40s exact=%d substr=%d utf16=%d %s'%(w,len(exact),sub,wide,s),flush=True)
