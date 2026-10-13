#!/usr/bin/env python3
# usage: dis.py exe VA count [VA count ...]
import sys,struct,mmap
if __import__('os').environ.get('CAPSTONE_PATH'):  # optional: capstone outside site-packages
    sys.path.insert(0, __import__('os').environ['CAPSTONE_PATH'])
import capstone
exe=sys.argv[1]
f=open(exe,'rb'); data=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
pe=struct.unpack_from('<I',data,0x3c)[0]
nsec=struct.unpack_from('<H',data,pe+6)[0]; opt=struct.unpack_from('<H',data,pe+20)[0]
imagebase=struct.unpack_from('<Q',data,pe+24+24)[0]
secs=[]
for i in range(nsec):
    name,vs,va,rs,ra=struct.unpack_from('<8sIIII',data,pe+24+opt+i*40)
    secs.append((name.rstrip(b'\0').decode(),va,vs,ra,rs))
def va2off(v):
    r=v-imagebase
    for n,va,vs,ra,rs in secs:
        if va<=r<va+rs: return ra+(r-va)
    return None
def cstr(v):
    o=va2off(v)
    if o is None: return None
    s=data[o:o+80].split(b'\0')[0]
    if len(s)>=3 and all(32<=c<127 for c in s): return s.decode()
    return None
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.detail=True
args=sys.argv[2:]
for i in range(0,len(args),2):
    va=int(args[i],16); cnt=int(args[i+1])
    o=va2off(va)
    code=data[o:o+cnt*15]
    print('---- 0x%x'%va)
    n=0
    for ins in md.disasm(code,va):
        cm=''
        for op in ins.operands:
            if op.type==capstone.x86.X86_OP_MEM and op.mem.base==capstone.x86.X86_REG_RIP:
                t=ins.address+ins.size+op.mem.disp
                s=cstr(t)
                cm='  ; 0x%x%s'%(t,(' "%s"'%s) if s else '')
        print('0x%x: %-24s %s %s%s'%(ins.address,ins.bytes.hex(),ins.mnemonic,ins.op_str,cm))
        n+=1
        if n>=cnt: break
