#!/usr/bin/env python3
# usage: aobscan.py exe patterns_file(name<TAB>pattern per line)   -> prints matches as VA
import sys,re,struct,mmap
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
    return None,None
def compile_pat(p):
    out=b''
    for t in p.split():
        if t in ('?','??','*','**') or '?' in t or '*' in t: out+=b'.'
        else: out+=re.escape(bytes([int(t,16)]))
    return re.compile(out,re.S)
maxhits=int(sys.argv[3]) if len(sys.argv)>3 else 5
for line in open(sys.argv[2]):
    line=line.rstrip('\n')
    if not line or line.startswith('#'): continue
    name,pat=line.split('\t',1)
    rx=compile_pat(pat)
    hits=[]
    for m in rx.finditer(data):
        hits.append(m.start())
        if len(hits)>maxhits: break
    s=', '.join('%s:0x%x'%(off2va(h)[1],off2va(h)[0]) for h in hits[:maxhits])
    print('%-28s %s%s %s'%(name,len(hits) if len(hits)<=maxhits else '>%d'%maxhits,'',s),flush=True)
