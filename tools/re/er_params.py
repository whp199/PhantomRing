#!/usr/bin/env python3
"""Read Elden Ring's regulation.bin offline (params: AtkParam, Bullet, SpEffectParam, NpcParam...).

Used to pick the param rows PhantomRing's combat code spawns/applies (docs/COMBAT.md) and to check
the field offsets the host hard-codes (er/Params.h) against the real data.

  er_params.py fields SP_EFFECT_PARAM_ST [filter]      field offsets (from fromsoftware-rs generated.rs)
  er_params.py row SpEffectParam 6000 [field...]       one row (all fields, or the named ones)
  er_params.py find SpEffectParam 'sleep_attack_power>0' [show fields...]   rows matching python expr
  er_params.py ids Bullet                              row count, id range

regulation.bin: AES-256-CBC (the public SoulsFormats ER key) -> DCX/ZSTD -> BND4 -> PARAM files.
Layouts: fromsoftware-rs crates/eldenring/src/param/generated.rs (repr(C) Rust structs, Paramdex
names in snake_case; bitfield bytes are named bits_XX).
"""
import os
import re
import struct
import sys

# ER_GAME_DIR: Elden Ring's Game folder (default: the Steam install). FSRS_DIR: a checkout of
# https://github.com/vswarte/eldenring-rs (its generated param layouts; required for `fields` and `row`).
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import game_paths  # noqa: E402
GAME = game_paths.er_game_dir()
FSRS = os.environ.get("FSRS_DIR")
CACHE = os.environ.get("ER_PARAM_CACHE", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "er_params"))
KEY = bytes.fromhex("99BFFC366A6BC8C6F5827D093602D676C42892A01C207FB024D3AF4E493FEF99")

PRIM = {"u8": ("B", 1), "i8": ("b", 1), "bool": ("?", 1), "u16": ("H", 2), "i16": ("h", 2), "u32": ("I", 4), "i32": ("i", 4), "f32": ("f", 4),
        "u64": ("Q", 8), "i64": ("q", 8), "f64": ("d", 8)}

# Param file name -> struct name (only the ones we use; others: pass the struct explicitly with -s)
STRUCT_OF = {
    "AtkParam_Pc": "ATK_PARAM_ST", "AtkParam_Npc": "ATK_PARAM_ST", "Bullet": "BULLET_PARAM_ST", "SpEffectParam": "SP_EFFECT_PARAM_ST",
    "NpcParam": "NPC_PARAM_ST", "EquipParamGoods": "EQUIP_PARAM_GOODS_ST", "BehaviorParam_PC": "BEHAVIOR_PARAM_ST", "BehaviorParam": "BEHAVIOR_PARAM_ST",
    "Magic": "MAGIC_PARAM_ST", "EquipParamWeapon": "EQUIP_PARAM_WEAPON_ST", "AssetEnvironmentGeometryParam": "ASSET_GEOMETORY_PARAM_ST",
}


def regulation_params():
    """Extract every .param file once into CACHE; returns the directory."""
    os.makedirs(CACHE, exist_ok=True)
    stamp = os.path.join(CACHE, ".stamp")
    reg = os.path.join(game_paths.require(GAME, "Elden Ring's Game folder", "ER_GAME_DIR"), "regulation.bin")
    mtime = str(os.path.getmtime(reg))
    if os.path.exists(stamp) and open(stamp).read() == mtime:
        return CACHE
    from Crypto.Cipher import AES
    from compression import zstd
    data = open(reg, "rb").read()
    dcx = AES.new(KEY, AES.MODE_CBC, data[:16]).decrypt(data[16:])
    assert dcx[:4] == b"DCX\0", "regulation.bin did not decrypt to a DCX"
    start = dcx.find(b"\x28\xb5\x2f\xfd")
    csize = struct.unpack_from(">I", dcx, 0x20)[0]
    bnd = zstd.ZstdDecompressor().decompress(dcx[start:start + csize])
    assert bnd[:4] == b"BND4"
    count = struct.unpack_from("<I", bnd, 0xC)[0]
    hsize = struct.unpack_from("<Q", bnd, 0x20)[0]
    unicode, fmt = bnd[0x30], bnd[0x31]
    if not bnd[9]:  # bitBigEndian false: the format byte is stored bit-reversed (SoulsFormats Binder.ReadFormat)
        fmt = int(f"{fmt:08b}"[::-1], 2)
    off = 0x40
    for _ in range(count):
        h = bnd[off:off + hsize]
        off += hsize
        p = 8
        size = struct.unpack_from("<q", h, p)[0]
        p += 8
        if fmt & 0x20:
            p += 8
        if fmt & 0x10:
            doff = struct.unpack_from("<q", h, p)[0]
            p += 8
        else:
            doff = struct.unpack_from("<I", h, p)[0]
            p += 4
        if fmt & 0x02:
            p += 4
        no = struct.unpack_from("<I", h, p)[0]
        if unicode:
            e = no
            while bnd[e:e + 2] != b"\0\0":
                e += 2
            name = bnd[no:e].decode("utf-16le")
        else:
            name = bnd[no:bnd.index(b"\0", no)].decode("shift_jis")
        base = name.replace("\\", "/").split("/")[-1]
        open(os.path.join(CACHE, base), "wb").write(bnd[doff:doff + size])
    open(stamp, "w").write(mtime)
    return CACHE


_layouts = {}


def layout(struct_name):
    """[(name, offset, fmt or None, size)] for a generated.rs repr(C) struct."""
    if struct_name in _layouts:
        return _layouts[struct_name]
    if not FSRS:
        raise SystemExit("set FSRS_DIR to a fromsoftware-rs checkout (https://github.com/vswarte/eldenring-rs)")
    src = open(os.path.join(FSRS, "crates/eldenring/src/param/generated.rs")).read()
    m = re.search(r"pub struct " + re.escape(struct_name) + r" \{\n(.*?)\n\}", src, re.S)
    if not m:
        raise SystemExit(f"no struct {struct_name} in generated.rs")
    fields = []
    off = 0
    align_max = 1
    for line in m.group(1).splitlines():
        fm = re.match(r"\s*(?:pub )?([a-z_0-9]+): (.+),\s*$", line)
        if not fm:
            continue
        name, ty = fm.groups()
        am = re.match(r"\[(\w+); (\d+)\]", ty)
        if am:
            base, n = am.group(1), int(am.group(2))
            fmtc, size = PRIM[base]
            align = size
            size *= n
            fmtc = None
        else:
            fmtc, size = PRIM[ty]
            align = size
        off = (off + align - 1) // align * align
        fields.append((name, off, fmtc, size))
        off += size
        align_max = max(align_max, align)
    total = (off + align_max - 1) // align_max * align_max
    _layouts[struct_name] = (fields, total)
    return _layouts[struct_name]


def read_param(name):
    path = os.path.join(regulation_params(), name + ".param")
    d = open(path, "rb").read()
    rows = struct.unpack_from("<H", d, 0xA)[0]
    # ER PARAM (format 2D 0x07 / 64-bit offsets): header 0x40, row entries {u32 id, u32 pad, u64 data, u64 name}
    entries = []
    for i in range(rows):
        rid, _, doff, noff = struct.unpack_from("<IIQQ", d, 0x40 + i * 0x18)
        entries.append((rid, doff))
    return d, entries


def row_size(d, entries):
    offs = sorted(o for _, o in entries)
    sizes = {b - a for a, b in zip(offs, offs[1:])}
    return min(sizes) if sizes else None


def decode(d, doff, struct_name):
    fields, _ = layout(struct_name)
    out = {}
    for name, off, fmtc, size in fields:
        if fmtc:
            out[name] = struct.unpack_from("<" + fmtc, d, doff + off)[0]
        else:
            out[name] = d[doff + off:doff + off + size].hex()
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd == "fields":
        fields, total = layout(argv[2])
        flt = argv[3] if len(argv) > 3 else ""
        for name, off, fmtc, size in fields:
            if flt in name:
                print(f"{off:#06x} {size:3} {fmtc or 'raw'} {name}")
        print(f"size {total:#x}")
        return 0
    param = argv[2]
    st = STRUCT_OF.get(param)
    d, entries = read_param(param)
    if cmd == "ids":
        ids = [r for r, _ in entries]
        print(f"{param}: {len(ids)} rows, ids {min(ids)}..{max(ids)}, row stride {row_size(d, entries):#x}, struct {st} size {layout(st)[1]:#x}")
        return 0
    if cmd == "row":
        want = int(argv[3])
        names = argv[4:]
        for rid, doff in entries:
            if rid == want:
                row = decode(d, doff, st)
                for k, v in row.items():
                    if (not names and v not in (0, -1, "", 0.0)) or k in names:
                        print(f"{k} = {v}")
                return 0
        print("no such row")
        return 1
    if cmd == "find":
        expr = argv[3]
        show = argv[4:]
        n = 0
        for rid, doff in entries:
            row = decode(d, doff, st)
            if eval(expr, {}, row):
                print(rid, {k: row[k] for k in show})
                n += 1
                if n >= 200:
                    print("... (200 shown)")
                    break
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
