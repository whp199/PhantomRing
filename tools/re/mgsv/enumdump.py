import sys,struct,mmap
if __import__('os').environ.get('CAPSTONE_PATH'):  # optional: capstone outside site-packages
    sys.path.insert(0, __import__('os').environ['CAPSTONE_PATH'])
import capstone
from capstone import x86
f=open(sys.argv[1],'rb');d=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
pe=struct.unpack_from('<I',d,0x3c)[0];nsec=struct.unpack_from('<H',d,pe+6)[0];opt=struct.unpack_from('<H',d,pe+20)[0]
secs=[struct.unpack_from('<8sIIII',d,pe+24+opt+i*40) for i in range(nsec)]
IB=0x140000000
def va2off(v):
    r=v-IB
    for name,vs,va,rs,ra in secs:
        if va<=r<va+rs: return ra+r-va
def cstr(v):
    o=va2off(v)
    if o is None: return None
    s=d[o:o+80].split(b'\0')[0]
    if len(s)>=2 and all(32<=c<127 for c in s): return s.decode()
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);md.detail=True
s=int(sys.argv[2],16);e=int(sys.argv[3],16)
lastimm={}
out=[]
for ins in md.disasm(d[va2off(s):va2off(e)],s):
    ops=ins.operands
    if ins.mnemonic=='mov' and len(ops)==2 and ops[1].type==x86.X86_OP_IMM and ops[0].type==x86.X86_OP_REG:
        lastimm[ops[0].reg]=ops[1].imm
    if ins.mnemonic in('xor',) and len(ops)==2 and ops[0].type==x86.X86_OP_REG and ops[1].type==x86.X86_OP_REG and ops[0].reg==ops[1].reg:
        lastimm[ops[0].reg]=0
    if ins.mnemonic=='lea' and ops[1].type==x86.X86_OP_MEM and ops[1].mem.base==x86.X86_REG_RIP:
        t=ins.address+ins.size+ops[1].mem.disp; st=cstr(t)
        if st: out.append([st,ins.address,None])
    if ins.mnemonic=='call' and out and out[-1][2] is None:
        r8=lastimm.get(x86.X86_REG_R8D, lastimm.get(x86.X86_REG_R8))
        out[-1][2]=r8
        lastimm={}
print(' '.join('%s=%s'%(n,v) for n,a,v in out))
