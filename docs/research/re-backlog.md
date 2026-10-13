# PhantomRing RE backlog: findings (DESIGN.md §13)

Static RE of the two local executables. **Nothing here was run in a game.** Each item gives the
address, a unique AOB (count = 1 on the local exe unless noted), the signature and struct offsets, how
it was verified, and a code sketch. Each item ends with the exact live test that confirms it.

- ER = `eldenring.exe` 2.7.1.0 (sha256 `1a354710…8b597891`). Addresses are **RVAs**; add the module base.
- MGSV = `mgsvtpp.exe` 1.0.15.4 (sha256 `085c2f82…bb45`). Addresses are **VAs**; the base is fixed at
  `0x140000000`.

Tags: **[V]** = verified against local bytes (disassembly, Ghidra decompilation, AOB count) or local
source. **[V-src]** = what a local reference source says. **[U]** = inference that needs the live test.

Tools: `tools/ghidra_query.sh`, `tools/er_aob_scan.py`, `tools/re/mgsv/*.py`, plus these new scripts in `tools/re/`:
- `fieldaccess.py EXE DISP… [--write|--read] [--func] [--nostack] [--range LO HI] [--mnem m1,m2]`: every
  instruction that accesses `[reg+DISP]` (disp32). Candidates are decoded inside their .pdata function,
  so there are no misaligned hits. Works on both exes.
- `strcode32.py [--find EXE] NAME…`: Fox StrCode32/StrCode64, as implemented in the exe (see M1). With
  `--find` it also lists the code sites that use the hash as an imm32.
- `mgsv_enumtable.py EXE FUNC_VA`: dumps `{int, const char*}` tables that MGSV builds on the stack
  (used for `TppEquip` `EQP_*`/`BL_*`).
- **`verify_backlog.py`: re-checks every AOB and vtable slot in this file on both exes (exits 1 on
  drift). Current result: 32/32 OK.**

Data dumped for this file: `tools/re/data/tppequip_ids_1.0.15.4.txt` (910 `EQP_*` ids) and
`tools/re/data/tppequip_bullet_ids_1.0.15.4.txt` (114 `BL_*` ids).

### Summary

| Id | Answer in one line | Static status | Needs live |
|---|---|---|---|
| E1 | Replace the player's root-motion translation in **PlayerCtrl vfunc 12** (instance-vptr swap). ER turns it into proxy velocity and Havok resolves collision and gravity. `RequestProxyPosition` is only a deferred warp | chain fully decompiled | frame/sign check, speed cap, threading |
| E2 | **Damage module vfunc 7 `ReceiveDamage(dm, attacker, ChrDamage*)`** (`[[ChrIns+0x190]+0x98]`; player `0x44D050`). Amount `+0x228`, pos `+0x1A0`, dir `+0x1C0`. Skip the original to cancel | decompiled | field meanings beyond amount/pos/dir |
| E4 | Modal menu `CSMenuMan+0x798 != 0`; cutscene `[[CSRemo]+8]+0x14C & 1`; in-world `main_player` load_status 2; dead `ChrIns+0x1C5 & 0x80` | code verified | loading-screen flag, menu coverage |
| E6 | `CSActionButtonMan+0x20` = the presented prompt entry (`+0x08` param id). Pass A through to ER while it is set | decompiled | confirm visibility match |
| M1 | `Warp` = `Player2Impl vf54 0x1409CEAE0` → `loc->vf13(pos, rot)` is the native position set; no fade. Plan: per-frame `vf13` hold, else a treadmill | decompiled | whether the per-frame hold keeps locomotion |
| M2 | Saves = Steam Cloud `ISteamRemoteStorage` v012 FileWrite/FileDelete (Fox wrapper `0x1401703C0`). Title: A pulses (CONTINUE is focused), `gvars.ini_isTitleMode` | verified | pulse timing, error dialog |
| M3 | `ShotSolver 0x141044FF0` with return `0x141041219` = each round (origin/dir out); equipId `w+0x24C`, bulletId `w+0x240` → kind table; CQC attack serial `s+0x114` | verified + MGS5VR | bullet ids in practice, CQC ids |
| M5 | `BuddyType.WALKER_GEAR = 4`; `TppBuddyService.DidObtainBuddyType/CanSortieBuddyType/SetSortieBuddyType`, `TppBuddy2BlockController.CallBuddy/GetActiveBuddyType`; riding = vehicle id + status bit 26 | enums verified | the whole flow |
| M4 | Scene/stage-list structure plus a deterministic runtime method: find Snake's world-space palette bytes in CB uploads | partial | everything draw-level |

### Integration status (phase 0/1 build, 2026-10-12)

| Id | Where it is used | Default |
|---|---|---|
| E1 | `er/RootMotion.cpp` + `er/VptrSwap.h`; `[Puppet] motion = root_motion` (kinematic controller stays the fallback, automatic if the hook is silent for 1 s) | off (kinematic) until the E1 live test passes |
| E2 | `er/HitHook.cpp`; `[Puppet] hit_hook = true` (HP-delta detection stays for DoT/fall) | off until the E2 live test passes |
| E4 | `game::ModalMenuOpen` (`+0x798`) OR'ed with the learned UI states; `game::CutscenePlaying` (CSRemo) → ER owns the pad, puppet motion paused, `kHostCutscene`; every change logged | on |
| E6 | `game::ActionPromptParam` + `InputBridge::SetPrompt`: a fresh A press goes to ER while a prompt shows, MGSV never sees it | on (`[Input] prompt_button = 0x1000`) |
| M1 | `mgsv/Hold.cpp`, `[Motion] Hold = treadmill / native_treadmill / native` | treadmill |
| M2 | `mgsv/SaveGuard.cpp` (Steam Cloud FileWrite/FileDelete + file guards), A-pulse title boot in `mgsv/Pump.cpp` | on |
| M3 | `mgsv/Combat.cpp` (ShotSolver hook, CQC serial poll), ammo counters as fallback | on |
| M5 | `mgsv/GuestScript.cpp` buddy recipe; protocol v2 buddy block | D-Walker |
| M4 | phase 2 (renderer capture) | -- |

Found while integrating: the player's `ChrIns+0x58` is a `CS::PlayerCtrl` (vtable `0x2A2EB10`, created in
the PlayerIns constructor `0x140650C90` → `0x1403DDF10`), so a check for the plain `ChrCtrl` vtable rejects
the real player. `er/Game.cpp MainPlayer()` now accepts both (and the E1 vtable copy).

---

## E1 (ER): move the player through its Havok character proxy, with collision

### Answer

Inject Snake's displacement as the player's **animation root motion**. Do it in the per-frame
root-motion hook that the game already calls virtually: **`PlayerCtrl` vfunc 12 (`+0x60`)**. Elden Ring
then converts the displacement into a velocity (`disp / dt`), adds gravity, and lets the Havok character
proxy integrate it **with collision**: walls stop it, slopes and steps lift it, ledges drop it, gravity
and falling stay ER's. This is the same path that the Tarnished's own walking animation uses.

The hook needs no code patch. It is an **instance-vptr swap** on the player's `PlayerCtrl` object
(`ChrIns+0x58`), so Arxan never sees it.

`ChrCtrl::RequestProxyPosition` (`0x3C9270`) is **not** a collision move. It is a deferred warp: see
"Paths that are warps" below.

### The per-frame chain [V: Ghidra decompilation of every function listed]

```
CSBehCharaProxyDriver vfunc 5 (0xC46DF0; vtable 0x2B91050, RTTI CS::CSBehCharaProxyDriver)
  - called from the Havok-behaviour character update, once per character per frame
  -> ChrCtrl::PhysicsStep 0x3C8620(ChrCtrl* this, FD4Time dt)
       0x3FFB90(ChrIns)                     moving-platform displacement -> phys+0x180 (0 normally)
       if (ChrCtrl+0xFC) 0x3CD190           RequestProxyPosition / rotation = WARP (see below)
       if (!ChrCtrl+0xEA && !ChrCtrl+0x3BE)
         ChrCtrl::UpdateRootMotion 0x3CC6D0:
           rm = behavior(+0x28 module)+0x30..+0x5F  -> ChrCtrl+0x140 {vec4 translation, quat rotation, vec4 scale}
           vcall [vtbl+0x60](this, &ChrCtrl+0x140)   <== PlayerCtrl 0x3DE610 / EnemyCtrl 0x3D1BE0 / ChrCtrl ret
           0x4064E0(action_flag module, &rm)          debug speed scale only (WorldChrManDbg)
           ChrCtrl+0xEB/+0xEC/+0xED flags may zero parts of rm (ladder/locked states)
       CSChrPhysicsModule::PrepareStep 0x4609C0(phys, dt, &ChrCtrl+0x140, noGravityOrNotLocal)
           copies rm -> phys+0xD0 (translation), +0xE0 (rotation), +0xF0 (scale)
           picks the proxy mode phys+0x388 (1 idle, 2 character proxy, 3 kinematic, 4/5/6 special)
           0x467280: worldDisp = rotate(q_phys(+0x50), rm.xyz)    (0x463D90 gives q, 0x1695EF0 rotates)
                     vel = phys+0x160 + worldDisp * (1/dt) + slide + gravity(0x3D683B0 * phys+0x1CC) * dt
                     CsChrProxy::SetVelocity 0xC5DB30(proxy(+0x98), &vel)   (+ the 2nd proxy +0xA0)
           0x467E20: applies the rotation root motion to phys+0x50/+0x60
       CSChrPhysicsModule::ProxyStep 0x461730(phys, dt) switch (phys+0x388):
           2: 0x465F90: proxy pos = pos + platformDisp(+0x180) + groundOffset, then
              CsChrProxy::Integrate 0xC5D080(proxy, frameData(+0x390), &gravity, otherProxy)
              -> hknp character proxy checkSupport (0x18A51A0) + integrate (0x18A5AC0)  = COLLISION
           3: 0x465CC0: pos += platformDisp (no collision), then Integrate
           1: velocity := 0, SyncProxyPosition 0x45C910 (idle, nothing to move)
```

Mode selection in `0x4609C0` [V]: mode 2 (collision) is used when the character is a local character
with gravity (`ChrIns+0x1C4 bit5` clear), and `0x466820` says it is moving or not grounded. Any
non-trivial displacement, or not standing on solid ground, counts as moving. Mode 3 (kinematic) is
chosen when `phys+0xCC` is set. That happens for "no gravity" characters (`ChrIns+0x1C4 bit5`), for
characters that are not simulated locally (`0x3F0A20(chr) != 0`, which reads the ChrSetEntry
flags/load state), or for `phys+0x320 & 8`. **So the host must not set `ChrIns+0x1C4 bit5`, `phys+0x1D2`, `phys+0x1D5`, or
`phys+0x3F1`.** Those switch off gravity or collision. Do not use the CT "NoGravity" flag on the puppet.

### Addresses and AOBs (ER, RVAs)

| What | RVA | AOB / how to resolve | count | Status |
|---|---|---|---|---|
| `CS::PlayerCtrl` vftable | `0x2A2EB10` (22 vfuncs) | RTTI name `.?AVPlayerCtrl@CS@@` (common/Hook `FindVtable`) | 1 | [V] |
| PlayerCtrl vfunc 12 `ModifyRootMotion(PlayerCtrl*, hkVector4* rm)` | `0x3DE610` | `48 8b c4 48 89 58 18 48 89 78 20 55 48 8d 68 a1 48 81 ec b0 00 00 00 0f 29 70 e8 0f 29 78 d8 44 0f 29 40 c8 44 0f 29 48 b8 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 45 07 48 8b 41 10 48 8b fa` matches **2** (`0x3D1BE0` = the EnemyCtrl twin). Resolve through the vtable slot instead, and check that slot 12 equals this RVA | 2 | [V] |
| Call site that proves the slot (`call [rax+0x60]` with rdx = &ChrCtrl+0x140) | `0x3CC85C` | `0f 29 07 48 8b 03 48 8b d7 48 8b cb ff 50 60 48 8b 43 10` | 1 | [V] |
| ChrCtrl::UpdateRootMotion | `0x3CC6D0` | `40 53 48 81 ec 80 00 00 00 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 60` | 1 | [V] |
| CSChrPhysicsModule::PrepareStep | `0x4609C0` | `48 8b c4 56 57 41 56 48 83 ec 70 48 c7 40 a0 fe ff ff ff 48 89 58 08 48 89 68 10` | 1 | [V] |
| root motion → velocity | `0x467280` | `4c 8b dc 55 57 41 56 49 8d ab 58 ff ff ff` | 1 | [V] |
| CsChrProxy::SetVelocity | `0xC5DB30` | `48 8b 81 a0 00 00 00 48 85 c0 74 06 0f 28 02 0f 29 00` | 1 | [V] |
| ChrCtrl::PhysicsStep | `0x3C8620` | (informational) | | [V] |

What the original `0x3DE610` does to rm [V]: it scales rm by `module(+0xC0)+0x24C`, and by
`[ChrCtrl+0x3B0 or +0x18]+0x90` (ride/manipulator scale). Then it rotates rm by the quaternion at
`[ChrCtrl+0x178]+0x30`. Its input and output are **character-local**. `0x467280` then rotates by the
physics orientation `phys+0x50` into Havok world space.

### Code sketch (host, `er/Puppet`)

```cpp
// One-time, when the player ChrIns is ready (load_status==2):
//   ctrl = *(void**)(chr + 0x58); check *(void**)ctrl == base+0x2A2EB10 (PlayerCtrl vftable)
//   and vtbl[12] == base+0x3DE610 and the call-site AOB at 0x3CC85C; otherwise disable the injection.
static void** g_origVtbl; static void* g_fakeVtbl[1 + 22];          // [0] = COL copy (vtbl[-1])
static std::atomic<uint64_t> g_pendingSeq; static Vec4 g_pendingWorld; // written by the task, consumed here
using ModifyRm = void(*)(void* ctrl, float* rm);

static void HookModifyRootMotion(void* ctrl, float* rm) {
	reinterpret_cast<ModifyRm>(g_origVtbl[12])(ctrl, rm);  // keep the game's own bookkeeping
	if (!Puppet::Active()) return;                          // plain ER control: leave rm alone
	Vec4 d = Puppet::ConsumeDisplacement();                 // Havok-world metres since the last call
	const float* q = PhysOrientation(ctrl);                 // phys = [[[ctrl+0x10]+0x190]+0x68]; q = phys+0x50 (xyzw)
	Vec3 local = RotateByConjugate(q, d.xyz);              // inverse of 0x1695EF0's rotation
	rm[0] = local.x; rm[1] = local.y; rm[2] = local.z;      // replace the Tarnished's own root motion
	// rm[3] (w) is left alone; rotation rm+4 and scale rm+8 are left alone (yaw comes from SetYaw)
}
// Install: memcpy(g_fakeVtbl, origVtbl-1, 23*8); g_fakeVtbl[1+12] = HookModifyRootMotion;
//          *(void***)ctrl = &g_fakeVtbl[1].  Restore the original vptr on unload, load screens, and
//          when main_player changes (a new PlayerIns gets a new PlayerCtrl).
```

- **Threading [U]:** the driver runs inside the Havok-behaviour update. That may be a job thread.
  The host task (ChrIns_PrePhysics 78, or any task) only **adds** to an accumulator. The hook
  **exchanges it to zero**. Then each MGSV metre is applied exactly once, whichever runs first.
- **Vertical:** pass Δy = 0 (the MGSV sandbox is flat). ER gravity and ground snapping do the rest.
  Pass the MGSV Δy only for a later "MGSV jumps/vaults" feature.
- **Yaw:** keep calling `ChrIns::SetYaw` (`0x3F6180`) from the task. Compute `local` from the same `q`
  that `0x467280` uses (`phys+0x50`; if `phys+0x1DC` is set the game uses a yaw-only quaternion
  from `0x463D90`. That is the same thing for an upright player).
- **Speed limits [U]:** I found no clamp between `disp/dt` and `0xC5DB30`. Whether the hknp
  integrator (`0x18A5AC0`) clamps the speed was not analysed. MGSV's dash (~6–7 m/s) and D-Walker's
  (~15+ m/s) are just larger displacements. The live test checks this.
- **Fallback** if the vptr swap is refused at runtime: write the world displacement into the
  **moving-platform input** `phys+0x180` via `0x45FEA0(phys, &disp, &zero)`. That path is a
  position set followed by the proxy integrate (partial collision only: penetration is resolved, walls
  are not respected). This is **inferior**; use it only as a diagnostic.

### Paths that are warps (no collision) [V]

- `ChrIns::SetPosition` `0x3F2690` → `0x3C7680` → `0x45EEF0` (sets pos, `+0x90/+0x91`) → SyncProxy
  `0x45C910` → `CsChrProxy::SetPosition` `0xC5D860` (writes hknp object `+0x80`, then `0x18A7250`).
- `ChrCtrl::RequestProxyPosition` `0x3C9270` sets `ChrCtrl+0xFC|=1`, `+0x100=pos`. The next
  `PhysicsStep` consumes it in `0x3CD190`: `0x45FE70(phys, ChrCtrl+0x100)` (SetPosition without the
  tail-jump), then SyncProxyPosition. **This is the same warp, deferred by one physics step.** The only
  caller is `0x3FA566`.
- Rotation request: `ChrCtrl+0xFC bit1` → `0x45FD00(phys, ChrCtrl+0x110)` (SetYaw path).

### Gravity / fall paths the host must respect [V]

- Gravity = `[0x3D683B0]` (world gravity vector) × `phys+0x1CC` (gravity_multiplier, default 1.0).
  It is added in `0x467280` unless `phys+0x3F1`, `phys+0x1D2`, or (`[0x3D68383]` && `phys+0x3E4`).
- `phys+0x1D0` is_falling = not standing on solid ground (`+0x92`) (set in `0x4609C0`).
- Fall damage and death stay ER's (ChrIns fall module `+0x70`). Do not zero velocity while falling.
  Keep injecting the horizontal displacement while falling. [U] ER may cap air control; the live
  test checks it.

### Live test (E1)

1. Puppet active, pad masked, Tarnished idle. Install the vptr swap and log once per second:
   `phys+0x388` (expect 2 while moving, 1 when idle), `phys+0x70` before and after, the consumed Δ, and
   the hook call count (expect 1 per frame per player).
2. Inject a constant 2 m/s forward for 3 s on flat ground. Expect the puppet to move about 6 m, with
   `phys+0x70` advancing about 2·dt per frame and no teleport jitter.
3. Walk into a wall. Expect it to stop at the wall and slide along when the input is angled. Walk up a
   slope or stairs: Y rises. Walk off a ledge: it falls with gravity, lands, and takes fall damage when
   high (NoDead keeps it alive).
4. Check the frame (yaw) convention. With the puppet yawed 90° (SetYaw), inject world +X. It must
   move world +X, not rotated. If it is rotated, the conjugate is wrong (use `q` instead of `conj(q)`).
5. Input blocking: if the host sets ChrIns `+0x538` bit5 ("no move") to block ER input, check that
   the injected displacement still moves the puppet. The flag gates movement *input*. Whether it
   also gates root motion was not traced [U]. Prefer XInput-level masking (DESIGN §6).
6. Fog wall, grace, and Torrent-whistle areas: confirm that the hook is not called while
   `ChrCtrl+0xEA` or `+0x3BE` is set (cutscene/locked) and that nothing accumulates. Clear the
   accumulator when the hook has not run for more than 0.25 s.

---

## M1 (MGSV): hold Snake in place without disturbing his animation

### StrCode32 (needed for every GameObject command / message id) [V]

- `StrCode32` = thunk `0x1400234E0` → `0x141A098D0` → `StrCode64 0x141A09AC0(const char* s, size_t len)`.
  `StrCode64 = CityHash64WithSeeds(s+"\0", len+1, seed0 = 0x9AE16A3B2F90404F, seed1 = (signed char)s[0]<<16 + len) & 0xFFFFFFFFFFFF`
  (`0x141A09770`: the CityHash v1.0.3 `HashLen16(h - seed0, seed1)` tail, then the top 16 bits are cleared).
  `StrCode32 = (u32)StrCode64`.
- Reimplemented in `tools/re/strcode32.py`. **Verified:** for 12 command names, the computed
  StrCode32 occurs as an `imm32` at 1–23 code sites each (60+ hits total, e.g. `WarpAndWaitBlock`
  0x20C9893E at 0x1409C607A, `SetEnabled` 0xFD3A6179 at 14 sites). A wrong hash would give zero hits.
- GameObject command dispatch compares the **48-bit StrCode64** (`movabs rcx, 0x948020C9893E`), not the
  32-bit one.

| id | StrCode64 | StrCode32 |
|---|---|---|
| `Warp` | `0x7E4FAEC03838` | `0xAEC03838` |
| `WarpAndWaitBlock` | `0x948020C9893E` | `0x20C9893E` |
| `WarpToStation` | `0x6145E7534F72` | `0xE7534F72` |
| `SetPosition` | `0xB3D854925648` | `0x54925648` |
| message `WarpEnd` (sent by Warp) | | `0x4ABC76C9` |

### How `Warp` / `WarpAndWaitBlock` are executed [V: disassembly]

1. `TppPlayer2` GameObject command handler **`0x1409C5F80`** (vfunc of the class with vtable at
   `0x1422953D0`; the slot is at `0x142295450`). `rcx` = TppPlayer2 GO impl, `r8d` = player index,
   `[rbp+0x78]` = StrCode64 of `id`. `Warp` and `WarpAndWaitBlock` share one branch (`0x1409C61EB`):
   it reads `pos` (table) and `rotY` (radians), builds a Y-quaternion, and calls
   `[[this+0x20]]+0x1B0` with `waitBlock = (id == WarpAndWaitBlock)` (`0x1409C6574..0x1409C658B`).
2. `this+0x20` is a sub-object whose vtable is `0x142295490` (set in the ctor at `0x1409BB13A`). Its slot
   54 (`+0x1B0`) is **`Player2SystemImpl::Warp` = `0x1409CEAE0`**:
   `void Warp(Player2Impl* p, u32 index, const Vec4* pos, const Quat* rot, bool waitBlock)`.
   `p+0x214` is the local player index (it matches tpp-mod's `Player2SystemImpl.localPlayerIndex`). `p` is
   exactly the object that the global `Player2System` (`0x142A82C28`) `+8` points to (stored at `0x1409BAAFB`).
   - It stores `pos.xyz, yaw` into the per-player array `[[p+0x60]->vf(0x10)()+0x20][index]` (16 B each).
   - `slot = *(u8*)(*(p+0x78)+index)`, `obj = *(u16*)(*(p+0x70)+2*index)`,
     `inst = *(p+0x378) + slot*0x6A8`.
   - **`loc = (*(inst+0x78)+0x238)->vf[0x388/8](obj)`; `loc->vf[0x70/8](&rot)`; `loc->vf[0x68/8](pos, &rot)`**
     ← this is the native "set position + rotation" of the player character (the transform/controller).
   - then `(inst+0xC0)->vf[0x90/8](obj)` and `(inst+0xA0)->vf[0x80/8](obj)` (unidentified resets [U]).
   - local player: `[p+0xB8]+0x274 = yaw` (camera/aim yaw [U]); `[p+0x2C8]+0x18/+0x1C/+0x20 = (int)pos`.
   - **`Warp`** (waitBlock=false): `p->vf[0x90/8](index, StrCode32("WarpEnd"))`, which posts a message.
     **`WarpAndWaitBlock`**: `p+0x464 = 2` (a state machine that waits for terrain blocks [U: probably freezes
     the player until streaming finishes]).
   - **Neither variant fades.** The fade in scripted warps comes from the Lua callers (TppUI). Use
     `Warp`, not `WarpAndWaitBlock`, for a treadmill. It does not wait for streaming, and in the
     sandbox the blocks are already loaded.

AOBs (MGSV, each count 1, made with `tools/re/mgsv/mkaob.py`): see the table at the end of this section.

### Recommendation

Each MGSV frame, the guest reads Snake's root (camera owner `+0x30` matrix from the `0x14044E960`
setter hook, or `loc` itself) and adds the frame delta to the odometer. Pick the hold from the options
below in this order; the live test decides:

1. **Native per-frame hold (preferred, needs the live test):** once per frame, **after** the player
   update and before the camera publish (the camera-setter hook `0x14044E960` is a good place, on the
   game thread), call `loc->vf13(loc, &anchorPos, &currentRot)` with the **current** rotation. This is the
   same call `Warp` makes, but without the component resets (`inst+0xC0`/`+0xA0`), the camera yaw write,
   or the `WarpEnd` message. So the animation state machine should not be disturbed. Odometer: delta =
   (root position before the hold) − anchor.
2. **Treadmill with the native call:** let Snake walk up to R = 8 m, then do the same call once and
   exclude that jump from the odometer.
3. **Treadmill with Lua (zero RE risk, proven script path):** from the pcall pump:
   `GameObject.SendCommand({type="TppPlayer2",index=0},{id="Warp",pos={x,y,z},rotY=vars.playerRotY*math.pi/180})`.
   This also runs the resets and `WarpEnd`. Expect a visible hitch in the animation [U].

Code sketch for (1) (guest, `mgsv/Motion`):

```cpp
// Resolve once per mission (cache, re-check the vtable pointers every frame):
//   impl  = *(u8**)(0x142A82C28 + 8)   Player2System->impl  [V: set to TppPlayer2GO+0x20 at 0x1409BAAFB]
//   check *(void**)impl == 0x142295490 and ((void**)0x142295490)[54] == 0x1409CEAE0, *(u32*)(impl+0x214) < 16
//   idx   = *(u32*)(impl+0x214); slot = *(u8*)(*(u8**)(impl+0x78)+idx); obj = *(u16*)(*(u8**)(impl+0x70)+2*idx)
//   inst  = *(u8**)(impl+0x378) + slot*0x6A8
//   mgr   = *(void**)(*(u8**)(inst+0x78) + 0x238);  loc = vcall<void*>(mgr, 0x388/8, obj)
// Per frame (game thread):
//   alignas(16) float rot[4] = current player quaternion (camera owner +0x30 matrix -> quat, or vars.playerRotY)
//   alignas(16) float pos[4] = { anchor.x, anchor.y, anchor.z, 0 };
//   vcall<void>(loc, 0x68/8, pos, rot);
```

### Live test (M1)

1. Log `impl`, `GO+0x20`, `*(void**)(GO+0x20)` (expect `0x142295490`), `inst`, `mgr`, `loc`, and
   `*(void**)loc` once per mission. Record the `loc` vtable for the next RE pass, then decompile its slot
   13/14 and the slot `0x388/8` of `mgr`.
2. With Snake standing, call option (1) every frame with anchor = current position. Expect no
   visible change.
3. Push the stick forward. Expect the running animation to play in place, `vars.playerPosX/Z` to stay
   at the anchor (±1 cm), and the odometer to advance about 5–6 m/s (run) / about 7 m/s (dash).
   Check crouch-walk, prone crawl, dive, side-roll, and CQC throws (CQC moves Snake; the odometer must
   capture it).
4. If (1) breaks locomotion (stuck in idle, foot sliding, falls), try (2) with R = 8 m and then (3).
   Note which one works in DESIGN §5.2.
5. Check that `WarpEnd` is not sent in mode (1) (hook `p->vf[18]` and log any `0x4ABC76C9`).

AOBs for M1 (MGSV VAs; `tools/re/mgsv/mkaob.py`, each verified unique in the file):

| What | VA | AOB |
|---|---|---|
| TppPlayer2 command handler | `0x1409C5F80` | `48 8B C4 4C 89 48 20 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 A8` |
| Player2 `Warp` impl (vtbl `0x142295490` slot 54) | `0x1409CEAE0` | `4C 8B DC 57 41 57 48 81 EC A8 00 00` |
| Warp vcall site (`lea rcx,[r14+20h]; mov r10,[rcx]; mov rax, StrCode64(WarpAndWaitBlock)`) | `0x1409C6563` | `49 8D 4E 20 4C 8B 11 48 B8 3E 89 C9` |
| StrCode64 | `0x141A09AC0` | `48 89 6C 24 18 56 48 83 EC 20 80 3C` |

---

## M3 (MGSV): native fire point, CQC hits, ammo kinds

### Gun fire [V: disassembly + MGS5VR `controller_rig.cpp` (MIT), which hooks the same function on this exe]

- **`FireWeapon` `0x141041110`** `(WeaponComponent* c /*rcx*/, WeaponState* w /*rdx*/, u32 playerIndex /*r8d*/)`.
  It runs once per trigger pull that releases a round.
- **`ShotSolver` `0x141044FF0`** `void (WeaponComponent* c, Vec4* originOut, Vec4* dirOut, WeaponState* w, u32 playerIndex)`.
  It is called from FireWeapon at `0x141041214`, **return address `0x141041219`**. It is also called
  from `0x14103F902` and `0x141094159`, which are not fire events (aim and trajectory previews [U]).
  After the original returns, `*originOut` is the world-space shot origin and `*dirOut` is the unit
  direction **after** MGSV's own spread and convergence. This is what MGS5VR logs as
  "Controller shot … muzzle=… direction=…".
- Per pellet, FireWeapon then calls the bullet system: `[c+0x60]->vf[0x138/8](sys, u32 bulletId, BulletRequest* req)`
  (`0x1410414F1..0x1410414FC`). `req+0x00` = origin, `req+0x10` = direction, `req+0x20` = f32 spread
  step. The loop runs `w+0x3DF` times (pellet count, ≥1).
- WeaponState fields [V: used in FireWeapon; meaning from use]:
  - `+0x24C` u32 **equipId** (`TppEquip.EQP_WP_*`). Ex-handguns `1067..1070` get special handling, and
    `tools/re/data/tppequip_ids_1.0.15.4.txt` maps these ids to names.
  - `+0x240` u32 **bulletId** (`TppEquip.BL_*`), or `+0x244` for the alternate mode
    (`[w+0x27C]&0x400000` and `(w+0x3D0 & 0x180000)==0x80000`).
  - `+0x25C`/`+0x25E` u16 rounds in the magazine (main/alternate, chosen by `w+0x27C & 0x200`);
    `+0x260`/`+0x262` reserve.
  - `+0x27C` flags (`&0x1C0 == 0x40` firearm, `0x80` throwable; `&0x200` alternate magazine);
    `+0x3C0` mode; `+0x3DF` pellets; `+0x140` aim target point (MGS5VR); `+0x40`/`+0x80` muzzle socket
    matrix (MGS5VR).
- Ownership check (MGS5VR, verified constants): `*(void**)c == 0x1423B3E80`; `c+8` = character; `character+0x80 == c`;
  `w == *(u8**)(c+0x58) + (idx - *(u32*)(*(u8**)(c+0x38)+0x24))*0x610`. Snake's index is
  `owner+0x3A0` (camera owner, vtable `0x1423B8218`).

**Ammo kind by bulletId.** The full `BL_*` table is in `tools/re/data/tppequip_bullet_ids_1.0.15.4.txt`
(dumped from the enum builder `0x140A2C6D0` with `tools/re/mgsv_enumtable.py`):

| Kind (`kGEvFire.ammo`) | bulletIds |
|---|---|
| **Sleep / tranquilizer** → ER sleep build-up | 10 HgTranq, 11 HgTranqP, 26 HgSleep, 30 40mmSleep, 34 20mmSleep, 40 RfTranq, 49 ms02Sleep, 67 SupplySleep, 98 UavSleepGasGrenade |
| **Stun / non-lethal** → ER poise/stagger | 7 HgRubber, 17 RfRubber, 22 12GAAir, 101 12GAAirW, 25 HgStun, 29 40mmStun, 33 20mmStun, 12–14 Water*, 242 HAND_StunArm, 244 HAND_StunRocket |
| **No damage** (utility) | 24 HgSmoke, 28 40mmSmoke, 32 20mmSmoke, 66 SupplySmoke, 68 SupplyChaff, 69 Flare, 97 UavSmokeGrenade, 105 ms02Fulton |
| **Explosive** (area) | 23 HgGrenade, 27 40mmGrenade, 31 20mmGrenade, 35 20mmRocket, 43–60 ms* (missiles), 63–65, 70–85, 245 HAND_KillRocket, 96 UavGrenade |
| **Lethal** (everything else) | 2–6, 8–9, 15–16, 18–21, 36–39, 41–42, 61–62, 86–95, 99–100, 102–104, 106–107 |

The kinds come from the names. The real damage parameters live in the game's Lua equip tables, not in
the exe [U]. The live test confirms the main cases.

Code sketch (guest `mgsv/Combat`):

```cpp
using ShotFn = void(*)(void* c, float* origin, float* dir, void* w, uint32_t idx);
static ShotFn g_shot;
static void HookShot(void* c, float* origin, float* dir, void* w, uint32_t idx) {
	g_shot(c, origin, dir, w, idx);
	if (_ReturnAddress() != (void*)0x141041219) return;          // only real fire
	if (idx != SnakeIndex() || *(void**)c != (void*)0x1423B3E80) return;
	auto* wb = (uint8_t*)w;
	uint32_t equip = *(uint32_t*)(wb + 0x24C), bullet = *(uint32_t*)(wb + 0x240);
	uint8_t pellets = std::max<uint8_t>(1, wb[0x3DF]);
	Events::Fire(origin, dir, equip, bullet, KindOfBullet(bullet), pellets);  // kGEvFire, MGSV world
}
// MinHook on 0x141044FF0 after checking the AOB "48 8B C4 55 56 57 41 56 41 57 48 8D A8 D8 FE FF FF 48 81 EC 00 02 00 00 0F".
```

### CQC / melee [V: MGS5VR `motion_melee.cpp` (MIT) constants, re-checked on these bytes]

- CQC component: vtable `0x1423BD2F8`. Its interface sub-object at `+0x38` has vtable `0x1423BD370` and is reached from
  `character+0xD8`, so `c = *(character+0xD8) - 0x38`; `c+0x40 == character`.
- Per-player CQC state: `s = *(u8**)(c+0x58) + (idx - *(u32*)(*(u8**)(c+0x28)+0x24))*0x130`:
  - `s+0x10` action callback (non-null while a grab/throw is in progress)
  - `s+0xE0` attack shape (`shape+0x20 == c`, `shape+0x28 == s+0x110`; `shape->vf[0x58/8]()` returns the
    geometry: `+0x10` Vec3 centre, `+0x20` f32 radius, MGSV world)
  - `s+0xE8` collision query
  - `s+0x114` u16 **attack serial** (a new value for each new strike), `s+0x116` u16 attacker index,
    `s+0x118` u16 **attack id** (7 = `ATK_Kick` per MGS5VR; punches, combos, and throws use other ids [U])
- Contact filter **`0x141176560`** `u32 (ctx, shape, target, contact, cqc)` returns 1 for an accepted contact.
  `target+0x50` is a u64 tag (bit 63 = GameObject, low 16 bits = GameObject id).
  AOB `40 55 41 54 41 56 48 83 EC 60 48 8B` (count 1).

**For PhantomRing:** the sandbox has no MGSV targets, so contacts never happen. Detect the **attack
window** instead: poll `s+0x114` once per frame on the game thread. When it changes and `s+0x118 != 0`,
emit `kGEvMelee {attackId, centre, radius}` with the geometry read through the shape. Keep re-reading
the geometry while the serial is unchanged and the shape is active (the swept hit volume). The host
tests that sphere against ER enemies.

### Live test (M3)

1. Fire the tranquilizer pistol (WU S.Pistol, `EQP_WP_West_hg_010`): expect equip 1, bullet 10 or 11,
   pellets 1. Fire an assault rifle: bullet 15 or 16. Fire a shotgun: pellets > 1. Fire a rubber
   shotgun or SMG: bullets 22/17. Log origin and dir; |dir| must be 1, and the origin must be within
   about 1 m of the camera-owner root plus the hand height.
2. Confirm that `HookShot` with return address `0x141041219` fires exactly once per round (full-auto
   included) and never for aim previews.
3. Do a CQC strike, combo, kick, and throw in empty space. Log `s+0x114`, `s+0x118`, the geometry
   centre and radius per frame. Expect a serial increment per strike and the centre in front of Snake
   within about 1.5 m.

---

## E2 (ER): where an incoming hit is applied to the player

### Answer [V: decompilation]

The game applies every attack hit on a character through **the target's damage module vfunc 7
(`+0x38`): `ReceiveDamage(CSChrDamageModule* target, ChrIns* attacker, ChrDamage* dmg)`**.
For the player that is `CSPlayerDamageModule` vtable `0x2A3A3E0` slot 7 = **`0x44D050`**. That
function forwards to the shared `CSChrDamageModule::ReceiveDamage` `0x4455C0`, and from there to the
team-relation table and the HP/reaction apply `0x444B40`.

The damage module is **`[[ChrIns+0x190]+0x98]`** (verified in code: `*(param_2[0x32]+0x98)` in
`0x44AE70`). This settles the fsrs `damage` field that was [U] in elden-ring-runtime-re.md §3.3.

Call chain:

```
DmgMan hit resolution (task DmgMan_* 120-122)
  -> 0x51BD50 -> 0x51BC60 HitDispatch(ChrDamage* dmg, ChrIns* attacker): switch target(dmg+0x1E0)->vf2() (FieldInsType)
       1 (Chr) -> 0x44AE70 ProcessChrHit(ChrDamage* dmg, ChrIns* target, ChrIns* attacker)
           returns 0 at once if target ChrIns+0x538 bit3 ("no hit"), WorldChrManDbg+0x14 (0x3D6A21C), or dmg+0x54 == 3
           dmg+0x1C0 := normalize(target.pos - attacker.pos)           (hit direction, Havok space)
           guard / parry checks via the damage-module vfuncs 9 (+0x48) and 13 (+0x68)
           targetDmgMod->vf[7](targetDmgMod, attacker, dmg)   <== ReceiveDamage (0x44D050 for the player)
           attackerDmgMod->vf[12](attackerDmgMod, target, dmg) (attacker bookkeeping: hit-stop, FP regain)
           0x3F9FC0(attacker, dmg+0x150 * …, target)            (stamina/"on hit" effect on the attacker)
```

ChrDamage fields seen in this path [V: offsets used; meaning partly U]:

| Off | Use | Meaning |
|---|---|---|
| `+0x34` | `< 0x40` normal, `== 0x40` special branch | attack category [U] |
| `+0x54` | `== 3` → ignore | hit state [U] |
| `+0x115` | bit1 → player-specific path, bit2 → line-of-sight check, bit4 | flags |
| `+0x118` | read with `+0x60` (attacker-side handle) | attacker/behaviour id [U] |
| `+0x1A0` | hkVector4 | **hit position** (Havok space) |
| `+0x1C0` | hkVector4 written by ProcessChrHit | **hit direction** (attacker → target, unit) |
| `+0x1E0` | FieldIns* | target |
| `+0x228` | i32 (`< 1` = no damage; packed as i16 into the net message) | **total HP damage** |
| `+0x22C`, `+0x234`, `+0x238` | copied into the net message | damage type / AtkParam id / status [U] |
| `+0x25E` | u16 `0x101` when guarded | guard result |
| `+0x11C` | selects the team-relation table | |

AOBs (ER RVAs):

| What | RVA | AOB | count |
|---|---|---|---|
| CSPlayerDamageModule::ReceiveDamage (slot 7) | `0x44D050` | `48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 41 0f b6 80 15 01 00 00` | 1 |
| ProcessChrHit | `0x44AE70` | `48 8b c4 55 56 57 41 56 41 57 48 8d a8 78 fc ff ff` | 1 |
| CSPlayerDamageModule vftable | `0x2A3A3E0` | RTTI `.?AVCSPlayerDamageModule@CS@@` | 1 |

### Code sketch (host `er/Combat`): cancel and forward

```cpp
// Instance-vptr swap on the player's damage module (no .rdata or code write):
//   dm = *(void**)(*(u8**)(player+0x190) + 0x98); check *(void**)dm == base+0x2A3A3E0 and vtbl[7] == base+0x44D050.
using Receive = void(*)(void* dm, void* attacker, uint8_t* dmg);
static void HookReceiveDamage(void* dm, void* attacker, uint8_t* dmg) {
	if (!Puppet::Active()) return g_origReceive(dm, attacker, dmg);
	int32_t amount = *(int32_t*)(dmg + 0x228);
	const float* pos = (float*)(dmg + 0x1A0), *dir = (float*)(dmg + 0x1C0);
	Events::Hurt(amount, pos, dir, attacker, *(uint32_t*)(dmg + 0x22C) /*kind, U*/);   // kHEvHurt
	// Cancel: do NOT call the original. No HP loss, no hit reaction (a reaction would freeze root
	// motion), no status build-up. Snake's life is MGSV's.
}
```

- Keep NoDead (`data+0x19B` bit0) as a safety net. **Do not** set ChrIns `+0x538` bit3: it makes
  ProcessChrHit return before ReceiveDamage, so the hit would never reach the hook.
- Damage-over-time (poison, scarlet rot, bleed procs) goes through ChrIns vfunc 64 `0x3FAA80` →
  `0x441320` → `0x436A40` (HP delta), not ReceiveDamage. Catch it with the per-frame HP-delta
  fallback (`data+0x138`), and reset HP to max every frame.
- Fall damage: `0x44E700` (fraction of max HP; called from `0x44E460`) also uses `0x436A40`.
  The same HP-delta fallback covers it, so DESIGN §8.2 "fall damage is Elden Ring's" holds.

### Live test (E2)

1. Install the swap and log every call: amount, `+0x22C`, `+0x234`, `+0x238`, pos, dir, attacker
   handle. Let a soldier hit the puppet. Expect exactly one call per landed hit, `amount` equal to the
   HP the original would have removed (compare once with the original enabled), and `dir` pointing
   from the soldier to the player.
2. With the original skipped: the HP bar does not move, the Tarnished (hidden) does not flinch, the
   puppet keeps moving with the E1 injection.
3. Arrows/spells (bullets) and boss AoE: same hook. Fall damage and poison: not in the hook; seen by
   the HP delta.

---

## E4 (ER): menu open, loading, cutscene, dead/respawning

| State | Read | Status | Evidence |
|---|---|---|---|
| **Modal/blocking menu open** (Start menu, inventory, map, grace, shop, talk, dialogs) | `*(u64*)(CSMenuMan + 0x798) != 0` (CSMenuMan = `*0x3D6F820`) | code [V], meaning [V-src]+[U] | `0x8C5960` (FeSystemAnnounceView update) writes `view+0xB08 = (CSMenuMan+0x798 == 0)`. fsrs documents that field as "false while a blocking menu is open on CSMenuMan". The same test gates HUD/prompt logic at `0x758EA0`, `0x759600`, `0x7F00A0`, `0x7F0220`, `0x887C9A`, `0x8881A7` (10 sites) |
| Menu/HUD suppressed (cutscene or special menu) | `IsMenuOrCutsceneSuppressing` **`0x80E490`**: `CSMenuMan+0x658 != 0 \|\| CSRemo::IsPlaying` | [V] | decompiled; AOB `48 89 5c 24 08 57 48 83 ec 20 48 8b 05 ?? ?? ?? ?? 32 db` (1) |
| **Cutscene playing** | `remo = *(u8**)(CSRemo + 8)`; `remo && (*(u32*)(remo + 0x14C) & 1)` with CSRemo = `*0x3D72AC8` | [V] | `CSRemo::IsPlaying 0xA9E000` → `0xAAB5C0` (`mov eax,[rcx+0x14C]; and eax,1`). 37 callers. Slot AOB (CT, unique): `48 85 C0 74 ?? 80 B8 ?? ?? ?? ?? 00 0F B6 DB 0F 45 DF 48 8B 0D $ { ' }` → `0x3D72AC8` |
| HUD allowed by cutscene logic | `CSMenuMan+0x19` (set 1 at `0xB03660` when no remo plays, 0 at `0xB03405`) | [V] code, [U] meaning | |
| **Loading / not in world** | `p = [WorldChrMan+0x1E508]`; in world iff `p && *(u8*)(*(u8**)(p+0x10)+8) == 2` and `GameMan+0x10` (warp_requested) == 0 | [V] offsets, [U] as a complete loading gate | |
| Loading screen shown | `CSNowLoadingHelper (*0x3D64F28) + 0xED` (1 code read at `0xC34468`, changes the map-object streaming budget 1000 → 8), `+0xD0` counter (fsrs) | [U] | probe |
| **Dead** | `ChrIns+0x1C5 & 0x80` (set by Kill `0x3EE730`), or `data+0x138 <= 0` | [V] | elden-ring-runtime-re.md §3.2 |
| Respawning | dead flag set → main player reload: `main_player` becomes null or a new pointer, or `chr_set_entry` load_status != 2, then back to 2 with the dead flag clear | [U] sequence | probe |
| Fade (warps, graces, cutscene edges) | `CSFade (*0x3D763B0)+0x10` plates[9]; `plate+0x10` current RGBA (a > 0 = dimmed) | [V-src] fsrs | |

Recommended host gate (sets `kPadNeutral` for MGSV and pauses the odometer injection):

```cpp
bool ErBlocksGameplay() {
	auto* menu = *(uint8_t**)(base + 0x3D6F820);
	auto* remoMgr = *(uint8_t**)(base + 0x3D72AC8);
	auto* wcm = *(uint8_t**)(base + 0x3D69FF8);
	if (!menu || !wcm) return true;
	if (*(uint64_t*)(menu + 0x798)) return true;                       // modal menu
	if (*(uint8_t*)(menu + 0x658)) return true;                        // menu-side suppression [U meaning]
	if (remoMgr) { auto* r = *(uint8_t**)(remoMgr + 8); if (r && (*(uint32_t*)(r + 0x14C) & 1)) return true; }
	auto* p = *(uint8_t**)(wcm + 0x1E508);
	if (!p || *(uint8_t*)(*(uint8_t**)(p + 0x10) + 8) != 2) return true;  // loading / not in world
	if (*(uint8_t*)(p + 0x1C5) & 0x80) return true;                    // dead
	return false;
}
```

Use `SafeRead` for every hop; the managers can be torn down during title/quit.

### Live test (E4)

Add a dev-console command `pr er 'probe e4'` that logs, on every change, `menu+0x798`, `+0x658`,
`+0x19`, `+0x1A`, `+0x138`, `+0x188`, the 0x46 `ui_states` bytes at `menu+0x90`, remo playing,
`NowLoading+0xD0/+0xEC..+0xEF`, `GameMan+0x10`, main_player, load_status, the dead flag, and the max
fade alpha. Then walk through: Start menu, inventory, map, grace rest, talk to an NPC, a shop, an item
pickup popup, a message read, fast travel (loading screen), a boss cutscene (Margit intro), death →
YOU DIED → respawn, quit to title. Record which field flips for each one, then trim the gate.
Expectation: `+0x798` is non-zero for every modal menu, the remo flag for cutscenes,
`load_status`/main_player for loads.

---

## E6 (ER): action-prompt state (interaction arbitration)

### Answer [V: decompilation of the manager's per-frame update]

`CSActionButtonManImp` = `*0x3D72478` (FD4 singleton `CS::CSActionButtonMan`; RTTI vtable `0x2B48140`).
Slot AOB: `48 8b 0d ?? ?? ?? ?? 80 79 2b 00 75 14` (count 1; rip at +3, end +7).

| Off | Field | Evidence |
|---|---|---|
| `+0x08` | active request list (singly linked, sorted by priority then distance) | `0xA65AB0` merge |
| `+0x10` | expiring/free list | `0xA65AB0` |
| `+0x18` | requests submitted this frame (`ExecActionButton` → `0xA653B0`) | `0xA653B0` |
| **`+0x20`** | **entry currently presented (the on-screen prompt); null = no prompt** | `0xA65AB0` sets it to the entry flagged bit2 |
| `+0x29` | prompts enabled (0 → presses ignored) | `0xA655D0` |
| `+0x78` | hold timer (f32) | `0xA655D0` |
| `+0x80` | press consumed this frame | `0xA64FA0` |
| **`+0x81`** | **action button pressed** (from `CSInGamePad`) | `0xA655D0` |
| `+0x82` | alternate press (used for param 6001) | `0xA64FA0` |
| `+0x86/+0x87` | cycle to previous/next prompt when more than one is active | `0xA655D0` |
| `+0x88` | force press: makes `+0x81/+0x82 = 1` that frame | `0xA655D0` [U purpose: debug/auto] |

Request entry (0x78 B): `+0x00 next`, **`+0x08 i32 ActionButtonParam id`**, `+0x0C u8 category`,
`+0x10 i32` (text/region id [U]), `+0x14 i32 priority`, `+0x18 f32 distance`, `+0x20 u8 flags`
(bit0, bit1 = requested this frame, **bit2 = presented**, bit3 = disabled, bit4 = changed),
`+0x48..+0x70` caller data.

Functions: `ExecActionButton 0xA641C0` (fsrs `cs_action_button_man_execute_action_button`; the fsrs
pattern has 2 matches, and the first one is right), the per-frame update `0xA64850` (calls `0xA65AB0` select,
then `0xA655D0` input), `IsTriggered 0xA64FA0(man, entry)` (`entry.flags&4 && man+0x81 && !(flags&8)`).
AOBs: select `0xA65AB0` `40 57 48 83 ec 40 48 c7 44 24 20 fe ff ff ff 48 89 5c 24 50 0f 29 7c 24 30 48 8b fa 48 8b d9`;
input `0xA655D0` `48 8b c4 41 56 48 83 ec 60 48 c7 40 b8 fe ff ff ff 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 0f 29 78 e8` (each 1).

### How the host should use it

- **Prompt showing:** `auto* m = *(u8**)(base+0x3D72478); bool prompt = m && *(void**)(m+0x20);`
  Optionally show `*(i32*)(entry+0x08)` (ActionButtonParam id) in the dev HUD.
- **Arbitration (recommended, no writes):** while `prompt` is true, add **A** (ER's interact button
  on a pad) to the XInput allow-list returned to Elden Ring, and send MGSV a neutral A. ER's own
  `0xA655D0` sees the press through `CSInGamePad` and triggers the prompt normally, including hold
  prompts and multi-prompt cycling. Release the allow-list entry one frame after the prompt
  disappears so a held A does not leak into MGSV.
- **Trigger without the pad [U]:** write `m+0x88 = 1` for one frame. It forces the "pressed"
  flags. Use this only if pad masking is done in `CSInGamePad` (`allow_polling = false`), where the
  real A cannot reach the manager.

### Live test (E6)

1. Log `m+0x20`, its param id, and `m+0x81` every frame for 10 s near a grace, a door, an item on
   the ground, and an NPC. Expect `+0x20` non-null exactly while the prompt is visible, and a
   param id that changes per object.
2. With the pad masked except Start/View, check that pressing A with a prompt visible
   (allow-list active) opens, picks up, or talks, and that MGSV gets no A on that frame.
3. Try `+0x88 = 1` once at a door with the A allow-list disabled. Expect the door to open
   (confirms the force-press semantics).

---

## M2 (MGSV): block all saves; title screen → Continue → gameplay

### How MGSV writes saves [V]

All saves (`TPP_GAME_DATA`, `TPP_CONFIG_DATA`, `PERSONAL_DATA`, names at `0x142275A48/58/68`, used by
`0x1409379A4..BE`) go through a Fox storage interface whose Steam implementation calls **Steam Cloud
`ISteamRemoteStorage`**. `steam_api64.dll` in the game folder exports
`STEAMREMOTESTORAGE_INTERFACE_VERSION012`, and the exe imports `SteamRemoteStorage()` (IAT
`0x14208E118`). On disk these files live in `<Steam>/userdata/<id>/287700/remote/`.

Steam storage implementation vtable at `0x1420E5C60` (methods, MGSV VAs):

| Slot | VA | Calls ISteamRemoteStorage v012 slot | Meaning |
|---|---|---|---|
| 0 | **`0x1401703C0`** | `[0]` **FileWrite**(name, data, size) | `int Write(this, const char* name, const void* data, u32 size)`: 0 = ok, 7 = error |
| 1 | `0x14016FBD0` | `[12]` GetFileSize | |
| 2 | **`0x14016EAE0`** | `[3]` **FileDelete** | `bool Delete(this, name…)` (returns `!ok`, see the code) |
| 3 | `0x14016F210` | `[10]` FileExists | |
| … | `0x14016F520` etc. | FileRead | |

A second user, `TppNetworkUserStorage` (`0x1407D47E0`, strings "TppNetworkUserStorage",
"mgsv_gz_transfer_data"), calls FileDelete, FileRead, FileWrite, FileExists, and GetFileSize
directly. It handles the Ground Zeroes transfer data.

**Blocking (guest):** cover both paths at the Steam interface, outside the game code:

```cpp
// After SteamAPI_Init (or lazily on the first SteamRemoteStorage() call):
auto* rs = reinterpret_cast<void**(*)()>(GetProcAddress(GetModuleHandleW(L"steam_api64.dll"), "SteamRemoteStorage"))();
void** vt = *reinterpret_cast<void***>(rs);              // ISteamRemoteStorage v012
// slot 0 FileWrite(const char*, const void*, int32) -> bool : return true, write nothing, log the name
// slot 2 FileForget(const char*) -> bool                    : return true
// slot 3 FileDelete(const char*) -> bool                    : return true
// slot 6 FileWriteStreamOpen(const char*) -> u64 handle      : return k_UGCFileStreamHandleInvalid (all ones)
// slot 7 FileWriteStreamWriteChunk / slot 8 FileWriteStreamClose : return false
Hooks::SwapVtable(vt, 0, &FakeFileWrite);  // VirtualProtect the steamclient-owned vtable (process-local)
```

Belt and braces: also MinHook the game wrapper `0x1401703C0` → `return 0` and `0x14016EAE0` →
report success. AOBs: Write `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B D9 49 8B F8 48 8B F2 FF`,
Delete `40 53 48 83 EC 20 48 8B DA FF 15 ? ? ? ? 48 8B` (each count 1).
Return **success** (not failure). A failed write makes TPP show its "save failed" dialog, which blocks
the sandbox [U].

### Title → Continue → gameplay [V-src MGS5VR + V strings; flow U]

- **Detect the title:** Lua `gvars.ini_isTitleMode` (true at the title; MGS5VR `native_presentation.lua`),
  `TppSequence.GetCurrentSequenceName()`, `vars.missionCode` (title mission 5 = `SYS_MISSION_ID.TITLE`).
  Native: MGS5VR hooks the title menu `Show 0x1412D73E0` / `Update 0x1412D86A0` (object vtable
  `0x1423D7E58`) and the "Press Start" prompt `Show 0x1412D6D70` (vtable `0x1423D7DD8`).
  AOBs: `40 53 48 83 EC 20 48 8B 41 40 48 8B D9 C6`, `40 56 48 83 EC 40 48 8B F1 48 8B 49`,
  `40 57 48 83 EC 40 48 8B F9 48 8B 49 08` (each count 1, identical to the MGS5VR entry checks).
- **Cheapest robust Continue (proven by MGS5VR on this exe):** XInput **A pulses** through the
  PadInject slot (press 100 ms, release 400 ms). Use them for the logos and "press any key", then on
  the title menu, which opens with **CONTINUE focused**. Never send a guessed Up/Down sequence;
  MGS5VR documents that this routed a bad title state. Repeat until `gvars.ini_isTitleMode == false`
  and `TppMission`/`GameObject` are ready (MGS5VR readiness script, mgsv-runtime-re.md §3.4).
  MGS5VR reached `30010` in about 18 s this way. Further A pulses dismiss the loading tip ("press any
  key").
- **Into the sandbox:** once in gameplay (any mission), from the pump:
  `TppMission.ReserveMissionClear{missionClearType=TppDefine.MISSION_CLEAR_TYPE.FROM_HELISPACE, nextMissionId=30050}`
  (Mother Base free roam). If the save resumes in the ACC (`vars.missionCode == 40010/40020/40050`)
  use the same call. IH's direct form is `gvars.mis_nextMissionCodeForMissionClear=30050;
  mvars.mis_showLoadingTipsOnMissionFinalize=false; TppMission.ExecuteMissionFinalize()`
  (mgsv-runtime-re.md §5.7). Then warp to the command-platform deck with `TppPlayer2` `Warp` (M1).
- **Mission clear/finalize saves:** they go through the same storage writes and are blocked by the hook.
  The game still updates its in-memory save state.

### Live test (M2)

1. Hook installed, `pr lua 'TppSave.VarSave() return 1'` (or trigger a checkpoint): the log shows
   `FileWrite TPP_GAME_DATA blocked`, there is no error dialog, and the mtime of
   `userdata/*/287700/remote/TPP_GAME_DATA` does not change.
2. Cold boot: count the A pulses from launch to `gvars.ini_isTitleMode == false` and the time to
   gameplay. Confirm `vars.missionCode` and that mission 30050 loads after the ReserveMissionClear call.

---

## M5 (MGSV): D-Walker — select, call, developed?, riding

### Enums [V: Lua enum registration `0x140A46DC0`, `0x140554650`]

- `BuddyType`: NONE 0, HORSE 1, DOG 2, QUIET 3, **WALKER_GEAR 4** (D-Walker), BATTLE_GEAR 5, DISABLE 7.
- `BuddyInitStatus`: NORMAL 0, HALO 1, **RIDE 2** (start mounted), ENTRY_GATE 3, ESPIONAGE 4.
- `PlayerStatus` (bit indices into the native status flags `u32[5]` at `state+0xC0`, mgsv-runtime-re.md §7.4;
  `PlayerInfo.AndCheckStatus{PlayerStatus.X}` in Lua): STAND 4, SQUAT 5, CRAWL 6, NORMAL_ACTION 7,
  PARALLEL_MOVE 8, IDLE 9, GUN_FIRE 11, GUN_FIRE_SUPPRESSOR 12, STOP 20, WALK 21, RUN 22, DASH 23,
  ON_HORSE 25, **ON_VEHICLE 26**, ON_HELICOPTER 37, HORSE_* 41–47, SUBJECT 56, BINOCLE 58, CUTIN 61,
  DEAD 62, DEAD_FRESH 63, NEAR_DEATH 64, NEAR_DEAD 65, FALL 67, CBOX 68, CBOX_EVADE 69 (values from
  `lea r8d,[r9+N]` with r9 = 1). The guest can publish stance and locomotion from these bits.
- vars: `buddyType` (u8, `vars+0x2D68`), `playerVehicleGameObjectId` (u16, `vars+0x110`; NULL_ID when on foot).
  Other buddy vars (names only, registration not dumped): `sortieBuddyType`, `obtainedBuddyType`,
  `deadBuddyType`, `initialBuddyPos/RotY/Status`, `buddyGearMainWPType`, `buddyGearAmmoCount`,
  `userPresetDWalkerParts`.

### Lua API present in the exe [V: luaL_Reg arrays]

`TppBuddyService` (registered in `0x140A50180`/`0x140A50300`): `CanSortieBuddyType(type)→bool`
(`0x140A4E870`), `DidObtainBuddyType(type)` (`0x140A4F640`), `SetSortieBuddyType(type)` (`0x140A54A60`),
`IsEnableCallBuddyType`, `IsDisableCallBuddyType`, `UnsetDisableCallBuddyType`, `IsDeadBuddyType`,
`UnsetDeadBuddyType`, `SummonBuddy`, `SetVarsMissionStart`, `ReserveBuddyForLandStart`,
`IsEnableBuddyCommand`, `GetFriendlyPoint` …

Buddy block controller module (registered in `0x140A45BE0`; name string `TppBuddy2BlockController`):
`GetActiveBuddyType()` (`0x140A46030`), **`CallBuddy`** (`0x140A44560`, args: number type + a Vector3 +
a number [U exact order]), `CallBuddyWithHalo` (`0x140A44950`), `ReserveCallBuddy` (`0x140A49160`),
`DeleteBuddy`, `SetCallPosition`, `RequestWalkToPosition`, `SetVarsBuddyType(type)` (`0x140A493C0`),
`SetVarsInitialPosition/RotationY/Status`, `SetVarsPositionFromNearPlayer`, `Load`, `Unload`,
`IsLoading`, `IsBlockActive`.

### Recipe (guest, Lua from the pcall pump) [U until the live test]

```lua
-- 1) developed?  (run once in gameplay; also usable at the ACC)
local ok = TppBuddyService.DidObtainBuddyType(BuddyType.WALKER_GEAR)
       and TppBuddyService.CanSortieBuddyType(BuddyType.WALKER_GEAR)
-- 2) select it for the sandbox mission, before ReserveMissionClear (M2):
TppBuddyService.SetSortieBuddyType(BuddyType.WALKER_GEAR)
vars.buddyType = BuddyType.WALKER_GEAR            -- vars+0x2D68
-- optional: start mounted
TppBuddy2BlockController.SetVarsInitialStatus(BuddyInitStatus.RIDE)
-- 3) in free roam, if it is not deployed:
if TppBuddy2BlockController.GetActiveBuddyType() ~= BuddyType.WALKER_GEAR then
  TppBuddyService.UnsetDisableCallBuddyType(BuddyType.WALKER_GEAR)
  TppBuddy2BlockController.CallBuddy(BuddyType.WALKER_GEAR, Vector3(vars.playerPosX+3, vars.playerPosY, vars.playerPosZ), 0)
end
```

**Riding:** Snake mounted ⇔ `vars.playerVehicleGameObjectId ~= GameObject.NULL_ID` **and** the native
status bit `ON_VEHICLE` (26) [U: D-Walker counted as a vehicle]. While mounted, Snake's root (camera
owner `+0x30`) moves with D-Walker, so the M1 odometer works unchanged. **The M1 hold must then move
the walker, not Snake**: `GameObject.SendCommand(vars.playerVehicleGameObjectId, {id="SetPosition", pos={x,y,z}, rotY=r})`
(IH's walker-gear warp form; `SetPosition` StrCode64 `0xB3D854925648` is compared in the handler
`0x140B2B710` [U: which GameObject class]).

**D-Walker gun fire:** not identified statically. Candidates are the other two ShotSolver callers,
`0x14103F902` (in `0x14103EB70`) and `0x141094159` (in `0x141093F80`), plus the bullet-system vf39
call. The live test logs the ShotSolver return address while D-Walker fires.

### Live test (M5)

1. In gameplay: `pr lua 'return TppBuddyService.DidObtainBuddyType(4), TppBuddyService.CanSortieBuddyType(4), vars.buddyType, TppBuddy2BlockController.GetActiveBuddyType()'`.
2. Run the recipe in Mother Base free roam. Expect D-Walker to appear within 3 m and
   `GetActiveBuddyType() == 4`.
3. Mount it (A prompt): log `vars.playerVehicleGameObjectId`, status bits 25/26, root speed while
   dashing and hovering.
4. Fire D-Walker's main weapon while mounted, and let it fire autonomously. Log every ShotSolver
   return address, `w+0x24C`/`w+0x240`, and the bullet-system vf39 calls with their bulletId.

---

## M4 (MGSV): Fox renderer — G-buffer pass, skinning matrices, UI pass

Static analysis went as far as the scene/pass structure. The D3D11 calls themselves are made through
GR wrapper objects: the global device/context `0x142C6B870/78` is referenced only by the device-creation
code `0x1419F4B34..0x1419F515A`, so the draw-level questions need the runtime capture below.
The recipe makes the answers deterministic instead of heuristic.

### Static facts [V unless marked]

- **Render job** `0x1401BD390` → per view `0x14024A1D0` (window visible?) → **scene draw `0x1401BEEC0`**
  (MGS5VR `render_job_rva`/`scene_draw_rva`). Scene draw walks `g_renderWork = *0x142B73580`:
  `**(g_renderWork+8)` is an array of **stage lists**. Each stage node has `+0x30 next`,
  `+0x50 u8 enabled`, `+0x48/+0x4C` view-mask bit/word, `+0x60` child list, and `vf[0x30/8]()` execute
  (`0x1401BEE40` draws a stage's children; `0x1401BED80` is the post pass). List 0 runs first (shadow/pre
  passes [U]), then per-view lists `[2..6]` for views with `view+0x6E2 & 1`, then list 1.
- Per-view constants go to `*0x142B73668`: `+0x680..+0x6BF` = view `+0x280..+0x2BF` (the 4×4 view
  matrix), `+0x880` = `[view+0x570]+0x60`, flags `+0x890 |= 9`, `+0x894 |= 8`, and a 0x40-byte matrix
  stack at `+0x8A0` (count `+0x898`, capacity `+0x89C`). MGS5VR reads "native viewport bytes
  +0x280..+0x4BF" and builds world/view via `0x140438AC0`/`0x140438C20`.
- **Skinning source on the CPU** (MGS5VR, signatures verified in mgsv-runtime-re.md §7.5):
  `model` (vtable `0x1420F4D90`) `+0xE0` = **world-space 4×4 palette** (64 B per bone), `+0xF8` u16
  palette count (MGS5VR accepts 2..128; Snake's body = 132 per §7.5, so the body may span more than
  one draw [U]), `+0xE8` bone-name hashes (StrCode32: `SKL_004_HEAD` = `0xC3A88A1B`, 16 code sites).
  MGS5VR `ui_renderer.cpp` (marker class `0x1421E0540`) confirms the palette is **world space** and
  that the marker pass copies it.
- **UI pass** (MGS5VR `ui_renderer_experiment`): queue `0x1402D3380` (return `0x1402E7D98`), worker
  execute `0x1402E7770`, node draw `0x1402E6BE0`, projection return `0x1402E68A0`. The UI is drawn by a
  separate worker after the scene, so the HUD layer begins at the first `0x1402E7770` execution of a
  frame.

### Runtime identification recipe (guest `mgsv/Capture`, D3D11 under DXVK)

1. Hook `ID3D11DeviceContext::OMSetRenderTargets (33)`, `ClearRenderTargetView (50)`,
   `VSSetConstantBuffers (7)`, `Map (14)/Unmap (15)`, `UpdateSubresource (48)`, `DrawIndexed (12)`,
   `DrawIndexedInstanced (20)`, `IASetInputLayout (17)`, `IASetVertexBuffers (18)`. Use dummy-object vtable
   hooks; the context comes from `0x142C6B878` once it is non-null (tpp-mod fact).
2. **G-buffer pass:** log, per frame, every OMSetRenderTargets with `NumViews ≥ 3` and each RTV's
   format (`GetDesc`). The G-buffer is the first MRT binding after the shadow passes with 3–4 targets of
   the screen size (e.g. RGBA8 albedo + RGB10A2/RGBA16F normals + RGBA8 material) and a D24S8/D32
   depth. Record the formats and slot order once. Bracket it with the stage-list index: hook the stage
   `vf[0x30/8]` calls from `0x1401BEEC0` and log which stage node binds that MRT.
3. **Which draws are Snake:** after the player update, snapshot Snake's palette
   (`model+0xE0`, count `+0xF8`, model from the MGS5VR chain `owner+0x370 → +0x10 → +0x10 → +0x68`).
   In `Unmap`/`UpdateSubresource` of any buffer later bound by `VSSetConstantBuffers`, search the
   uploaded bytes for bone 0's matrix in three encodings: (a) 4×4 as stored, (b) 3×4 transposed (rows
   = columns of the 4×4, 48 B), (c) (a)/(b) with the translation minus the camera position
   (camera-relative). The first hit gives **slot**, **offset**, **stride** (48 or 64), and
   **world vs camera-relative**. Then the same search on every frame tags draws as Snake without
   distance heuristics.
4. Record per draw: input layout (look for `BLENDINDICES`/`BLENDWEIGHT` semantics), VB/IB, and SRVs
   0..7 (albedo/normal/SRM per Fox `fmdl` material) for the render ring.

### Live test (M4)

Run the recipe for 300 frames in the MB sandbox with Snake idle, then running, then on D-Walker.
Save to the guest log: the MRT format table, the stage index of the G-buffer, the skin-CB slot,
offset, stride, and encoding, and the number of Snake draws per frame (body, head, arm, gear, D-Walker
parts). Expect stable answers across all 300 frames. Any change between frames means the encoding guess
in step 3 is wrong.
