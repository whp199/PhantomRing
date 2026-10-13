#!/usr/bin/env python3
# usage: luareg.py exe funcStartVA funcEndVA [...]  -> extract {name,cfunc} pairs built on stack (luaL_Reg arrays) + all strings referenced
import sys,struct,mmap
if __import__('os').environ.get('CAPSTONE_PATH'):  # optional: capstone outside site-packages
    sys.path.insert(0, __import__('os').environ['CAPSTONE_PATH'])
import capstone
from capstone import x86
exe=sys.argv[1]
f=open(exe,'rb'); data=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
pe=struct.unpack_from('<I',data,0x3c)[0]
nsec=struct.unpack_from('<H',data,pe+6)[0]; opt=struct.unpack_from('<H',data,pe+20)[0]
imagebase=struct.unpack_from('<Q',data,pe+24+24)[0]
secs=[]
for i in range(nsec):
    name,vs,va,rs,ra,_,_,_,_,ch=struct.unpack_from('<8sIIIIIIHHI',data,pe+24+opt+i*40)
    secs.append((name.rstrip(b'\0').decode(),va,vs,ra,rs,ch))
def va2off(v):
    r=v-imagebase
    for n,va,vs,ra,rs,ch in secs:
        if va<=r<va+rs: return ra+(r-va)
def isexec(v):
    r=v-imagebase
    for n,va,vs,ra,rs,ch in secs:
        if va<=r<va+vs: return bool(ch&0x20000000)
    return False
def cstr(v):
    o=va2off(v)
    if o is None: return None
    s=data[o:o+100].split(b'\0')[0]
    if len(s)>=2 and all(32<=c<127 for c in s): return s.decode()
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.detail=True
args=sys.argv[2:]
for k in range(0,len(args),2):
    start=int(args[k],16); end=int(args[k+1],16)
    code=data[va2off(start):va2off(end)]
    pend=None; out=[]; strs=[]
    for ins in md.disasm(code,start):
        ops=ins.operands
        if ins.mnemonic=='lea' and len(ops)==2 and ops[1].type==x86.X86_OP_MEM and ops[1].mem.base==x86.X86_REG_RIP:
            t=ins.address+ins.size+ops[1].mem.disp
            s=cstr(t)
            if s and not isexec(t):
                pend=s; strs.append(s)
            elif isexec(t) and pend:
                out.append((pend,t)); pend=None
    print('==== func 0x%x-0x%x'%(start,end))
    for n,fn in out: print('  %-45s 0x%x'%(n,fn))
    others=[s for s in strs if s not in dict(out)]
    print('  other strings:',others[:40])
