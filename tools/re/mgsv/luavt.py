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
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
known={0x141a10ca0:'lua_atpanic',0x141a10cc0:'lua_call',0x141a10d10:'lua_checkstack',0x141a10db0:'lua_concat',0x141a10e40:'lua_cpcall',0x141a10e80:'lua_createtable',0x141a10ef0:'lua_dump',0x141a10f40:'lua_equal',0x141a10fa0:'lua_error',0x141a10fb0:'lua_gc',0x141a11160:'lua_getfenv',0x141a111e0:'lua_getfield',0x141a11250:'lua_getmetatable',0x141a112b0:'lua_gettable',0x141a112e0:'lua_gettop',0x141a112f0:'lua_getupvalue',0x141a11390:'lua_insert',0x141a113e0:'lua_iscfunction',0x141a11410:'lua_isnumber',0x141a11440:'lua_isstring',0x141a11480:'lua_isuserdata',0x141a114b0:'lua_lessthan',0x141a11500:'lua_load',0x141a11550:'lua_newthread',0x141a11590:'lua_newuserdata',0x141a11600:'lua_next',0x141a11640:'lua_objlen',0x141a116c0:'lua_pcall',0x141a11750:'lua_pushboolean',0x141a11770:'lua_pushcclosure',0x141a11830:'lua_pushfstring',0x141a11880:'lua_pushinteger',0x141a118a0:'lua_pushlightuserdata',0x141a118c0:'lua_pushlstring',0x141a11930:'lua_pushnil',0x141a11950:'lua_pushnumber',0x141a11970:'lua_pushstring',0x141a119a0:'lua_pushthread',0x141a119d0:'lua_pushvalue',0x141a11a00:'lua_pushvfstring',0x141a11a50:'lua_rawequal',0x141a11aa0:'lua_rawget',0x141a11ae0:'lua_rawgeti',0x141a11b20:'lua_rawset',0x141a11ba0:'lua_rawseti',0x141a11c20:'lua_remove',0x141a11c70:'lua_replace',0x141a11d40:'lua_setallocf',0x141a11d60:'lua_setfenv',0x141a11e00:'lua_setfield',0x141a11e70:'lua_setlevel',0x141a11e80:'lua_setmetatable',0x141a11f40:'lua_settable',0x141a11f70:'lua_settop',0x141a11fd0:'lua_setupvalue',0x141a120c0:'lua_toboolean',0x141a120f0:'lua_tocfunction',0x141a12120:'lua_tointeger',0x141a12150:'lua_tolstring',0x141a121f0:'lua_tonumber',0x141a12230:'lua_topointer',0x141a122a0:'lua_tothread',0x141a122c0:'lua_touserdata',0x141a12300:'lua_type',0x141a12330:'lua_typename',0x141a12350:'lua_xmove',0x141a16500:'lua_newstate',0x141a16480:'lua_close',0x141a16980:'luaL_openlibs',0x141a16bf0:'luaL_addlstring',0x141a16c90:'luaL_addstring',0x141a16cb0:'luaL_addvalue',0x141a16d70:'luaL_argerror',0x141a16e60:'luaL_buffinit',0x141a16e80:'luaL_callmeta',0x141a16f00:'luaL_checkany',0x141a16f40:'luaL_checkinteger',0x141a16f90:'luaL_checklstring',0x141a17020:'luaL_checknumber',0x141a17080:'luaL_checkoption',0x141a17140:'luaL_checkstack',0x141a17180:'luaL_checktype',0x141a171c0:'luaL_checkudata',0x141a17280:'luaL_error',0x141a172e0:'luaL_findtable',0x141a17410:'luaL_getmetafield',0x141a17490:'luaL_gsub',0x141a178e0:'luaL_loadbuffer',0x141a17910:'luaL_loadfile',0x141a17b30:'luaL_loadstring',0x141a17b70:'luaL_newmetatable',0x141a17bf0:'luaL_newstate',0x141a17c30:'luaL_openlib',0x141a17db0:'luaL_optinteger',0x141a17e20:'luaL_optlstring',0x141a17ea0:'luaL_optnumber',0x141a17f10:'luaL_prepbuffer',0x141a17f70:'luaL_pushresult',0x141a17fc0:'luaL_ref',0x141a18080:'luaL_register',0x141a18090:'luaL_typerror',0x141a180f0:'luaL_unref',0x141a18170:'luaL_where'}
start=int(sys.argv[2],16); n=int(sys.argv[3])
for i in range(n):
    slot=start+i*8
    fn=struct.unpack_from('<Q',d,va2off(slot))[0]
    o=va2off(fn)
    if o is None: print('slot 0x%x -> 0x%x (non-image)'%(slot,fn)); continue
    insns=list(md.disasm(d[o:o+48],fn))[:8]
    desc=[];tgt=None
    for ins in insns:
        if ins.mnemonic in('jmp','call') and ins.op_str.startswith('0x'):
            tgt=int(ins.op_str,16); desc.append('%s %s'%(ins.mnemonic,known.get(tgt,hex(tgt)))); 
            if ins.mnemonic=='jmp': break
        else: desc.append(ins.mnemonic+' '+ins.op_str)
        if ins.mnemonic=='ret': break
    print('slot+0x%03x [0x%x] 0x%x: %s'%(i*8,slot,fn,' ; '.join(desc)))
