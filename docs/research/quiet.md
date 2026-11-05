# Quiet as a sniper buddy in Elden Ring, and resting at Sites of Grace: design study (2026-11-04, HEAD `ccc1710`, protocol v17)

The player's ask (2026-11-04): "let's start on adding Quiet. It would be fun to have her rest with us at Grace sites when she is
deployed, if we can add that feature." This is the plan for the build rounds: one agent on MGSV, one on Elden Ring, then both
games together, as for D-Dog ([ddog.md](ddog.md)). No source was edited, nothing was built and nothing was committed in the study
except this file.

The study had four parts on 2026-11-04. This document merges them and settles where they disagree (§2.0):
- **MGSV, offline RE**: Ghidra (`tools/ghidra_query.sh mgsv`, its lock, nice 19), objdump and Python over the exe, MGSV's unpacked
  Lua and data, the community mtar path dictionary (facts only).
- **MGSV, live**: real MGSV 1.0.15.4 with the host stand-in (`fake_er --actors near`) on `/dev/shm/phantomring_g`. Two sessions,
  06:57-07:36 and 07:37-07:49. The saves were restored after each session, and all 7 files were equal by sha256.
- **Elden Ring, live**: real ER 1.17.2 (`--er-headless 3840x2160`, the exclusive virtual pad) with `fake_mgsv --buddy quiet` and a
  replay of the real Snake on `/dev/shm/phantomring_h`, 06:59-07:26. There were six rests at three graces, none with an NPC talk.
  The saves were restored, and all 8 files were equal by sha256.
- **Architecture, offline**: what Quiet can reuse of the code D-Walker, D-Horse and D-Dog use on both sides.

**Revised after review (2026-11-04, same day; doc only, no game run).** 35 critiques were checked against the exe, the code and
the study's logs; the ones that held are folded in where they belong. The main changes:
- The order record needs one more dereference: R = `[[sys+0xB0]+8] + i*0x200` (§2.4, §2.11; objdump of three sites).
- The seat is placed only inside a black that is **observed**, not on a timer. A rest that turns into a talk (no black) never gets
  a seat, and the host never shows her at the seat before the guest's echo (§3.9, U12).
- Fire needs MGSV's mark to survive: the guest's periodic `ClearMarkers` (every 5 s by default) is held off while a stand-in is
  dedicated. The stand-in stands at the enemy's height, and after the order it may move onto a clear MGSV line (§3.4, Q4, Q5).
- Her rest pose is **unverified**: live she kneels with her rifle **up**. "Rifle lowered" is now a measured exit item (§3.9, Q2).
- The save states are told apart by bits, with a fourth case (recruited, not yet deployable). The borrow is off by default, kept
  for the whole guarded session, re-applied after each sandbox load and checked by a raw byte diff (§2.2, §3.1, Q14).
- Elden Ring's world shifts move every kept Elden Ring position (posts, the grace, the seat, `g_gracePos`) (§3.2, §3.9).
- The grace rest ships together with posts and drawing, never on 0.8.0's Quiet (§7, §9).

**The player's answers (2026-11-04)** are recorded in §0 and override the study's "off by default" where they say so: Borrow =
`always` and `BorrowListInIdroid = true` as the shipped defaults (each save state gated by Q14), v1b's watch accepted as the rest
pose if nothing lowers her rifle, cloaked while Snake travels. **Round I0 (2026-11-04, offline)** committed protocol v18 (§4, with
the changes listed there), the fakes, the `[Quiet]` / `[Grace]` ini sections (everything off) and `tests/fixtures/quiet/`.

**Integration (0.9.0-alpha, 2026-11-04, both games): what shipped is §11.** In short: the borrow on any save, posts and drawing,
spotting and the grace rest are **on by default**; her orders are built but **opt-in** (`[Quiet] orders`, host): MGSV offers Fire only
from its own snipe points. Five things the real games showed changed the design: Quiet (Scout) / (Attack) greyed only while she is
out; the seat echoed only once she has settled (~3 s after her order) and faded in after the black; no get-up order while she can be
seen; the holder waits out a treadmill jump; Elden Ring's bars left alone at a grace (Pass time).

Every fact is marked:
- **[V-exe]**: read from `mgsvtpp.exe` 1.0.15.4 (`<steam-library>/common/MGS_TPP/mgsvtpp.exe`, 166,517,760 bytes). The bytes are
  listed in §2.11.
- **[V-data]**: MGSV's own files: Lua (file and function), lang keys, packs, fmdl/mtar headers.
- **[V-code]**: this repository at `ccc1710`, cited as file:line.
- **[V-live M]**: the MGSV live sessions above (guest log lines, frames, Lua read-outs, `/proc` reads), quoted here because the
  study's scratch files are not kept.
- **[V-live E]**: the Elden Ring live session above (host log lines, frames, `hud shot` luminance, console output), quoted the
  same way.
- **[V-doc]**: recorded live earlier, in GUEST.md, HOST.md, COMBAT.md or ddog.md (cited).
- **[U]**: unverified. Each one has a probe Q1-Q18 (MGSV) or U1-U14 (ER) in §2.10.

New Elden Ring addresses: none. The host reuses `CastRay`, the character list, `GeomProbe`, the Actors table, `Marking`, the
bullet route and the sleep ladder, which are already verified for both ER tables. The new MGSV sites are in §2.11. Each must be
byte-checked in `mgsv/Addresses.h` and `mgsv/verify_addresses.py`.

---

## 0. What the player gets (summary)

| # | Feature | How | Protocol |
|---|---|---|---|
| 1 | **Quiet on saves that have her.** If the save has her, she deploys as she is. If she left after "A Quiet Exit", was recruited but isn't deployable yet, or was never recruited, she is borrowed **in memory for the session**: on by default for every save (`Borrow = always`, `BorrowListInIdroid = true`: the player's answer to open question 1), each save state only once it has passed Q14 live (a state that fails stays off, with a log line and a README note). MGSV's save is never written, and the borrowed bytes are put back at shutdown | in-memory `TppBuddyService` calls chosen from the save's bits (LOST was the gate on the player's save), only while the save guards are complete. Re-applied after each sandbox load, and checked by a raw byte diff when put back | none |
| 2 | **She holds a sniper's post in Elden Ring.** While enemies are near, she takes ground 25-55 m from Snake with a clear line to him and to them. Near Snake when it is calm. While Snake travels she is "on the move" (cloaked, unseen) and appears at her next post when he stops. Never floating, never buried, never with a roof's props | the host picks the post from Elden Ring's terrain (rays). The guest holds her MGSV body at the matching place (the "mirror"), so MGSV draws her when Elden Ring's camera sees her post. The host draws her at the post. A post change in view is shown as her cloak (a 0.3 s fade out and in) | v18 (post) |
| 3 | **Fire / Take aim on Elden Ring enemies.** Look at an enemy through the binoculars: MGSV marks his stand-in, so the call menu's "Fire" and "Take aim" become live. Order Fire: Quiet fires with MGSV's own aim and shot, and the round lands on the Elden Ring enemy if she has a line of sight from her post. Wicked Butterfly is lethal, Guilty Butterfly puts him to sleep, Sinful Butterfly hits hard. Take aim: she fires when Snake next fires, or on Fire | one dedicated inert proxy soldier at the host's pick, at his height (D-Dog's machinery), with MGSV's periodic marker wipe held off while he is out. MGSV's own order and shot. Her hit is seen at its sender (0x140D04D50) and becomes `kGEvQuietShot`. The host fires from her post with Elden Ring's line of sight. MGSV's own geometry can still stop a shot (MGSV has an airport where Elden Ring has none): that is reported as "no shot (MGSV)", and Q5 decides whether the stand-in moves onto a clear MGSV line after the order | v18 (target flags, marked bit, event) |
| 4 | **Spotting.** From her post she marks hostiles she can see within 60 m (provisional marks; never a boss, a neutral or a spirit). The corner says "Quiet: Enemy spotted (N)" | host only, rays from her post's sight point | none |
| 5 | **Cover me / Cease cover** (opt-in after its live test). She shoots, one shot every ~8 s, at the hostile that threatens Snake most and that she can see. Until then the entry says "not in Elden Ring yet" | the host's threat pick plus the same order job, while MGSV's own cover mode is on | v18 (cover bit, threat flag) |
| 6 | **Rest at a Site of Grace.** When Snake rests with Quiet out, she is beside the grace when the screen comes back from black: 2 m from the light, on Snake's free side, in view and clear of the menu, facing the grace. She stays through the grace menu, Pass time and Level up. When he walks away she gets up and goes back to a post. **Her pose**: live she kneels with her rifle up; if Q2 finds nothing that lowers it, she kneels **on watch** (v1b: 2 m from the grace, facing out, the rifle pointed away from Snake), which the player accepted (open question 2), and the rest ships with it | the host finds the grace and a seat once Elden Ring's rest black is **observed** (U12), places her inside it, and shows her there only after the guest's echo. No black (a rest that turns into a talk) = no seat. MGSV plays her own kneeling standby (v1, or v1b's watch). A true seated pose (her helicopter cabin's cross-legged wait) is v2, a bonus if a way to play it is found, never blocking | v18 (grace; buddy-generic) |
| 7 | **Her orders in the corner.** "Quiet: Fire (kill)", "Quiet: Take aim", "Quiet: no target -- look at an enemy through your binoculars", "Quiet: no clear shot from her post", "Quiet: no shot (MGSV)", "Quiet: Cover me -- not in Elden Ring yet", and a reticle over the enemy she would take while LB is held or the binoculars are up | host texts, `RenderMarkers` | none |
| 8 | **A small fix now:** her orders no longer pause D-Walker's auto-follow (the D-Dog bug of 0.8.0, again) | `mgsv/Follow.cpp` | none |

She never targets a friendly or neutral character, a spirit summon or a boss (the pick refuses them, and the effect checks again).
Elden Ring's enemies don't attack her (§3.10). Nothing of this runs while she isn't out, and in Tarnished mode she holds still and
stays out of it (§3.11). With the host's `[Quiet]` off she is 0.8.0's Quiet: MGSV's own buddy, crouching beside Snake, and she
never comes to the grace (the rest needs the layer and posts, §3.9).

**Open questions for the player: ANSWERED 2026-11-04** (the answers override the study's defaults where they differ):
1. **Borrowing.** His own save is the "gone" case, so with `Borrow = off` he would get no Quiet. Should a Quiet the save has had
   be borrowed in memory by default, and listed in MGSV's iDroid at boot? Should never-recruited and killed saves need `always`?
   **Answer: everyone, any save, by default.** The shipped defaults are `[Quiet] Borrow = always` and `BorrowListInIdroid = true`
   (no ini edit needed; she is listed in MGSV's iDroid BUDDY SUPPORT), memory only, MGSV's save never written. Each save state
   still has to pass Q14 live (gone = the player's own save; never recruited, killed and recruited-not-deployable simulated in
   memory); a state that fails stays off with a log line (`quiet: borrow for <state> off: <why>`) and a README note. Round I0
   ships `Borrow = off` in the ini and the code until the integration turns it on by §7's Borrow line (§5, §9).
2. **The rest pose.** If nothing lowers her rifle (Q2), is v1b acceptable? **Answer: yes.** If no seated or rifle-lowered pose is
   found, "kneel on watch" (v1b: kneeling 2 m from the grace, the rifle pointed away from Snake, keeping watch) is accepted and the
   grace rest ships with it (`SitPose = watch`). A true seated pose (her helicopter cabin's cross-legged wait, Q3 / P11) is still
   looked for as a bonus, never blocking (§3.9).
3. **Unseen while travelling.** **Answer: yes, as designed**: cloaked while Snake travels, like MGSV's Quiet, appearing at her
   post when he stops.
4. **Tarnished mode.** Not asked: no grace rest in Tarnished mode, as designed (`[Grace] tarnished = false`).

---

## 1. Quiet in PhantomRing today (what the code does at `ccc1710`)

- **Deploying**: `PR.Buddy('quiet', ...)`, `BUDDY_CLASSES.QUIET = { 'TppBuddyQuiet2' }` [V-code GuestScript.cpp:138-146]. It
  refuses with `missing: not developed in this save` when `DidObtainBuddyType(QUIET)` is false and with `missing: cannot sortie`
  when `CanSortieBuddyType(QUIET)` is false [V-code GuestScript.cpp:191,198]. `AdoptBuddy` follows an iDroid deployment of Quiet;
  a dismissal from the iDroid means no buddy [V-code Pump.cpp:534-555]. The iDroid's DISMISS QUIET entry is left active [V-code
  GuestScript.cpp:796]. On the player's save she is "missing" (§2.2).
- **Her place**: the Tick reads Lua `GetPosition` every tick and uses the skin only when that is missing [V-code
  GuestScript.cpp:1062-1067, Pump.cpp:331-350]. Quiet answers GetPosition [V-live M], so `buddyPosValid` comes from Lua. The
  automatic skin hint runs only while `!buddyPosValid` [V-code Pump.cpp:512], so it never runs for her, and `buddyYaw` is the
  Tick's motion heading. She has no real heading today (90.0 while she stood still [V-live M]). GUEST.md 8.3 still says "she
  takes the D-Dog path (no GetPosition assumed, skin learned)", which is outdated.
- **Treadmill**: Quiet is left out on purpose (`buddyActive != kBuddyTypeDog && != kBuddyTypeQuiet` [V-code mgsv/Frame.cpp:700-701]).
  In Elden Ring's terms she jumps **6.00-6.03 m at every treadmill jump** (9 of 9 measured [V-live M]): D-Dog's old "6 m flash".
- **Capture**: any draw within 4 m of `buddyPos` is the buddy's, rigid or not [V-code Capture.cpp:220-224]. D-Dog's learned mesh
  set is the dog's alone. On a roof, 225 rigid props were taken as Quiet in one minute [V-live M].
- **Orders**: Follow.cpp's command detour reports her commands 1-7 as `kGEvBuddyOrder` with b = 3 [V-code Follow.cpp:491]. The host
  shows them as "Buddy order N" [V-live E; V-code BuddyOrders.cpp:29-70]. **Bug:** they set D-Walker's auto-follow hold, and the hold
  survives a buddy change. Live: a D-Walker deployed after her stood 25.4 m away (`buddy walker: ready (paused by the player's
  command) ... player commands 8 (last 7)`) until the player whistled [V-live M; V-code Follow.cpp:470-485].
- **Her shot**: nothing reaches Elden Ring. Her rifle never goes through ShotSolver, so there is no `kGEvBuddyFire` (0 `shots:`
  lines in the whole session [V-live M]). ROADMAP 2.5 assumed "Quiet's rifle through ShotSolver"; that is wrong (§2.6).
- **Host**: `Buddy::kQuiet = 3` [V-code er/Buddy.h:33]. She is placed like any buddy: mapped `buddyPos`, a ground ray from 2.5 m up
  and 8 m down, a floor search only within 30 m of Snake, and the drawn lift clamped to ±1.5 m [V-code er/Buddy.cpp:314-345,
  er/Buddy.h:76, er/RenderScene.cpp:172]. The step guards and the Wait pin are D-Dog's only [V-code er/Buddy.cpp:255,
  er/RenderScene.cpp:247].
- **Grace**: the host sends `kHEvRested a = 0` on the rest and keeps `g_atGrace` / `g_gracePos` (the **player's** spot, not the
  grace's) until he is 2 m away, a load or a death [V-code er/Host.cpp:72-79, 265-296, 658-665]. Nothing else reads them. The
  guest gets only the one-shot event: life and ammo refilled, `rested: grace rest` [V-code mgsv/Frame.cpp:253-266].
  `kHEvRested 0` fires on the press, before Elden Ring shows anything: also when the rest turns into a talk (Melina, the HUD never
  hid) [V-code er/Host.cpp:261-263]. A rest seen through the HUD (`viaHud`, :278) is detected only when the HUD hides. `g_gracePos`
  is not moved by Elden Ring's world shifts (`Host::OnWorldShift` moves the ladder state only [V-code er/Host.cpp:1027-1034]).
- **Tools**: `fake_mgsv` publishes Quiet with object id 0x0405 [V-code tools/phantomring/fake_mgsv.py:139]. The real id is 0x2400.
- The README says "Quiet is untested" (README.md:340, 635). That is true for the release.

---

## 2. MGSV facts

### 2.0 How the four studies were reconciled

Where the studies disagreed, live evidence won over offline evidence, and a complete decode won over a partial one.

| topic | what was said | chosen, and why |
|---|---|---|
| Her SendCommand handler | architecture: 13 hashes compared, 2 named; RE: 29 cases, 19 named | **29** (the RE decoded the whole switch); GetPosition confirmed live |
| GetPosition | both offline studies: "probably" | **[V-live M]** she answers it, 3.0 m from Snake |
| Her shot | ROADMAP 2.5 and the architecture study: through FireWeapon/ShotSolver, so `kGEvBuddyFire`; RE: unknown | **live**: no ShotSolver round at all. Her hit is a damage message `0x5CFEE2E03274` sent to the soldier. Read again in the exe for this document: its sender 0x140D04D50 sends only to soldier ids (§2.6) |
| Bond gating of Take aim / Cover me / Shoot this | RE: 0x140A52140 needs a raw friendly value ≥ 250 / 500 / 750 | **live**: on the player's save `IsEnableBuddyCommand` is true for all three with bond 0, and the menu offered Take aim and Cover me. So the gate's value is not the current bond (perhaps the highest ever reached) [U Q12]. The design depends only on Fire |
| The memory-only unlock | RE: seven calls (unset LOST, HOSPITALIZE, DYING, Disable, Dead; set Obtained, Sortie) | **live**: three calls were enough on a "gone" save (Obtained, Sortie, unset LOST); obtained + sortie without unsetting LOST left the block unloaded. Design: the three always, plus each other unset only when that bit is set (§3.1) |
| Where her MGSV body is kept | RE: leave her at MGSV's sniping point, never treadmill her; architecture: move her with every jump; MGSV live: keep her at the place matching Elden Ring, moved with the treadmill; ER live: the host owns the perch and moves her draws | **the host owns the post, and the guest keeps her MGSV body at the matching place** (§3.2). Live: she never moves by herself in the sandbox, so there is no "running back to her point"; MGSV culls her by its own camera; and her aim and MGSV's buddy marker are right only when she is at the matching place. The host draws her at the post whatever MGSV does (the translation absorbs the treadmill), so an MGSV that refuses still fails safe (branch N, §3.2) |
| Where she sits at the grace | RE: 1.6 m beside Snake, facing the grace; architecture: 1.6-2.2 m from the grace, away from the camera | **ER live**: 2.0 m from the grace (not from Snake), on the camera's side right of Snake, in view and right of the grace menu. It was measured at three graces with the real MGSV rest camera, which is frozen 5.1 m behind Snake. "Away from the camera" would hide her |
| The grace's position | architecture: unknown | **[V-live E]**: the `AEG099_060_9NNN` asset, or `BonfireWarpParam` + `[player+0x80]` |
| Her Fire target | RE: write the target into her record (R+0x186 / R+0x182); MGSV live: the command's argument was MGSV's binocular-marked soldier, and she shot him | **both**: a stand-in soldier that MGSV's binoculars mark makes MGSV's own choice right (the menu needs a marked soldier anyway, §2.4), and the guest writes R+0x186 / R+0x182 after the handler as a safeguard. Her shot function takes its target from R+0x186 [V-exe 0x140D0655D] |
| The cloak | architecture: maybe a refraction pass the capture misses | **[V-live M]**: MGSV draws nothing at all while she is cloaked |
| A seated idle | RE: v1 her crouch idle, v2 the helicopter cabin's seat; MGSV live: none found, and RIDE puts her prone at a fixed map spot | **v1 = her own kneeling standby, whichever motion that is.** Live she kneels on one knee **with her rifle up**; which motion plays (`q_idl_l`, `q_rdy_idl`, `q_snr_idl`) and whether anything lowers the rifle are **[U Q2]**. v2 needs RE (Q3) |
| The save states | design draft: by missions (10050) | **by bits** (§2.2): killing her in mission 11 clears 10050 too, and a recruited Quiet before side op `mtbs_q99011` is obtained but not sortie-able |
| Skin and heading | architecture: the skin hint's 12 m / 3 m limits won't find her | **[V-live M]**: GetPosition gives her place. The skin (136 bones) gives her heading once hinted, `!buddy` -> `yaw 1.6`. The automatic hint must stop waiting for `!buddyPosValid` |

### 2.1 Identity, packs, ids

- `TppBuddyQuiet2`, GameObject type **18** (`GAME_OBJECT_TYPE_BUDDYQUIET2`), id **0x2400** [V-exe: the name table at 0x142A7C690,
  entry 18 -> string 0x1422DD030; the Quiet code hard-codes `mov eax,0x2400`, e.g. at 0x140CE10D1] [V-live M:
  `GameObject.GetGameObjectIdByIndex("TppBuddyQuiet2", 0)` = 9216]. `BuddyType.QUIET` = 3 [V-live M]. The Mother Base Quiet is
  another class (`TppMbQuiet`, type 39), and so is the mission-11 boss (`TppBossQuiet2`, type 25) [V-exe]. That matters for the
  "no suggestive motions" rule: the Mother Base cell and shower scenes belong to `TppMbQuiet` and its packs, not to the buddy.
- Lua enums [V-exe 0x140A518F0]: `BuddyFriendlyType.QUIET` = 0; `BuddyCommonFlag`: BOSS_QUIET_KILL 0, BUDDY_QUIET_DYING 1,
  BUDDY_QUIET_HOSPITALIZE 2, **BUDDY_QUIET_LOST 3**, BUDDY_QUIET_VANISH 4; `BuddyCommand`: QUIET_AIM_TARGET 3, QUIET_COMBAT_START 4,
  QUIET_SHOOT_THIS 6. `BuddyInitStatus` [V-live M]: NORMAL 0, HALO 1, RIDE 2, ENTRY_GATE 3, ESPIONAGE 4.
- Packs [V-data]: `Assets/tpp/pack/buddy/quiet/buddy_quiet2_00.fpk` (body `qui0_main0_def.fmdl`), `_common` (`BuddyQuiet2_layers.mtar`),
  `_add` (`BuddyQuiet2_add.mtar`, `BuddyQuiet2Facial.mtar`, `BuddyQuiet2_layers.mog`), and the weapon `bq_wp_00.fpk`.

### 2.2 The save's Quiet: four states, told apart by bits

All of these are `vars` in one block, `[0x142C16920] + offset` [V-data `docs/research/data/mgsv/vars_registrations_1.0.15.4.txt`]:
`obtainedBuddyType` 0x2D69 and `sortieBuddyType` 0x2D6A (bit 3 = Quiet), `disableCallBuddyType` 0x2D6B, `deadBuddyType` 0x2D6C,
`buddyCommonFlag` 0x2D80, `buddyCallCount` 0x2D88, `buddyFriendlyTimer` 0x2DC4, `buddyFriendlyPoint` 0x2DE4 (index 0 = Quiet),
`buddyFriendlyMaxPoint` 0x2DF4, `buddyFriendlyCount` 0x2E04, `buddyQuietEquipType` 0x2E44, `buddyQuietCostumeType` 0x2E46, and the
`*MissionStart` copies of obtained / sortie / dead (0x2D09-0x2D0B), common flag (0x2D20) and friendly points (0x2D24). The story's
gvar is `str_didLostQuiet` [V-data TppGVars.lua]. **Not in `vars`**: `SetDisableBuddyType` ORs its bit into the static byte
**0x142C2919E** [V-exe 0x140A53EFF `40 08 3d 98 52 1d 02`, `or [rip+...], dil`], so a snapshot of the vars block alone misses it.

The classification uses the bits only (missions are context, not tests):

| state | bits (read-only Lua) | today's guest says |
|---|---|---|
| **present** | obtained ∧ sortie ∧ ¬LOST ∧ ¬dead ∧ ¬HOSPITALIZE. MGSV's BUDDY SUPPORT lists her | deploys |
| **recruited, not deployable yet** | obtained ∧ ¬sortie ∧ ¬LOST. She becomes sortie-able only when side op `mtbs_q99011` is cleared (below) | `missing: cannot sortie` [V-code GuestScript.cpp:198] |
| **gone** ("A Quiet Exit") | LOST ∨ `gvars.str_didLostQuiet`. On the player's save also not obtained, 10260 cleared, bond 0: `TppStory.RequestLoseQuiet` ran (TppStory.lua:609-611) after `_UpdateS10260OpenFlag` set LOST (:514) [V-data] | `missing: not developed in this save` |
| **never recruited** | ¬obtained ∧ ¬LOST ∧ ¬`str_didLostQuiet`. Sub-case **killed**: `BOSS_QUIET_KILL` set (killing her in mission 11 clears mission 10050 too, so "10050 not cleared" is not a test) [V-data TppStory.lua:598, `IsOccuringBossQuiet`] | `missing: not developed in this save` |
| (sub-states of present / recruited) in sickbay / dying / dead | HOSPITALIZE / DYING set, or `IsDeadBuddyType` true | `missing: dead` (or revived with `revive`) |

**Mission start sets sortie, never clears it** [V-data TppMission.lua:2324-2335, called from TppMain.lua:102 at every mission
load]: `SetSortieBuddy` does nothing while LOST is set; otherwise, once `mtbs_q99011` is cleared and she is obtained, it sets
sortie. So a borrowed sortie bit is not dropped by that function. Whether a sandbox load restores `vars` from the `*MissionStart`
copies (or from the save data in memory) and so drops a borrow is **[U Q14]**.

**The player's save is "gone"** [V-live M, `pr lua -f qstate.lua` at 07:01]: BUDDY SUPPORT lists D-HORSE, D-DOG, DISMISS D-WALKER
and CHANGE EQUIPMENT, with no Quiet (frames). Obtained false, sortie false, `buddyCommonFlag` 8 (= LOST), `str_didLostQuiet` true,
10050 / 10086 / 10260 cleared, story sequence 33, bond 0 of 100 (D-Dog's 100), `buddyCallCount[QUIET]` 46 (she was deployed 46
times before she left), `buddyQuietEquipType` 4 (Sinful Butterfly), costume 0. Her command unlocks survived:
`IsEnableBuddyCommand(3 / 4 / 6)` = true / true / true.

### 2.3 Deploying her in memory ("borrow")

- [V-live M] `!buddy quiet` on this save: `boot: Quiet: missing: not developed in this save`.
- [V-live M] `SetObtainedBuddyType(QUIET)` + `SetSortieBuddyType(QUIET)` alone: `!buddy quiet` asked the block 6 times in 60 s
  (`waiting for block (active 0)`), then `boot: Quiet not deployed after 60 s`. D-Walker had been unloaded meanwhile.
- [V-live M] **LOST is the gate.** After `UnsetBuddyCommonFlag(BUDDY_QUIET_LOST)` (`buddyCommonFlag` 8 -> 0): `boot: Quiet back next
  to Snake`, `IsBlockActive()` true, id 0x2400. In session 2, the three calls first and then `!buddy quiet`: `Quiet: block
  requested` 07:44:22.193 -> `active TppBuddyQuiet2 9216` 07:44:23.194, **deployed in 1.0 s**, beside Snake.
- [V-live M] **Undoing it**: `UnsetObtainedBuddyType`, `UnsetSortieBuddyType`, `SetBuddyCommonFlag(LOST)`,
  `SetFriendlyPoint(QUIET, 0)`, `vars.buddyCallCount[QUIET] = 46`. The read-out (qstate.lua) matched the start again. MGSV's
  counters move in memory with use: the bond rose to 5 and the call count to 47 during the session. **What that read-out did not
  check** [U Q14]: `buddyFriendlyTimer`, `buddyFriendlyMaxPoint` and `buddyFriendlyCount` (bond accrual moves them too), the
  `*MissionStart` copies, and the static disable byte 0x142C2919E (§2.2). So "equal" is shown only for the fields read; the revised
  borrow snapshots and diffs the raw bytes (§3.1).
- [V-live M] **Nothing reached MGSV's save.** The guest logged save activity only during the load (Steam Cloud writes dropped,
  local copies redirected to `PhantomRingSaveSink/`), and no `saves:` line at all afterwards. `!status`: `steam saves guarded:
  true`. All 7 MGSV files were equal by sha256 at the start, before each restore, and after it.
- `RequestReunionQuiet` (the game's own reunion) was **not** used and must not be: it sets the bond to 100, resets the reunion
  counter and refreshes Mother Base management (TppStory.lua:613) [V-data]. The gvars are never touched.
- [U Q14] A never-recruited, a killed or a recruited-not-deployable save: whether her block loads after the calls of its plan
  (§3.1). No such save is on this machine; Q14 simulates each in memory on the player's save.
- [U Q14] Whether MGSV's iDroid BUDDY SUPPORT lists a borrowed Quiet. It should, because it reads the same bits (`TppTerminal`).
- [U Q14] Whether a borrow survives a sandbox reload (the guest reloads by itself: `Boot::kLoadSandbox` when MGSV leaves the
  sandbox, deaths, continues [V-code mgsv/Pump.cpp:977-983]), and what putting the bits back does to a Quiet that is still out
  (the live round trip dismissed her first).

### 2.4 Her handlers, orders and the call menu

- **Message handler 0x140CE3330**, in two vtables at slots 0x14232CD40 / 0x14232CEB0. The next slot (0x14232CD48 / 0x14232CEB8)
  is the **SendCommand handler 0x140CE1E10**. It is the dog's layout [V-exe]: rcx = the Quiet system, rdx = the out result, r8d =
  the index (checked `< [rcx+8]`), r9 = the message. A detour on 0x140CE3330 gets the system pointer, as `Dog.cpp`'s
  `HandlerDetour` does [V-code mgsv/Dog.cpp:389-399].
- **Order record R** = `[[sys+0xB0]+8] + index*0x200`: `[sys+0xB0]+8` holds the **pointer** to the R array, so there are two
  loads [V-exe, objdump for the revision: the Fire case 0x140CE3B25 `mov rax,[r14+0xb0]`, 0x140CE3B35 `shl rdx,9`, 0x140CE3B39
  `add rdx,[rax+0x8]` (`48 03 50 08`), then `mov [rdx+0x182],ax`; the order processor 0x140D04615 `mov r9,[rax+0xb0]`,
  0x140D04620 `shl rbx,9`, 0x140D04624 `add rbx,[r9+0x8]` (`49 03 59 08`); the hit sender 0x140D04DC4 `mov rcx,[r8+0x70]`,
  `mov rcx,[rcx+0xb0]`, `mov rcx,[rcx+0x8]`, `mov r8,[rdx+rcx]`]. The draft's `[sys+0xB0]+8 + i*0x200` (and the RE notes'
  §1.2) missed the second load; every read or write of R (the Ordered write of R+0x186, the cover bit, R+0x198, the probes) uses
  the corrected form. In the processor and the hit sender, rcx is one of her **action components**, whose +0x70 is the system:
  only the handler detour (0x140CE3330) gets the system itself.

  | offset | meaning |
  |---|---|
  | +0x180 | active target |
  | +0x182 | requested target |
  | +0x186 | kept aim target |
  | +0x198 | disabled bits (bit 4 fire, bit 5 aim, set = off) |
  | +0x19C | active orders |
  | +0x1A0 | requested orders |
  | +0x1AC | order delay, **1.8 s**, written by every order |
  | R[0] bit 0 | in cover mode |

  The order processor 0x140D04600 acts only once the delay reaches 0. It then makes the first of R+0x186, R+0x182 and R+0x184 that
  is set the active target. With none set, she plays her "can't" voice 0xCFBC3928 [V-exe].
- **Commands 1-7** (call menu -> 0x140A4E0B0 -> message to 0x2400) [V-exe cases, V-data labels]:

  | cmd | message | label | her handler | live [V-live M] |
  |---|---|---|---|---|
  | 1, 2 | 0x7E380C754E3A, +0xA = 1 | Fire (kill / tranq enemy) | target = `[[[sys+0x1E0]+0xE8]+0x70]` (else R+0x186) -> R+0x182; requests aim + fire | arg 1104 = 0x450 (the marked soldier); **she shot him 3.1-4.3 s after the click** |
  | 3 | same, +0xA = 0 | Take aim | same target; aim only | arg 0x450; aimed (MGSV's laser on him) |
  | 4 | 0x56A36C25640E, byte 1 | Cover me | cover mode on | "Quiet: Engaging enemy" 1.8 s later; stayed on her perch |
  | 5 | same, byte 0 | Cease cover | cover mode off | "Quiet: Standing down" |
  | 6 | 0xE26B1E1A8A9E | Shoot this (grenade ricochet) | order bit 0x400 | never shown (no grenade in hand) [U Q13] |
  | 7 | 0xA265A20DFEEF | Quiet (talk) | order bit 0x10 | arg 0xFFFF; she lowered her rifle and turned to Snake; **MGSV's camera turned to her** |

  Commands 1 and 2 are the same message. The label follows her rifle: the Sinful Butterfly gives "Fire (kill enemy)", and the
  tranquilizer rifle gives "Fire (tranq enemy)" [V-live M frame; V-data `tpp_hud` 0x637b5a20].
- **The call menu needs a marked enemy** [V-live M frames]. With nothing marked it offers Knock, Fire (greyed), Take aim (greyed),
  Quiet (talk) and Sonar. With an enemy marked through the binoculars, Fire and Take aim are live and the talk slot becomes Cover
  me. While she covers, it becomes Cease cover. **So an order on an Elden Ring enemy needs a soldier that MGSV's binoculars have
  marked** (§3.4). MGSV's binoculars marked an inert proxy soldier (`p1_binoc.png`, MGSV's red triangle) [V-live M]. That run was
  under `!proxy on`, the one config in which the guest's periodic `TppMarker.DisableAll` is off. With the shipped defaults
  (`[Proxies] Enabled = false`, `[Boot] ClearMarkersSec = 5`) the guest wipes every MGSV mark every 5 s [V-code mgsv/Pump.cpp:968-975,
  Config.h:69, :304, :330], so a stand-in's mark would last at most 5 s. §3.4 holds the wipe off while a stand-in is out [U Q4].
- The target provider `[[[sys+0x1E0]+0xE8]+0x70]` is presumably the binocular target. Who writes it is [U Q4]. Live, the command's
  argument and the soldier she shot were both the binocular-marked one.
- **SendCommand ids** [V-exe, 29 cases]. Named: GetPosition 0x1241CA2D, GetStatus, **WarpToPosition** 0xF3C59B40 {position,
  rotationY}, **MoveToPosition** 0xE0DF3433 {position, rotationY, disableAim, index}, SetLife, RequestRideHeli, **EnableFire**
  0xC9771EEA, **EnableAim** 0xD95B5357, EnableDying, EnableEscape, EnableHumming, EnableAnnounceLog, SetEscapeTime, SetStrongMode,
  SetShootStance, SetDownIdle, RequestSupplyFlare.
  - WarpToPosition moves her location object through vf 0x68 (the `kLocationSet` slot) when R[0] bit 53 is set, and otherwise only
    her position cache.
  - MoveToPosition does nothing within 2.0 m of her cached place.
  - **Not in her list**: SetPosition, Warp, SetEnabled, and any motion or idle command [V-exe]. [V-live M]: `SetPosition` does
    nothing, and `CallBuddy(QUIET, ...)` on an active Quiet returns true and does nothing.
  - Unnamed: 0x94E6D8CD {targetId} writes R+0x180 directly, 0xBA46F702 {enable} toggles a costume-0 visual (vf 0xF8), and eight
    more.
- **Lua block controller** [V-exe]: `TppBuddy2BlockController.RequestMoveToPosition`, `RequestWalkToPosition` and
  `RequestMoveToSnipePoint`, with QUIET as the active type, all become message 0xF27142902A3F ("move to this point").
  `RequestMoveToPosition` with another type starts a buddy swap (GUEST.md 18.3 [V-doc]).
- **iDroid** [V-data TppTerminal.lua:9; V-live M]: BUDDY SUPPORT has QUIET SCOUT ("recon an outpost"), QUIET ATTACK ("sniper support
  at a point": MGSV's map with its predefined sniping points) and DISMISS QUIET. ATTACK sent her 135 m to a radio tower 22.2 m up
  in ~9 s, and then 102 m to a hangar roof in ~21 s.

### 2.5 Her place, movement and placement

- [V-live M] **Called beside Snake, she kneels on one knee with her rifle up 3-6 m from him (median 3.6 m) and never moves by
  herself.** Her MGSV place stayed the same to the centimetre for 4.4 min while Snake stood, walked and ran with 9 treadmill jumps.
  With the treadmill off and Snake 131 m away for ~2.5 min, she stayed put. She relocates only on MGSV's orders.
- [V-live M] Her own relocation (iDroid ATTACK): a 22 m drop from the tower in ~0.9 s, then bursts of **20-33 m/s for 0.3-0.5 s
  with 0.2-0.4 s slow-downs**, then a 9 m climb.
- [V-live M] **Placement that works today**: `TppBuddy2BlockController.ReserveCallBuddy(QUIET, BuddyInitStatus.NORMAL, Vector3(x,
  y, z), 0)` + `Reload()` puts her at the asked point (0.1 m off) **within 1.5 s** (one observation, `k5_normal_back.png`),
  crouched and aiming. She is gone for ~1 s (a block reload). The "1.0 s" elsewhere in the study is the borrow's first deploy by
  block load (07:44:22.193 -> 07:44:23.194), not a timed Reload. A Reload **re-creates** her, so any `EnableFire` / `EnableAim` /
  `EnableDying` sent before it most likely went to the old instance [U Q18: ten timed Reloads, R+0x198 read before and after].
- [V-live M] RIDE (`BuddyInitStatus.RIDE`, with or without `HeliSpaceSetting`) puts her at a fixed spot of the Africa map
  (-25.43, -0.12, -53.29), whatever point was asked, lying prone with her rifle. It is useless here.
- [V-exe, U live Q1] Her location object is reached as `this->vf48(index)` -> `+0x10` -> vf 0x388 (position +0x20, rotation +0x60).
  WarpToPosition uses its vf 0x68. Whether it is the location class (vtable 0x1424C8EF0) that `buddyloc::Finder` finds, and whether
  her AI tolerates being moved (moving D-Dog's object ended MGSV's Wait within 3 s [V-doc ddog.md §9]), is **Q1**.
- [U Q2] What `WarpToPosition` and `MoveToPosition` do live: instant or not, a walk or a cloaked dash, and how long she stays.

### 2.6 Her shot: a damage message, not a bullet

- [V-live M] Fire on an inert proxy soldier (0x450): the puppet was knocked flat in a dust burst ~4.25 s after the click and got up
  again (it is damage-immune). With `!dog trace on` + `!dog watch 0x450` (the message box send detour), the one new message at the
  shot was: `message 0x5cfee2e03274 (u16 +8 0x124) sent to 0x450 from 0x140d04df4 -> result 0`. 0x124 = 292 = `ATK_Quiet_sr_030`.
  There were **0 `shots:` lines in the session and no `kGEvBuddyFire`**.
- [V-exe, read for this document] The sender is **0x140D04D50** (0xA9 bytes; the return address 0x140D04DF4 is its last call).
  Arguments: rcx = her action component (`[rcx+0x70]` = the system), edx = her index, r8 = the message box, r9w = the target, the
  5th = the attack id. It:
  - returns at once when the target is 0xFFFF, and **sends only when `(target & 0xFE00) == 0x400`** (a soldier: type 2);
  - builds the message: `0x5CFEE2E03274`, +8 u16 = the attack id (its 5th argument), +0xA u16 = `0x2400 | index` (the attacker),
    +0xC u8 = bit 60 of R[0] (read through the two loads, 0x140D04DC4-0x140D04DD3) [U meaning];
  - sends it through the message box's vf 0x38 to the target.

  Its only caller is **0x140D0663C**, in 0x140D06310, her shot action. That action takes the target from **R+0x186**
  (`movzx ebx, word [rdi+0x186]` at 0x140D0655D) and the attack id from her weapon component (`[obj->vf 0x150] + 0x74`). It sends
  at a timed moment of the action; no hit roll was seen on the part of the path read here [V-exe; U whether she can miss: Q5].
- So **her round is a hitscan damage message to her aim target's soldier**, and it is sent only to soldiers. One hook (0x140D04D50,
  called for the original) sees every rifle hit with its target and attack id. The same message id also appears in 0x140D039B0
  (in Quiet's code, presumably her dash attack, ATK 141/142 [U]) and in three non-Quiet functions, senders or receivers
  (0x140EDAA80, 0x141360F90, 0x14146DC70).
- **Rifles** [V-data EquipIdTable / EquipParameters; V-live M rows read natively from `kDamageParams` 0x142A1A130, row = +8 + id ×
  0x1A]:

  | rifle | equip | attack id | lethal / non-lethal | share of a 2600-life soldier |
  |---|---|---|---|---|
  | **Wicked Butterfly** (Renov-ICKX) | sr_010 (59) | 290 | 2500 / 0 | 0.96 lethal |
  | **Guilty Butterfly** (tranquilizer) | sr_020 (60) | 291 | 0 / 1200 | sleep only |
  | **Sinful Butterfly** (Brennan LRS-46, anti-materiel) | sr_030 (61) | 292 | 3800 / 0 | 1.46 lethal |
  | (her dash attacks) | | 141 / 142 | 65535 / 0, 0 / 65535 | |

  `buddyQuietEquipType` 4 = Sinful: the player's save has 4, and her hit carried 292 [V-live M]. Values 0-3 are presumably Wicked,
  Wicked + suppressor, Guilty, Guilty + suppressor [U Q11]. The save offers all three rifles in CHANGE EQUIPMENT [V-live M frame].
- [V-live M] **No MGSV alert**: phase 0 and 0 `alert:` lines through three hits on a proxy, Cover me and Cease cover.

### 2.7 Her look: skeleton, draws, culling, cloak

- [V-live M] **Skin: 136 bones** (`skeleton: buddy skin learned ... (136 bones) at 821.59,-11.90,1176.27, 3.00 m from the hint`),
  exactly at her GetPosition. `!buddy` gave `yaw 1.6` (rad, her real facing). The file header also says 136 [V-data], as it matched
  the dog (94) and the horse (69).
- [V-live M] At rest MGSV draws her as **30 skinned draws in the G-buffer pass** (the capture owns these: 27-47 buddy draws a frame
  with her rifle, 31 at rest) **plus ~63 skinned draws in the forward pass** `0x012b210000fe3ac4`, which is not captured (for Snake
  either). How she looks in Elden Ring without them is [U Q9].
- [V-live M] **MGSV culls her with its own camera**: with her beside or behind MGSV's camera the capture gets 1 draw instead of 31.
  Anything Elden Ring should show of her must be inside MGSV's view.
- [V-live M] **The cloak draws nothing**: when a dash starts, her 32 G-buffer draws and her ~63 forward draws vanish in the same
  frame, and no other pass gains a skinned draw. Only MGSV's dust effects remain. Not every fast stretch is cloaked: one burst at
  0.27-0.6 m a frame kept her 27-35 draws.
- [V-live M] The rifle is drawn. MGSV draws a cyan laser from her rifle to a marked target. Whether the capture takes the laser is
  [U Q9].

### 2.8 Her idles, and a seated pose

- [V-data] 99 of her 143 motion names were resolved from the hash dictionary. Naming: `qui0non_<stance>_<action>`, with s =
  standing, q = crouched, p = prone.
  - Her kneeling standby beside Snake: live she **kneels on one knee with her rifle up** (`q2_look.png`, `a_look_after.png`)
    [V-live M]. **Which motion that is was never identified** [U Q2]: `qui0non_q_idl_l` (in `BuddyQuiet2_add.mtar`; whether that
    pack is loaded with her is also [U]) or a ready / sniping crouch from `BuddyQuiet2_layers.mtar` (`qui0non_q_rdy_idl`,
    `q_snr_idl` [V-data mgsv-re layers.txt]). A kneel with the rifle up reads more like the latter. **v1's grace pose is "her
    standby kneel"**, and it counts as a rest only if Q2 finds a way to lower the rifle (§3.9).
  - `qui0uth_q_wait_a..j`, `q_wait_idl`: her **seated waits in the helicopter cabin**, with faces `qui0face_heli_in_cross_leg_l/r`
    (cross-legged). They are **not in her buddy packs**, and no command plays them. **v2's pose**, if Q3 finds a way.
  - **Never**: `qui0non_p_rain` / `q_rain`, the Mother Base `mb_qui2_*` set (cell, bed, shower; another class), or the hurt "down"
    idles (`p_dam_slep`, `dwn_idl_*`, `SetDownIdle`).
- [V-exe] Her SendCommand has no motion or idle command. The closest are `EnableAim {enable=false}` (stop aiming: presumably the
  rifle lowered [U Q2]), `SetShootStance`, and `MoveToPosition {disableAim=true}`.
- [V-live M] Nothing tried made her sit. The talk command (7) turns her to Snake, still kneeling, with her rifle lowered (the only
  time it was seen lowered), and turns MGSV's camera, so it is barred; RIDE is a prone snipe at a fixed spot.

### 2.9 Elden Ring facts for the grace and for posts

**The rest** [V-live E: six rests at Rampart Tower (indoor), Gatefront (outdoor, by walls) and The First Step (open hilltop)]:

| when (after Snake's Y) | what |
|---|---|
| +0 ms | `ER interaction (ActionButtonParam 6101)` and `ER: rested at a grace ... -> kHEvRested 0`, in the same millisecond. The prompt (radius 1.5 m, any facing) shows within 1.15-1.42 m of the grace |
| +0.24 to +1.0 s | fade-out (mean frame luminance 72 -> 0, `restlum_firststep`: 2.3 at +0.95 s, 0.0 from +1.05 s) |
| **+1.05 to +2.49 s** | **fully black** (0.0 in every ~0.1 s frame) |
| +2.37 to +2.45 s (5 rests); **+2.93 s** (rest #2) | `ER HUD hidden (UI state 7)`. Rest #2 (07:07:59.406 -> 07:08:02.340) was a re-rest at Rampart Tower without leaving the grace |
| +2.59 to +3.0 s | fade-in (1.9 at +2.59, 9.4 at +2.70, 41.5 at +2.91, 66.5 at +3.01); the grace menu from +3.0 s, covering the left ~31 % of the screen |
| B | `ER interaction over` 0.33-0.35 s later, **no fade**; Snake has the pad |
| walk 2 m | `ER: left the grace (2.0 m from where he rested)` (`kGraceRadius`, er/Host.cpp:79) |

**How firm this timeline is**: the black window comes from **one** luminance series at one grace (The First Step); the HUD times
from six rests, one of them 0.5 s later than the rest. Whether the black moves with it (rest #2 had no luminance series), what
Pass time and a re-rest do, and how much it varies, are [U U12]. **A rest that turns into a talk has no black at all**: at
Castleward Tunnel Melina came after "the interaction (3.5 s, HUD still up)" [V-doc HOST.md:212-213], while `kHEvRested 0` had
already fired on the press [V-code er/Host.cpp:261-263], and `ER dialog ... 6001` starts only after that. So no fixed timer after
the press can tell a black screen from a talk; the design gates on observed black (§3.9).

- The hidden Tarnished sits where he stands (y +0.015 m) and **turns to face the grace** within ~0.8 s (to within 9.4°). The puppet
  writes nothing while Elden Ring interacts (er/Puppet.cpp:1135).
- **What is drawn**: Snake, standing in MGSV's idle; the buddy where it was. **The camera stays MGSV's**, frozen where it was at the
  press: with the real Snake, 5.1 m behind him, 1.55 m up, level. Elden Ring's own grace camera is never seen [V-live E; V-code
  er/CameraDriver.cpp:66].
- **The grace's position** (three sources that agreed exactly at all three graces):
  1. The asset `AEG099_060_9NNN`: `geom all <r> AEG099_06` finds it. Scanning 16,791 assets took about one frame (round trip 25 ms
     against 15 ms for a plain command) [V coarse].
  2. `BonfireWarpParam` (422 rows, 0xEC each) `pos` + `[player+0x80]`, when the row's block equals the block id at `PlayerIns+0x6D0`.
     Rows 100005 Rampart Tower, 61423700 Gatefront, 61423601 The First Step. In Stormveil 7 of 9 rows land within 0.15 m of their
     asset; rows 100000 and 100007 land 2.4 m and 1.6 m off (the warp point).
  3. Not measured, but implied by the turn: the player's spot + ~1.25 m along his facing 0.8 s after the press.

  `g_gracePos` is the player's spot, not the grace.
- **Each open-world grace checked hides a team-0 character** (`npc 10000000`, 1939 HP) 0.5 m under the grace. The pick rules
  already never take it, nor White Mask Varré 7.8 m from The First Step's grace [V-live E `dog pick`].
- **Seat survey** [V-live E]: 64 candidates (rings of 1.5 / 2.0 / 2.5 / 3.0 m × 16 directions) with ~17 rays each:

  | grace | passed | chosen seat | from Snake | screen x |
  |---|---|---|---|---|
  | Rampart Tower | 42 / 64 | 2.0 m @68° | 2.49 m | 0.63 |
  | Gatefront | 38 / 64 | 2.0 m @68° | 2.49 m | 0.63 |
  | The First Step (real camera) | 35 / 64 | 2.0 m @45° | 2.61 m | 0.62 |

  The survey found two gaps in the rule. Gatefront's 0.3-0.5 m stone border escapes knee rays at 0.5 m, so a second ring at
  0.25 m is needed. Indoors the ray filter hits loose planks: use the ground filter 0x2000016 for the floor.
- **Posts** [V-live E]: 120 candidates per place (rings 20 / 30 / 40 / 50 / 60 m × 24 directions), 9-11 rays each, judged by the
  **ER study's** rule (elevated 2-25 m, one down-ray).
  - Gatefront: 5 passed. The best was 40 m @165°, +8.4 m, seeing Snake and 2 of 7 enemies. Without the 25 m height cap it was a
    wall top 59.6 m up.
  - The First Step: 3 passed. The best was a chapel cornice 17.8 m up, which a single down-ray accepts; a 4-ray footprint rejects
    it.
  - **The design's rule (§3.2) was never evaluated** [U U13]: other rings (25 / 35 / 45 / 55 m), other heights (−3..+25 m), a
    4-ray footprint that was never cast. Recounting the recorded spots with the design's height, distance, slope, headroom,
    sight and enemy-clearance rules (the study's rings, no footprint) gives Gatefront 15 (20 m: 6, 30: 4, 40: 3, 50: 2) and The
    First Step 16 (20 m: 7, 30: 6, 40: 2, 50: 1); about 40 % of them are on the 20 m ring the design doesn't use, and the
    footprint will remove some of the rest. So the design's pass rate is unknown.
  - Elden Ring enemies stand far above Snake: at Gatefront 3 of 7 were at y 40.5-43.0 with Snake at 6.6, 34-36 m above him
    (`perch_gatefront.json`). That matters for the stand-in's height (§3.4).
  - The cost of one ray is not measured [U U1].
- **A far buddy today** [V-live E, `!dog move`]: at 40 m uphill the 8 m ground ray started inside the hill and hit a buried face. At
  40-60 m the buddy was drawn 7-12 m under the surface, or 8.6 m above the ruins downhill. The ±1.5 m lift clamp buried a correct
  +2.35 m placement. The step guard (dog only) froze each 20 m move for 2 s, then popped it in view.
- **Her shots through today's path** [V-code er/Combat.cpp:821-938]: `kGEvBuddyFire` traces from the MGSV muzzle mapped into Elden
  Ring along MGSV's direction (wrong for a far Quiet), has no boss check, ignores the guest's damage share for buddy rounds, and
  `HitCharacter` doesn't check hostility (:984-1003).

### 2.10 What is still unknown -- the probes for the build round

Settled live in the study: the player's save state, the borrow on a "gone" save (LOST is the gate), GetPosition, 136 bones, her
heading from the skin, her kneel (rifle up) and no self-relocation, the cloak (no draws), MGSV's culling, the 4 m prop problem,
the menu's need for a marked enemy (under `!proxy on`), the command arguments, the hit message and its sender, the damage rows, no
alert, the Follow.cpp bug, ReserveCallBuddy NORMAL (once), RIDE (useless), the rest's HUD times (six rests) and camera, one
luminance series, the grace's position, the ER study's seat and post rules on three graces, and the far-buddy failures. **Not
settled**, though the draft leaned on them: the black window's spread (U12), her motion and the rifle (Q2), the design's post rule
(U13), the stand-in's mark under the shipped config (Q4), MGSV's own line of fire (Q5), the borrow on other saves and across
reloads (Q14), Reload's timing (Q18).

**What round M1 settled** (MGSV agent, 2026-11-04 09:38-, real MGSV 1.0.15.4 + the host stand-in (`fake_er --actors near`, a
renamed wrapper) on `/dev/shm/phantomring_g`, the shipped `[Proxies] Enabled = false` / `[Boot] ClearMarkersSec = 5`, the player's
"gone" save borrowed in memory by `!quiet borrow`; backup `backups/20261104-093820`; the observe-only build of `mgsv/Quiet.cpp`:
the handler / hit / "can't" detours, the location finder, `!quiet hold|shift|where|r|trace`. Facts marked **[V-live M1]**):
- **Q1 -- her location object: branch M (the mirror) is possible.** `!quiet scan`: `quiet: location object 0x41323e70 stands on it
  (30 scanned) -- confirming` -> `confirmed (31 checks on it)` 0.5 s later, its feet exactly at GetPosition and the skin
  (821.59,-11.90,1176.26) [V-live M1]. `!quiet shift 3 0`: the object moved in the frame it was set (trace frame 6461), GetPosition
  and the skin followed **2 frames later** (6463: the frame the next draws show, as Snake's own placement), and **she stayed there
  60 s** (four reads 15 s apart, unchanged to the cm), kneeling, no walk back [V-live M1]. Held at 30 m (`!quiet hold rel 30 0`)
  through a 20 s run with **10 treadmill jumps** (her object moved by each jump at the next frame, `quiet::Shift`): her feet stood on
  the held point in every one of 2949 frames (offset 0.000 m); **her draws in odometer space (what Elden Ring sees: the draw lists'
  anchor + her draws' mean - k) never stepped more than 0.5 m between consecutive lists: 0 of 2829 steps, median 0.000 m** (fake_er
  `--trace-draws`, `drawsteps.py`) [V-live M1]. The rotation passed to her object (vf 0x68) and WarpToPosition's / MoveToPosition's
  `rotationY` do **not** turn her: her AI keeps its own facing (skin yaw 1.6 deg before and after a -90 deg placement) [V-live M1].
- **The placement method: `object`** (her location object's setter, one frame; `SitPlacement = object`). `WarpToPosition {position,
  rotationY}` is one frame too (R[0] bit 53 is set on her: `0x00223010000011a4`, so it goes through the same vf 0x68; GetPosition at
  the target 0.5 s later, y snapped to the ground) and is the fallback (`warp`). **Reload is not needed** (Q18 not run: with a
  one-frame method the grace has its method; `reload` stays refused).
- **Q2 -- the rifle: something lowers it. `SitPose = kneel` (v1) works, v1b is not needed** [V-live M1]:
  - `EnableAim {enable=false}` alone: R+0x198 = 0x20 (bit 5 set) but she kneels on **aiming** (`q2_aimoff`); `SetShootStance
    {stance=0/1/2}`: the same kneel, aiming; `stance=3`: **prone** with the rifle (R+0x1E4 = 3; set back to 2); the talk (7) lowers it
    for ~1 s, then she aims again (`q2_talk2`; it also turns MGSV's camera: barred).
  - **`MoveToPosition {position = a point >= 2 m away (closer is ignored), disableAim = true}`**: she goes there and then **crouches
    with the rifle held upright, not aiming** -- 10 of 10 frames 2 s apart over 20 s (`q2_rest_*`, the exit line's ">= 9 of 10"). Moved
    afterwards by her location object she keeps that pose (`q2_move_side`).
  - **Instantly**: MoveToPosition to T (2.5 m off) and her object placed at T in the next tick: the rifle is down **~0.4 s after the
    placement** (her rifle meshes' offset from her body, per draw list: 0.54 m aiming -> 0.18 m upright; +401 ms and +416 ms in two
    runs), with a short arrival (a dust puff, MGSV's effect: not captured). MGSV announces "Quiet: On the move" / "Moved to new
    position" (`EnableAnnounceLog {enable=false}` first: R+0x198 bit 9). So the grace's sit is: in the black, `EnableAnnounceLog
    false` + `MoveToPosition (seat, disableAim = true)` while she is still >= 2 m from it (from her post; if nearer, first placed 2.5 m
    off) and her object at the seat the same tick; the rifle is down ~0.4 s later -> **the method's placement time is ~0.5 s**, so
    the seat deadline = U12's shortest black - 0.5 s - 0.2 s (about 0.75 s after black start with the study's 1.45 s black).
  - Getting up: `EnableAim {enable=true}` alone does **not** raise it (she stayed resting); she aims again at her next decision (a
    new MoveToPosition with `disableAim = false`, or an order). The guest's get-up sends both.
  - Her facing at the seat is her AI's (she rests facing where she turned on arrival), not the seat's yaw: [U] a way to turn her
    (the host's `graceSeatYaw` is kept; with `kneel` the rifle is down, so the facing no longer matters for safety).
- **Q7 -- the mirror region (`MirrorRegion`)**: she was held at 15 / 30 / 45 / 60 m from the sandbox centre (820.60, -11.95,
  1179.10) on 8 bearings, then 35-55 m on 337.5 / 0 / 315 / 22.5 deg, MGSV's camera turned onto her: **MGSV kept her where she was put
  at all 48 points** (GetPosition 0.00-0.22 m off after 5 s, y -12.70 .. -11.61: the apron's own ground) and drew her (30-55 of her
  draws a list) everywhere except toward the terminal: **0 draws at 0 deg (north, +z) from 50 m, at 315 deg from 45 m** (behind the
  terminal's sandbag line: MGSV's occlusion) [V-live M1, `q7.jsonl`, `q7b.jsonl`, frames `q7_*`]. Bearing = atan2(dx, dz).
  **`MirrorRegion = 45,60,60,60,60,60,60,40`** (radii in m at 0, 45, ..., 315 deg; 337.5 deg (42.5) and 22.5 deg (52.5) are inside what
  was measured clear: 55 m at both). Posts farther out are pulled in along MGSV's camera line (§3.2).
- **Q8 -- a high post: `MirrorHeight = ground`.** Warped 8 m above the apron (30 m out) she fell to the ground within ~1 s (GetPosition
  y -5.12 at 0.2 s, -11.90 at 1.2 s): MGSV holds her on its ground only [V-live M1]. Her body 30 m out on the apron is drawn with MGSV's
  camera pitched 15 deg up (30 draws) and not at 25 deg up (0) -- MGSV's vertical FOV reads 35.6 deg in GuestState (`pr status`: half
  17.8 deg, she sits ~3.6 deg below the horizon) [V-live M1]. So **the host's drawable rule stands as designed**: `|dh| <= d *
  tan(fovY / 2) - 2 m` with fovY the shared camera's (35.6 deg in this session: 9.2 m at 35 m, 15.3 m at 55 m).
- **Q10 -- her heading**: the automatic hint now runs for Quiet (Pump.cpp: she answers GetPosition, so it hints at her GetPosition
  within 3 m when her skin isn't known): `skeleton: buddy skin learned ... (136 bones) at 821.60,-11.90,1176.27, 0.02 m from the
  hint` 0.4 s after `active TppBuddyQuiet2 9216` [V-live M1]. Her skin yaw reads in **degrees** small values (1.6 deg facing +z; the
  study's "yaw 1.6 (rad)" was this 1.6 deg); after MoveToPosition it reads -131 / 117 deg: it is her real facing.
- **M0 live**: Quiet's Fire (1, taken over) and talk (7, taken over) from MGSV's call menu -> `follow: the player's buddy command 1
  (arg 1104) -- Quiet's` / `... command 7 (arg 34820) -- Quiet's` (no "auto-follow paused"); then `!buddy walker`: `buddy walker:
  ready (on), whistles 0 ... 34 steered, 6 moved by the treadmill` and `walker: lead #1: 8.8 s led` after a run -- **D-Walker leads
  without a whistle** [V-live M3, 10:52-10:53].
- **Q4 -- the stand-in and MGSV's call menu (the big M1 finding): MGSV's Fire / Take aim are live only while Quiet is in her own
  sniping state at one of MGSV's snipe points with the stand-in as her target** [V-live M1, M3]:
  - The binoculars mark the stand-in under the shipped config (`[Proxies] Enabled = false`, `[Boot] ClearMarkersSec = 5`, logged at
    boot), MGSV's red triangle over him; the talk slot turns into **Cover me** (live) -- but with Quiet kneeling beside Snake (her
    standby, also at the mirror) **Fire and Take aim stay greyed** (frames `q4hold_*`, `q4b_*`, `q4c_*`, `q4d_*`): the menu's entries
    live in the buddy system (0x142C29030 = `[[0x140BFF050()+0x98]+0xE8]`: entry ids +0x78..+0x84 (1 Fire kill, 3 Take aim, 4 Cover
    me), their live bytes +0x170..+0x173, the menu's target +0x70); her sniping action state (0x140D03060) sets the Fire byte only when
    she holds a target (R+0x186 / R+0x184, her target lookup's vf 0x28) and R+0x1B8 > 0.2 s and not R[0] bit 51 [V-exe].
  - With her on the hangar roof (MGSV's snipe point 76, 730.72, -2.63, 1127.62; she went there on her own after a fresh deploy with
    the stand-in present, or after `TppBuddy2BlockController.RequestMoveToPosition(QUIET, ...)`), **Fire was live** (`q4f_zoom`,
    `s2b_zoom`; the buddy system's +0x170 = 01 01) and **every order the job ran through MGSV's menu hit**: 4 of 4 before M3 (Sinful
    292 x2, Wicked 290, Guilty 291), **3.1 s** from the order to `quiet: her hit: attack 29x ... on 0x450` each time, then Hit / Released.
  - From her standby a Fire sent natively (`!quiet cmd 1 0x450`, her R+0x182 / R+0x186 written as the job's safeguard) does nothing
    (`quiet: her "can't" path ran`, or the order dropped); forcing the menu's live byte (`!quiet menufire on`) makes MGSV *offer* Fire
    but she still can't take the shot. Holding her at the mirror while she wants a snipe point fails (she runs back: 40 placements
    of 0.5-1.5 m each, then she was back on the roof) [V-live M1].
  - MGSV's snipe points are the iDroid's table at 0x142A713E0 (`+0x10 + i*0x30`: position +0x10, rotation +0x20; 76 the hangar roof,
    75 the radio tower). Moving entry 76 next to Snake did not make her snipe there (she picked the tower), nor did rewriting R+0x10
    (her snipe destination) [V-live M1]. **Her own snipe-point choice is not under the guest's control in this build.**
  - **The decision (branch for orders, M2): her post is the mirror while calm; for an order she snipes from MGSV's snipe point.** The
    guest dedicates the stand-in when the binoculars are up on the host's pick (MarkProxy), asks her once to take up a sniping
    position (`RequestMoveToPosition(QUIET, Snake)` when Fire isn't live 1.5 s after the stand-in is ready), lets the holder go meanwhile,
    and sets **`kBuddyCloaked`** while she is away from her post for it (she dashes cloaked and snipes from ~100 m in MGSV: Elden Ring
    hides her, as MGSV's Quiet is unseen while she snipes); **`kBuddyQuietMarked` is MGSV's own verdict**: the buddy system's Fire byte
    live while her stand-in is ready. Her hit is reported as before (kGEvQuietShot, v0 = her MGSV snipe point: the host fires from its
    post with Elden Ring's line). When the stand-in goes (KeepMarkMs after the last binocular look, or after the order), the holder puts
    her back at the post (hidden, out of view or behind the host's fade).
  - Latency: she needs her dash to the snipe point (~10-20 s from beside Snake, live) before Fire is live: the host's LB hint
    ("mark an enemy with your binoculars to order Fire") stays until `kBuddyQuietMarked` comes. [U] a way to make her snipe from the
    mirror (her snipe-point choice: P-new).
- **Q5 -- the hit hook**: `quiet: her hit: attack <id> on 0x450 (a soldier) (her index 0, R[0] bit 60 = 1)` once per order, 3.1 s
  after the click; the job's Hit / Released followed in the same 0.5 s; no second hit without a new order; her "can't" (0x140D01410)
  is hooked and logged (it ran only for the standby Fire above). `StandInOnBearing`: not built (Q5's building case not run): **false**.
- **Q11 -- her rifle**: `TppBuddyService.ChangeQuietWeaponType(n)` (memory; `vars.buddyQuietEquipType` = n at once, no redeploy):
  **0 -> attack 290 (Wicked), 2 -> 291 (Guilty), 4 -> 292 (Sinful)**, each from a live order's hit [V-live M1]; the menu reads "Fire
  (kill enemy)" for 0 and 4. Odd values (1, 3, 5) not checked (the suppressed variants, presumably the same ids) [U]. Guilty's row is
  0 / 1200: kGEvQuietShot's b for 291 is its **non-lethal** share (4615).
- **Q14 -- the borrow, every save state** [V-live M1-M3]:
  - **gone** (the player's save): `!quiet borrow` / the boot's borrow (BorrowListInIdroid) -> the three calls -> deployed in 1.0 s;
    **MGSV's iDroid BUDDY SUPPORT lists QUIET (SCOUT) and QUIET (ATTACK)** (frame `idroid_3`; the study's save showed none) and
    QUIET (ATTACK) deploys her (`boot: the player deployed Quiet from the iDroid (was D-Walker) -- following that choice`: her HALO
    drop from 2 km onto the sniping point); put back **while she was out** (she stays out; diff CLEAN) and **after a dismissal** (diff
    CLEAN); two sandbox loads (Africa -> Afghanistan -> Africa) with her borrowed and out: **the bits survive** (no re-borrow needed),
    but **MGSV copies them into the `*MissionStart` vars** (0x2D09 / 0x2D0A / 0x2D20), which the Lua setters don't put back: the
    put-back now restores her bits there raw by mask (`MaskedSpans`) [V-live, fixed in M2].
  - **never recruited, killed, recruited-not-deployable**, each simulated in memory on the player's save (LOST cleared and
    `gvars.str_didLostQuiet = false` for the session; `BOSS_QUIET_KILL` for killed; obtained for recruited): classified right, the
    plan (obtained + sortie / sortie only), **her block loads** (`active TppBuddyQuiet2 9216`), put back CLEAN, the synthetic state
    undone, and the raw block (via `/proc`) equal to before but for D-Walker's deploy position bytes (`initialBuddyPos/RotY`).
  - The refusal: `!quiet borrow test-unguarded` -> `quiet: borrow refused -- the save guards are incomplete (test ...)`.
  - `buddyFriendlyCount` [0..7] moved with her hits ([2], [5]): they are hers (put back raw now).
  - MGSV's 7 save files were sha256-equal to the start after every session (below).
- **Q9 -- capture**: with her layer on, skinned draws within 1.5 m of her root and rigid ones within 1.2 m are hers, any other draw
  within 4 m of her is dropped (`KeepBuddyDraw`); at 15-60 m she has 30-55 draws a list (Q7), 31 kneeling beside Snake.
- **Q12 -- the bond gate**: `IsEnableBuddyCommand(QUIET_AIM_TARGET / COMBAT_START)` true / true with `GetFriendlyPoint` 0-1: Take aim
  and Cover me exist on this save (as the study saw).
- **Q15 / Q16 / Q17**: no dash attack (141 / 142) on a stand-in was seen in any session; 0 `alert:` warnings in all sessions (the
  phase lock never had to calm); the tan figure at the terminal was never her target (her hits: only 0x450, the stand-in).
- **Q3 (offline)** not done this round (no seated motion; v1's kneel with the rifle lowered is the rest pose).
- Incidental: the tan figure of Q17 stands at the terminal's entrance (~45 m north of the centre; frames `q2_move_side`,
  `q2_move_after5`); her live weapon read (`[[sys+0x70]+0x30]`) gave 16456 (0x4048): that offset is not the weapon type on this
  build [U]; the weapon comes from `vars.buddyQuietEquipType` (4) and each hit's attack id.

**Rounds M2 / M3 (MGSV agent, 2026-11-04, five MGSV sessions 09:38-11:22 with the host stand-in; backups `20261104-093820` ..
`-111855`; MGSV's 7 save files sha256-equal to the start before the final restore and after it)** -- what is built and what held:
- **Built** (`mgsv/Quiet.{h,cpp}`, `mgsv/QuietLogic.{h,cpp}`, `tests/test_guest_quiet.cpp`; shared: Follow.cpp, Frame.cpp, Pump.cpp,
  Capture.cpp, Hud.*, ActorProxy*, Addresses.h, Config.*, the ini): the borrow (by bits, snapshot, the plan, re-applied after loads,
  the put-back through the setters + her counters raw + her bits raw by mask, the raw diff; refused unguarded or for a sick Quiet);
  the location finder and the holder (post = the mirror pulled into `MirrorRegion`, on MGSV's ground; the treadmill shift; bursts of
  placements, none while she drops in or dashes; `kBuddyAtPost` + `buddyPostSeq` echoed only when measured there); the grace sit
  (§2.10 Q2's method: her own MoveToPosition with aim off, then her object at the seat; `kBuddySeated` + `graceSeqSeated`; up = her
  order with aim to the post); the order job (Pick / Realize / Ordered / Hit / Released / Refused / Lost with reasons; the hit hook;
  R+0x186 / R+0x182 written as the safeguard; kGEvQuietShot with MGSV's share, non-lethal for 291); MarkProxy (the stand-in at the
  enemy's height while the binoculars are up, KeepMarkMs), KeepMarks (the wipe held while he is out), `kBuddyQuietMarked` = MGSV's
  Fire live, the sniper move asked once per stand-in, `kBuddyCloaked` while she is away for it; FireOnlyOnOrders (her fire on only
  while a stand-in of hers is out); commands 4 and 7 taken over with the layer; NoDying; QUIET SCOUT / ATTACK greyed with the layer;
  Capture's rule (skinned within 1.5 m of her root, rigid within RigidRadius, other draws within 4 m dropped); the per-buddy phase
  guard (re-armed on each change of the active buddy, a give-up turns off the active buddy's orders: `quiet::DisableOrders` or
  `dog::DisableAttack`; a NONE between two of the same buddy keeps the soldier); Hud's own marker flag for her; the sites of §2.11
  byte-checked at Install and by `verify_addresses.py` (all ok on 1.0.15.4); `!quiet ...` console.
- **Defaults** (the player's answers + M1): `Borrow = always`, `BorrowListInIdroid = true`, `SitPose = kneel`, `SitPlacement =
  object`, `MirrorRegion = 45,60,60,60,60,60,60,40`, `Cover = false`, `StandInOnBearing = false`, everything else on. All but the
  borrow act only while the host sets `kHostQuietLayer` (the host's `[Quiet]` ships off).
- **M3 live, what held**: the boot borrow (`quiet: borrowed for this session ... at the sandbox's start`), MGSV's BUDDY SUPPORT listing
  her and deploying her (her HALO drop, adopted by the guest); posts: `!buddy quiet` -> held at post #1 in one placement (11.6 m),
  every new post placed at once, from the roof 103-117 m away too; the rest: 4 rests (two after a host restart) seated in the frame
  the seat came (`seated at the grace seat (rest N)` 17 ms after the sit's order), rifle upright from ~0.4 s (frames `sit2_*`,
  `rest_rec2`), up and back to the post on `grace 0`; **M0** (below); D-Dog (`!buddy dog`, attack order 11 -> Ordered -> Connected ->
  Released), D-Walker (follows and leads without a whistle after Quiet's commands 1 and 7) and D-Horse (deployed, its location
  object confirmed, whistled after a run) unchanged; **0 `alert:` lines and 0 warnings** in all five sessions.
- **M3 live, what did NOT hold -- the Orders exit line**: in the 10-minute run (`m3.py`: 10 cycles of a post, a run, the binoculars
  on the pick, Fire) she went to the hangar roof in cycles 1-4 (Fire live there, `kBuddyQuietMarked` came 17-21 s after the look) but
  the menu clicks became Cover me (taken over: "not in Elden Ring yet"), and in cycles 5-10 the sniper move left her near Snake and
  Fire never came live: **0 hits of 4 clicks in that run**. Outside it **every order that reached the job hit**: 6 of 6 over the
  sessions (292 x4, 290, 291), 3.1 s each, including the recorded one. So the order path works once MGSV holds her in its sniping
  state with Fire live; getting her there is the open problem (MGSV's own snipe-point choice, latency 10-20 s, not reliable).
  **Orders stay off in the integration until that is solved** (the host's `[Quiet] enabled` decides; the guest's switches are inert
  without it). Next: RE of her snipe-point choice (TppSnipePoint2 / the 0x142A713E0 table's consumer) so she snipes from the mirror.
- **Her drawn place**: treadmill jumps never moved her draws more than 0.5 m in odometer space (Q1: 0 of 2829 steps); a post change
  and the sit / get-up placements are single-frame jumps by design (the host fades a post change in view, the sit happens in its
  black); while she relocates for an order she is `kBuddyCloaked` (MGSV draws her ~100 m away on the roof; the host hides her).
- **Recordings** (`recordings/`, git-ignored): `quiet_orders.prrec` (80 s: D-Walker -> Quiet deployed, post, a run, the binoculars,
  her cloaked move to the roof, one Fire: BuddyOrder 1, kGEvQuietShot Ordered / Hit 292 / Released), `quiet_rest.prrec` (55 s: post,
  a run, a rest with the seat (`kBuddySeated`, rifle down), the walk-away back to the post).

**MGSV probes** (round M1; `pr lua` = the guest's Lua console; read-only or memory-only, saves guarded):

| # | question | exact probe | decides |
|---|---|---|---|
| Q1 | her location object; does she stay where it is put | build: point `buddyloc::Finder` at her skin (`!quiet scan`): vtable 0x1424C8EF0 objects whose feet are within 0.75 m of her skin root, confirmed by 10 checks over 0.5 s. Then `!quiet shift 3 0`: do GetPosition and the skin follow in the same frame, does she stay kneeling 60 s, does she walk back? Then hold her at a point 30 m away and run 20 treadmill jumps (`!quiet shift` each jump): her place, her draws and her AI after each | branch M or N (§3.2); the grace placement method |
| Q2 | WarpToPosition, MoveToPosition, EnableAim/Fire; **which motion she kneels in, and what lowers the rifle** | `GameObject.SendCommand(0x2400, {id='WarpToPosition', position=Vector3(...), rotationY=deg})` 3 m and 30 m away (instant? one frame?); `{id='MoveToPosition', position=..., rotationY=..., disableAim=true, index=0}` 10 m and 40 m away (a walk, or a cloaked dash; does she stay); `{id='EnableFire', enable=false}` (does she still fire on command 1). **Rifle**: frames of her at rest (MGSV's camera on her, 3 m) after each of `EnableAim false`, `MoveToPosition {disableAim=true}` to her own place + 2.5 m, and `SetShootStance` (each value the handler accepts), each judged "rifle lowered / still raised / aiming" over 10 frames 1 s apart; with nothing marked and with a marked stand-in. **Motion**: compare her skin's bone pose (the 136-bone skin, `!buddy` dump) against the candidate gani (`q_idl_l`, `q_rdy_idl`, `q_snr_idl`: the hip and knee heights and the right-hand-to-head distance), and whether `BuddyQuiet2_add.mtar` is resident | the grace pose v1 (rifle lowered, or the open question); placement fallbacks; FireOnlyOnOrders |
| Q3 | a seated motion | offline first: decompile `Player.RequestToPlayDirectMotion` 0x1409DA7D0 down to its motion-controller call, and see whether her realized object's controller can be driven the same way; find which pack holds `qui0uth_q_wait_*` (the helicopter's) and whether it can be loaded alone. Live only if a path exists: frames of the cross-legged wait beside Snake | v2 pose |
| Q4 | the menu with a stand-in, **under the shipped config** | `[Proxies] Enabled = false`, `[Boot] ClearMarkersSec = 5` (record both in the log), with the build's wipe hold (§3.4). One dedicated inert soldier 25 m ahead: binoculars on him for 1 s / 3 s (marked?), binoculars down, then LB (Fire live?) at +1, +5, +10 and **+20 s** (the mark survives `KeepMarkMs`?); the same with the hold off (expect greyed by 5 s); move him 10 m (still live?); release him (greyed again?); `kBuddyQuietMarked` against the menu each time; read `[[[sys+0x1E0]+0xE8]+0x70]` each time. **Elevated case**: an actor 20-30 m up (`pr er actor` at that height) with the stand-in at the enemy's height (`Height = enemy` for Quiet's slot): does MGSV hold the puppet there (GetPosition after 5 s), do the binoculars looking up mark him, is Fire live? **Aim pull**: Snake fires 10 rounds at a different actor with a marked stand-in out: any `kGEvFire` direction bent toward the stand-in (logged angle), with `AimHeightFix` off and on for the dedicated slot | §3.4's stand-in rule, `KeepMarkMs`, the wipe hold, the slot's height |
| Q5 | the hit hook, and MGSV's own line | detour 0x140D04D50 (call the original first): log target, attack id, attacker, the +0xC byte. Fire ×5, Take aim + Snake's shot ×3, with the soldier in the open and behind an airport building: one call per hit? any order that never sends (a miss, "Unable to fire": does `kQuietNoTarget` 0x140D01410 run)? any second hit without a new order (the soldier is immune)? Then the same behind a building with **the stand-in moved after Ordered** onto her MGSV bearing, 15-25 m from her on open apron: does the order follow him, does she hit? Does the order survive the mark being cleared after Ordered? | the job's Hit / Lost rules and reasons; `StandInOnBearing` |
| Q6 | fire only on our orders | `EnableFire false`, then Cover me for 60 s with a marked stand-in: no hit sent. Then the job's sequence (EnableFire true -> command 1 -> hit -> EnableFire false): does it hit? | `FireOnlyOnOrders` |
| Q7 | MGSV draws her at the mirror; **where the apron is free** | hold her (Q1's way) at 15 / 30 / 45 / 60 m from the sandbox centre in 8 bearings, including behind the terminal (~40 m north) and the hangars (west), at Snake's MGSV y: log `GetPosition` after 5 s (did MGSV push her out, walk her, drop her?), the y she settles at, and frames; turn MGSV's camera onto her: count her G-buffer draws (`!capture`), her LOD (vertex counts), and Fire on a stand-in 20 m from her. Record the free region as a per-bearing max radius (or a polygon) | `MirrorRegion` (replaces a 45 m circle), the open area |
| Q8 | a high post | with her body on the apron and MGSV's camera pitched 15° / 25° / 35° up at where the post would be: her draws counted (vertical culling) | `MirrorHeight` |
| Q9 | her look and her meshes | `!capture probe` at rest and on a hangar roof (iDroid ATTACK): with the new mesh rule (skinned rooted on her, rigid within 1.2 m) 0 props kept; is the laser a captured draw? Frames of her in Elden Ring (fake_er draws to a file) without the forward pass | Capture rule |
| Q10 | her heading | `!buddy hint` automatic for Quiet (Pump.cpp:512 must not wait for `!buddyPosValid`): learned within 2 s of the deploy; `buddyYaw` from the skin root vs frames | §3.8 |
| Q11 | the weapon byte; **what each rifle does to the stand-in** | iDroid > BUDDY SUPPORT > CHANGE EQUIPMENT for each rifle (GMP in memory only): `vars.buddyQuietEquipType`, the live weapon `[[sys+0x70]+0x30]`, the menu's Fire label, the attack id at the hit; and `PhantomRing.FultonSoldier(id)` (life, state) 1 s and 5 s after each rifle's hit on the stand-in (does 291 put the puppet to sleep in MGSV? does MGSV offer its Fulton on him?) | `QuietWeapon` mapping; release after a Guilty hit |
| Q12 | the bond gate | `TppBuddyService.GetFriendlyPoint(QUIET)` vs the friendly store's vf 0x440 (read natively) on the borrowed Quiet; the menu's entries | whether Take aim / Cover me exist on other saves |
| Q13 | Shoot this | a grenade in hand + LB: the entry, command 6, her shot at MGSV's grenade, and what the guest's `kGEvExplosion` and the host's own flown grenade do then | v2 |
| Q14 | the borrow's round trip | Snapshot the raw bytes `[0x142C16920]+0x2D08..+0x2E48` and the byte 0x142C2919E (`/proc` read) -> borrow -> deploy -> iDroid lists her? -> DISMISS QUIET -> deploy again from the iDroid -> switch to D-Dog and back to Quiet from the iDroid (she stays listed) -> **reload the sandbox** with her borrowed, once out and once not out, then deploy her (does the borrow survive? are the `*MissionStart` copies changed?) -> put back **while she is out** on one run (what happens to the active TppBuddyQuiet2?) and after DISMISS on the other -> raw diff: **pass = no byte of Quiet's fields or of 0x142C2919E differs from the snapshot**, and every other differing byte is logged and explained (another buddy's use) -> MGSV's 7 files sha256-equal. **Synthetic states**, each in memory on the player's save and put back after: never recruited (LOST cleared, obtained clear), killed (`SetBuddyCommonFlag(BOSS_QUIET_KILL)` as well), recruited-not-deployable (obtained set, sortie clear, LOST clear): does her block load after that state's plan? **Refusal**: offline in `test_guest_quiet` with an injected `SavesGuarded() = false`, and live with `!quiet borrow-test unguarded`, which makes only the borrow's own guard check fail while the real save guards stay on. Never run MGSV with `[Saves] Block = false` | `Borrow` per state; the restore rule |
| Q15 | her dash attack | does she ever dash-attack (ATK 141/142) a stand-in near her? (0x140D039B0) | ignore or report |
| Q16 | alerts over time | 10 min with a binocular look every 15 s and 10 orders: 0 `alert:` lines, phase 0 | exit criteria |
| Q17 | the unknown figure | the tan figure ~50 m west of the sandbox centre (frames `a_look_after.png`, `w4_follow.png`): which object (`!skins 80`, the message trace while she covers)? | safety of Cover me |
| Q18 | Reload as a placement | ten `ReserveCallBuddy NORMAL` + `Reload` to points 3-10 m away: the time from the call to `active TppBuddyQuiet2` and to her GetPosition at the point (min / median / p90); R+0x198 (corrected R) before and after one Reload with `EnableFire false` / `EnableAim false` sent before it: do the bits survive? | the grace's reload fallback (allowed or not, and its deadline); where Enable* go |

**Elden Ring probes** (round E2; from the ER study's list):

| # | question | probe |
|---|---|---|
| U1 | the cost of one `CastRay` | QPC around each batch in the post and seat searches: `quiet: post search N rays in X us`; the host task time with the search on and off over 600 frames (`pacing probe`) |
| U2 | `GeomProbe::Near` cost in a big area | QPC the grace lookup at each rest; over 2 ms -> the BonfireWarpParam route (add a `Spec`: `BONFIRE_WARP_PARAM_ST`, 0xEC, SoloParam index 43) |
| U3 | a grace on a map-tile border | compare `[player+0x6D0]` with the row's area/grid; expect a 256 m tile offset per grid step |
| U5 | moving her draws by the published root | `buddy trace on` frame by frame while she is held at a post: the drawn body's step per frame |
| U6 | a shot from a far origin | new console `quiet shoot <x y z> <target>`: `Targets()` finds a target 60 m from the post and 120 m from the player |
| U7 | characters past the nearest 65 / 128 | the pick's candidate count in Stormveil's courtyard with a post 60 m out |
| U8 | Pass time, Level up | rest, Pass time: she stays seated through the chained screens (the HUD shows ~4.7 s in Pass time) |
| U9 | a Tarnished-mode rest, a respawn | no seat (`tarnished = false`); a death's respawn (`kHEvRested 1`) never seats her |
| U10 | stairs, bridges, caves (**an exit item**, §7) | the seat survey at Stormveil Cliffside (scaffolding) and a cave grace the save has rested at without a talk; the fallback fires |
| U11 | the player's own camera angle at the rest (**an exit item**, §7) | rest after turning the camera 90° and 180°: a visible seat, or the clean "not there" fallback |
| U12 | **the rest's black, observed** | (a) find Elden Ring's fade state: `CSFade` is already resolved for both tables (er/Addresses.cpp:65 / :232, console `fade`); `peek` its plate (`CSFD4FadePlate` / `CSFadeImp` in the RTTI list) every frame of a rest and find the alpha that goes 0 -> 1 -> 0 with the luminance; (b) else the mean luminance of the host's presented frame from a small read-back (the overlay's `hud shot` path, downsampled), with its cost per frame. Log both at **10+ rests**: the three graces, a re-rest without leaving (rest #2's +2.93 s), Pass time, Level Up. Report the black's start and end after the press (min / median / max) and the HUD-hide time in each. Decides the grace's gate and the `seat_*` keys |
| U13 | the design's post rule on real geometry | `perch.py` (the ER study's script) with the design's rings (25 / 35 / 45 / 55 m), heights (−3..+25 m), the 4-ray footprint, the frustum rule (§3.2) and the one sight point, at Gatefront and The First Step: log the passes per ring, and record the raw `call rayall` hits as fixtures. Decides the rings, the cap, the zero-candidate path |
| U14 | the pick's refusals near real neutrals and a boss | a dry-run console `quiet pick` (prints every candidate and why each is refused; no order, no shot, no mark) at The First Step's grace facing White Mask Varré, the grace's hidden team-0 character, and the large `a boss` seen there by `dog pick` (from the grace, no approach; `combat set hurt_mult 0`, 1 after); also with spotting on (none of them marked). Part of the Orders exit line |

### 2.11 Sites to byte-check (MGSV 1.0.15.4)

All bytes were read from the exe on disk [V-exe]. The first block is the RE study's; those marked **(new)** were read for this
document, and those marked **(revision)** for its revision (objdump, 2026-11-04).

| proposed `Addresses.h` name | VA | first bytes |
|---|---|---|
| `kBuddyCommandQuietFire` (cases 1, 2) | 0x140A4E24A | `48 b8 3a 4e 75 0c 38 7e 00 00 c6 45 ba 01 4c 8d 4d b0 48 89 45 b0 0f b7` |
| `kBuddyCommandQuietAim` (case 3) | 0x140A4E26E | `48 b8 3a 4e 75 0c 38 7e 00 00 c6 45 ba 00 4c 8d 4d b0 48 89 45 b0 0f b7` |
| `kBuddyCommandQuietCover` (case 4) | 0x140A4E217 | `48 b8 0e 64 25 6c a3 56 00 00 c6 45 b8 01` |
| `kBuddyCommandQuietCease` (case 5) | 0x140A4E232 | `48 b8 0e 64 25 6c a3 56 00 00 c6 45 b8 00` |
| `kBuddyCommandQuietShootThis` (case 6) | 0x140A4E2A2 | `48 b8 9e 8a 1a 1e 6b e2 00 00 48 8d 55 f0` |
| `kBuddyCommandQuietTalk` (case 7) | 0x140A4E292 | `48 b8 ef fe 0d a2 65 a2 00 00 48 8d 55 e8` |
| `kQuietMessageHandler` | 0x140CE3330 (slots 0x14232CD40, 0x14232CEB0) | `40 55 56 57 41 56 41 57 48 8d 6c 24 d0 48 81 ec 30 01 00 00 48 8b 05 e5` |
| `kQuietSendCommand` | 0x140CE1E10 (slots 0x14232CD48, 0x14232CEB8) | `4c 89 4c 24 20 55 53 56 57 41 54 41 56 41 57 48 8d 6c 24 d9 48 81 ec 00` |
| `kQuietFireTarget` | 0x140CE3B3D | `49 8b 86 e0 01 00 00 48 8b 88 e8 00 00 00 0f b7 41 70 66 41 3b c2` |
| `kQuietOrderDelay` | 0x140CE3B72 | `c7 82 ac 01 00 00 66 66 e6 3f` |
| `kQuietOrderProcessor` | 0x140D04600 | `40 53 55 56 41 54 41 55 48 83 ec 20 48 8b 41 70 8b da 41 8b e8 4c 8b 88` |
| **`kQuietRecordFire`** (revision: R's second load, the Fire case) | 0x140CE3B25 | `49 8b 86 b0 00 00 00 48 8b d6 41 ba ff ff 00 00 48 c1 e2 09 48 03 50 08` (the last four: `add rdx,[rax+8]` at 0x140CE3B39) |
| **`kQuietRecordProcessor`** (revision: R's second load, the processor) | 0x140D04615 | `4c 8b 88 b0 00 00 00 4c 8b 50 10 48 c1 e3 09 49 03 59 08` (the last four: `add rbx,[r9+8]` at 0x140D04624) |
| **`kQuietRecordHitSend`** (revision: R[0] in the hit sender) | 0x140D04DC4 | `49 8b 48 70 48 8b 89 b0 00 00 00 48 8b 49 08 4c 8b 04 0a` |
| `kSetDisableBuddyStatic` (revision; the borrow's snapshot only) | 0x140A53EFF | `40 08 3d 98 52 1d 02` (`or [0x142C2919E], dil`) |
| `kQuietOrderPickTarget` | 0x140D04773 | `0f b7 83 86 01 00 00 4a 8d 0c ad 00 00 00 00 49` |
| `kQuietNoTarget` | 0x140D01410 | `48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 30 48 8b 71 70 45` |
| **`kQuietHitSend`** (new) | **0x140D04D50** | `48 83 ec 38 4d 8b d8 4c 8b c1 b9 ff ff 00 00 41 0f b7 c1 66 44 3b c9 0f` |
| `kQuietHitSoldierOnly` (new) | 0x140D04D6D | `b9 00 fe 00 00 66 44 23 c9 b9 00 04 00 00 66 44 3b c9 75 73` |
| `kQuietHitMessage` (new) | 0x140D04D84 | `48 b9 74 32 e0 e2 fe 5c 00 00` |
| **`kQuietShotAction`** (new) | **0x140D06310** | `48 89 5c 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8d 6c 24 f0 48 81 ec` |
| `kQuietShotTarget` (new) | 0x140D0655D | `0f b7 9f 86 01 00 00` (`movzx ebx, word [rdi+0x186]`) |
| `kQuietShotCall` (new) | 0x140D0663C | `e8 0f e7 ff ff` (call 0x140D04D50) |
| `kQuietWarpToPosition` | 0x140CE5C70 | `40 53 55 56 57 41 56 48 81 ec a0 00 00 00 0f 29 b4 24 90 00 00 00 48 8b` |
| `kQuietMoveToPosition` | 0x140CE1D70 | `48 8b 41 30 0f 28 e3 0f 57 c9 44 8b ca 44 8b d2 8b d2 48 c1 e2 04 49 c1` |
| `kQuietVisualToggle` (vf 0xF8, optional) | 0x140CE48C0 | `48 89 74 24 10 57 48 83 ec 20 48 8b 41 10 8b fa 41 0f b6 f0 48 8b 40 28` |
| `kVarBuddyQuietEquipType` (VarSite) | 0x14099F241 | `4c 8b 05 d8 76 27 02 49 81 c0 44 2e 00 00` |
| `kBuddyCommandGate` (Q12 only) | 0x140A52140 | `48 83 ec 28 83 ea 03 0f 84 8d 00 00 00 ff ca 74 6d 83 ea 02` |
| `kPlayerDirectMotion` (Q3 only) | 0x1409DA7D0 | `48 8b c4 55 41 54 41 55 41 56 41 57` |
| `kAtkQuietSr010..030` | ids 290-292 | read back through `TppDamage.ATK_Quiet_sr_010..030` in the boot Lua once, as for the dog's |
| shared, already committed | 0x140A4E0B0 (command fn), 0x140BFF050 (message box), 0x141AD56D0 (`kLocationSet`) | as committed |

Constants: Quiet type 0x12, id 0x2400; R = `[[sys+0xB0]+8] + i*0x200` (two loads: the pointer at `[sys+0xB0]+8` is the array's
base; checked by the three `kQuietRecord*` sites above); targets at R+0x180/0x182/0x184/0x186; disabled bits R+0x198;
active / requested / cancelled orders R+0x19C / 0x1A0 / 0x1A4; delay R+0x1AC; position cache `[[sys+0x10]+0x20] + i*0x10`. Any
mismatch turns the Quiet layer off and logs why (the `Follow.cpp` rule).

---

## 3. The design

### 3.1 Deploying her on any save, without touching MGSV's save

- **At boot** the guest reads her state, read-only, classifies it **by bits** (§2.2), and logs it: `quiet: save state gone (LOST,
  str_didLostQuiet, bond 0, deployed 46 times)`.
- **present**: nothing new. She deploys from the iDroid, `[Buddy] Type = quiet` or `!buddy quiet` as any buddy.
- **`[Quiet] Borrow`** = `off` / `memory` / `always` (**the planned shipped default**, the player's answer to open question 1,
  2026-11-04: everyone, any save; each save state on only once it passed Q14 live, a failing state off with a log line and a README
  note; round I0 ships `off` until then):
  - `memory` borrows for a Quiet the save has had: **gone**, **recruited-not-deployable**, and the sickbay / dying / dead
    sub-states.
  - `always` also borrows for **never recruited** and **killed** (`BOSS_QUIET_KILL`): a player who hasn't reached mission 11
    would meet her early (a spoiler in the iDroid), and one who killed her chose that, so it is never implied by `memory`.
- **The borrow**, **only while `SavesGuarded()`** [V-code Pump.cpp:386-389]:
  1. **Snapshot** the raw bytes `[0x142C16920]+0x2D08..+0x2E48` (every buddy var, the `*MissionStart` copies included) and the
     static byte 0x142C2919E (§2.2), and log their hash.
  2. **The plan** (pure, `QuietLogic::BorrowPlan(bits)`), by state:
     - recruited-not-deployable: `SetSortieBuddyType(QUIET)` only;
     - gone: `SetObtainedBuddyType(QUIET)`, `SetSortieBuddyType(QUIET)`, `UnsetBuddyCommonFlag(BUDDY_QUIET_LOST)` [V-live M];
     - never recruited / killed (`always`): obtained + sortie [U Q14 whether the block loads];
     - each of these, and only if that bit is set: `UnsetBuddyCommonFlag(BUDDY_QUIET_HOSPITALIZE)`, `UnsetBuddyCommonFlag(
       BUDDY_QUIET_DYING)`, `UnsetDeadBuddyType(QUIET)`, `UnsetDisableBuddyType(QUIET)`.
     Never `RequestReunionQuiet`, never a gvar, never the bond, never `BOSS_QUIET_KILL`.
  3. Then the normal `PR.Buddy` path.
  4. Log `quiet: borrowed for this session (memory only -- MGSV's save is not changed)`. Corner text: "Quiet joins for this
     session only -- your MGSV save is not changed".
  - When: with `BorrowListInIdroid = true` (**the planned shipped default**, the player's answer; I0 ships false), at the end of boot, so MGSV's own BUDDY
    SUPPORT lists her [U Q14]. Otherwise only when the ini (`[Buddy] Type = quiet`) or the console asks for her.
  - **After every sandbox load** (the guest's own `kLoadSandbox`, a death, a continue), before `PR.Buddy` and while guarded:
    the state is read again and the plan re-applied if a bit was lost, logged `quiet: re-borrowed after a load (<bits>)`.
    `SetSortieBuddy` doesn't clear the bits (§2.2), but a load restoring `vars` might [U Q14].
  - If her block doesn't load within 60 s: everything is put back, `kBuddyMissing`, and the corner says "Quiet can't join on this
    save".
- **Put back** only at guest shutdown, `!quiet borrow off` (or the ini reloaded with `Borrow = off`), and a save guard lost; **not**
  on DISMISS QUIET or another buddy deployed, so she stays in BUDDY SUPPORT and the player can switch back from the iDroid for the
  whole session. If she is out at that moment, she is dismissed first (the DISMISS path; the live round trip was only ever
  dismiss-then-restore) and the bits follow once her block is gone. Quiet's fields go back through the same Lua setters (the
  counters, timer, max point and count by `vars` writes; the static byte by `UnsetDisableBuddyType` / `SetDisableBuddyType` to
  its snapshot bit), logged as before -> after. Then the raw block is **diffed** against the snapshot, and every byte that differs
  is logged: none may be Quiet's (the other buddies' use of the session may differ, and is named). Belt and braces: MGSV never
  saves under PhantomRing, and `pr launch` backs the saves up.
- With `Borrow = off`: today's `missing: not developed in this save` (or `cannot sortie`), `kBuddyMissing`, and the host's "not
  available", with the corner naming the ini key once.
- The bond: left as the save has it. On the player's save the gated commands are enabled anyway [V-live M]. On other saves
  Take aim / Cover me may be missing [U Q12]. Fire needs no bond.
- **iDroid**: QUIET SCOUT and QUIET ATTACK are greyed out while the host's layer is on (`SetMbTopMenuItemActive(item, false)`, the
  mechanism `PR.BlockMissions` uses [V-code GuestScript.cpp:790-815]), because her post is Elden Ring's. DISMISS QUIET stays.

### 3.2 Following and perching in Elden Ring terms ("posts")

MGSV's Quiet never follows Snake on foot [V-live M]. She holds a point and moves only when told, in cloaked dashes. In Elden Ring,
**the host decides where she is**, and the guest makes MGSV agree.

**The host's post** (er/QuietLogic, a pure rule plus rays; the ER study's rule with its two fixes). There are three modes:
- **Cover** (a hostile within 60 m of Snake):
  - candidates on 24 directions × rings of 25 / 35 / 45 / 55 m round Snake, the camera's forward half first;
  - standable: a ground-filter ray (0x2000016) from 70 m above, normal `ny ≥ 0.8`, a footprint of 4 rays at ±0.5 m within 0.3 m of
    each other, 1.6 m of headroom;
  - height −3 to +25 m over Snake's feet; 3D distance ≤ 60 m;
  - **drawable** (until Q8 settles `MirrorHeight`): MGSV's and Elden Ring's cameras share one orientation, and her MGSV body
    stands on the apron. So when the camera centres the post, her mirrored ground point must still be inside the frustum:
    |Δh| ≤ d · tan(fovY / 2) − 2 m (Δh = the post's height over Snake, d = its horizontal distance; fovY Elden Ring's current
    vertical field of view). A post 25 m up at 35 m (≈ 35° up) fails it; such a post would be drawn empty;
  - no hostile within 15 m and no character of any team within 3 m;
  - **her sight point** = the post's feet + 0.9 m (her kneeling muzzle height; the shot is cast from this same point, §3.6) has a
    clear ray to Snake's chest and to at least one hostile's chest, chest = 0.7 × his height as the shot uses (with no hostile,
    to the camera's aim point);
  - score: enemies seen × 3, − |d − 35 m| × 0.05, + min(height, the drawable bound) × 0.1 (no bonus past what MGSV can draw;
    the draft's ×0.2 up to 15 m steered her to exactly the posts that fail the frustum).
- **Near** (no hostile within 60 m): rings of 8 / 12 / 16 m and height −2 to +8 m; the same standable, drawable and sight rules;
  prefer higher ground within the bound. MGSV's own Quiet kneels 3-6 m from Snake when nothing happens.
- **On the move**: Snake has moved faster than 3 m/s for 2 s. **No post: she is not drawn** (MGSV's Quiet relocates cloaked and
  unseen). When Snake slows below 1 m/s for 1.5 s, or a hostile comes within 60 m, the host picks a post.
- **No candidate passes** (a cave, a corridor, a cliff): Cover falls back to the Near rings; if none of those passes either, she
  keeps her last post while it still sees Snake; else she has no post (hidden, as on the move), and the host logs
  `quiet: no post (cover 0 / near 0 of N candidates)` once per search. Orders then end Refused with "no clear shot".
- **The pass rate is unknown** [U U13]: the draft's "5 / 3 passed" were the ER study's own rule on other rings (§2.9). E2 runs the
  design's rule on the recorded places before the rings and heights are tuned.

The search runs only when it is needed: on deploy, when she has no post, when Snake is 25 m from the search centre (cover) or
20 m from the post (near), when the post has lost its line to Snake for 2 s, when a hostile comes within 8 m of her, or when an
order's target can't be seen from her post (search biased to see him). It costs ~24 candidates × ~10 rays, spread over 8 frames
(≤ 30 rays a frame, `post_rays_per_frame`). It keeps the current post unless a candidate scores 20 % better.

**A change of post is never a visible jump**:
1. If she is drawn and in view, the host fades her out over 0.3 s (her cloak), then publishes the new post (`buddyPostSeq + 1`).
2. The guest places her and echoes the sequence.
3. The host fades her in at the new post.

Out of view the change is at once.

**The guest: the mirror (branch M, the design).** For a new post (HostState `buddyPost`, corrected for the treadmill as
`ActorRecord.pos` is, OffsetHistory [V-code mgsv/ActorProxyGame.cpp:455-470]):
- Her MGSV place = the post's MGSV point, on MGSV's ground (Snake's MGSV y), facing `buddyPostYaw`. **That the apron is flat and
  free there is [U Q7]**: the study's frames put the terminal ~40 m north, hangars to the west and containers ~50 m west inside a
  45 m disc, and no ray or height read of the apron exists. If the point is outside `MirrorRegion` (Q7's free region: a max radius
  per bearing, or a polygon, measured by holding her there and reading back where MGSV leaves her), she is pulled in along the
  line from MGSV's camera to the point until she is inside it, never nearer than 3 m to Snake. On that line she stays in the same
  direction from the camera, so MGSV's frustum still holds her.
- She is placed through her location object (Q1). The fallback is `WarpToPosition` (Q2), and then `ReserveCallBuddy NORMAL` +
  `Reload` (1.0-1.5 s [V-live M]), used only while she is hidden.
- She is shifted with every treadmill jump in the jump's frame, like `dog::Shift` (a Quiet case in Frame.cpp's treadmill branch,
  HEAD :696-699). She is placed again when her MGSV place drifts more than `PostTolerance` (3 m) from the mirror (the slip, a
  post change, MGSV moving her).
- The guest publishes `kBuddyAtPost` + `buddyPostSeq` while she is held there.

Why mirror: (a) MGSV culls her by its own camera [V-live M], and with the mirror MGSV's camera sees her exactly when Elden Ring's
camera sees her post; (b) her aim and her rifle point at the stand-in, which then stands in Elden Ring's direction from her;
(c) MGSV's own buddy marker sits over her (hidden anyway, §3.8).

**Branch N** (if Q1 shows that her AI walks back or breaks when moved, or Q7 shows MGSV won't draw her at mirrored places): leave
her MGSV body where MGSV keeps it, beside Snake. The host still draws her at the post (§3.8) whenever MGSV draws her. She is seen
less (only when MGSV's camera happens to hold her body), and her aim direction is MGSV's. Same protocol; `[Quiet] Mirror = false`.

**Heading**: the published `buddyYaw` comes from her skin root (136 bones; Q10) when the skin is fresh, so it moves with her draws
in the same frame. Otherwise it is the mirror's yaw. `buddyPos` likewise comes from the skin when it is fresh, and from GetPosition
otherwise. A difference over 0.3 m is logged.

**Slip** (Elden Ring's walls stop Snake while MGSV's Snake runs on): the post is in Elden Ring's coordinates and the host draws her
there, so a slip moves only her MGSV body (re-placed past 3 m), never what Elden Ring shows. D-Dog's `SlipObserver` / `WaitPin`
are not needed for her.

**Elden Ring's world shifts**: Elden Ring moves its world origin on an 8 m grid while Snake travels (live 32-40 m, y included
[V-code er/WorldShift.h:3-7, 24-28]), and every system that keeps an absolute Elden Ring position across frames follows it
(er/WorldShift.cpp:57-65). The post is such a position, held while Snake moves up to 20-25 m. So `Quiet::OnWorldShift(delta)`
moves `post_ER`, the search centre, the candidates in flight, and the fade's old and new places, and is added to WorldShift.cpp's
list. Without it a shift draws her 32 m off her post in view, or fails her "line to Snake" and re-posts her. Unit test: a 32 m
shift leaves the drawn post unchanged on screen.

### 3.3 Orders: MGSV meaning → Elden Ring meaning

| cmd | MGSV | Elden Ring (v1) |
|---|---|---|
| 1 Fire (kill) / 2 Fire (tranq) | fire at the binocular target | the job (§3.5) on the host's pick. The effect comes from the **attack id of her hit** (290 Wicked lethal, 291 Guilty sleep, 292 Sinful heavy), which is what MGSV fired. The command clicked sets only the texts |
| 3 Take aim | aim; fire on Fire or on Snake's next shot | the same job, held up to `AimHoldMs` (20 s). Her hit comes when Snake fires (MGSV's Snake fires in MGSV too) or on a later Fire |
| 4 Cover me / 5 Cease cover | fire at will / stop | **With `[Quiet] Cover = false`** (v1's default): command 4 is taken over (not passed to MGSV), and the corner says "Quiet: Cover me -- not in Elden Ring yet". If MGSV's cover bit is ever found on (`kBuddyQuietCover`), the guest sends command 5 itself. So she never says "Engaging enemy" for nothing, and MGSV's real cover mode is never on while `FireOnlyOnOrders` is unproven (Q6). **With `Cover = true`** (opt-in after Q16/Q17): passed to MGSV, so the menu toggles and she says "Engaging enemy"; the guest runs the job every `CoverEveryMs` (8 s) on the host's threat pick (`kActorQuietThreat`) while MGSV's cover mode is on (R[0] bit 0, published as `kBuddyQuietCover`) |
| 6 Shoot this | shoot Snake's grenade near the nearest enemy | v1: passed to MGSV with her fire disabled, so nothing happens. The corner says "Quiet: Shoot this -- not in Elden Ring yet". v2: Q13 + `er/Throw`'s flown grenade, set off early beside the nearest enemy |
| 7 Quiet (talk) | she turns to Snake; **MGSV's camera turns to her body** | **Taken over while `kHostQuietLayer` is on** (not passed to MGSV): Elden Ring's camera follows MGSV's (er/CameraDriver.cpp:66), and her MGSV body is not where Elden Ring draws her (the mirror's pull-in, branch N), so the view would swing to an empty spot mid-fight. The corner says "Quiet" and `kGEvBuddyOrder` goes out. With the layer off it is MGSV's own, as today. Never sent by PhantomRing |

The guest takes 1-4 and 7 over only while the host's layer is on (`kHostQuietLayer`), as D-Dog's 11-15 are taken (4 only while
`Cover = false`). **Follow.cpp**:
commands 1-7 (like 8-16) never set or change D-Walker's auto-follow hold, and a change of the active buddy type clears the hold
(the hold survived a buddy change live [V-live M]).

### 3.4 Choosing her target (host), and the stand-in MGSV marks

- **Candidates**: StrictHostile (er/Dog.cpp:143), alive, rendered, not a boss (`Cqc::IsBoss`, including NpcParam
  `isSoulGetByBoss`), not held by CQC; within the Actors radius (60 m of Snake) and `target_range` (100 m) of her post; a clear
  ray from her post's sight point (feet + 0.9 m) to his chest (0.7 × his height): **the same ray the shot casts** (§3.6), so the
  pick never chooses a target the shot would refuse from the same post (a unit test checks it); and a clear ray from the camera
  (the player sees him). With her weapon tranquilizer (buddyFlags), not asleep.
- **The pick**: the candidate nearest the view centre, within 6° while the binoculars are up (`kGuestBinoculars`) and 10°
  otherwise. Characters the host has marked come first. D-Dog's latch applies (500 ms, a click within 800 ms of the reticle takes
  him). It is published as **`kActorQuietTarget`** on that one record, and re-checked at the Actors boundary (hostile, alive, not a
  boss) [V-code er/Actors.cpp:62-64].
- If the enemy at the view centre can't be seen from her post, the host searches a post that sees him (§3.2), fades her over, and
  only then publishes the flag. The corner says "Quiet: moving to get a shot".
- **The stand-in** (guest, `MarkProxy`): while `kActorQuietTarget` exists **and** the binoculars are up, or were up within
  `KeepMarkMs` (20 s), the guest dedicates one inert proxy soldier to him (`proxy::Dedicate`: puppet + restrict-notice, the phase
  lock [V-code ActorProxy.h:287-330]). MGSV's binoculars then mark that soldier as MGSV marks any soldier [V-live M, under
  `!proxy on`], and **MGSV's call menu makes Fire and Take aim live** (§2.4). If he already has a proxy (an asleep proxy, or full
  proxies), that soldier is used. The soldier follows the enemy (ActorProxy) and is released when the pick goes, `KeepMarkMs`
  passes with no order, or after the order (§3.5).
  - **At the enemy's height** (`[Quiet] StandInHeight = enemy`): `Dedicate` stands D-Dog's soldier on the sandbox ground at the
    enemy's x/z, because the dog has to reach him [V-code ActorProxy.h:292-296]. A sniper's targets are often far above Snake
    (Gatefront: 34-36 m, §2.9), and binoculars looking up at them would never find a soldier on the apron, so Fire would stay
    greyed for exactly those enemies. Quiet's slot uses the height path the full proxies have (`[Proxies] Height = enemy`, GUEST.md
    20.5); D-Dog's stays on the ground. Whether MGSV holds a puppet up there, and whether the binoculars then mark him, is [U Q4].
  - **MGSV's mark must survive**: with the shipped config the guest's Pump wipes all marks every `ClearMarkersSec` (5 s) unless
    full proxies keep them [V-code mgsv/Pump.cpp:968-975]. The wipe is held off while a Quiet stand-in is dedicated
    (`keepMarkers |= quiet::StandInMarked()`). The narrower variant, a wipe that spares the stand-in's id (`TppMarker.Disable` per
    id, [U] that MGSV's Lua has it per id), is used if Q4 shows MGSV marking anything else in that time. D-Walker's search marks are
    not a concern: only one buddy is out.
  - **Aim pull**: a marked MGSV soldier once pulled Snake's aim onto itself [V-code mgsv/Config.h:66-68]. At the enemy's mapped
    place and height a pull points at the right enemy anyway; Q4 measures Snake's rounds with a stand-in marked, and if they bend,
    `CorrectShot` (today a no-op unless full proxies run, ActorProxy.h:331-334) is enabled for the dedicated slot.
  - **After the order is taken** (Ordered: R+0x186 = the soldier), the menu no longer needs the mark. With
    `[Quiet] StandInOnBearing` (decided by Q5) the soldier moves onto a clear MGSV line from her MGSV body: along her MGSV bearing
    to the target, 15-25 m from her, inside `MirrorRegion` (known-clear apron). The message carries only the soldier id, so
    MGSV's geometry (an airport Elden Ring doesn't have) can't veto a shot Elden Ring allows. Without it, MGSV's line from her body
    to the stand-in at the enemy's mapped place is a second veto (§3.5 Lost).
  - **`kBuddyQuietMarked`** (guest -> host): MGSV holds a mark on the current stand-in (read from MGSV's marker state for that id
    [U Q4: where; else "the binoculars were on him within the last 0.5 s and no wipe ran since"]), so Fire is live.
- The corner, while LB is held with Quiet out and **`kBuddyQuietMarked` clear**: "Quiet: mark an enemy with your binoculars to
  order Fire" (the host can't see MGSV's mark otherwise: its red marker is filtered out of the layer, §3.8).
- **Reticle** (`RenderMarkers` owner `kQuiet`): over the pick while LB is held or the binoculars are up; white while unmarked,
  Quiet's colour once `kBuddyQuietMarked` is set.
- **Cover me's threat pick** (`kActorQuietThreat`, a separate flag): the same hard rules, scored by in combat with Snake
  (`kActorInCombat`), then distance to Snake (≤ 20 m counts double), then sight from her post. There is one dedicated slot: the
  cover job uses it only while no binocular pick holds it (the binoculars are down and `KeepMarkMs` has passed), and yields it to
  a binocular look at once (its job ends Released).

### 3.5 The guest's order job (`mgsv/Quiet.cpp`, rules in `mgsv/QuietLogic.h`)

| state | enter | leave |
|---|---|---|
| **Idle** | | Follow.cpp's detour sees the player's 1, 2 or 3 with Quiet out, `kHostQuietLayer` and `[Quiet] Orders`. `quiet::OnPlayerCommand` takes it, and the original is not called yet. `kGEvBuddyOrder` still goes out (pad[0] = QuietWeapon + 1) |
| **Pick** | the `kActorQuietTarget` record, or the one seen within 1 s before (D-Dog's rule [V-doc ddog §9]); waits up to `PickWaitMs` (1500 ms) while the host re-posts her | none -> `kGEvQuietShot` **Refused** (handle 0; the host's corner: "Quiet: no target -- look at an enemy through your binoculars"). Found -> Realize |
| **Realize** | the dedicated soldier for that handle exists; otherwise `proxy::Dedicate` | location object confirmed within `RealizeTimeoutMs` (1500 ms) -> Ordered; timeout -> Refused |
| **Ordered** | `EnableFire true` (if `FireOnlyOnOrders`); `kBuddyCommand(system, cmd, soldier id)` with `t_ourCommand`; then, if R+0x186 isn't the soldier, write R+0x186 and R+0x182 = soldier id (R = `[[sys+0xB0]+8] + i*0x200`, two loads, §2.4; before the 1.8 s delay runs out); with `StandInOnBearing`, the soldier moves onto her clear MGSV line (§3.4); `kGEvQuietShot` **Ordered**; `kBuddyEngaged` | the hook on 0x140D04D50 sees target == soldier -> Hit. Otherwise **Lost**, with its reason in b (`QuietLostReason`, logged every time): `kQuietLostMgsv` (no hit within `HitTimeoutMs`, 8000 ms, live 3.1-4.3 s, for Fire, or `AimHoldMs`, 20 s, for Take aim; or her "can't" path `kQuietNoTarget` ran), `kQuietLostTarget` (the target dead or gone from the table), `kQuietLostCancelled` (a release trigger below). The host's corner: "Quiet: no shot (MGSV)" for the first (MGSV's geometry or MGSV's own refusal, **not** an Elden Ring line), nothing for the others |
| **Hit** | `kGEvQuietShot` **Hit**: a = phase, weapon, attack id; b = MGSV's share from the attack id's row (`Row()` [V-code mgsv/Combat.cpp:68]); v0 = her MGSV place; v1 = the direction to the soldier; pad = the handle | at once -> Released. Later hits on the same soldier within the job are logged and ignored |
| **Released** | `kGEvQuietShot` **Released**; `EnableFire false` (if `FireOnlyOnOrders` and no other job); the soldier released (`ReleaseAfterHit`), unless the pick still holds him and the binoculars were up within `KeepMarkMs`. **After a Guilty (291) hit he is always released** (FultonDisable + a fresh puppet) if Q11 shows 291 puts the puppet to sleep in MGSV: an MGSV-asleep stand-in for an enemy Elden Ring may have missed would be offered MGSV's Fulton, out of step with Elden Ring | Idle |

- The player's own 1-3 never reach MGSV while the layer is on. MGSV's own target (whatever its binoculars marked) is never ordered;
  the job orders her at the pick's soldier.
- Any hit on a soldier that isn't the job's (another proxy, her own Cover me) is logged (`quiet: a hit on 0x... -- not an order's,
  ignored`) and never reported.
- **Release triggers** (the job ends Lost/`kQuietLostCancelled` if it was Ordered, else Released, and **the stand-in is released
  at once**, whatever `KeepMarkMs` says): a rest at a grace (§3.9), a load, `kHEvTarnished 1`, the host's layer going off,
  **Quiet dismissed, or any change of the active buddy type** (`buddyType` change). The stand-in uses the one `Dedicate` slot
  D-Dog uses [V-code ActorProxy.h:290-298], so a switch to D-Dog within 20 s of a binocular look would otherwise leave Quiet's
  soldier (and her `EnableFire` state) holding the slot and block the dog's first "Get it".
- The dedicated soldier is held while she aims (`proxy::HoldDedicated`, D-Dog's lesson: never put back mid-order), and moved only
  with Snake's treadmill jumps (and, with `StandInOnBearing`, once onto her line).
- **The phase guard per buddy**: today `Dedicate` re-arms the alert guard only for the session's first dedicated soldier
  (`!g_dogRan && !proxies`), the give-up is latched once per guard, and a give-up calls `dog::DisableAttack` [V-code
  mgsv/ActorProxyGame.cpp:657, 700-704, 951-956; ActorProxy.h:308-310]. Generalized as is, Quiet's stand-ins (one per binocular
  look) would use up D-Dog's fresh guard, and a give-up latched during Quiet would leave D-Dog's later attacks with an uncalmed
  alert. So: the "ran" flag and the guard are keyed on the **active buddy type**; `RearmPhaseGuard` runs on every buddy-type change
  and on each buddy's first `Dedicate`; a give-up calls the active buddy's `DisableOrders` callback (D-Dog: `DisableAttack`;
  Quiet: orders off, "Quiet: orders off -- MGSV's alert"). Test (`test_guest_proxy`): Quiet's stand-in gives up, then D-Dog is
  deployed, then an alert -> D-Dog's `DisableAttack` fires.

### 3.6 Her shot in Elden Ring (`er/Quiet.cpp` → existing Combat paths)

On `kGEvQuietShot` Hit:
1. **Who**: the handle's character must still be StrictHostile, alive and not a boss. Otherwise nothing happens (logged: `quiet: hit
   on 0x... refused -- not hostile / a boss / dead`). This check is made here because `HitCharacter` doesn't make it.
2. **From where**: her **sight point** = the post's feet + 0.9 m (kneeling), the same point the post and the pick test from
   (§3.2, §3.4). If she has no post (on the move) or is seated (no orders then, §3.9), the shot is dropped (the job shouldn't
   have run). MGSV's muzzle and direction (v0/v1) are only logged.
3. **Line of sight**: one `CastRay` (ray filter 0x2000058, as `marking::RayClear`) from the sight point to his chest (capsule
   centre at 0.7 × height), the pick's own ray. If it is blocked (he moved, or the world did), it is a miss: no damage, corner
   "Quiet: no clear shot". **Elden Ring decides whether a shot lands; whether MGSV fires at all is MGSV's** (its own line from her
   body to the stand-in: §3.4 `StandInOnBearing`, §3.5 Lost).
4. **The effect**:
   - **lethal** (290 / 292): the bullet route Snake's rounds take with MGSV's share (docs/COMBAT.md §2): b / 10000 × his
     reference life, with bigness scaling. Without a share, `lethal_fallback` (1.0 lives). Sinful (292) also gets a knockdown's
     poise (`heavy_poise`).
   - **tranquilizer** (291): sleep build-up of `tranq_sleep` (1.1) × his sleep resistance, the sleep ladder (kTranq class, er/
     CombatLogic.cpp:150). Asleep -> `kActorAsleep` -> D-Dog's "Get it" and Snake's Fulton work on him.
   - The kill is the player's (runes), as Snake's rounds are.
5. **Noise**: none (`shot_noise = false`). Quiet is a sniper Elden Ring's enemies don't hear. `true` would make the existing AI
   sound at her muzzle, and they would go looking there.
6. **Visual**: the enemy's own hit reaction or death. Her muzzle flash is an MGSV effect and isn't captured [U Q9]. A host muzzle
   flash at her post is v2.

Rate: one effect per Hit, and one Hit per order (§3.5). Bosses are never hit: the pick refuses them, and the effect refuses again.
No `stagger` option for her in v1.

### 3.7 Spotting (host only, no protocol)

- From her post's sight point, while she has a post, isn't seated and Tarnished mode is off: characters within `spot_range` (60 m)
  of her and inside the Actors radius that pass **the pick's hard rules** (StrictHostile, which already excludes PlayerIns NPCs and
  spirit summons; not `Cqc::IsBoss`; alive; rendered), with a clear ray (she sees; D-Dog smells). So a ray past a fog wall never
  marks a field or arena boss, and a neutral is never marked. Two rays per frame at most, each candidate re-checked every
  `spot_every_ms` (500 ms). The dry-run `quiet pick` (U14) prints the same verdicts.
- They get `Marking::Provisional(handle, until)` (D-Dog's API [V-code er/Marking.h:33]), lasting `spot_linger` (10 s) after the
  last sight.
- Corner "Quiet: Enemy spotted (N nearby)", at most every `spot_text_every` (8 s).
- MGSV's own "Quiet: ..." announcement lines are white text in MGSV's HUD layer and stay. They name MGSV's stand-in, as D-Dog's do.

### 3.8 Drawing her

- **At a post** (`kBuddyAtPost` with the host's current `buddyPostSeq`): her draws are moved as a whole by `T = post_ER −
  anchor.PointToEr(buddyPos)`, computed every frame. The translation absorbs treadmill jumps, the slip and the mirror's pull-in.
  There is no rotation: her MGSV yaw is the mirror's. There is no ground ray (the post was chosen on standable ground) and no
  ±1.5 m clamp (per-buddy `lift_max` 30 m; the D-Dog / walker clamp stays). This is a new "relocated buddy" branch in `GroundBuddy`
  (er/RenderScene.cpp:164).
- **Seated** (`kBuddySeated` **and** `graceSeqSeated == graceSeq`): drawn at the host's own `graceSeat_ER` by the same per-frame
  translation (the seat was rule-checked on Elden Ring ground). Only from the frame the echo arrives **while the rest's black is
  still observed** (§3.9); an echo that comes later keeps her hidden for the rest of the rest. The guards are reset on the rising
  edge (inside the black). Between the seat's publish and the echo she is hidden (the screen is black).
- **Neither** (just deployed beside Snake, moving after an order): the normal placement with a far-ground rule for Quiet: a ray
  from 20 m above to 40 m below, taking the highest walkable hit not above mapped + 20 m. With no floor she is **hidden**, never
  floating.
- **Hidden**: no post (on the move); `kBuddyCloaked` (`cloak = hide`, MGSV's own look [V-live M]; `shimmer` = 15 % see-through
  with the screen-door dither as v2); her post changing in view (the 0.3 s fade); the host layer off and today's rules.
- **Guards**: StepGuard on her shown place (`speedMax` 12 m/s; real moves taken out of view or by the fade), enabled per buddy
  rather than `buddyType == kDog`.
- **Capture** (guest): Quiet's draws are the skinned draws rooted within 0.35 m of her skin root (LOD meshes too), plus rigid draws
  within `RigidRadius` (1.2 m) of her root whose keys were learned while she was within 15 m of Snake on the apron (her rifle).
  Other rigid draws within 4 m of her are not hers. This generalizes `dog::KeepBuddyDraw` [V-code Dog.cpp:1438-1463]; D-Dog's own
  rule is unchanged. Counter: `capture: Quiet props dropped N`.
- **`kBuddyCloaked`** (guest): set while her captured draws have dropped to ≤ 3 within one frame of a move faster than 12 m/s, and
  cleared when they are back.
- **MGSV's markers**: the HUD layer's marker-hue filter (`IsMgsvMarkerColor`, Hud.cpp:400-409) drops **every** UI draw whose
  `cVSLight` colour matches MGSV's green or red marker hues. For D-Dog it is on only while an order runs (a few seconds) and was
  verified only then [V-code Hud.h:51-53]. For Quiet it is scoped the same way: on while a stand-in is dedicated or an order job
  runs (`HideMgsvMarkers`), so the red marker on her stand-in never shows at its mapped spot; the host draws its own reticle. It is
  **not** widened to "while she is out" until `!capture hud` with Quiet out, the call menu open, the iDroid open and an alert icon
  showing has counted what it drops (`g_dogMarkersLeftOut`) and found only markers. Outside those windows MGSV's green buddy
  marker over her MGSV body may show; with the mirror it sits over her post.

### 3.9 The grace rest (buddy-generic protocol, Quiet's guest job; ships together with posts)

This part can be **built and tested** alone: the protocol's grace fields, the host's gate and seat rule, and the guest's sit job,
with Quiet deployed (the fakes or the real games) and no orders needed. It is **switched on for players only together with posts
and drawing** (§3.2, §3.8; §7's exit lines): the host runs the grace job only while `kHostQuietLayer` and `[Quiet] posts` are on.
With the layer off she is 0.8.0's Quiet (6 m jumps at every treadmill jump, rigid props drawn as her, QUIET ATTACK not greyed,
§1), and nothing would define how she gets up and away: she doesn't come to the grace then. The protocol is buddy-generic; only
the guest's sit job is Quiet's in this build (D-Dog could later lie down there through its Wait, §9).

**When she comes** (the gate):
- The host **arms** the grace job on its rest detection (`UpdateRest`, er/Host.cpp:265; `kHEvRested a = 0`, `g_atGrace` set),
  with Quiet out, `kBuddyCanRest` from the guest, and the layer and posts on. Arming changes nothing on screen.
- Not on a respawn (`a = 1`). Not in Tarnished mode (`[Grace] tarnished = false`: the parked Snake isn't resting, §3.11).
- **Only inside an observed black** [U U12]. The seat work starts on the first frame `RestDark()` holds (Elden Ring's fade state
  at full, else the presented frame's mean luminance ≤ 1, whichever U12 finds), and only if that comes within
  `seat_black_wait_ms` (1300 ms; the one series went black at +1.05 s) of the press. **No black by then = no seat this rest**:
  she stays at her post, and since Snake isn't moving the post rule leaves her there.
  - This is how a rest that turns into a **talk** is handled: Melina came with the HUD up and no black for 3.5 s (§2.9), while
    `kHEvRested 0` had already fired. The safety rules forbid testing that live, so the path is safe by construction (nothing
    moves without a black) and is covered offline: a fake_er / replay "talk" rest (the press, no fade, the HUD up, then
    `ER dialog`) publishes no seat and moves nothing.
  - A rest detected through the HUD (`viaHud`, ~+2.4 s) comes near the black's end; the deadline below normally refuses it.
- If `ER dialog` (6001) starts while she is seated or being seated, the seat is cancelled: the host stops drawing her (hidden)
  until the interaction ends, then the post rule places her (out of view at once, in view by the fade).

**Where she sits** (host, er/GraceLogic, pure, with an injected ray oracle so recorded rays can drive it, §7):
1. **The grace centre**, on the first black frame: the nearest `AEG099_060` asset within 2.5 m of the player (`GeomProbe::Near`,
   about one frame, ~10 ms [V coarse]). The draft ran it at +0, before the fade-out starts at +0.24 s: a visible hitch. In the black
   it is unseen [cost U U2]. Fallback: the player's spot + 1.25 m along his facing (he has turned to the grace by then, §2.9). A
   `BonfireWarpParam` route only if U2 says the scan is too slow.
2. **Candidates**: rings of 2.0, 2.5, 1.5, 3.0 m (in that order) × 16 directions, evaluated in score order (the direction whose
   screen x is nearest 0.62 first). The search stops at the first candidate that passes every hard rule with a score within 0.2
   of the best possible. Typically < 10 candidates, < 250 rays (≤ 7 frames); at most 64 candidates × ~25 rays, ≤ 36 rays a frame
   after the scan's frame (≤ 45 frames, 0.75 s; the deadline below drops a seat that is later).
3. **Hard rules** [the ER study's, with its two fixes]:
   - the ground-filter floor within 0.3 m of the floor under the grace; `ny ≥ 0.9`; footprint spread ≤ 0.15 m;
   - headroom 1.3 m clear; knee rays at 0.5 m **and 0.25 m** clear; the line from the grace to the spot at 0.5 m clear;
   - ≥ 1.0 m from Snake's spot; ≥ 2.0 m from any other character (except the grace's own hidden team-0 character within 1 m of
     the centre), checked in the black, so a character that only appears with a talk can't be there (no black, no seat);
   - in the rest view (the frozen MGSV camera at the press): in front, on screen, screen x ≥ 0.33 (right of the grace menu), the
     camera-to-spot line clear, not behind Snake's silhouette and not in front of it.
4. **Score**: |r − 2.0| + 2 |x_screen − 0.62| + 0.5 |dh|. She **faces the grace** (v1b: faces away from it, below).
5. **Fallback**: the spot 2 m from the grace opposite Snake with the floor, headroom and knee rules only. If even that fails,
   there is no seat: she stays where she is, unseen.

All three surveyed seats were measured with the camera behind Snake (§2.9). A player who rests with the camera turned sideways is
the case where the seat may be off-screen or behind the menu: U11 is an exit item (§7), and the clean "not there" is the answer
when no candidate passes the view rules.

**How she gets there** (the deadline depends on how MGSV can place her):
- The host publishes `kHostAtGrace` at once (seat NaN). The seat (`graceSeat`, MGSV-mapped, `graceSeatYaw`, `graceSeq + 1`) is
  published **only while `RestDark()` holds** and no later than `black_start + seat_deadline_ms`. The deadline = U12's shortest
  black − the method's placement time − 0.2 s:
  - **one-frame methods** (her location object, Q1; or `WarpToPosition` if Q2 shows it lands in one frame): with the study's one
    black (1.45 s) that is about **1.0 s after black start**;
  - **`ReserveCallBuddy NORMAL` + `Reload`** (only if Q1 and Q2 both fail): allowed only if the seat is ready within
    `seat_reload_ready_ms` (100 ms) of black start **and** Q18's p90 Reload time + 0.2 s fits inside U12's shortest black. With
    the study's numbers (a 1.45 s black, one Reload of up to 1.5 s) it does **not** fit: the draft's "1.0-1.5 s, done by +2.5 s"
    took the 1.0 s from the borrow's first deploy, not from a Reload, and a seat published at +2.0 s would have landed at
    +3.0-3.5 s, after the fade-in and with the menu up. So this fallback is off until Q18 and U12 say otherwise, and without a
    one-frame method there is no grace rest.
  A seat found after the deadline is dropped for this rest (logged `grace: seat found at +X ms after black -- too late, none this
  rest`).
- The guest, on a new `graceSeq` with a finite seat and Quiet out:
  1. Ends any order job (Lost, cancelled; the stand-in released).
  2. Places her at the seat (on MGSV's ground, `rotationY` = the seat's yaw) by the method M1 chose.
  3. **Then** `EnableFire false` and `EnableAim false` (or the Q2 command that lowers the rifle). After a Reload, only once
     `active TppBuddyQuiet2` is logged again: a Reload re-creates her, and commands sent before it are most likely lost [U Q18].
  4. Holds her there: re-placed if she drifts over 0.3 m, **only by a one-frame method**; never a Reload while the screen can be
     seen (it hides her for ~1 s).
  5. Publishes `kBuddySeated` + `graceSeqSeated`, with `buddyPos` = where she is.
- **The host draws her nowhere from the publish until the echo**, and at the seat only if the echo arrives **while `RestDark()`
  still holds** (§3.8). An echo after the black: she stays hidden through this rest (the clean "not there"), never a pop-in at the
  menu. Before the publish she is drawn as before (at her post; the screen is going black anyway).
- No walk is shown: the screen is black, and a walk from a post 40 m away would be a cloaked dash anyway.

**What MGSV plays** (a bet until Q2):
- v1 (`SitPose = kneel`): her own standby kneel, facing the grace, **with the rifle lowered**, if Q2 finds a command that lowers
  it (`EnableAim false`, `MoveToPosition {disableAim=true}`, `SetShootStance`). Live, the only standby kneel seen has the rifle
  **up** (§2.8), and which motion it is was never identified.
- v1b (`SitPose = watch`), if nothing lowers it: she kneels **facing out from the grace** (yaw = from the grace to the seat), on
  watch over Snake's rest, so the raised rifle points away from Snake and the light, never at them. **The player accepted v1b
  (open question 2, 2026-11-04): the grace rest ships with it** if no seated or rifle-lowered pose is found; the README says
  "she kneels on watch beside the grace". The host's `graceSeatYaw` always faces the grace; with `SitPose = watch` the guest turns her the other way (from
  `graceCentre` through `graceSeat`), so the host needs no pose setting (§4). Round I0's built-in
  `SitPose` is `watch` until Q2 says whether `kneel` (rifle lowered) works.
- v2 (`SitPose = seat`): the helicopter cabin's cross-legged wait, once Q3 / P11 finds a way: a bonus, never blocking v1 / v1b.
- Never the rain, shower or Mother Base motions, never a hurt "down" idle, never the talk command (it turns MGSV's camera).

**Getting up**:
- She stays through the grace menu, Level up, Pass time and the HUD chain between them (`g_atGrace` holds through them [V-doc HOST
  "Sitting at a grace"]), and after B while Snake stands at the grace.
- She gets up when `kHostAtGrace` falls: Snake walks 2 m (`ER: left the grace`), a load (fast travel from the grace menu), or a
  death.
  - The guest clears `kBuddySeated`. `EnableAim true` makes her ready her rifle (MGSV's own move), and `EnableFire` follows
    `FireOnlyOnOrders`.
  - The host's post rule takes her back: Snake walking away is "on the move", so she vanishes in her cloak fade once he passes
    3 m/s, or a post is picked if he lingers. Out of view at once, in view by the 0.3 s fade, never sooner than 1.5 s after he
    left. (This is why the rest needs posts on.)
- An order after B while still at the grace ends the sit: she gets a post first (§3.4).

**Other cases**:
- **World shifts**: `GraceRest::OnWorldShift(delta)` moves the grace centre and the seat (Elden Ring's coordinates), and
  `Host::OnWorldShift` now moves `g_gracePos` as well, so a shift during a rest never reads as "left the grace" (2 m from a stale
  point) and never un-seats her. Both are added to WorldShift.cpp's list (er/WorldShift.cpp:57-65). Unit test: a 32 m shift while
  seated keeps `kHostAtGrace` and the drawn seat.
- **Loads, deaths**: `g_atGrace` already ends on them. After a fast travel the host picks a post near Snake's new place (she is
  hidden through the load).
- **D-Dog or D-Walker deployed instead**: the guest doesn't set `kBuddyCanRest`, so the host computes no seat (no rays) and nothing
  changes for them.
- **Saves**: the sit is memory only (placement and `Enable*`). No `vars` are touched.
- **Safety**: the feature never starts a rest or a talk, never sends anything to Elden Ring's characters, does nothing without an
  observed black, and the seat keeps 2 m from every character. Live tests rest only at graces the save has rested at without a
  talk (§7).

### 3.10 Should Elden Ring's enemies attack her? **No (v1)**

- She is not an Elden Ring character. Giving Elden Ring's AI something to target is the open AI-target RE (the decoy lure's).
- She stands 8-55 m off with no hostile within 15 m of her post when it is chosen. A hostile coming within 8 m makes her re-post
  (her cloak), which is what MGSV's Quiet does ("Quiet: On the move").
- Nothing in the sandbox hurts her in MGSV (the proxies are inert). The guest also sends `EnableDying {enable=false}` while she is
  out (`NoDying`) [U effect].
- Her shots make no Elden Ring sound by default, so enemies don't search her post.

### 3.11 Tarnished mode

In Tarnished mode the player fights as the Tarnished (spirit ashes, Torrent), the guest is parked and Elden Ring has the pad
[V-doc HOST.md:263-272]. Quiet stays out of it, so she never snipes the Tarnished's fights on her own or chatters while he rides:
- `kHEvTarnished 1` releases any job and its stand-in (§3.5); no Cover job, no spotting, no post searches, no grace seat.
- Her post is frozen where it was; she stays drawn there while the Tarnished is within 60 m of it, and is hidden (as on the move)
  once he is farther.
- On `kHEvTarnished 0` a post is searched as after a fast travel.
- A README sentence says so, and the posts exit line includes one Tarnished-mode round trip (§7).

---

## 4. Protocol v18

Checked against the v17 header [V-code protocol/phantomring_protocol.h]: HostFlags bits 7+ are free; HostState `reserved` starts at
0x30; GuestState `reserved` starts at 0xC4; GuestBuddyFlags bits 6, 7 and 16-31 are free (8-15 are D-Dog's style); ActorFlags bits
6+ are free; GuestEventType 16 is free. No struct changes size (`HostState` 0x100, `GuestState` 0x200, events 48 bytes,
`ActorRecord` 64).

```cpp
inline constexpr std::uint32_t kVersion = 18;  // ...; v18: Quiet and the grace rest (docs/research/quiet.md §4) -- kHostQuietLayer,
                                              //      kHostAtGrace + HostState grace and post fields, GuestState echoes,
                                              //      buddyFlags kBuddyCloaked / kBuddySeated / kBuddyAtPost / Quiet's weapon /
                                              //      kBuddyQuietCover / kBuddyCanRest / kBuddyQuietMarked, kActorQuietTarget /
                                              //      kActorQuietThreat, kGEvQuietShot + QuietPhase / QuietWeapon /
                                              //      QuietLostReason, kGEvBuddyOrder pad[0] for Quiet

enum HostFlags : std::uint32_t {
    // ... kHostDogLayer = 1u << 6,
    kHostQuietLayer = 1u << 7,  // (v18) the host's [Quiet] layer is on: it picks her posts (buddyPost*) and her target
                                // (kActorQuietTarget) and applies her hits. Only while this is set does the guest hold her at posts
                                // and take the player's 1-3 over; otherwise Quiet is MGSV's own (0.8.0)
    kHostAtGrace = 1u << 8,     // (v18) the player rests at a Site of Grace: from the rest until he is 2 m from where he rested, a
                                // load or a death (er/Host.cpp g_atGrace); graceCentre / graceSeat valid when finite
};

struct HostState {
    // ... fallSpeed @0x2C
    // (v18) MGSV world as of the GuestState the host consumed (through the anchor, as ActorRecord.pos); the guest corrects them for
    // its treadmill warps since then as it does the actor table (OffsetHistory). Written every host frame.
    float         graceCentre[3];  // @0x30 the grace (asset / param / the player's facing); NaN = unknown or no rest
    float         graceSeat[3];    // @0x3C where the buddy sits (feet); NaN = no seat this rest (yet, or none found). Published
                                   // only while the host observes the rest's black (its fade state / frame luminance, not a
                                   // timer) and before the method's deadline; kept until kHostAtGrace falls or the seat is
                                   // cancelled (an ER dialog)
    float         graceSeatYaw;    // @0x48 MGSV yaw (rad) the seated buddy faces: toward the grace
    std::uint32_t graceSeq;        // @0x4C bumps with every published seat (a rest without one leaves it); 0 = none since the host started
    float         buddyPost[3];    // @0x50 Quiet's post (feet); NaN = no post: she is on the move (not drawn)
    float         buddyPostYaw;    // @0x5C MGSV yaw she faces there (toward the hostiles she covers, or along Snake's view)
    std::uint32_t buddyPostSeq;    // @0x60 bumps with every new post
    std::uint8_t  reserved[0x100 - 0x64];
};
static_assert(offsetof(HostState, graceCentre) == 0x30 && offsetof(HostState, buddyPostSeq) == 0x60 &&
              offsetof(HostState, reserved) == 0x64 && sizeof(HostState) == 0x100);

struct GuestState {
    // ... aimThrowKind @0xC0
    std::uint32_t buddyPostSeq;    // @0xC4 (v18) the post the guest holds the buddy at (with kBuddyAtPost); 0 = none
    std::uint32_t graceSeqSeated;  // @0xC8 (v18) the rest whose seat the buddy holds (with kBuddySeated); 0 = none
    std::uint8_t  reserved[0x200 - 0xCC];
};
static_assert(offsetof(GuestState, buddyPostSeq) == 0xC4 && offsetof(GuestState, reserved) == 0xCC && sizeof(GuestState) == 0x200);

enum GuestBuddyFlags : std::uint32_t {
    // ... kBuddyEngaged = 1u << 5 (v18: also Quiet, from kQuietOrdered until kQuietReleased)
    kBuddyCloaked    = 1u << 6,   // (v18) MGSV draws the buddy cloaked -- not at all (Quiet's stealth camo while she dashes): hide it
    kBuddySeated     = 1u << 7,   // (v18) the buddy rests at the grace seat of graceSeqSeated; buddyPos is where it sits
    kBuddyAtPost     = 1u << 16,  // (v18) Quiet is held at the host's post buddyPostSeq: the host draws her at that post's Elden Ring place
    kBuddyQuietCover = 1u << 20,  // (v18) MGSV's "Cover me" is on (her order record's cover bit)
    kBuddyCanRest    = 1u << 21,  // (v18) the guest seats this buddy at a grace seat ([Quiet] RestAtGrace): only then does the host
                                  // look for one (and only with kHostQuietLayer and posts on, docs/research/quiet.md §3.9)
    kBuddyQuietMarked = 1u << 22, // (v18) MGSV holds a mark on Quiet's current stand-in, so its call menu's Fire / Take aim are live:
                                  // the host's LB hint and the reticle's colour follow it
};
inline constexpr std::uint32_t kBuddyQuietWeaponShift = 17;  // (v18) bits 17-19: QuietWeapon + 1 while Quiet is out; 0 = unknown
inline constexpr std::uint32_t kBuddyQuietWeaponMask = 0x7u << kBuddyQuietWeaponShift;

enum QuietWeapon : std::uint32_t {  // (v18) from her live weapon (Q11), confirmed by the attack id of each hit
    kQuietLethal = 0,               // Wicked Butterfly (EQP_WP_Quiet_sr_010, ATK 290)
    kQuietTranq = 1,                // Guilty Butterfly (sr_020, ATK 291): sleep
    kQuietHeavy = 2,                // Sinful Butterfly (sr_030, ATK 292): lethal, anti-materiel
    kQuietWeaponUnknown = 0xFF,
};

enum ActorFlags : std::uint32_t {
    // ... kActorDogTarget = 1u << 5,
    kActorQuietTarget = 1u << 6,  // (v18) the host's pick for Quiet's next Fire / Take aim: hostile, alive, not a boss, seen from her
                                  // post's sight point and from the camera, the binoculars' / view centre's. At most one record.
    kActorQuietThreat = 1u << 7,  // (v18) Cover me's threat pick (same hard rules, scored by the threat to Snake). At most one record;
                                  // it may be the same record as kActorQuietTarget. The one stand-in slot goes to the target first.
};

enum GuestEventType : std::uint32_t {
    // kGEvBuddyOrder = 11: (v18) b == 3 (Quiet, commands 1-7): pad[0] = QuietWeapon + 1 for 1-3, 0 otherwise
    kGEvQuietShot = 16,  // (v18) Quiet and an Elden Ring target (docs/research/quiet.md §3.5): a = QuietPhase | QuietWeapon << 8 | MGSV
                         // attack id << 16 (290 / 291 / 292; 0 = unknown); b = MGSV's share of that attack in 1/10000 of a standard
                         // soldier's life, lethal or non-lethal by the weapon (0 = unknown); v0 = her place (feet, MGSV world);
                         // v1 = unit direction her -> the soldier (MGSV world; 0 = unknown); pad[0] | pad[1] << 32 = the target's
                         // ActorRecord handle (0 = none: kQuietRefused without a pick). For kQuietLost, b = QuietLostReason
};

enum QuietLostReason : std::uint32_t {  // (v18) kGEvQuietShot b when a & 0xFF == kQuietLost
    kQuietLostMgsv = 1,       // MGSV never fired: no hit in time, or her "can't" path ran (MGSV's own line or refusal): "no shot (MGSV)"
    kQuietLostTarget = 2,     // the target died or left the table first
    kQuietLostCancelled = 3,  // a release trigger: a rest, a load, Tarnished mode, the layer off, a dismissal or a buddy change
};

enum QuietPhase : std::uint32_t {  // (v18) kGEvQuietShot a & 0xFF
    kQuietOrdered = 0,   // MGSV took the order on the target's soldier: she aims
    kQuietHit = 1,       // her round hit his soldier in MGSV (her damage message, 0x140D04D50): the host fires from her Elden Ring post
                         // and Elden Ring's line of sight decides
    kQuietReleased = 2,  // the order is over (the soldier released or kept for the mark)
    kQuietRefused = 3,   // no pick, no soldier in time, or she can't take orders now
    kQuietLost = 4,      // ordered, then no hit (b = QuietLostReason)
};

constexpr std::uint32_t QuietShotA(QuietPhase, QuietWeapon, std::uint32_t a_attackId = 0);  // phase | weapon << 8 | (id & 0xFFFF) << 16
constexpr QuietPhase    QuietShotPhase(std::uint32_t a);
constexpr QuietWeapon   QuietShotWeapon(std::uint32_t a);
constexpr std::uint32_t QuietShotAttackId(std::uint32_t a);
constexpr std::uint32_t BuddyQuietWeaponBits(QuietWeapon);  // (w + 1) << 17, 0 for unknown
constexpr QuietWeapon   BuddyQuietWeapon(std::uint32_t a_buddyFlags);
// + static_asserts as for v17's dog helpers: QuietShotA(kQuietHit, kQuietHeavy, 292) == 0x01240201
```

- **State, not events, for the grace and the post.** A sit lasts as long as the rest, and a post as long as it holds. Level-held
  fields survive a guest re-attach or a missed event, the guest reads HostState every frame anyway [V-code Frame.cpp:835-836], and
  the echoes let the host know what the guest holds. `kHEvRested` stays the edge (the refill).
- **No new host event.** "Left the grace" is `kHostAtGrace` falling.
- `buddyPost*` and `graceSeat*` are named for any buddy; v18 uses the post for Quiet only.
- **Versions**: plugins of different versions never attach (`MismatchText` [V-code common/Version.h]), so v18 needs both plugins
  updated together. Within v18, `kHostQuietLayer` off = 0.8.0's Quiet **and no seats** (the host's grace job needs the layer and
  posts, §3.9), and `kBuddyCanRest` off = no seats. Old `.prrec` recordings replay unchanged (the new fields are zero; a zero
  `graceSeq` / `buddyPostSeq` means none).
- The observed black is the host's alone: the guest needs no flag for it, because the host publishes a seat only inside it and
  shows her only on an echo inside it.
- `tools/phantomring/proto.py` and `test_layout.py` mirror every field and enum, `kBuddyQuietMarked`, `kActorQuietThreat` and
  `QuietLostReason` included (prefix `kQuiet` for QuietPhase / QuietWeapon / QuietLostReason, as v17 did for the dog's).

**As committed in round I0 (2026-11-04).** Re-checked against the v17 header first: every bit, enum value and offset above was free,
no struct changed size (`HostState` 0x100, `GuestState` 0x200, `GuestEvent` / `HostEvent` 48 bytes with the same
type/a/b/frame/v0/v1/pad[2] layout, `ActorRecord` 64), and `test_layout.py` compares 126 + 595 values with 0 mismatches. The layout
is the plan's, byte for byte. What the header says beyond the sketch above (no clash, only precision):
- **A zero seq means none, whatever the floats hold.** A host or a `.prrec` older than v18 leaves the new HostState bytes zero, so
  `(0, 0, 0)` is not "a seat at the origin": the guest takes `graceSeat` only with `kHostAtGrace`, `graceSeq != 0` and finite
  values, and `buddyPost` only with `kHostQuietLayer`, `buddyPostSeq != 0` and finite values. The real host writes NaN and 0 every
  frame until `er/GraceRest.cpp` / `er/Quiet.cpp` publish (er/Host.cpp), and leaves `kHostQuietLayer` / `kHostAtGrace` clear.
- **`graceSeatYaw` always faces the grace.** v1b's watch (`SitPose = watch`, a guest setting) faces out: the guest turns her from
  `graceCentre` through `graceSeat`, so the host needs no pose setting.
- **`kBuddyEngaged`** covers Quiet from `kQuietOrdered` to `kQuietReleased` (D-Dog's stays Connected .. Released).
- Helpers: `QuietShotA` / `QuietShotPhase` / `QuietShotWeapon` / `QuietShotAttackId`, `BuddyQuietWeaponBits` / `BuddyQuietWeapon`
  with static_asserts (`QuietShotA(kQuietHit, kQuietHeavy, 292) == 0x01240201`; the weapon mask disjoint from `kBuddyAtPost`,
  `kBuddyQuietCover` and the dog's style). proto.py: `HostFlags.QuietLayer` / `AtGrace`, `GuestBuddyFlags.Cloaked` / `Seated` /
  `AtPost` / `QuietCover` / `CanRest` / `QuietMarked`, `ActorFlags.QuietTarget` / `QuietThreat`, `GuestEventType.QuietShot`,
  `QuietPhase` / `QuietWeapon` / `QuietLostReason` (prefix `kQuiet`), `quiet_shot_a` / `quiet_shot_fields`,
  `buddy_quiet_weapon_bits` / `buddy_quiet_weapon`, `BUDDY_QUIET_WEAPON_SHIFT` / `MASK`.
- Both plugins only log the new things so far: the host's `quiet: kGEvQuietShot ... -- logged only` (er/Events.cpp) and Quiet's
  weapon in the order log (er/BuddyOrders.cpp); the guest's Follow.cpp sends `pad[0] = 0` for Quiet until `mgsv/Quiet.cpp` reads
  her weapon.
- Ini keys spelled out beyond §5's table: the host's `near_height_min` / `near_height_max` / `near_research_snake` (the table's
  "near:" values) and `rest_dark = none` (no grace rest until U12 picks `fade` or `luma`).

---

## 5. Ini keys

As for `[Dog]`: round I0 ships every switch that changes what players see **off**. The "default" column is the value to ship once
the part's exit criteria (§7) are met. Parse rules as for `[Dog]` (negative numbers -> 0, enums with a warning, the shipped ini = the
built-in defaults, checked by `test_host` / `test_guest_link`).

**`PhantomRingGuest.ini [Quiet]`**

| key | default | meaning |
|---|---|---|
| `Enabled` | true | the layer; acts only while the host sets `kHostQuietLayer` |
| `Borrow` | **always** (the player's answer to open question 1, 2026-11-04: everyone, any save); each save state on only after Q14 passes twice for it, a failing state stays off with a log line and a README note. I0 ships `off` | `off` / `memory` (a Quiet the save has had: gone, recruited-not-deployable, sickbay / dying / dead) / `always` (also never recruited and killed). Memory only, only while the saves are guarded (§3.1) |
| `BorrowListInIdroid` | **true** (the player's answer: she is listed in MGSV's iDroid BUDDY SUPPORT with no ini edit). I0 ships `false` | borrow at boot, so MGSV's BUDDY SUPPORT lists her; false = only when the ini or console asks for her |
| `Mirror` | true | hold her MGSV body at the host's post (branch M); false = branch N |
| `MirrorRegion` | from Q7 | the apron's free region (a max radius per bearing, 8 or more); farther posts are pulled in along MGSV's camera line. Until Q7: 15 m in every bearing |
| `MirrorHeight` | ground | `ground` / `mapped` (Q8) |
| `PostTolerance` | 3 | m; re-placed when her MGSV place drifts farther from the mirror |
| `Treadmill` | true | shift her with Snake's jumps while at a post or seated |
| `Orders` | true | the player's 1-3 go to the host's pick |
| `MarkProxy` | true | a stand-in for the host's pick while the binoculars are up, so MGSV marks it and its menu's Fire is live |
| `StandInHeight` | enemy | `enemy` / `ground`: Quiet's stand-in at the enemy's height (Q4's elevated case); D-Dog's slot stays on the ground |
| `KeepMarks` | true | no periodic `ClearMarkers` while a Quiet stand-in is dedicated (else Fire greys within `ClearMarkersSec`, 5 s) |
| `StandInOnBearing` | from Q5 | after Ordered, move the stand-in onto a clear MGSV line from her body (15-25 m on her bearing) |
| `KeepMarkMs` | 20000 | the stand-in kept after the last binocular look |
| `FireOnlyOnOrders` | true | `EnableFire false` whenever no job runs: she never fires at anything of MGSV's (Q6) |
| `RealizeTimeoutMs` | 1500 | |
| `PickWaitMs` | 1500 | the order waits this long for a pick (the host may be re-posting her) |
| `HitTimeoutMs` | 8000 | Fire: order -> hit (live 3.1-4.3 s) |
| `AimHoldMs` | 20000 | Take aim: held until her hit |
| `ReleaseAfterHit` | true | |
| `Cover` | false | Cover me runs the job on the host's threat pick (opt-in until Q16/Q17 and the both-games test). While false, command 4 is taken over ("not in Elden Ring yet") and MGSV's cover bit is turned off if found on |
| `TakeOverTalk` | true | command 7 is taken over while the layer is on (MGSV's camera would turn to her MGSV body) |
| `CoverEveryMs` | 8000 | |
| `RestAtGrace` | true | sets `kBuddyCanRest`; seats her at the host's grace seat |
| `SitPose` | `kneel` if Q2 lowers the rifle, else `watch` (v1b: the player accepted it, open question 2). I0's built-in value: `watch` | `kneel` (v1, rifle lowered) / `watch` (v1b, facing out from the grace) / `seat` (v2, once Q3 finds a way; a bonus) |
| `SitPlacement` | from Q1 / Q2 / Q18 | `object` / `warp` / `reload` (`reload` only if Q18 and U12 make it fit, §3.9) |
| `RigidRadius` | 1.2 | m; her rigid draws (the rifle) |
| `HideMgsvMarkers` | true | MGSV's green / red marker hues left out while a stand-in is dedicated or an order job runs (as D-Dog's); never wider until the `!capture hud` count (§3.8) |
| `StrikeMgsvPosts` | true | QUIET SCOUT / ATTACK greyed out in the iDroid while the host's layer is on |
| `NoDying` | true | `EnableDying false` while she is out |

**`PhantomRingHost.ini [Quiet]`**

| key | default | meaning |
|---|---|---|
| `enabled` | true (off until §7's exit) | `kHostQuietLayer` |
| `posts` | true | the host picks her posts; false = she is drawn where MGSV has her (today) |
| `cover_rings` / `near_rings` | 25,35,45,55 / 8,12,16 | m from Snake (tuned by U13 after M1's Q7/Q8) |
| `post_dirs` | 24 | |
| `post_height_min` / `post_height_max` | −3 / 25 | m over Snake (near: −2 / 8); the drawable rule (§3.2) usually binds first |
| `post_drawable` | true | the frustum rule for MGSV's ground-level body (off only once Q8 proves `MirrorHeight = mapped`) |
| `post_height_score` | 0.1 | per m, up to the drawable bound |
| `sight_height` | 0.9 | m over her feet: the one point the post, the pick and the shot test from |
| `post_max_3d` | 60 | m |
| `post_enemy_clear` / `post_char_clear` | 15 / 3 | m |
| `post_rays_per_frame` | 30 | |
| `post_research_snake` | 25 | m (near: 20) |
| `move_hide_speed` | 3 | m/s for 2 s: on the move (hidden) |
| `fade_ms` | 300 | her cloak fade for an in-view post change |
| `far_ground_up` / `far_ground_down` | 20 / 40 | m; her ground ray when not at a post |
| `lift_max` | 30 | m |
| `target_range` | 100 | m from her post |
| `target_cone_deg` / `target_cone_binoculars_deg` | 10 / 6 | |
| `target_marked_first` | true | |
| `bosses` | off | `off` only (v1) |
| `pick_latch_ms` | 500 | |
| `spot` / `spot_range` / `spot_every_ms` / `spot_linger` / `spot_text_every` | true / 60 / 500 / 10 / 8 | |
| `lethal_share_mult` / `lethal_fallback` | 1.0 / 1.0 lives | |
| `heavy_poise` | true | Sinful's knockdown |
| `tranq_sleep` | 1.1 | share of his sleep resistance |
| `shot_noise` | false | |
| `cover_threat_range` | 20 | m from Snake |
| `indicator` | true | the reticle |
| `order_text` | true | corner texts for 1-7 |
| `cloak` | hide | `hide` / `shimmer` |

**`PhantomRingHost.ini [Grace]`** (buddy-generic)

| key | default | meaning |
|---|---|---|
| `buddy_rest` | true (off until §7's exit) | seats for a buddy that sets `kBuddyCanRest` |
| `seat_rings` | 2.0,2.5,1.5,3.0 | m from the grace, in search order |
| `seat_dirs` | 16 | |
| `seat_screen_x` / `seat_menu_x` | 0.62 / 0.33 | |
| `seat_step` / `seat_slope_ny` / `seat_spread` | 0.3 / 0.9 / 0.15 | |
| `seat_headroom` | 1.3 | m |
| `seat_snake_clear` / `seat_char_clear` | 1.0 / 2.0 | m |
| `rest_dark` | from U12 | `fade` (Elden Ring's fade state) / `luma` (the frame read-back); none found = no grace rest |
| `seat_black_wait_ms` | 1300 | no observed black within this of the press = no seat this rest (a talk) |
| `seat_deadline_ms` | from U12 (≈ 1000) | after black start; one-frame placement |
| `seat_reload_ready_ms` | 100 | the reload fallback only if the seat is ready this soon after black start (and Q18 fits) |
| `tarnished` | false | seats in Tarnished mode too |

---

## 6. File split

| side | files | owner |
|---|---|---|
| protocol (first, offline) | `protocol/phantomring_protocol.h` (§4), `tools/phantomring/proto.py`, `test_layout.py`; the fakes (§7); empty `[Quiet]` / `[Grace]` sections; **`tests/fixtures/quiet/`**: the study's `survey_rampart.json`, `survey_gatefront.json`, `survey_firststep_real.json`, `perch_gatefront.json`, `perch_firststep.json`, `restlum_firststep.json` (positions and verdicts only, no secrets), copied from the study's scratch folder (§9 I0) | integration I0 |
| guest new | `mgsv/Quiet.h/.cpp`: the handler detour (observe-only first: `sys`, R, `!quiet` console), the hit hook 0x140D04D50, the borrow, the post / mirror holder (`buddyloc::Finder`), the order job, the sit job, the trace. `mgsv/QuietLogic.h/.cpp` (pure): the job machine, the mirror and pull-in, weapon from the attack id, the borrow's bit plan, the sit hold | MGSV agent |
| guest shared | `Follow.cpp` (1-7 exempt from the hold; the hold cleared on a buddy change; `quiet::OnPlayerCommand`; pad[0]); `Frame.cpp` (treadmill and Fulton-anchor Quiet cases; publish the new bits and echoes; read the HostState fields); `Pump.cpp` (Quiet's automatic hint must not wait for `!buddyPosValid`; buddyPos / yaw from the skin when fresh; the borrow's boot step and its re-check after every sandbox load; `keepMarkers \|= quiet::StandInMarked()`); `GuestScript.cpp` (`PR.QuietBorrow` / `PR.QuietRestore`; the iDroid entries); `ActorProxy*` (`Dedicate` for Quiet with its own height; the phase guard keyed on the active buddy type, re-armed on every buddy change, the give-up through a per-buddy `DisableOrders` callback; release on a buddy change); `Capture.cpp` (Quiet's mesh rule); `Hud.cpp` (the marker filter for any buddy's order window); `Addresses.h` + `verify_addresses.py` (§2.11); `Config.*`, ini | MGSV agent |
| host new | `er/Quiet.h/.cpp`: posts, the pick and reticle, spotting, `kGEvQuietShot` effects, texts, the `quiet pick` dry-run console, `Quiet::OnWorldShift`. `er/QuietLogic.h/.cpp` (pure, an injected ray oracle): the post score and rules (the drawable rule, the one sight point, the zero-candidate path), the pick, the shot effect, the far-ground rule. `er/GraceRest.h/.cpp` + `er/GraceLogic.h/.cpp` (pure, an injected ray oracle): `RestDark()` (U12's signal), the grace centre, the seat rule, the gate and the method's deadline, `GraceRest::OnWorldShift` | ER agent |
| host shared | `er/Host.cpp` (expose `AtGrace()` and the rest time; `kHostAtGrace`, `kHostQuietLayer`, the HostState fields; `OnWorldShift` moves `g_gracePos`); `er/WorldShift.cpp` (Quiet and GraceRest in its list); `er/Buddy.cpp` (the far-ground rule; guards per buddy; seated and post placement); `er/RenderScene.cpp` (the relocated-buddy branch, the per-buddy lift clamp, hide and fade); `er/Actors.cpp` (`kActorQuietTarget` at the boundary); `er/Combat.cpp` (fire from a given origin at a handle, with the hostility check); `er/Marking*` (the binocular pick's access); `er/RenderMarkers.h` (`kQuiet`); `er/BuddyOrders.cpp` (texts 1-7, the LB hints); `er/Events.cpp`; `er/Config.*`, ini | ER agent |
| tools | `fake_mgsv.py`: Quiet's id 0x2400; `!quiet status`, `!quiet weapon lethal\|tranq\|heavy\|unknown`, `!quiet post on\|off` (follows `buddyPost*`, echoes the seq), `!quiet seat on\|off` (follows `kHostAtGrace` + `graceSeat`, echoes), `!quiet cloak on\|off`, `!quiet cover on\|off`, `!quiet shot <handle\|pick> [ordered\|hit\|released\|lost\|refused]`; `!order 1..7` with b = 3 and pad[0]. `fake_er.py`: `pr er quietlayer 0\|1`, `pr er actor <i> quiettarget\|noquiettarget`, `pr er grace 1 [dx dz yaw]\|0` (`kHostAtGrace` + seat + seq), `pr er post <dx dz yaw>\|none`; both print the new events, flags and echoes | integration I0 |
| tests | `tests/test_guest_quiet.cpp`, `tests/test_host_quiet.inc`, `tests/test_host_grace.inc`; `test_guest_follow` (1-7); `test_guest_proxy` (a Quiet slot); Capture; `test_layout.py`, `test_fakes.py` | each side |

---

## 7. Test plan

**Offline (no games)**
- `test_guest_quiet`:
  - the job: no pick -> Refused; pick -> Realize -> Ordered ≤ 1.5 s (simulated clock); the hook's hit on the soldier -> Hit ->
    Released; a hit on another soldier ignored; no hit in 8 s -> Lost `kQuietLostMgsv`; Take aim held 20 s; the target dying ->
    Lost `kQuietLostTarget`; a rest, a dismissal, a buddy-type change, `kHEvTarnished 1` -> Lost/Released `kQuietLostCancelled` and
    the stand-in released at once; a Guilty hit with an MGSV-asleep stand-in -> released whatever `KeepMarkMs` says;
  - R's address from a fake system: `[[sys+0xB0]+8] + i*0x200` (two loads), never `[sys+0xB0]+8+…`;
  - the weapon from the attack id (290 / 291 / 292), unknown ids -> unknown;
  - `FireOnlyOnOrders` toggles; the mirror and its pull-in along the camera line (inside `MirrorRegion` unchanged; a bearing with a
    short radius pulled in to it; never nearer than 3 m to Snake);
  - the treadmill correction of `buddyPost` / `graceSeat` (OffsetHistory);
  - the borrow's plan by bits: present -> no calls; recruited-not-deployable -> sortie only; gone -> 3 calls; never recruited /
    killed -> nothing under `memory`, obtained + sortie under `always`; sickbay / dying / dead -> the extra unsets; refused with an
    injected `SavesGuarded() = false`; the put-back restores Quiet's fields and the static byte, and the raw diff of a fake block
    reports a changed Quiet byte as a failure and another buddy's as "explained"; re-applied after a simulated sandbox load that
    dropped sortie; not put back on DISMISS or a buddy change; dismiss-then-restore when she is out at a guard loss;
  - the sit hold: a new seq -> place, then `Enable*`; after a Reload the `Enable*` wait for `active`; flag down -> stand; a seq with
    a NaN seat -> nothing; a drift past 0.3 m re-placed only by a one-frame method;
  - commands 4 (with `Cover = false`) and 7 taken over while the layer is on, passed to MGSV with it off;
  - the stand-in: `KeepMarks` holds the periodic wipe while dedicated, and the wipe resumes after release.
- `test_guest_follow`: 1-7 and 8-16 never set the walker's hold; a buddy change clears it.
- `test_guest_proxy`: Quiet's stand-in at the enemy's height and D-Dog's on the ground; Quiet's stand-in gives up the phase guard,
  then D-Dog is deployed, then an alert -> D-Dog's `DisableAttack` fires (a fresh guard per buddy); mark with Quiet, switch to
  D-Dog within 5 s -> the slot is free and "Get it" proceeds.
- Capture: a skinned draw on her root kept; a rigid prop 3 m from her dropped; her rifle (rigid, 0.6 m) kept.
- `test_host_quiet` (the ray oracle injected):
  - the post rule: the footprint rejects a ledge, the height cap, the drawable (frustum) rule rejects a post 25 m up at 35 m, no
    hostile within 15 m, sight to Snake and to one hostile from the sight point, the score without a bonus past the drawable
    bound, the 20 % hysteresis, the three modes, and the zero-candidate path (Near rings, then the last post, then hidden + log);
  - **the pick never selects a target the shot ray refuses from the same post** (randomized posts and targets, the same oracle);
  - the pick: the cone, binoculars first, sight from the post and from the camera, boss / non-hostile / dead / CQC-held refused,
    tranq never on sleepers, the latch; `kActorQuietTarget` and `kActorQuietThreat` set independently;
  - spotting: the pick's hard rules (a boss and a neutral in sight are never marked);
  - the shot: blocked -> no damage; lethal share; tranq -> sleep; non-hostile at Hit -> refused; `kQuietLostMgsv` -> "no shot
    (MGSV)"; Refused -> "no target -- look at an enemy through your binoculars";
  - the LB hint and the reticle's colour follow `kBuddyQuietMarked`;
  - a 32 m world shift leaves the drawn post unchanged; Tarnished mode freezes the post, hides her past 60 m, and runs no spotting,
    no cover and no searches;
  - the far-ground rule (highest walkable hit, hidden without floor); fade sequencing (fade-out -> publish -> echo -> fade-in).
- `test_host_grace` (the ray oracle fed from `tests/fixtures/quiet/`):
  - the seat rule on the three surveyed graces: the hard rules are re-applied to the recorded candidates' verdicts (walls,
    headroom, footprint spread, camera line), and the new knee ring and footprint are tested on E2's raw `rayall` hits once they
    are recorded (the study's files predate both fixes, so "within 0.25 m of the study's spot" is not a test of them);
  - the menu and Snake-silhouette rules; the fallback;
  - **the gate**: no seat without an observed black; a "talk" rest (the press, no fade, the HUD up, then `ER dialog`) publishes no
    seat and moves nothing; black after `seat_black_wait_ms` -> no seat; a seat found after the method's deadline -> dropped; the
    reload method refused unless the seat is ready within `seat_reload_ready_ms` and Q18's time fits; the host draws her at the seat
    only on an echo inside the black, hidden otherwise; `ER dialog` while seated -> hidden, then the post rule;
  - the asset scan runs on the first black frame, never before;
  - no seat without `kBuddyCanRest`, without the Quiet layer and posts, in Tarnished mode, or on a respawn;
  - a 32 m world shift while seated keeps `kHostAtGrace` (`g_gracePos` moved) and the drawn seat.
- `test_layout.py` (v18 offsets and enums, `kBuddyQuietMarked`, `kActorQuietThreat`, `QuietLostReason`), `test_fakes.py`
  (`kGEvQuietShot` round trip with a Lost reason, `kActorQuietTarget` / `kActorQuietThreat` seen by fake_mgsv, the grace fields seen
  and echoed, the post seen and echoed).

**MGSV + fake_er** (MGSV agent; `fake_er --actors near`, `pr er quietlayer 1`, `pr er actor 0 quiettarget`, `pr er post`, `pr er
grace`; frames through `gamescopectl screenshot`; the player's save, borrow in memory, **the shipped `[Proxies]` / `[Boot]`
config**, logged at the start):
- M1's probes Q1-Q18 (§2.10), each with a log line or a frame.
- The order through MGSV's real call menu (pad file: binoculars on the stand-in, LB, the Fire entry, R3) with `[Proxies] Enabled =
  false`: `quiet: order 1 for ER 0xffffffff00001000 -> soldier 0x450 ... Ordered`, `quiet: hit 292 on 0x450 -> kGEvQuietShot Hit`;
  fake_er prints the phases. Fire still live 20 s after the binoculars went down.
- Posts: 10 posts via `pr er post` with treadmill runs: her MGSV place within `PostTolerance` of the mirror, her draws counted every
  frame (> 20 while MGSV's camera holds her), and 0 props.
- The sit: `pr er grace 1 2 1 90` -> placed within 2 frames, `kBuddySeated`, then frames of her at rest: **the rifle lowered and
  not aiming in ≥ 9 of 10 frames** (or, if nothing lowers it, the v1b facing checked and the open question raised); `pr er grace
  0` -> up.

**Real ER + fake_mgsv** (ER agent; `--er-headless 3840x2160`, the virtual pad, `pr er "hud shot"` frames; **send `!autofire off` to
fake_mgsv right after starting it**: its default shots drew a Gatefront soldier in the study; `combat set hurt_mult 0` near enemies, 1
after):
- **U12 first**: the black signal over 10+ rests (three graces, a re-rest, Pass time, Level Up), before any seat is published.
- **The grace rest** at Rampart Tower, Gatefront and The First Step (rested at without a talk), plus U10's scaffold or stair grace
  and cave grace (chosen from graces the save has rested at without a talk), with `!buddy quiet` + `!quiet seat on`: ten rests or
  more. A frame every 0.1 s from the press to +3.5 s: the seat published only inside the observed black, the fake buddy drawn at the
  seat only after the echo inside the black, visible right of the menu, not on Snake; Pass time once and Level Up once (U8); U11's
  rests with the camera turned 90° and 180°; B, then a walk of 2 m -> up and away. One Tarnished-mode rest (U9: no seat).
- **Posts**: U13 first (the design's rule at both places, the raw rays kept as fixtures); Gatefront's soldiers: frames of her post
  (feet on the ground within 0.3 m, nothing floating); the post's search time (U1); one Tarnished-mode round trip.
- **The pick and the effect**: U14's `quiet pick` dry-run at The First Step (Varré, the grace's hidden character and the large
  enemy refused, from the grace, nothing fired); `fake_mgsv !quiet shot pick hit` with each weapon on fresh soldiers: lethal ->
  damage or death; tranq -> `ASLEEP`; heavy -> knockdown; a post with no line -> "no clear shot", no damage; `!quiet shot pick lost`
  -> "no shot (MGSV)". A boss refusal by unit test and by `pr er quiet test boss` (treats the pick as a boss; no real boss engaged).

**Both games** (integration; saves backed up by `pr launch`, restored and sha256-checked at the end, with the files counted and
named):
- Deploy her by the borrow; three rests; ten orders over 10 minutes at Gatefront; D-Dog, D-Walker and D-Horse regressions (deploy
  each; one order or ride each); a switch to D-Dog within 5 s of a binocular look, then "Get it".

**Exit criteria** (each part is switched on by default only when its own line is met; **the grace rest only together with posts
and drawing**):
- **The grace rest** (`[Grace] buddy_rest`, `[Quiet] RestAtGrace`; requires the posts line below): U12 found a black signal, and
  ≥ 10 rests at ≥ 5 graces (the three surveyed, U10's scaffold or stair grace and cave grace) with the real MGSV:
  - she is at her seat in the frame at +3.0 s in ≥ 9 of 10, and in the other one she is simply not there;
  - **no frame from +2.5 s to the menu shows her popping in or vanishing** (the 0.1 s series);
  - **her rifle lowered and not aiming in ≥ 9 of 10 rest frames** (v1); if Q2 found nothing that lowers it, this line is
    replaced by the player's answer to open question 2: **v1b's watch, accepted** (her rifle pointed away from Snake and the
    light, facing out from the grace, in ≥ 9 of 10 rest frames);
  - never inside geometry, on Snake, behind the menu, or within 2 m of another character;
  - U11: 3 rests with the camera turned 90° and 180°: a visible seat or the clean "not there";
  - she stays through Pass time and one Level Up screen;
  - she is up and gone (no pop in view) within 3 s of leaving in 10 of 10;
  - a fast travel and a death un-seat her; the offline "talk" rest test passes;
  - 0 warnings; MGSV's 7 files and the ER files sha256-equal after the restore.
- **Posts and drawing** (`[Quiet] posts`): 10 minutes of runs, stops and fights, at least one world shift seen in the log:
  - her drawn body never moves over 0.5 m in a 60 Hz frame in view, except inside a cloak fade;
  - her feet within 0.3 m of Elden Ring ground in every sampled frame at a post;
  - 0 props drawn as her; every in-view post change faded; she is drawn (not empty) at ≥ 9 of 10 sampled posts in view;
  - one Tarnished-mode round trip: no spotting, no cover, the post frozen, a post searched on return.
- **Orders** (`[Quiet] enabled`, `Orders`): 10 minutes, ≥ 10 orders through MGSV's real call menu on hostile Elden Ring enemies,
  with the shipped `[Proxies]` / `[Boot]` config:
  - every Hit applied only with a clear line from her post, with the weapon's effect;
  - **≥ 9 of 10 orders with a clear Elden Ring line from her post land** (each Lost logged with its reason);
  - 0 MGSV alerts; 0 `quiet:` warnings;
  - never a non-hostile or boss pick, and U14's dry-run refusals at The First Step;
  - D-Walker's following not paused by any of them; D-Dog's first "Get it" after a switch works.
- **Borrow** (`Borrow = always` and `BorrowListInIdroid = true`, the player's chosen defaults; per save state): Q14 passes twice for that state (deploy, dismiss,
  switch and back, a sandbox reload, the put-back while out and after dismissal, the raw diff clean for Quiet's bytes; a guest
  shutdown with her out, then the read-out at the next launch equal to the save's).
- **Cover me** (`Cover`): 5 minutes covering with 3+ hostiles: one shot per ≥ 8 s, only at the threat pick, 0 MGSV alerts, and
  Q17's figure never hit.

---

## 8. Risks (ranked) and fallbacks

1. **Her AI doesn't hold still when moved** (Q1), as D-Dog's Wait didn't. Then there is no mirror. Fallbacks: branch N (she is seen
   less, and her aim is MGSV's); `WarpToPosition` instead of the object. For the grace, **only a one-frame method** fits the black;
   ReserveCallBuddy NORMAL + Reload fits only if Q18 and U12 show it does (with the study's numbers it doesn't, §3.9), so without
   Q1 or Q2 there is no grace rest.
2. **No observable black** (U12): neither Elden Ring's fade state nor an affordable frame read-back is found. Then there is no safe
   gate, and the grace rest stays off (a timer can't tell a black from a talk, §2.9).
3. **Nothing lowers her rifle** (Q2): the only kneel seen has it up. Fallback v1b (facing out, on watch), which the player accepted
   (open question 2): the rest ships with it, and the README says she kneels on watch. Never shipped silently as "resting".
4. **MGSV doesn't draw her where Elden Ring wants her** (Q7/Q8): occlusion by the airport buildings, an apron that isn't free,
   distance LOD, a high post outside MGSV's vertical view. Fallbacks: `MirrorRegion` and the pull-in, `MirrorHeight = mapped`;
   the drawable rule keeps high posts out by default. Otherwise she is unseen at such posts: acceptable for a sniper, but logged.
5. **The menu's Fire stays greyed** (Q4): the periodic wipe (now held off while a stand-in is out), an elevated enemy the
   binoculars can't mark on a stand-in MGSV won't hold up there, or a moving or released stand-in. Fallbacks: the narrower
   per-id wipe; `StandInHeight = ground` with a nearer post; keep the stand-in through the order and refresh the mark with a
   binocular look; v2 RE of the menu's enable check.
6. **MGSV's geometry vetoes shots Elden Ring allows** (Q5): "no shot (MGSV)" at random from where Snake stands. Fallback:
   `StandInOnBearing`; the Orders exit line counts it.
7. **She fires again at the immune soldier** (Q5): only the job's first hit counts, `FireOnlyOnOrders` disables her fire, and the
   soldier is released after the hit (always after a Guilty hit that sleeps him, Q11).
8. **Her Cover me hits something of MGSV's** (Q17's figure, an asleep proxy): `FireOnlyOnOrders` keeps her fire off outside jobs;
   with `Cover = false` command 4 never reaches MGSV and its cover bit is turned off. Cover ships off until its exit line.
9. **The borrow doesn't survive a sandbox load, or doesn't load on other saves** (Q14): re-applied after each load; safe failure:
   put back, "Quiet can't join on this save". Off by default anyway (the player's choice).
10. **No seated motion** (Q3): v1 kneels. Tell the player. It is MGSV's own plain standby, never a suggestive motion.
11. **The seat search costs too much** (U1): the ordered search stops early; worst case 64 × 25 rays over 45 frames, and the
    deadline drops a late seat. Fallback: 32 candidates on r = 2.0 / 2.5 only.
12. **The grace lookup** (U2/U3): fallback, the player's facing; at worst a seat that is 0.3-0.5 m off the ideal ring (still rule-
    checked).
13. **A rest that becomes an NPC talk**: no black, so no seat and nothing moves (safe by construction); an `ER dialog` while seated
    hides her. Only tested offline; live tests never use such graces.
14. **The design's post rule passes too few candidates** (U13): the zero-candidate path (Near rings, the last post, hidden); retune
    the rings after Q7/Q8.
15. **The slip and treadmill nudges break her standby**: the host's translation keeps Elden Ring's picture right regardless; the
    guest re-places only past 3 m. **World shifts** are followed by `OnWorldShift` (§3.2, §3.9).
16. **Her draws without the forward pass** look incomplete (Q9). Snake has the same and looks right; if she doesn't, capture the
    forward pass for her draws (a capture change, out of this round).
17. **Dog-only names in shared code** (the phase lock's `dog::DisableAttack` and `g_dogRan`, `dog::KeepBuddyDraw`, the guards'
    `buddyType == kDog`): generalized per buddy type, the phase guard re-armed on every buddy change (§3.5), so D-Dog's behaviour
    is unchanged (its regression tests stay green, plus the switch tests of §7).
18. **The study's fixtures are lost** before I0 (they live in a session scratch folder): E2 re-records them (U13 and the seat
    survey), and E1's grace tests wait for that.

---

## 9. Ordered plan for two live agents + integration

**I0 Integration (offline, before the live agents):** **first copy the study's recorded files into `tests/fixtures/quiet/`**
(from the study's `er-grace/` scratch folder, done in round I0:
`survey_rampart.json`, `survey_gatefront.json`, `survey_firststep_real.json`, `perch_gatefront.json`, `perch_firststep.json`,
`restlum_firststep.json`; positions, verdicts and luminance only, no secrets; with a README naming the session and what each
holds). Then protocol v18 (§4), `proto.py`, `test_layout`, the fakes' commands (§6), empty `[Quiet]` / `[Grace]` sections
(everything off). Builds and `check_host` / `check_guest` / `check_windows` green; `test_fakes` with the new checks.

**MGSV agent (real MGSV + fake_er on `/dev/shm/phantomring_g`; saves backed up by `pr launch`, restored and sha256-checked; the
shipped `[Proxies]` / `[Boot]` config)**
- **M0** (small, first): Follow.cpp. Commands 1-7 are exempt from the hold, a buddy change clears it, and `test_guest_follow`
  covers both. Verify live: Quiet's orders, then D-Walker leads without a whistle.
- **M1, live RE with an observe-only build**: `!quiet` (the handler detour: `sys`, R through **two loads**, the provider, the
  weapon), the hit hook in log-only mode, the borrow by console (with the raw snapshot and diff), `!quiet scan` / `shift`,
  `!quiet borrow-test unguarded`, the `KeepMarks` hold behind a console switch. Run Q1, Q2, Q18, Q4, Q5, Q6, Q7, Q8, Q9, Q10, Q11,
  Q12, Q14, Q15, Q16 and Q17 in that order, and Q3 offline in parallel. **Decide branch M or N, the placement method (and whether
  the grace has one), `MirrorRegion`, `StandInHeight`, `StandInOnBearing`, the rifle command (or open question 2) and the
  `QuietWeapon` mapping, report Q7/Q8 to the ER agent before E2 tunes posts, and write them into this document.**
- **M2, build**:
  1. the borrow (by bits, re-applied after loads) and the put-back with the raw diff;
  2. the mirror holder and the treadmill case;
  3. Capture's Quiet rule;
  4. the stand-in (`MarkProxy`, its height, `KeepMarks`, `kBuddyQuietMarked`, `StandInOnBearing`), the per-buddy phase guard, and
     the order job with the hit hook;
  5. commands 4 and 7 taken over;
  6. the sit job (placement first, then `Enable*`);
  7. `Frame.cpp`'s publishing;
  8. the sites (the three `kQuietRecord*` among them) and `verify_addresses.py`;
  9. the unit tests.
- **M3, live with fake_er**: the MGSV list of §7; 10 minutes with 10 orders, 0 alerts; recordings into `recordings/` (never
  committed): `quiet_orders.prrec`, `quiet_rest.prrec` (a deploy, a run, a binocular Fire, a rest, the walk-away) for the host's
  regressions.

**ER agent (real ER + fake_mgsv on its own mapping; `!autofire off` first)**
- **E1, offline build**: `GraceLogic` + `GraceRest` (the gate, centre, seat, the method's deadline) first, then `QuietLogic`
  (post with the drawable rule and one sight point, pick, effect, far ground) + `Quiet`; `OnWorldShift` for both and `g_gracePos`;
  the shared changes; the unit tests on an injected ray oracle fed from `tests/fixtures/quiet/`.
- **E2, live**: **U12 first** (the black signal; nothing else of the grace before it), then U13 (the design's post rule at both
  places, raw rays recorded as fixtures, after M1's Q7/Q8), U14 (`quiet pick` dry-run), then the grace rest list (§7), posts, the
  pick and the effects; U1-U14, each with a log line or a frame.

**Integration (both games, after M3 and E2)**: the both-games list of §7. Turn on what met its exit line: **posts and drawing
together with the grace rest** (the player's ask; the rest never ships on 0.8.0's Quiet), then orders; **`Borrow = always` and `BorrowListInIdroid = true` become the shipped
defaults (the player's answer, 2026-11-04), each save state switched on only once Q14 passed twice for it** (gone on the player's
own save; never recruited, killed and recruited-not-deployable simulated in memory); a state that fails stays off with a log line
and a README note. Then the README, GUEST.md 8.3's outdated line (§1), ROADMAP 2.5's ShotSolver
assumption, and STATUS. **The README items**, each a sentence or two in "Ordering Quiet" / "Resting with Quiet" / Known issues:
- look at an enemy through the binoculars before Fire; Fire and Take aim stay greyed until MGSV marks him; "no target" means none
  was in view;
- the save cases: she deploys if the save has her; otherwise she is borrowed for the session by default (`[Quiet] Borrow =
  always`, listed in the iDroid's BUDDY SUPPORT; `memory` for only a Quiet the save has had, `off` for none), memory only, MGSV's
  save untouched; any save state that failed its live check named; the bond is the save's (0 when she left), so Take
  aim / Cover me may be missing on some saves (Q12);
- she is unseen (cloaked) while Snake travels and appears at her post when he stops (open question 3: as designed);
- the rest: what she does (the kneel, rifle lowered, or v1b's watch, accepted by the player), when she gets up; no rest in Tarnished
  mode, nor when a grace starts a talk, nor with the Quiet layer off;
- Cover me and Shoot this are not in Elden Ring yet; the talk entry only shows "Quiet";
- "no shot (MGSV)" means MGSV's own map blocked her, not Elden Ring's;
- in Tarnished mode she holds her post and stays out of the fight;
- Known issues: whatever the exit rounds leave [U].

**Later (not this build)**: D-Dog lying down at the grace (place it at the seat, then its Wait: [U] whether MGSV's waiting dog
sits or lies down); Shoot this (Q13); a true seat (Q3); a host muzzle flash at her post; Elden Ring's enemies reacting to her.

---

## 10. Reproducing the RE

MGSV (offline; `EXE=<steam-library>/common/MGS_TPP/mgsvtpp.exe`):

```sh
x86_64-w64-mingw32-objdump -d -M intel --start-address=0x140D04D50 --stop-address=0x140D04DF9 $EXE   # her hit sender
x86_64-w64-mingw32-objdump -d -M intel --start-address=0x140D06520 --stop-address=0x140D06650 $EXE   # her shot action: R+0x186, the call
x86_64-w64-mingw32-objdump -d -M intel --start-address=0x140A4E0B0 --stop-address=0x140A4E2D8 $EXE   # the command function, cases 1-7
x86_64-w64-mingw32-objdump -d -M intel --start-address=0x140CE3B25 --stop-address=0x140CE3B7C $EXE   # R's two loads (Fire case)
x86_64-w64-mingw32-objdump -d -M intel --start-address=0x140D04600 --stop-address=0x140D04640 $EXE   # R's two loads (processor)
x86_64-w64-mingw32-objdump -d -M intel --start-address=0x140A53EB0 --stop-address=0x140A53F10 $EXE   # SetDisableBuddyType's static byte
python3 tools/re/mgsv/pdata.py $EXE 0x140d04df4 0x140d0663c                                          # containing functions
GHIDRA_PROJECTS=<abs path>/ghidra-projects nice -n 19 tools/ghidra_query.sh mgsv \
  dec:0x140ce3330 dec:0x140ce1e10 dec:0x140d04600 dec:0x140d06310 dec:0x140a52140                   # handler, SendCommand, processor, shot, gate
```

- The callers of 0x140D04D50: scan `.text` / `.xtext` for `E8 rel32` whose target is 0x140D04D50; there is one, at 0x140D0663C.
- The message id's other users: search for the bytes `74 32 e0 e2 fe 5c 00 00` (6 hits: 0x140D03ACB, 0x140D03B5E, 0x140D04D84,
  0x140EDAE29, 0x141362301, 0x14146DD25).
- The SendCommand names are StrCode32 (`tools/re/strcode32.py`) of candidate names, kept only when the fields read agree.
- Motion names: CityHash64 path codes of the community mtar dictionary's entries, matched against the mtar's hashes (51 bits).

MGSV live (the guest's console; read-only except the borrow, saves guarded):

```lua
-- the state (one value per line in the study's qstate.lua)
return BuddyType.QUIET, TppBuddyService.DidObtainBuddyType(BuddyType.QUIET), TppBuddyService.CanSortieBuddyType(BuddyType.QUIET),
  TppBuddyService.CheckBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_LOST), TppBuddyService.GetFriendlyPoint(BuddyFriendlyType.QUIET),
  gvars.str_didLostQuiet, TppStory.IsMissionCleard(10050), TppStory.IsMissionCleard(10260), vars.obtainedBuddyType,
  vars.sortieBuddyType, vars.buddyCommonFlag, vars.buddyQuietEquipType, vars.buddyCallCount[BuddyType.QUIET]
-- the borrow (memory only), then '!buddy quiet'
TppBuddyService.SetObtainedBuddyType(BuddyType.QUIET); TppBuddyService.SetSortieBuddyType(BuddyType.QUIET)
TppBuddyService.UnsetBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_LOST)
-- her place
return GameObject.SendCommand(GameObject.GetGameObjectIdByIndex('TppBuddyQuiet2', 0), { id = 'GetPosition' })
-- placing her by a block reload (~1 s gone)
TppBuddy2BlockController.ReserveCallBuddy(BuddyType.QUIET, BuddyInitStatus.NORMAL, Vector3(x, y, z), 0); TppBuddy2BlockController.Reload()
```

- Her hit, live: `!proxy on` (note: the one config without the periodic marker wipe, §2.4) with fake_er's actors 20 m and 27 m ahead, the binoculars on the soldier, LB > Fire, with `!dog trace on`
  and `!dog watch <soldier id>` (the message box send detour logs `message 0x5cfee2e03274 (u16 +8 <attack id>) sent to <id> from
  0x140d04df4`).
- The damage rows: `kDamageParams` 0x142A1A130 (vtable 0x1421B1BD8 checked), row = +8 + id × 0x1A, u16[0] lethal, u16[1] non-lethal.
- The cloak: `!capture probe 12` fired when her GetPosition moved faster than 12 m/s.

Elden Ring live (the host's console):
- The grace: `geom all 30 AEG099_06` (the asset), `peek player+0x6C0` / `+0x6D0` / `+0x80` (block position and id, physics minus
  block), the row's `pos` from `BonfireWarpParam` (regulation.bin through `tools/re/er_params.py`).
- The rest's timeline: `hud shot` every ~0.1 s from the press and each frame's mean luminance (one series, The First Step); the HUD
  times from the host log's `ER HUD hidden (UI state 7)` against `rested at a grace` (six rests).
- The post recount (§2.9): `perch_*.json` with the design's height (−3..+25 m), 3D distance (≤ 60 m), `ny ≥ 0.8`, headroom, sight to
  Snake, ≥ 1 enemy seen and the nearest enemy > 15 m.
- The seat and post surveys: `call rayall` and `call ray <from> <to> 0x2000016` per candidate (the floor, the footprint, the
  headroom, the knees, the sight lines), the candidates projected with the camera of the rest (`pr er cam`).

---

## 11. Integration (0.9.0-alpha, 2026-11-04): what shipped vs. what was planned

Both games together (real MGSV 1.0.15.4 + Elden Ring 1.17.2, the shipped inis, `--er-headless 3840x2160`, the exclusive virtual pad;
`hurt_mult 0` near enemies, 1 after; four launches 12:05-13:10; saves restored from `backups/20261104-120515`: ER 8 of 8 and MGSV 7 of 7
files sha256-equal to before the launch, MGSV's already equal before the restore). The ER and MGSV rounds' results are in §2.10 (M1-M3)
and docs/HOST.md (E1-E2); this is what the integration added. Evidence: docs/STATUS.md "0.9.0-alpha".

| §0 row | Planned | Shipped |
|---|---|---|
| 1 Borrow | `always` + `BorrowListInIdroid`, each state after Q14 | as planned (the player's answer); **the borrow waits for the host's `kHostQuietLayer`** (review: otherwise every save would have listed 0.8.0's Quiet) and is re-applied when Elden Ring comes back after "Elden Ring is gone" (live: put back CLEAN, borrowed again 15 s later, put back CLEAN again). Q14 passed once per state (M1-M3), not twice: the gone state again here on the real save. **Quiet (Scout) / (Attack) are greyed only while she is out**: greyed from the boot (§3.1's rule) they couldn't be chosen, so no one could deploy her (live); Quiet (Attack) deployed her by her HALO drop |
| 2 Posts | the host's post, the mirror, cloaked while Snake travels | as planned, on. Live: posts published and echoed at once; kneeling on a rock ledge +4.6 m at Gatefront (her MGSV marker under her: it sits on MGSV's ground). The holder now waits 250 ms after a treadmill jump (the host's post, corrected by k 33 ms ago, was off by the jump for a frame: placed 5.9 m and back). "On the move" (no hostile within 60 m) not re-shot with both games |
| 3 Fire / Take aim | the job on MGSV's own menu | **built, opt-in** (`[Quiet] orders`, new host key: the pick, the reticle, the LB hint; the guest's switches are unchanged and act only on a published pick). One try at Gatefront: the stand-in marked, the "sniping position" request brought her beside Snake, Fire never live. Review fixes in: her "can't" ends the order, her dash is no round, an unknown rifle is refused (never lethal) |
| 4 Spotting | provisional marks from her post | as planned, on (live: Stormveil, Gatefront) |
| 5 Cover me | opt-in after Q16/Q17 | not built in Elden Ring (command 4 taken over: "not in Elden Ring yet") |
| 6 The grace rest | seated inside the black, the echo inside the black, v1 kneel with the rifle lowered | **on**, with two changes: MGSV's Quiet takes ~3 s from her MoveToPosition to settle into her rest (a turn, a hop of up to 0.7 m, the crouch with the rifle upright) while the black lasts ~1.5 s -- she hopped in view 1.5 s into the menu. Her order now goes out from 2.5 m off the seat; `kBuddySeated` is echoed once she has settled (`kSitSettleMs` 3200 after the order, still 300 ms, at most 4500); an echo after the black is **faded in** (`[Grace] seat_late_echo_ms` 6000; before: hidden through the rest). And **getting up**: her get-up order (MoveToPosition with aim) is a cloaked dash that vanished her in view and that the holder can't override -- she now keeps her rest pose at her posts (placed by her object, which keeps it) and gets up only while she has no post (unseen). 9 rests with the final build at all three graces (one with the camera at 90 deg, one after a world shift), each faded in beside the grace, through Pass time and Level Up |
| 7 Texts | as listed | as listed; with orders off the order texts can't come (Fire / Take aim greyed) |
| 8 Follow.cpp | orders don't pause D-Walker | as M0 (D-Walker led live after Quiet in this round too) |

Also found live and fixed (not Quiet's): **Pass time gave Snake the pad inside the grace menu** since 0.7.0's `[Overlay] er_bars`
(ErBars re-hid Elden Ring's HUD 1.5 s after the interaction ended; the grace menu's own hide ~4 s later was masked). Elden Ring's bars
now stay its own while Snake sits at a grace.

**Exit criteria (§7) as met**: the grace rest -- 9 of 9 at her seat once faded in (the "+3.0 s" line replaced by "faded in once settled",
~1-2 s after the menu), no pop-in or vanish in the frame series, her rifle upright and not aiming in every rest frame after the fade,
never on Snake or behind the menu, U11 (90 deg) visible, Pass time and Level Up stayed, up and moved on out of view; U10 (stairs,
scaffold, cave) not run (only these three graces are allowed). Posts and drawing -- 10 minutes at Rampart Tower (mostly indoors: 67
of 93 samples drawn at a post or the seat, 26 hidden), 0 warnings; no per-frame trace of her drawn body with both games. Orders --
not met (opt-in). Borrow -- met for the gone state with both games; the other three states by M1-M3's simulated round trips.

**Next**: RE of her snipe-point choice so she snipes from the mirror (orders on by default); a host-side per-frame trace of her drawn
body; the seated pose (Q3); Cover me; a grace on stairs / in a cave.
