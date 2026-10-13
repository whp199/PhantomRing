# usage: mkaob.py exe name=VA ... -> shortest unique AOB (>=12 bytes) with rel32/rip disp wildcarded
import sys,struct,mmap,re
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
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);md.detail=True
for arg in sys.argv[2:]:
    name,va=arg.split('='); va=int(va,16); o=va2off(va)
    toks=[]
    for ins in md.disasm(d[o:o+96],va):
        b=list(ins.bytes); mask=[True]*len(b)
        if ins.mnemonic in('call','jmp') or ins.mnemonic.startswith('j'):
            if ins.operands and ins.operands[0].type==x86.X86_OP_IMM and len(b)>=5:
                for i in range(len(b)-4,len(b)): mask[i]=False
        for op in ins.operands:
            if op.type==x86.X86_OP_MEM and op.mem.base==x86.X86_REG_RIP:
                off=ins.disp_offset
                for i in range(off,off+4): mask[i]=False
        toks+=[('%02X'%x if m else '?') for x,m in zip(b,mask)]
        if len(toks)>=64: break
    found=None
    for n in range(12,len(toks)+1):
        pat=toks[:n]
        rx=b''.join(re.escape(bytes([int(t,16)])) if t!='?' else b'.' for t in pat)
        hits=[m.start() for m,_ in zip(re.finditer(rx,d,re.S),range(2))]
        if len(hits)==1: found=' '.join(pat); break
    print('%-28s 0x%x  %s'%(name,va,found or 'NOT UNIQUE within 64 bytes'),flush=True)
