#!/usr/bin/env python3
# usage: varsdump.py exe startVA endVA  -> extracts ScriptVar registrations (name, base global, offset, hash, flags)
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
    name,vs,va,rs,ra=struct.unpack_from('<8sIIII',data,pe+24+opt+i*40)
    secs.append((name.rstrip(b'\0').decode(),va,vs,ra,rs))
def va2off(v):
    r=v-imagebase
    for n,va,vs,ra,rs in secs:
        if va<=r<va+rs: return ra+(r-va)
def cstr(v):
    o=va2off(v)
    if o is None: return None
    s=data[o:o+100].split(b'\0')[0]
    if len(s)>=2 and all(32<=c<127 for c in s): return s.decode()
start=int(sys.argv[2],16); end=int(sys.argv[3],16)
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.detail=True
code=data[va2off(start):va2off(end)]
regs={}  # reg -> (global, off)
entries=[]; cur=None
for ins in md.disasm(code,start):
    ops=ins.operands
    if ins.mnemonic=='mov' and len(ops)==2 and ops[0].type==x86.X86_OP_REG and ops[1].type==x86.X86_OP_MEM and ops[1].mem.base==x86.X86_REG_RIP and ins.op_str.startswith(('r8','r9','rcx','rdx','rax')) and 'qword' in ins.op_str:
        regs[ops[0].reg]=(ins.address+ins.size+ops[1].mem.disp,0)
    elif ins.mnemonic=='add' and len(ops)==2 and ops[0].type==x86.X86_OP_REG and ops[1].type==x86.X86_OP_IMM and ops[0].reg in regs:
        g,o=regs[ops[0].reg]; regs[ops[0].reg]=(g,o+ops[1].imm)
    elif ins.mnemonic=='lea' and ops[1].type==x86.X86_OP_MEM and ops[1].mem.base==x86.X86_REG_RIP and ops[0].reg==x86.X86_REG_RDX:
        t=ins.address+ins.size+ops[1].mem.disp
        s=cstr(t)
        if s:
            r8=regs.get(x86.X86_REG_R8)
            cur=[s,r8,None,None,ins.address]
            entries.append(cur)
    elif ins.mnemonic=='mov' and cur and ops[0].type==x86.X86_OP_MEM and ops[1].type==x86.X86_OP_IMM and 'dword' in ins.op_str:
        if ops[0].mem.disp==0x10 and cur[2] is None: cur[2]=ops[1].imm & 0xffffffff
        elif ops[0].mem.disp==0x14 and cur[3] is None: cur[3]=ops[1].imm & 0xffffffff
    elif ins.mnemonic=='call':
        pass
seen=set()
for name,r8,h,fl,a in entries:
    if name in seen: continue
    seen.add(name)
    if r8: print('%-40s base=[0x%x]+0x%-5x hash=%s flags=%s  @0x%x'%(name,r8[0],r8[1],('0x%08x'%h) if h is not None else '-',('0x%08x'%fl) if fl is not None else '-',a))
    else: print('%-40s (no r8) @0x%x'%(name,a))
