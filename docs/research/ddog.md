# D-Dog as a battle buddy in Elden Ring: design study (2026-11-03, HEAD `24f0c79`, protocol v16)

Offline study: no game was run and nothing was built. It is the plan for the next round, in which one agent works on MGSV and one
on Elden Ring. Every fact is marked:

- **[V-exe]**: read in this session from `mgsvtpp.exe` 1.0.15.4 (`<steam-library>/common/MGS_TPP/mgsvtpp.exe`, 166,517,760 bytes)
  with `x86_64-w64-mingw32-objdump` and Ghidra (`tools/ghidra_query.sh mgsv`, its flock, nice 19). Bytes are listed in §2.11.
- **[V-data]**: MGSV's own game files: `tpp_hud`, `tpp_announce_log`, `tpp_item` and `tpp_tips_info` `.eng.lng2` (keys are the
  StrCode32 = low 32 bits of the exe's StrCode64 lang ids, checked against the exe's switches), and the unpacked `Assets/tpp/script/lib/*.lua`
  (both under `<old-project>/staging/`; used for facts only).
- **[V-code]**: this repository's code, cited by file:line at `24f0c79`.
- **[V-live]**: a result recorded live in GUEST.md, HOST.md or COMBAT.md, cited by section.
- **[U]**: unverified. Each one has a check in §7 or §9.

No new Elden Ring address is needed. The host part reuses functions that are already verified for both ER tables (2.7.1.0 and
2.7.2.0): the character list, `CastRay`, ChrIns+0x538 hold bits, the bullet route, the sleep ladder and the world markers.
Every new site is in MGSV (§2.11), and it has to be byte-checked in `mgsv/Addresses.h` and `mgsv/verify_addresses.py`.

---

## 0. What the player gets (summary)

| # | Feature | How | Protocol |
|---|---|---|---|
| 1 | **D-Dog stays put across treadmill jumps.** No 6 m jumps in ER; Wait works; a dog left far behind is brought back out of view | the guest moves the dog through its own location object, the way D-Horse and D-Walker are moved (`Horse.cpp` machinery) | none |
| 2 | **D-Dog spots ER enemies.** Provisional red marks within ~40 m of the dog, and "D-Dog: Found enemy" in the corner | host only: `Marking` gets provisional marks and the dog's ER position | none |
| 3 | **Bark** (call menu, command 10): enemies within ~20 m stop and stare at the dog | host only: the decoy stare machinery, keyed on `kGEvBuddyOrder` a=10 | none |
| 4 | **Attack.** "Keep 'em busy" / "Do it (kill / knock out / wound)": D-Dog runs to the ER enemy at the centre of the view, leaps and bites with MGSV's own animation, and the host applies the matching effect (pinned / killed / asleep / wounded) | guest: one dedicated inert proxy soldier at the enemy's place plus MGSV's own order; the bite seen at the soldier's dog-attack handler. Host: picks the target, applies the effect | v17 (§4) |
| 5 | **Get it**: D-Dog fultons a sleeping ER enemy | the existing asleep proxy (`[Fulton] Method = proxy`) is the order's target; MGSV's own fulton becomes `kGEvFulton` | v17 (target flag) |
| 6 | Polish: a paw marker over the enemy the dog would take while LB is held, order texts, MGSV's own "D-Dog: Found enemy" announcement | host markers; optional MGSV announcer call | none |

Bosses are never attacked by default (`[Dog] bosses = off`). ER enemies don't attack the dog (§3.8).

---

## 1. D-Dog today (what the code does)

- `TppBuddyDog2`, GameObject id **0x2600** (type 19 = `GAME_OBJECT_TYPE_BUDDYDOG2`, index 0). [V-live GUEST 8.3] [V-exe: the name
  table at 0x142A7C690 is indexed by type: 2 SOLDIER2, 13 HORSE2, 19 BUDDYDOG2, 44 WALKERGEAR2, matching the live ids 0x400.., 0x1A00,
  0x2600, 0x5800; the dog code also hard-codes `mov eax,0x2600` (e.g. 0x140C5DB12)]. `BuddyType.DOG` = 2 [V-live GUEST 8.3, TOOLS.md].
- Lua has no GetPosition or SetPosition for it [V-live GUEST 8.3]. Its SendCommand handler **0x140C56770** compares only three
  command hashes: 0x883C8A3A, 0xD0707353 and 0xFD3A6179 (`SetEnabled`) [V-exe]. Neither of the first two is GetPosition, SetPosition,
  Warp, SetDisableDamage, SetInvincible or ChangeLifeState, and no name in MGSV's Lua or exe strings hashes to them (93,735 names
  tried). The guest takes the dog's place from its **skin**: a 94-bone skeleton, learned 0.2 m from the hint, and the capture owns
  its ~18 draws a frame [V-live GUEST 8.3]. The host places it from `buddyPos` and snaps it to ER ground with a ray from 2.5 m above,
  8 m down [V-code er/Buddy.cpp `Update`].
- It follows Snake with MGSV's own AI: a 20 s run, median 1.9 m, max 5.8 m [V-live GUEST 8.3, MGSV distances].
- **Treadmill:** the guest moves D-Walker (location object) and D-Horse (location object) with Snake's treadmill jumps. It leaves
  **D-Dog and Quiet alone**: `a_lua.buddyActive != kBuddyTypeDog` [V-code mgsv/Frame.cpp, the treadmill branch, HEAD :682-701]. The radius is 6 m
  [V-code PhantomRingGuest.ini `[Motion] Radius = 6`]. So while Snake runs, every jump moves MGSV's world under the dog, and in ER the dog
  should jump up to ~6 m and then run back. D-Walker had exactly this "6 m flash" before its location-object treadmill
  (GUEST 18.3 "Led alongside"). [U: the size of the dog's flash as drawn; nobody measured it]
- **Combat: none in ER.** The "D-Dog: Found enemy" and "Found weapon" lines seen live (GUEST 8.3, 2026-10-14) are MGSV's own
  announcements about MGSV objects. No code marks ER enemies for the dog. **README.md:303 and docs/STATUS.md:293 say that "D-Dog
  ... marks Elden Ring's enemies", and that is not true today.** GUEST.md 8.3 has it right: "Its combat (attack, mark) needs
  proxies". (I don't own those files; flagged in the final report.)

---

## 2. MGSV facts (RE this session)

### 2.1 The commands: what the player can order, and what each does inside MGSV

The buddy command function **0x140A4E0B0** (the buddy system's vf 0x2D0; `Follow.cpp` already detours it, `kBuddyCommand`) sends
one message per command through the message box `[0x140BFF050()+0x60]` vf 0x38. Jump table: **0x140A4E2D8** (26 int32 RVAs).
[V-exe] Labels and help texts are from the switches 0x14110D320 / 0x14110D0D0 and `tpp_hud.eng.lng2` [V-exe + V-data]:

| cmd | case (jump table) | message (StrCode64) | call-menu label / help text [V-data] | what the dog's handler does (§2.2) [V-exe] |
|---|---|---|---|---|
| 8 | 0x140A4E17B | 0xE8345CEE216E | **Whistle (call)**: "Whistle for D-Dog to come over." | order bits `&0xEE3FF9C3 \| 0x800`, target +0xC2 = 0xFFFF, timer +0x5C = 30.0; system +0xD0 \|= 0x20 |
| 9 | 0x140A4E18E | 0xD429F0083A9E | **Wait**: "Have D-Dog wait at his current position." | `&0xEEFFF3C7 \| 0x4`, timer 30.0 |
| 10 | 0x140A4E1A1 | 0x0D5CB991EBF6 | **Bark**: "Have D-Dog bark to attract nearby enemies." | 0x140C581A0: `&0xEEFFF3C3` (bit 3 kept) `\| 0x1000000`, timer 30.0 |
| 11 | 0x140A4E1C7 | **0xF28E1C4CC23A** + u16 arg | **Keep 'em busy**: "...attack the enemy in the center of your view, keeping the enemy busy." | style byte this+0xD4 ≠ 1: `&0xFE7FF3D3 \| 0x10000010` (attack); = 1: `&0xEEFFF3E3 \| 0x20` (fetch). Then timer 30.0, **target +0xC2 = the u16 arg**, +0xCF = 0 |
| 12 | same | same | **Do it (kill enemy)**: "...killing the enemy." | same |
| 13 | same | same | **Do it (knock out enemy)**: "...rendering the enemy unconscious." | same |
| 14 | same | same | **Do it (wound enemy)**: "...critically wounding the enemy." | same |
| 15 | same | same | **Get it**: "Have D-Dog Fulton extract nearby soldiers/prisoners/wild animals." | same |
| 16 | 0x140A4E1B4 | 0xEDB2466B86D3 | **DD (call)**: "Talk to DD." | like the whistle, plus the per-dog flags `\|= 0xA00` / `\|= 0x216` and HUD flag 0x100000 (the pet) |

- Commands 11 to 15 are **one message**. The message holds only the u16 argument; the command number never reaches the dog. The five
  labels are the menu's wording for whatever the dog is equipped with [V-data `tpp_item`]:

  | D-Dog equipment (item text) | the attack command |
  |---|---|
  | "Naked D-Dog. Use the 'Keep 'em busy' command to have him keep an enemy busy." | 11 |
  | "Sneaking Suit for D-Dog. Equipped with a knife. Use the 'Do it (kill enemy)' command..." | 12 |
  | "Sneaking Suit for D-Dog. Equipped with a stun knife. Use the 'Do it (knock out enemy)' command..." | 13 |
  | "Battledress for D-Dog. Use the 'Do it (wound enemy)' command to have him critically wound an enemy." | 14 |
  | "Tactical vest for D-Dog. Equipped with a Fulton extraction device. Use the 'Get it' command..." | 15 |

- `BuddyCommand.DOG_BARKING` = **10** [V-exe enum registration 0x140A51C0C-0x140A51C16: `mov r9d,1; lea r8d,[r9+9]`]. Bark has to be
  unlocked in the save: "'Bark' command for D-Dog is now available." [V-data tpp_menu] [U: whether the player's save has it].
- `Follow.cpp`'s `CommandDetour` already sends `kGEvBuddyOrder` for every command the player gives. Commands 8 to 16 map to
  b = 2 (dog) [V-code mgsv/Follow.cpp `CommandDetour`, HEAD :465]. The host's `OrderText` only has texts for 20 to 26 [V-code er/BuddyOrders.cpp:27-42].
  For a dog order it shows "Buddy order N".

### 2.2 The dog's message handler

**0x140C56B10**, in the dog's vtables at 0x142315C90 and 0x142315DD0; its SendCommand handler 0x140C56770 sits in the next slot
[V-exe]. Registers: rcx = the dog system (`this`), rdx = out result (i32; −1 = refused), r8d = dog index (checked `< [this+8]`),
r9 = message (`[r9]` u64 StrCode64 id, `[r9+8]` u16 argument). Every order case first calls **0x140C56670(index)**, which is true
only when the dog's 32-byte state record (`[[this+0x20]+8] + index*0x20`) has +0x1B ≠ 2 and (+0x1D & 3) ∉ {1,2,3}, meaning the dog can
take orders. The order record is `R = (this+0x28)->vf 0x58(index)`: +0x54 u32 order bits, +0x5C f32 timer (30.0 on every order),
**+0xC2 u16 target GameObjectId**, +0xCF u8. The attack store is at **0x140C579D2** (`mov word [rcx+0xC2],ax`, with ax = `[r15+8]`,
the message argument). [V-exe] [U: what +0x5C counts (an order timeout?), and how long Wait lasts]

The same handler answers **0xD0A5D0C7E373**, a transform query that a soldier sends to the dog (§2.4). Its reply in the message
is +0x10 a quaternion (16 B), +0x20 a position (xyz, w), +0x30 u8 = 1. It comes from `(this->vf 0x38(index))+0x10 -> vf 0x388`, and
the object it returns has the **location-class layout** (+0x20 position, +0x60 rotation). Snake's and the soldiers' location
objects use the same layout (GUEST 20.5). When the dog isn't realized, the position comes from a cache at `[[this+0x10]+0x20] +
index*0x10` and the rotation is built from a yaw. So **the dog has a location object, and the guest can read the dog's position
without its skin** by sending this message natively (§2.6). [V-exe] [U: whether that object's vtable is the location class
0x1424C8EF0, which D-Horse and D-Walker have (GUEST 8.1a, 18.3)]

### 2.3 The call menu's target argument

The call menu calls vf 0x2D0 at **0x14110F167** with `r8w = [menu+0x34]` (0x14110F13C) [V-exe]. A u16 at `+0x34` is written at
**0x14110EADE** from `[provider+0x32]`, where `provider = [[obj+0x18]+0xE0]`, and only when the provider's vf 0x70 says it has a
target (0x14110EAC6). Otherwise the old value stays. [V-exe] [U: that the object written there is the same menu object; the
evidence is the matching offset and the call-menu code around it] So the argument should be **the GameObjectId at the centre of the
player's view**, which is what the help text says. With nothing there it is stale or junk: live D-Walker orders carried 0xA000,
0xA400 and 0xA401 (guest log 2026-11-03, `follow: the player's buddy command 25 (arg 41985)`). [V-live log]

**So the player's own "Do it" has no ER target**, because no MGSV object stands where ER's enemies are. The guest must replace the
argument (§3.5).

### 2.4 The bite chain: dog attack state → soldier

- **Dog attack state 0x140C64DE0** (rcx = owner, edx = index, r8d = event: **0 enter**, 1 exit, **3 update**, 4 re-enter = enter;
  r9 = state data S: **+0x20 u16 target**, +0x22 u16 last target, **+0x24 u8 mode**, +0x25 u8 result, **+0x27 u8 flags**, +0x14 f32
  timer) [V-exe]:
  - Enter clears order bit 28 (0x10000000, set by the attack order: "order consumed") and sets the timer to 1.0.
    - Target type **2 (TppSoldier2)**: sends the target one of three messages through the message box, with u16 sender =
      `0x2600 | index`:
      - mode 2 or 4: **0x53E70021556A**, plus a byte = (mode == 2) (knife: lethal or not);
      - mode 3: **0x48E4EDE3DAED** (stun knife);
      - anything else: **0x887890EBE04A** (bite: keep busy).
    - Target type 35/36 (WOLF / JACKAL [V-exe name table]): always 0x887890EBE04A.
    - Any other type: no attack.
    - If the target accepts (result ≥ 0): S+0x27 |= 1.
  - Update: not accepted → result 7 (abort).
    - The per-dog flags word (`[[owner+0x78]+8] + index*0x10`) bit 23 rising within the timer → S+0x27 |= 2 and HUD flag
      `[[0x140BFF050()+0x98]+0xE8]+0xA0 |= 0x800`.
    - Bit 23 falling afterwards → HUD flag 0x100000, result 13 or 14.
    - No bit 23 before the timer runs out → abort.
    - Exit clears HUD 0x800.
  - **Mode → style** (static; consistent with the item texts and the lethal byte, [U] that S+0x24 is a copy of this+0xD4 from §2.2):

    | mode | style | command |
    |---|---|---|
    | 0 | busy (bite) | 11 |
    | 1 | fetch | 15 |
    | 2 | kill (knife, lethal) | 12 |
    | 3 | knock out (stun knife) | 13 |
    | 4 | wound (knife, not lethal) | 14 |

- **The soldier's dog-attack handler 0x141491A70** (TppSoldier2; rcx = component, edx = local index, r8 = out result, r9 = message)
  [V-exe] compares the three ids at 0x141491A9D, 0x141491AB9 and 0x141491ACC.
  - It **refuses** (no result write, no forwarding) when the soldier's 64-byte record has +0x3E == 2 or (+0x12 & 3) ∈ {1,2,3}.
    The record is `[[[this+0x78]+0x38]+8] + n*0x40`, where n = the u16 at `[[[this+0x78]+0x18]+0x28] + idx*2`. **It does not
    check the puppet bit (27) or restrict-notice (bit 51)**.
  - Otherwise it accepts:
    - stores the sender at record+0x208;
    - sets the reaction bits at +0x210: bite `&~0x10 | 0x28`, knife `| 0x30` plus 0x100 if lethal, stun knife `&0xFFD7 | 0x10`;
    - sends **0xD0A5D0C7E373** (the transform query the dog's handler answers, §2.2) and copies the reply to +0x40..+0x7C;
    - writes result 0 and forwards to the next component (vf 0xA8).
- **Hook for "the bite connected":** detour 0x141491A70. After the original runs: the message id ∈ {three}, sender >> 9 == 0x13,
  `*r8 == 0` → the soldier accepted. Map the soldier back to its ER handle through the proxy slot table (`soldier id → handle`; the
  local index converts through the u16 table above [V-exe: the same lookup at 0x141491AF2-0x141491B07]). Fallbacks:
  1. poll R+0x54 bit 28 (set by our order, cleared when the dog engages);
  2. poll the per-dog flags bit 23 (contact).

  [U: that a puppet proxy with SetDisableDamage accepts; whether the soldier plays the reaction; the dog's run and leap play anyway
  once accepted]
- **Lua's `DogBiteConnect` is not this.** It is a `Player` message in `TppHero.Messages`, handled with `e.HorseRided` next to
  `ZombBiteConnect`: Snake being bitten [V-data TppHero.lua].

### 2.5 Attack ids and damage rows

`TppDamage` enum registration in 0x1405CAB30, {value, name} pairs [V-exe]. Calibration: the same table gives ATK_Kick 7, ATK_Grenade
178, ATK_SmokeGrenade 181, ATK_StunGrenade 182, ATK_Smoke 284 and ATK_SleepGus 286, all equal to the live ids in mgsv/Logic.cpp:284-300.
- **ATK_DDogBite 144, ATK_DDogKnife 145, ATK_DDogStunKnife 146, ATK_DDogBodyAttack 147.**
- 147 is set up with attacker 0x2600 at 0x140C5DAF9-0x140C5DB17 (`mov eax,0x93` ... `mov eax,0x2600`).
- `DAM_SOURCE_DDog` exists (string 0x1421B1AA8).
- Their damage rows (lethal u16 [0], non-lethal u16 [1]) can be read with the guest's `Row()` (mgsv/Combat.cpp, HEAD :65, `kDamageParams`,
  verified live for weapons, COMBAT §3). [U: the row values; read them live and log them once, as `shots:` does]

### 2.6 Moving and reading the dog natively

- **Location object.** [U] that it is the location class. The finder in Horse.cpp (scan for vtable 0x1424C8EF0 whose feet are within
  0.75 m / 1 m of the buddy's skin root, confirmed after 10 checks over 0.5 s; GUEST 8.1a) should work unchanged with the dog's
  skin. The transform query below is a second, independent check. The setter is vf 0x68 (`kLocationSet`), byte-checked as for the horse.
- **Transform query.** Message 0xD0A5D0C7E373 to target 0x2600 through `[0x140BFF050()+0x60]` vf 0x38. Arguments: rcx = box,
  rdx = &i32 result, r8w = target id, r9 = 0x40-byte message with [0] = the id. The calling convention is the one the buddy command
  function uses at 0x140A4E2B8-0x140A4E2C2. Call it on the game thread only. [V-exe] [U live]
- **Do not use** `TppBuddy2BlockController.RequestMoveToPosition` (with the walker's type it removed the walker; with another type it
  swapped the buddy, GUEST 18.3). Do not use `SetPosition` either: the dog ignores it, and its `position = Vector3` form sent D-Walker
  to the origin (GUEST 8.3, 18.3). `RequestWalkToPosition` did nothing for the walker [U for the dog; not part of the design].

### 2.7 Spotting and announcements

- The tips text: D-Dog "follow[s] you and plac[es] provisional markers when detecting an enemy, ... markers on the map when
  discovering medicinal plants or mines" [V-data tpp_tips_info 0xD348F869].
- Its announcements are a 10-entry StrCode64 table at **0x142A9AD40** [V-exe; texts V-data tpp_announce_log]:

  | index | StrCode64 | text |
  |---|---|---|
  | 0 | 0xB8A0BF169F98 | in none of the extracted `.eng.lng2` files |
  | 1 | 0x5E472FCDE246 | **"D-Dog: Found enemy"** |
  | 2 | 0xE8EF578D974E | "Found wild animal" |
  | 3 | 0x9F5728572521 | "Found weapon" |
  | 4 | 0xD7352FB834B1 | "Found prisoner" |
  | 5 | 0xD48F90114488 | "Bond ... Decreased" |
  | 6 | 0x0D2E9E453C29 | "... Significantly" |
  | 7 | 0xA93BD2409423 | "D-Dog Near Death [Heroism -%d]" |
  | 8 | 0xB7508C615914 | "D-Dog: Left mission area" |
  | 9 | 0x953720E89B62 | "Treated D-Dog's Wounds [GMP -%d]" |

- The poster is **0x140C61AB0**, which tail-calls `[announcer+0x18]` vf 0x1A0 with (id, flag & 1, value). The queue (10 slots of 12
  bytes, delayed) is drained by 0x140C61BE0. [V-exe] [U: how to reach the announcer instance; optional, §3.1]
- **Detection range: [U].** It is not found statically. The distance check is somewhere in whatever enqueues index 1. The host uses
  its own range (§3.1).
- TppBuddyService has `SetDogBarkBossRadius` (0xA54010) and `SetDogSnarlDecoyFriendlyPoint` (0xA54160) [V-exe binder table,
  docs/research/data/mgsv_lua_libs.tsv]. These are dog tuning knobs; not needed here.

### 2.8 vars

The `vars` registrations near 0x14099F300, (VarSite address, offset), all u8 (flags 0x805) [V-exe]:

| var | VarSite | offset |
|---|---|---|
| `buddyDogFlag` | 0x14099F4C3 | 0x2E47 |
| `buddyDogMode` | 0x14099F458 | 0x2E48 |
| `buddyDogEquipType` | 0x14099F317 | 0x2E49 |
| `buddyDogEyeType` | 0x14099F382 | 0x2E4A |
| `buddyDogColorType` | 0x14099F3ED | 0x2E4B |

For comparison, `buddyType` is 0x2D68 (Addresses.h `kVarBuddyType`) and `buddyGearAmmoCount` is 0x2E4C. [U: the values of
EquipType and Mode. Log them at boot next to this+0xD4, which is the value that decides (§2.2, §2.4)]

### 2.9 Lua surface (facts only)

- `GameObject.GetGameObjectIdByIndex("TppBuddyDog2", 0)`, and SendCommand `{id="GetPlant", uniqueId=}` /
  `{id="GetPlacedItem", index=}` (TppPlayer.lua 1243/1274); `{type="TppBuddyDog2", index=0}` with `{id="SetEnabled", enabled=false}`
  (TppMbFreeDemo.lua 171) [V-data].
- Player messages `CalcDogFultonPercent` and `Player.SetDogFultonIconPercentage` (TppPlayer.lua 1025/1185): MGSV computes the dog's
  Fulton success with the same function as Snake's [V-data].
- TppBuddyService: `SetTargetAnimalId` (0xA54AF0), `IsEnableBuddyCommand` (0xA52200), `SetDisableCallBuddyType`, `IsDeadBuddyType`
  / `UnsetDeadBuddyType` [V-exe binder table]. None is needed for the design. `UnsetDeadBuddyType` already runs at boot (GUEST 18.3
  "Revival").

### 2.10 What is still unknown in MGSV (each has a live step in §9 M1)

1. Does the dog's AI attack a **puppet + restrict-notice** proxy (the soldier side doesn't check either bit; the dog's pathing or
   targeting might)?
2. Does a bitten inert soldier raise MGSV's alert phase?
3. Is the dog's location object the location class, and does its AI tolerate being moved?
4. What are the ATK 144-146 rows?
5. Do this+0xD4 and the equipment line up as in the §2.4 mode table?
6. Can the dog be hurt by Snake's MGSV rounds while it bites, and is there an invincibility switch (none among its 3 SendCommand ids)?
7. What is the detection range?

### 2.11 Sites to byte-check (MGSV 1.0.15.4)

| name (proposed `Addresses.h`) | VA | first bytes |
|---|---|---|
| `kBuddyCommandDogWhistle` (case 8) | 0x140A4E17B | `48 b8 6e 21 ee 5c 34 e8 00 00 48 8d 55 ec` |
| `kBuddyCommandDogWait` (case 9) | 0x140A4E18E | `48 b8 9e 3a 08 f0 29 d4 00 00 48 8d 55 cc` |
| `kBuddyCommandDogBark` (case 10) | 0x140A4E1A1 | `48 b8 f6 eb 91 b9 5c 0d 00 00 48 8d 55 e4` |
| `kBuddyCommandDogAttack` (cases 11-15) | 0x140A4E1C7 | `48 b8 3a c2 4c 1c 8e f2 00 00 4c 8d 4d b0 48 8d 55 f4 48 89 45 b0 0f b7` |
| `kBuddyCommandDogTalk` (case 16) | 0x140A4E1B4 | `48 b8 d3 86 6b 46 b2 ed 00 00 48 8d 55 d4` |
| `kDogMessageHandler` | 0x140C56B10 | `40 53 57 41 56 41 57 48 81 ec 18 01 00 00 48 8b 05 0b 28 ef 01 48 33 c4` |
| `kDogAttackOrderStore` | 0x140C579D2 | `66 89 81 c2 00 00 00 c6 81 cf 00 00 00 00` |
| `kDogAttackState` | 0x140C64DE0 | `48 89 5c 24 10 48 89 6c 24 20 57 48 83 ec 40 49 8b d9 8b fa 48 8b e9` |
| `kSoldierDogAttack` | 0x141491A70 | `40 55 53 56 57 41 54 41 55 41 56 41 57 48 8d 6c 24 a8 48 81 ec 58 01 00` |
| `kSoldierDogStunKnife` / `Knife` / `Bite` | 0x141491A9D / 0x141491AB9 / 0x141491ACC | `48 b9 ed da e3 ed e4 48 00 00` / `48 b9 6a 55 21 00 e7 53 00 00` / `48 b9 4a e0 eb 90 78 88 00 00` |
| `kMessageBoxAccessor` | 0x140BFF050 | `48 83 ec 28 e8 47 ff ff ff 48 8b 40 08 48 83 c4 28 c3` |
| `kDogAnnounceTable` (optional) | 0x142A9AD40 | qwords `0xB8A0BF169F98, 0x5E472FCDE246, ...` (§2.7) |
| `kDogAnnouncePost` (optional) | 0x140C61AB0 | `44 0f b6 42 05 44 8b 0a 48 8b 49 18 0f b6 52 04 48 8b 01` |
| `kVarBuddyDogEquipType` / `Mode` / `Flag` | 0x14099F317 / 0x14099F458 / 0x14099F4C3 | `4c 8b 05 02 76 27 02 49 81 c0 49 2e 00 00` / `4c 8b 05 c1 74 27 02 49 81 c0 48 2e 00 00` / `4c 8b 05 56 74 27 02 49 81 c0 47 2e 00 00` |
| `kAtkDDogBite..BodyAttack` | (ids 144-147) | read the names back through `TppDamage.ATK_DDogBite` in the boot Lua once (as for the 492 ids, mgsv/Logic.cpp:284) |

Any mismatch turns the dog layer off and logs the reason, the same rule `Follow.cpp` uses.

---

## 3. The design

### 3.1 Spotting (host only; no protocol)

- Each frame, `er/Dog.cpp` works only while D-Dog is out (`buddyType == 2`, Present|PosValid), Snake plays, and there is no menu,
  cutscene or load. It takes the dog's ER position (`Buddy::Position()`), then every living character from `game::Characters` that is
  ER-hostile and rendered, and keeps those:
  - within `spot_range` (40 m) of the dog;
  - within `spot_height` (8 m) vertically, so that a cave below or a rampart above doesn't count.

  These get a **provisional mark**. MGSV's dog finds enemies by smell, so there is **no line-of-sight check**. All the filters already
  exist in `Marking`, `Actors` and `Combat::HostileToPlayer` [V-code].
- Provisional marks are the binocular marks with a hollow triangle and no number, and a distance only near the view centre. They last
  while the enemy is in range, plus `spot_linger` (10 s). They fade when he dies. A binocular look makes the mark permanent (today's
  rule). New API: `Marking::Provisional(handle, untilMs)`. No new ER address.
- Corner text "D-Dog: Found enemy" when the provisional set gains someone, at most every `spot_text_every` (8 s); "(3 nearby)" when
  there are several.
- Optional (guest, later): MGSV's own announcement through the dog's poster (§2.7, `kHEvDogFound` would be needed). It is left out
  of v1: the announcer instance is [U], and the host's corner already says the same thing.

### 3.2 Orders: MGSV meaning → Elden Ring meaning

| cmd | MGSV | In Elden Ring (v1) |
|---|---|---|
| 8 Whistle, 16 DD | comes to Snake (target cleared) | unchanged (MGSV's AI). The guest also uses 8 itself to call the dog back after an attack (`WhistleBack`) |
| 9 Wait | stays | stays where it is in ER. Needs the dog treadmill (§3.7), otherwise each jump moves it 6 m |
| 10 Bark | "attract nearby enemies" | host: the decoy machinery (`decoy::Notices`, the active-decoy rule: heard, no cone, no ray) centred on the dog. Every awake, non-boss hostile within `bark_range` (20 m) that isn't fighting Snake (> 4 m from him) stops and stares `bark_stare` (5 s), with ChrIns+0x538 no-attack / no-move [V-live HOST "Decoys"]; `bark_cooldown` 15 s per enemy. ER enemies **don't walk over**: ER has no lure (HOST "Decoys": that needs its AI's sound/target RE). MGSV plays the bark sound and animation itself |
| 11 Keep 'em busy | bite and pin | §3.5 to §3.6: the dog bites the target; the host **holds** him (the CQC hold bits) for `busy_hold` (6 s) with a flinch every `busy_flinch_every` (1.5 s). A held enemy is open to CQC and Fulton |
| 12 Do it (kill) | knife kill | the knife's lethal share (ATK 145 row, else 1.2 lives) × `kill_share_mult`, plus a knockdown. Dies only if it reaches 0 (bigness scaling as for rounds) |
| 13 Do it (knock out) | stun knife | sleep build-up `knockout_sleep` (1.1 of his resistance, the choke's ladder) plus a knockdown → asleep → `kActorAsleep` → Fulton-able |
| 14 Do it (wound) | critically wound | a non-lethal hit of `wound_share` (0.9 of his reference HP; stops at 1 HP: `nonLethal`) plus a knockdown |
| 15 Get it | Fulton the neutralized one | target = a **sleeping** non-boss enemy. The dog fultons his existing asleep proxy (`[Fulton] Method = proxy`); MGSV's fulton becomes `kGEvFulton` a=1 as today; the host lifts him and gives runes [V-live HOST "Fulton"]. [U: the dog's fulton produces the same GetStatus 0 / GetActionStatus 1 the guest watches] |

A boss (`Cqc::IsBossHp`) is never a target with `bosses = off` (the default). With `bosses = stagger` he gets one stagger
(`cqc::Response::kStagger`, CQC's rule) and nothing else, with a 20 s cooldown.

### 3.3 Choosing the target (host)

- **Candidates:** within `target_range` (25 m) of Snake and not nearer than `target_min` (3.5 m). Closer enemies are CQC's, and
  the proxy would be parked inside ActorProxy's `SnakeClearance` (3.5 m) anyway [V-code ActorProxy.h]. Also: hostile, alive,
  rendered, not a boss (unless `bosses = stagger`), not held by CQC (`Cqc::IsHeld`). For the fetch style (the guest's style, §4),
  only sleeping ones.
- **Pick:** the candidate nearest the view's centre within `target_cone_deg` (10°). Enemies already marked (binoculars or dog) come
  first. Line of sight is checked from the camera with `CastRay`, using `marking::RayClear` as the binoculars do.
- **Clear run:** the dog has no ER collision, so its run must not cross walls. Sample the straight line dog → target every 0.5 m with
  a down-ray from 2 m above. Refuse when a step between samples is over `run_step_max` (0.8 m), when a sample has no ground within
  4 m (a drop), or when a knee-high ray (0.5 m) along the line hits ER geometry. That is 2 rays per metre on a 25 m line, about 50
  rays, once at the pick and then every 0.25 s for the current pick. Pure rule `dog::ClearRun(samples)`, unit-tested.
- **Publish:** `kActorDogTarget` on that one ActorRecord (§4); the flag is cleared when nobody qualifies.
- **While LB is held** (the call menu is open; `BuddyOrders` already reads LB): a small paw-shaped world marker over the pick, using
  `RenderMarkers`' new owner `kDog`. With no pick, the corner says "D-Dog: look at an enemy (25 m)" once per hold.

### 3.4 Moving the dog to an ER enemy: options

| Option | What moves the dog | Animation | Verdict |
|---|---|---|---|
| **A: one dedicated inert proxy + MGSV's own order** | MGSV's AI runs to a TppSoldier2 that stands at the enemy's mapped x/z on MGSV's ground (`Height = ground`) and is moved every frame (ActorProxy v1's location-object setter [V-live GUEST 20.5]) | MGSV's run, leap, bite or knife, and pin | **Recommended.** It reuses ActorProxy v1/20.6: inert = restrict-notice + puppet before enable, the phase lock, the sweep [V-live 20.6: 11 min, 0 alerts, with 4 proxies] |
| B: the existing asleep proxy | the same, for sleeping targets | MGSV's fulton | used for **Get it** only |
| C: lead through the location object (WalkerLead) + a host-made bite | our setter, every frame | its AI wants Snake: it would glide or moonwalk; no bite animation | fallback only, not in v1 |
| D: Lua `RequestWalkToPosition` / `RequestMoveToPosition` | MGSV | unknown | no: the walker didn't react, and `RequestMoveToPosition` removed or swapped buddies (GUEST 18.3) |

Why one dedicated proxy and not full proxies: full proxies stay off because of aim assist and alerts (GUEST 20.5/20.6). A dog-target
proxy exists only from an order until its release (< 30 s, MGSV's own order timer), for one enemy. Auto-aim stays as it is
(`[Boot] AutoAim = false` by default, so no aim pull).

### 3.5 The guest's attack job (`mgsv/Dog.cpp`, rules in `mgsv/DogLogic.h`)

| State | Enter | Leave |
|---|---|---|
| **Idle** | | `Follow.cpp` `CommandDetour` sees the player's command 11-15 with the dog out and `[Dog] Attack` on: `dog::OnPlayerCommand(cmd)` takes it, so the **original is not called yet**. `kGEvBuddyOrder` still goes out, as today (pad[0] = style+1, §4) |
| **Pick** | read the ActorTable (seqlock) for the `kActorDogTarget` record | none → `kGEvDogAttack` **Refused** (handle 0) and our whistle 8. The host says "D-Dog: no enemy in sight". Found → Realize |
| **Realize** | the target sleeps and has an asleep proxy → use it (Get it); otherwise `proxy::Dedicate(handle, kDogTarget)`: one pool soldier, `PR.ProxyEnable` (inert), Height ground, **no `BuddyClearance`** for this slot (the dog has to reach it), `SnakeClearance` kept | its location object confirmed and placed (budget `RealizeTimeoutMs`, 1500; GUEST 20.5: realization plus a ~220 ms scan) → Ordered. Timeout → Refused, slot released |
| **Ordered** | call 0x140A4E0B0(system, **12**, soldier id) with `t_ourCommand` (any of 11-15 works; the dog's style decides). `kGEvDogAttack` **Ordered** | 0x141491A70 accepts from sender 0x2600 → Connected. No accept within `ReachTimeoutMs` (8000), the target dead or gone from the table, or the dog stuck (distance to the proxy not shrinking for 2 s) → **Lost**, whistle back |
| **Connected** | `kGEvDogAttack` **Connected** (style, ATK id, MGSV share). `kBuddyEngaged` set | per-dog flag bit 23 falls, the attack state exits, `BusyMaxMs` (8000) passes, or the target dies → **Released** |
| **Released** | `kGEvDogAttack` **Released**; slot released (soldier disabled again); whistle 8 (`WhistleBack`) | Idle |

- **Treadmill:** ActorProxy keeps the slot on the ER enemy with its treadmill offset correction [V-code `plan::SelectTargets`,
  OffsetHistory]. The **dog** has to move with Snake's jumps too (§3.7), or a jump during an attack moves it 6 m relative to its prey.
- **Capture:** in ER the dog bites right at the proxy's place. Today a draw within `CaptureRadius` (1.5 m) of a proxy, and nearer to
  it than to Snake, is dropped [V-code Capture.cpp:228, ActorProxyGame.cpp:823]. That would remove the **dog's** draws mid-bite. Fix:
  a learned set of the dog's mesh keys. Mesh keys are content hashes; collect them while the dog's draws are buddy-owned with no proxy
  within 4 m. A draw with a known dog key stays the buddy's whatever proxy is near. The soldier's own draws are still dropped.
  Counter: `capture: buddy draws kept near a proxy N`.
- **Snake's rounds in MGSV** can hit the dog while it bites the proxy. ShotSolver only reports to ER, but MGSV may hurt its dog [U].
  If M1 shows the dog being hurt: lift Snake's MGSV round over the dog (the walker had a similar follow-spot fix, GUEST 18.3), or
  accept it, since saves are blocked.

### 3.6 The bite in Elden Ring (`er/Dog.cpp` → existing Combat / Cqc paths)

| Style (guest) | Host effect | Pipeline |
|---|---|---|
| busy (0) | flinch + `Cqc::HoldFor(handle, busy_hold)` (bits 0x30 at ChrIns+0x538, only ours cleared [V-code Cqc.cpp:26]); a flinch every 1.5 s with a token 1 HP; let go on Released | the CQC grab's effect and hold |
| kill (2) | MGSV's share (b) × reference HP, knockdown poise | `Combat` bullet route with top-up [V-live COMBAT §2] |
| knock out (3) | sleep build-up `knockout_sleep` × resistance on the hit bullet's SpEffect slots, knockdown | the sleep ladder [V-live COMBAT §2] |
| wound (4) | a `wound_share` hit, `nonLethal` (stops at 1 HP), knockdown | `combat::NonLethalCorrection` |
| fetch (1) | nothing on Connected; the Fulton arrives as `kGEvFulton` | HOST "Fulton" |

- Rate: one effect per Connected per target. The `Cqc` cooldown (8 s) also applies to dog holds.
- The hit direction is v1 (dog → target), so a raised shield faces the dog.

### 3.7 Drawing, ground, keeping up

- **Where the dog is:** `buddyPos` from its skin (live). The guest can cross-check it with the transform query (§2.6); log when the
  two differ by more than 0.3 m.
- **Dog treadmill (prerequisite, `[Dog] Treadmill`):** generalize `Horse.cpp` into a buddy location-object helper (find, confirm,
  shift, give up after 3 failures) and call it for the dog in Frame.cpp's treadmill branch (HEAD :682-701, a third case next to
  D-Horse and D-Walker).
  - Moving the dog only while it is within `TreadmillMaxDistance` (40 m) of Snake keeps its MGSV place inside the airport. Beyond
    that, the host hides it (out of sight anyway) and a whistle brings it.
  - **Leash:** a dog more than `Leash` (30 m) from Snake, outside the camera's ~70° view, not waiting and not on an order, is put
    2.5 m behind and 2 m beside Snake through its object (D-Walker's catch-up rule, GUEST 18.3).
- **Ground in ER:** the host snaps the dog to ER ground under it [V-code er/Buddy.cpp]. Three gaps and their fixes:
  1. **No ground within 8 m below** (beside a tower top, a cliff edge after a ladder): today the offset becomes 0 and the dog floats at
     MGSV height. Fix: try the floor at Snake's height ±1.5 m first; then move the drawn spot along the line toward Snake until it
     finds that floor. This is a render-only change; MGSV's dog is untouched.
  2. **A leap:** if MGSV raises the model root during the leap, a per-frame snap would flatten it. Low-pass the offset (0.2 s).
     [U: whether the root rises at all]
  3. **Steps:** the clear-run rule (§3.3) keeps attack runs on walkable ground. Ordinary following pops at ledges as D-Walker does.
- **Climbs:** the buddy isn't drawn while Snake climbs [V-code RenderScene.cpp:141]; afterwards rule 1 applies.
- **Jumps** (R3, ER-side): MGSV's Snake doesn't move, so the dog is unaffected.
- **Riding:** D-Dog can't be ridden. Torrent goes through Tarnished mode: Snake parks, the dog stays, and on return it is placed
  relative to Snake again.
- **Grace travel:** the anchor moves with Snake, and the dog comes along relative to him.

### 3.8 Should ER enemies attack the dog? **No (v1)**

- The dog is not an ER character. Giving ER's AI something to target needs an ER-side character or AI-target RE, the same open item
  as the decoy lure.
- MGSV's dog has its own life and "Near Death" handling (§2.7 index 7). Nothing in the sandbox hurts it: proxies are inert, and MGSV
  blasts don't happen there (`MgsvBlasts`).
- The bitten enemy is held (no attack), so the dog doesn't look immortal in the moment.
- Later option: an ER AoE (a boss slam, an explosion) whose centre is within 2 m of the dog's ER spot sends whistle 8: the dog
  retreats. No damage.

---

## 4. Protocol v17 (proposed; `protocol/phantomring_protocol.h` + `tools/phantomring/proto.py`)

```cpp
inline constexpr std::uint32_t kVersion = 17;  // v17: kActorDogTarget, kGEvDogAttack, kBuddyEngaged, kGEvBuddyOrder pad[0] dog style

enum ActorFlags : std::uint32_t {
    // ... kActorAsleep = 1u << 4,
    kActorDogTarget = 1u << 5,  // (v17) the host's pick for D-Dog's next attack/fetch order: hostile, alive, in range, near the
                                // view centre, a clear run from the dog, never a boss with [Dog] bosses = off. At most one record.
};

enum GuestBuddyFlags : std::uint32_t {
    // ... kBuddyMissing = 1u << 4,
    kBuddyEngaged = 1u << 5,    // (v17) the buddy carries out an attack/fetch order on an Elden Ring target (Connected..Released)
};

enum GuestEventType : std::uint32_t {
    // ... kGEvCqc = 14,
    kGEvDogAttack = 15,  // (v17) D-Dog and an Elden Ring target: a = DogPhase | DogStyle << 8 | MGSV attack id << 16 (ATK_DDog*, 0 =
                         // unknown); b = MGSV's damage share of that attack (1/10000 of a standard soldier's life, as kGEvFire pad[1];
                         // 0 = unknown: the host's per-style values); v0 = the dog's feet (MGSV world); v1 = unit direction dog ->
                         // target (MGSV world; 0 = unknown); pad[0] | pad[1] << 32 = the target's ActorRecord handle (0 = none)
};

enum DogPhase : std::uint32_t {  // (v17) kGEvDogAttack a & 0xFF
    kDogOrdered = 0,    // MGSV took the order on the target's proxy
    kDogConnected = 1,  // the bite / knife landed (MGSV's soldier accepted the dog's attack): the host applies the style's effect
    kDogReleased = 2,   // the dog let go (or BusyMaxMs): the host releases its hold
    kDogRefused = 3,    // no target (handle 0) / no proxy in time / the dog can't take orders: nothing happened
    kDogLost = 4,       // ordered but never connected (out of reach, target gone): the dog is whistled back
};

enum DogStyle : std::uint32_t {  // (v17) kGEvDogAttack (a >> 8) & 0xFF; numbered as MGSV's style byte (dog system +0xD4)
    kDogBusy = 0, kDogFetch = 1, kDogKill = 2, kDogKnockOut = 3, kDogWound = 4, kDogStyleUnknown = 0xFF,
};

// kGEvBuddyOrder (v7): (v17) pad[0] = DogStyle + 1 for D-Dog's orders (b == 2), 0 = unknown or another buddy.
```

`kGEvDogAttack` follows the conventions of `kGEvFire` (share in b), `kGEvCqc` (direction in v1) and `kGEvFulton` (handle in pad).
No GuestState or HostState field changes size: the flags use free bits, and the event uses the free type 15.

Old peers: a v16 host never sets `kActorDogTarget`, and a v17 guest sees no target, so the player's attack orders keep MGSV's
meaning. Versions never attach across a mismatch anyway (`MismatchText`).

---

## 5. Ini keys (proposed)

**`PhantomRingGuest.ini [Dog]`**

| key | default | meaning |
|---|---|---|
| `Enabled` | true | the whole layer (false = today) |
| `Treadmill` | true | move the dog with Snake's jumps through its location object |
| `TreadmillMaxDistance` | 40 | m; farther = left to MGSV (and hidden by the host) |
| `Leash` | 30 | m; a free dog out of view farther than this is put beside Snake |
| `Attack` | true | the call menu's 11-15 go to the host's ER target |
| `TargetPuppet` | true | the target proxy is a puppet (false: restrict-notice only, if M1 shows the dog ignores puppets) |
| `RealizeTimeoutMs` | 1500 | |
| `ReachTimeoutMs` | 8000 | |
| `BusyMaxMs` | 8000 | |
| `WhistleBack` | true | |
| `KeepDrawsNearProxy` | true | the dog mesh-key set in Capture |
| `Announce` | false | MGSV's own "Found enemy" through 0x140C61AB0 [U] |

**`PhantomRingHost.ini [Dog]`**

| key | default | meaning |
|---|---|---|
| `enabled` | true | |
| `spot` | true | provisional marks |
| `spot_range` | 40 | m from the dog |
| `spot_height` | 8 | m, vertical |
| `spot_linger` | 10 | s |
| `spot_text_every` | 8 | s between "Found enemy" lines |
| `target_range` | 25 | m from Snake |
| `target_min` | 3.5 | m from Snake (= the proxy's `SnakeClearance`) |
| `target_cone_deg` | 10 | |
| `target_marked_first` | true | |
| `clear_run` | true | |
| `run_step_max` | 0.8 | m |
| `indicator` | true | the paw marker while LB is held |
| `bosses` | off | `off` / `stagger` |
| `busy_hold` | 6 | s |
| `busy_flinch_every` | 1.5 | s |
| `kill_share_mult` | 1.0 | |
| `kill_share_fallback` | 1.2 | lives, when MGSV's share is unknown |
| `wound_share` | 0.9 | |
| `knockout_sleep` | 1.1 | share of his sleep resistance |
| `bark` | true | |
| `bark_range` | 20 | m |
| `bark_stare` | 5 | s |
| `bark_cooldown` | 15 | s |
| `order_text` | true | corner texts for 8-16 |

---

## 6. File split

| Side | Files | Owner |
|---|---|---|
| guest new | `mgsv/Dog.h/.cpp` (detours: 0x140C56B10 observe-only to learn `this`, style `[this+0xD4]` and orders; 0x141491A70 connect; 0x140C64DE0 optional phases; the attack job; `!dog` console), `mgsv/DogLogic.h/.cpp` (pure job machine, style table, timeouts), `mgsv/BuddyLocation.*` (from `Horse.cpp`: finder, setter, tracker, used by horse and dog) | MGSV agent |
| guest shared | `mgsv/Follow.cpp` (`CommandDetour` → `dog::OnPlayerCommand`; `kGEvBuddyOrder` pad[0]), `mgsv/Frame.cpp` (treadmill dog case), `mgsv/ActorProxy*.{h,cpp}` (`Dedicate`/`Release`, a slot purpose without BuddyClearance), `mgsv/Capture.cpp` (dog mesh keys), `mgsv/Combat.cpp` (export `Row`/`ShareOf` for ATK 144-146), `mgsv/Addresses.h`, `mgsv/verify_addresses.py`, `mgsv/Config.*`, `PhantomRingGuest.ini` | MGSV agent |
| host new | `er/Dog.h/.cpp` (spot, pick, indicator, events, effects, bark), `er/DogLogic.h/.cpp` (pure: pick score, `ClearRun`, spot set, style → effect, boss rule) | ER agent |
| host shared | `er/Actors.cpp` (`kActorDogTarget`), `er/Marking*` (`Provisional`), `er/Cqc.*` (`HoldFor`), `er/Decoys.*`/`DecoyLogic` (stare around a point for bark), `er/BuddyOrders.cpp` (texts 8-16; LB hint per buddy), `er/Events.cpp`, `er/RenderMarkers.h` (`kDog`), `er/Buddy.cpp` (floor-near-Snake rule), `er/Config.*`, `PhantomRingHost.ini` | ER agent |
| protocol | `protocol/phantomring_protocol.h`, `tools/phantomring/proto.py` | integration (first, offline) |
| tools | `fake_er.py`: `pr er actor <i> dogtarget`; `fake_mgsv.py`: `!dog attack <handle\|nearest> <busy\|kill\|knockout\|wound\|fetch> [connect\|release\|lost]` | integration |
| tests | `tests/test_guest_dog.cpp`, `tests/test_host_dog.inc`; `test_fakes.py` checks for the new event and flag | each side |

---

## 7. Test plan

**Offline (both agents; no games)**
- `test_guest_dog`, the job machine:
  - an order with no target → Refused + whistle;
  - with a target → Realize → Ordered ≤ 1.5 s (a simulated clock);
  - accept → Connected; bit 23 fall → Released;
  - a target that dies → Released;
  - no accept for 8 s → Lost;
  - the player's whistle mid-attack → Released;
  - style mapping 0-4.
- `test_guest_proxy`: a `kDogTarget` slot is never parked for BuddyClearance, and still is for SnakeClearance.
- Capture: a dog key near a proxy is kept; a soldier draw is dropped.
- `test_host_dog`:
  - pick: the cone, range, min, marked first, boss refused, CQC-held refused, fetch → sleepers only;
  - `ClearRun`: a step, a drop, a wall;
  - spotting: range, height, linger, death fade, text throttle;
  - style → `HitEffect`, including non-lethal at 1 HP and the boss stagger;
  - bark: stare set, cooldown, not near Snake.
- `test_fakes.py`: `kGEvDogAttack` round trip, and `kActorDogTarget` seen by fake_mgsv.

**MGSV + fake_er** (MGSV agent; `fake_er --actors near`, `pr er actor 0 dogtarget`; MGSV's view through
`GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 gamescopectl screenshot`). M1 RE steps first (§9), then:
- the order through the virtual pad (LB, RS toward the attack entry, release RS, click R3);
- the guest log:
  - `dog: order 12 for ER 0xffffffff00001000 -> soldier 0x450 realized in N ms -> command 12 arg 0x450`
  - `dog: bite connected (0x887890EBE04A accepted by 0x450)`
  - `released`
- fake_er's log shows `kGEvDogAttack` phases 0/1/2;
- MGSV screenshots of the dog on the soldier;
- `alert: ...` never, phase 0;
- the dog as drawn (odometer + buddy draws, as GUEST 18.3 measured the walker): largest step per frame < 0.5 m across 20 treadmill
  jumps (today: [U], expected ~6 m).

**Real ER + fake_mgsv** (ER agent; `--er-headless 3840x2160`, the virtual pad, frames with `pr er "hud shot"`; Gatefront's soldiers,
never a boss):
- spotting frames: hollow marks within 40 m of the fake dog;
- `pr er dog pick`: the paw marker over the soldier in the view centre;
- `fake_mgsv !dog attack nearest busy|kill|knockout|wound`, each once on a fresh soldier. Logs + frames:
  - busy: pinned 6 s, flinches;
  - kill: dead, runes;
  - knock out: `ASLEEP` in `combat targets`;
  - wound: ≥ 1 HP;
- a boss refusal by unit test and by `pr er dog test boss` (treats the pick as a boss; no real boss engaged);
- bark: `npc ... stares at D-Dog`.

**Both games** (integration; saves backed up by `pr launch`, restored and sha256-checked at the end):
- D-Dog deployed from the iDroid;
- 10 orders over 10 min at Gatefront, numbers:
  - order → realized (target ≤ 1.0 s);
  - order → connected (≤ 5 s at 15 m);
  - effect landed;
  - dog back within 3 m of Snake ≤ 6 s after release;
  - 0 alerts;
  - 0 buddy draws dropped near a proxy;
- frames every 0.5 s of one bite;
- Wait + a 50 m run away and back: the dog in place in ER;
- a ladder climb with the dog out: hidden on the ladder, on a floor after.

---

## 8. Risks (ranked) and fallbacks

1. **The dog won't attack a puppet or restrict-notice soldier** (§2.10 #1). The soldier side doesn't check either bit, but the dog's
   AI might. Fallback: `TargetPuppet = false` (restrict-notice only, which raises the alert risk), then option C.
2. **MGSV's alert phase from a bitten soldier** (radio, CP). The phase lock (calm + sweep, give up after 20 s) runs while a dog slot
   exists. If it gives up: Attack off for the session, corner "D-Dog can't attack here".
3. **MGSV's airport geometry between the dog and the proxy** (the terminal is ~40 m north, GUEST 8.1). A 25 m range keeps targets
   near the treadmill centre, which is open apron. A stuck dog → Lost after 2 s with no progress.
4. **The capture drops the dog's draws near the proxy** [V-code]. The mesh-key set fixes it. Without the fix, the dog disappears
   exactly when it bites.
5. **The dog's location object** may not be the location class, or its AI may fight being moved (§2.10 #3). Without it there is no
   Wait, no attack across jumps, and today's flash stays. Fallback: shift the dog only while it attacks or waits (no leash); whistle
   it otherwise.
6. **Style and equipment mapping** (§2.10 #5): the host effect follows the guest's style. A wrong mapping gives the wrong effect, not
   a crash. The log prints `this+0xD4`, `buddyDogEquipType` and the call menu's label.
7. **Boss cheese** (a pinned or asleep boss): bosses are refused by default and only stagger with `stagger`.
8. **Two holds on one enemy** (CQC + dog): `Cqc::IsHeld` excludes; the dog hold uses the same bookkeeping.
9. **A dog injured by Snake's MGSV rounds** (§2.10 #6): saves are blocked; mitigation as in §3.5.
10. **Spotting through walls** reveals enemies on another floor. `spot_height` 8 m limits it; that is how MGSV's dog works.

---

## 9. Ordered plan for two live agents + integration

**I0 Integration (offline, before the live agents):** protocol v17 (§4), `proto.py`, the fakes' commands, empty `[Dog]` sections.
Builds and `check_host` / `check_guest` stay green.

**MGSV agent (real MGSV + fake_er)**
- **M1: live RE, read-only and console-driven.** Each step needs a log line or screenshot.
  1. `!dog` (new, observe-only detour of 0x140C56B10): the dog system `this`, `[this+0xD4]`, `vars.buddyDogEquipType` / `Mode`, the
     ATK 144-147 rows, the call menu's label for the attack entry. This settles §2.10 #4 and #5.
  2. `!dog where`: the transform query against the skin root (< 0.3 m); then the location-object finder on the dog's skin
     (`confirmed`, as for the horse); `!dog shift 3 0` with Snake still (settles #3).
  3. `!dog proxy 10 0` (one inert proxy 10 m ahead) + `!dog order 12 <id>`: screenshots of the run and the bite; the 0x141491A70
     accept line; `alert` lines; repeat with `TargetPuppet = false` (settles #1, #2, #6).
  4. `!dog fetch` on an asleep Fulton proxy: `kGEvFulton a=1`.
- **M2: build.** In order: `BuddyLocation` + dog treadmill + leash; the Capture mesh-key fix; `Dedicate`/`Release` in ActorProxy;
  `Dog.cpp`/`DogLogic` job; `Follow.cpp` interception; `kGEvDogAttack`; Addresses + `verify_addresses.py`; unit tests.
- **M3: live with fake_er**, the §7 MGSV list. 10 minutes with 10 orders: 0 alerts. Recordings into `recordings/` (never committed).

**ER agent (real ER + fake_mgsv)**
- **E1: offline build.** `DogLogic` (pick, `ClearRun`, spot set, effects, bark) + tests; `Actors` flag; `Marking::Provisional`;
  `Cqc::HoldFor`; `BuddyOrders` texts 8-16 and the LB hint per buddy; `RenderMarkers` `kDog`; the floor-near-Snake rule in `Buddy`.
- **E2: live with fake_mgsv**, the §7 ER list. Frames of the marks, the paw, each style's effect, bark stares. `combat set hurt_mult 0`
  while testing in a patrol, 1 again after.

**Integration (both games, after M3 and E2):** the §7 both-games list; the README section "Ordering D-Dog" (and the correction of
README.md:303 / STATUS.md:293, §1); `pr record` of a bite for replay regressions.

Exit criteria for turning the layer on by default: 10 minutes with ≥ 10 orders, 0 alerts, 0 warnings from `dog:`, every style that
the save's equipment allows working once, and the dog as drawn never jumping more than 0.5 m in a frame outside a leash.

---

## 10. Reproducing the RE

`<scratch>/peutil.py` (PE section mapping), `<scratch>/xref.py` (rip-relative reference search) and `<scratch>/lng.py` (the LANG v3
big-endian reader: header `LANG`, count at +0xC, values at +0x10, keys at +0x14, entries {u32 StrCode32, u32 offset}, each value a
u16 then UTF-8) are not included. Everything else:

```sh
EXE=<steam-library>/common/MGS_TPP/mgsvtpp.exe
x86_64-w64-mingw32-objdump -d -M intel --start-address=0x140A4E0B0 --stop-address=0x140A4E2D8 $EXE   # command function, cases
python3 tools/re/mgsv/pdata.py $EXE 0x140c578f7 0x141491ace                                        # containing functions
GHIDRA_PROJECTS=<abs path>/ghidra-projects nice -n 19 tools/ghidra_query.sh mgsv \
  dec:0x140c56b10 dec:0x140c64de0 dec:0x141491a70 dec:0x14110D320 dec:0x14110D0D0                   # handler, attack state, soldier side, labels
```

`tools/ghidra_query.sh` needs an absolute `GHIDRA_PROJECTS`: its default `$here/../../ghidra-projects` contains `..`, which Ghidra 12.3
refuses ("Path element starting with '.' is not permitted").
