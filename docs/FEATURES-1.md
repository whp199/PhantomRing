# Features round 1 (2026-10-14): what was merged, its switches, and the live test plan

Ten features built in parallel on protocol v13 (`kGuestBinoculars`, `kGuestDiving`, `kGEvTimeSkip`, `kGEvCqc` + `CqcAction`,
`kOwnerProjectile`), each reviewed and fixed, then merged offline into one branch (`features-1-merged`, on master 384530e).
Then three more, merged into master after the player's session 7 (Stormveil): **solid ground** (`feature/ground`), **the
jump** (`feature/jump`, protocol v14 `kGuestJumping`) and **decoys** (`feature/decoy`, v13 `kAmmoDecoy`) -- "Round 1b" below.
**Live pass A (below) ran the round 1b items and plans 1, 2, 3, 6 and 8 with both real games; live pass B the rest (4, 5, 7, 9,
10, 13) and a regression pass over README's How to play: every feature has now been run with both games (0.4.0-alpha).** Offline, after the round 1b merges and docs:
`check_host`, `check_guest`, `check_windows`, `test_layout` (117 + 529 values), `test_fakes` (51/51), `test_plugins` (30/30),
`test_replay` (27/27), `test_release`, `test_launcher --unit`, `mgsv/verify_addresses.py` (all signatures match) and
`verify_er_addresses` (71/71: + `collision layer 0x16`) all pass; the build has no warnings. New MGSV addresses (C4, D-Horse,
proxies) are in `mgsv/Addresses.h` and byte-checked; the one new Elden Ring address is ground's collision layer table.

## Switches at a glance

Off by default unless safe and purely additive. Host = `PhantomRingHost.ini` (in `build/`), guest = `PhantomRingGuest.ini`
(in MGSV's folder; `pr launch` installs it).

| Feature | Switch | Default | Docs |
|---|---|---|---|
| Keyboard and mouse play Snake | host `[Keyboard] enabled` (+ `chord_tap = view` since pass B) | **off** (opt-in; **live pass B: works** with injected keys and mouse) | README How to play, HOST.md "Keyboard and mouse" |
| Binocular marking | host `[Marking] enabled` (`rendered_only = true`) | **on** (reads and rays only; **live pass A: works**) | README, HOST.md "Marking" |
| Phantom Cigar = Elden Ring time skip | guest `[Cigar] TimeSkip`; host `[TimeSkip] enabled` | guest **on since live pass B** (was off; the cigar is put into a free item slot at boot); host on | README, GUEST.md 18.16, HOST.md "Phantom Cigar" |
| Dive = hop | host `[Puppet] hop` (`hop_*`); guest `[Motion] DiveFlag` | host **on since live pass A** (was off); guest on | README, HOST.md "Dive = hop", GUEST.md 18.17 |
| CQC holds and throws | host `[Cqc] enabled`; guest `[Cqc] Enabled` (`MgsvHold = false`) | host **on since live pass B** (was off); guest on | README, COMBAT.md 7a, GUEST.md 21 |
| Projectile meshes / muzzle flashes | host `[Render] projectile_meshes`, `[Combat] muzzle_flash`; guest `[Capture] ProjectileMeshes` | all **off** (meshes need host and guest on; **live pass A: neither works yet**, see below); live switches `pr er "throw meshes on"`, `pr er "boom set muzzle_flash on"` | README Guns, HOST.md "Projectile meshes and muzzle flashes", GUEST.md 15.5 |
| C4's detonator | guest `[Combat] C4Detonator` / `C4Backup` / `C4DetonateAll` | detonator **on** (watches MGSV's own Y); backup **on since live pass A** (MGSV's own never answered); all-at-once off | README C4, GUEST.md 18.8a |
| D-Horse treadmill | guest `[Buddy] HorseTreadmill`, `HorseMoveRider` | `HorseTreadmill` **on since live pass B** (was off); `HorseMoveRider` off | README D-Horse, GUEST.md 8.1a |
| Inert full proxies | guest `[Proxies] Enabled` (+ `Inert`, `PhaseLock`, `Height = ground`, `AimHeightFix`, ...) | **off** (live pass B: safe -- phase 0 for 11 min -- but no aim assist seen) | README, GUEST.md 20.6 |
| Material colours | guest `[Capture] Revalidate`; host `[Render] role_guard`, `poster_check` (`off \| log \| on`) | Revalidate **on**; both host checks `log` (count and log, change nothing on screen) | README Known issues, HOST.md "Material roles", GUEST.md 15.3 |
| *Round 1b* | | | |
| Solid ground (5 ground probes, the characters' collision layer) | host `[Puppet] ground_probes`, `ground_filter`, `wall_filter`, `wall_filter_also`; `ground_keep`, `ground_layers` (live pass A: Snake's own ray query) | **on**: `ground_probes = true`, `ground_filter = 0x2000016`, `wall_filter = 0` (= `ray_filter` 0x2000058), `wall_filter_also = 0x2000016`; `ground_keep = 0`, `ground_layers = 0xFB` (= the game's cast_ray); the way back: `ground_filter = 0x2000058`, `wall_filter_also = 0`, `ground_probes = false` | README Known issues, HOST.md "Collision filters and the ground under Snake", "In the air" |
| Jump (R3) | guest `[Jump] Enabled`, `Button` (rs), `PassWith` (lb rb lt + the D-pad since pass B), `Lift` (1.0), `MaxAirMs` (2000); host `[Puppet] jump`, `jump_apex` (1.2), `jump_min_room` (0.15), `jump_air_control` (3), `jump_air_step` (0.3), `jump_camera_hold` | **on** both sides (the player asked for it); kinematic motion only | README controls / Known issues, HOST.md "Jumps", GUEST.md 6 "Jump" |
| Decoys | host `[Decoys] enabled` (+ `range` 15, `active_range` 25, `cone` 70, `stare` 6, `cooldown` 15, `pop_distance` 1, `snake_close` 4, `shock_radius` 2.5, `height`, `turn`); guest `[Combat] Decoys`, `DecoyPopInMgsv` | host **on** (`turn` **off**); guest `Decoys` **on**, `DecoyPopInMgsv` **off** (**live pass B: works**: thrown with LT+RT) | README Decoys, HOST.md "Decoys", GUEST.md 18.8 |

## Merge notes

- Branches merged (`--no-ff`, in this order): `feature/kbm`, `feature/binoculars`, `feature/cigar`, `feature/divehop-fix`
  (= `feature/divehop` + its review fixes), `feature/cqc-fixes` (= `feature/cqc` + fixes), `feature/projectiles`, `feature/c4`,
  `feature/horse-fix` (= `feature/horse` + fixes), `feature/proxies-fix` (= `feature/proxies` + fixes), `feature/materials`.
  None left out.
- Conflicts were all "both sides added something at the same spot" (Config, the inis, Events/Console dispatch, test lists,
  CMake test targets, README, docs): both kept. The cigar and the dive both took GUEST.md §18.16: the dive is now **§18.17**.
- Two fixes where features met (their own commits):
  - **Dive hop x master's stairs fix (384530e):** the hop's take-off head room now measures each probe from its own ground,
    as `BodyHeadroom` does; at the foot of stairs the next step was read as a ceiling and the hop refused. New synthetic lane
    (stairs, x = 80 in `tests/test_host_hop.inc`): fails without the fix, full hop with it.
  - **Projectile meshes x proxies' aim fix:** a rocket's draws are tagged along MGSV's own shot direction, not the copy the
    proxy aim correction turns toward an Elden Ring enemy (MGSV's rocket doesn't follow that copy).
- Docs: `pr guest '...'` (no such command) is `pr lua '...'` throughout.
- Cross-feature: with `[Keyboard]` on, V held while not aiming = RB = binoculars, LMB held = RT held = a CQC grab (with
  `[Cqc]` on), Space = X = dive (and the hop with `[Puppet] hop` on), E/F = Y = C4's detonator with C4 in hand. With full
  proxies on, MGSV's own marker shows on a stand-in as well as the host's binocular triangle on the enemy.

### Round 1b merge notes (ground, jump, decoys; on master 3d59f15)

- Merged `--no-ff` in this order: `feature/ground` (94d1c93), `feature/jump` (5a2214c), `feature/decoy` + its review fix
  (86abf9a). Docs, Config, the inis, README and test lists: "both added" conflicts, both kept.
- **Decoys review** (its review step never ran): equip ids, event routing, stare bits and the C4 path checked; one fix on
  the branch (373ebcc): decoys the host drops without a pop (set before a load, re-announced but unknown) are told to the
  guest with `kHEvDetonate`, as the mines' discard, so it stops re-announcing them at every puppet start.
- **One ground model** (`er/Kinematic.*`, hand-merged): ground's five-probe `BodyGround` everywhere -- standing, walking,
  falls, `SupportedStart` and the landing of a jump and of the dive's hop (`LandingGround`). The jump branch's own seam/gap
  bridging (`GroundUnder`, `[Puppet] ground_ring`) is dropped: the probes cover both its cases (its `TestScaffold` is kept on
  the merged model). Two rules the merge needed: a landing on side probes alone only where the ground snap keeps him (a hop
  landed on a 0.6 m ledge's rim and the snap put him back at its foot); the landing segment ends 0.05 m below the frame.
- **One air** (`AirStep`): a hop and a jump fly the same way (own velocity, the air's knee ray, the horizontal ceiling refusal
  from each probe's own ground -- master's head-room fix --, the head check, `LandingGround`, a landing counted only below
  the take-off); gravity at the frame's mean speed for every airborne frame; no hop in a jump, no jump in a hop. A plain
  fall's knee ray is low too (`kFallStep`, 0.1 m): with step_up's 0.55 m a falling body walked into a ledge's face below its
  top and "landed" inside it (a 6 m/s walk crossed a 2 m gap).
- Behaviour changes from the branches' own tests, adapted with notes: two probes hold a walking body at an edge until his
  centre is up to 0.8 x radius past it (ground's rule: the hop's off-a-rim test and the scaffold's end); a level hop lands at
  0 m/s (was ~3.4: no damage either way); 4 m/s into a 1 m gap now falls in.
- Cross-feature: with `[Keyboard]` on, the middle mouse button = R3 = the jump (with Q = LB held, the call menu's select).
  The walk-off log (`puppet: Snake left the ground ...`) says what both filters and all five ground rays see; it skips a
  jump's and a hop's take-off.

## Live pass A (2026-10-14 night, both real games, Stormveil/Roundtable Hold/Limgrave)

Both games on the default mapping (`pr launch --er-headless 3840x2160`, virtual pad), five Elden Ring launches (new host DLLs)
and three MGSV launches; the saves taken at the first launch (`backups/20261014-215529`) restored at the end. Results:

| Item | Result | Evidence |
|---|---|---|
| Solid ground (11) | **works**: Snake stands, walks and climbs on Stormveil's cliffside wooden scaffold | Tarnished mode off on the planks of the zig-zag stairs' landing: `below the start: -0.05 m (ground_filter), -1.39 m (ray_filter 0x2000058)` (another start there: +0.00 / -0.49); 10 s standing, then up the ramp to the 19.9 m landing and down 4 m of wooden stairs to the walkway: no `Snake left the ground`, `ceilings 0`, `unstuck 0`; jumped onto the raised plank walkway and walked it; walking off its side and off the landing's open west end logged one `left the ground` line each (`- - - - 0.00`: a real edge; the latter a 60 m chasm -> the fall watchdog, as the player's fall #1 at 19:42:01, whose log shows the same `world shift (24, 32, -24)` then `falling 3.5 s, 60 m`) |
| Filters at the feet | 0x16 sees the scaffold's planks (layer 0x39); 0x58 sees only what is under them | `call rayall` at the landing: 0x16 `0.00 layer 0x39`, 0x58 `-0.49 layer 0x37`; the stair flights above the walkway are seen by 0x16 only (0x58 9-15 m lower) |
| The collector / broad-phase (task's open question) | **neither matters there** | a byte-verified copy of cast_ray's query (`game::CastRayKeep`, address table 80/80) keeping class 2/5 materials and all broad-phase layers: no difference anywhere at the scaffold; a 1935-ray survey (all layers): materials 1/3/4/101/104 only, nothing in the 0x04 broad-phase layer. Kept as diagnostics (`call rayall`, `raykeep`, `puppet set ground_keep / ground_layers`), off by default |
| Roundtable Hold basement stairs (regression) | **works** | from the basement floor (-0.61) up the stairs to the hall (9.01): `walls 4 (31 frames)` before and after, `ceilings 0`, `unstuck 0` |
| Broken side stairs / getting in | **works with the jump** | the cliffside path from the grace: two rock steps of 1.1-1.3 m stopped the walk (`stuck`); a running jump took each (`Snake jumps -- 1.20 m up at 1.3 / 2.7 m/s`), a standing jump the raised walkway (`0.1 m/s`), then the wooden stairs to the scaffold |
| Fall with damage | not re-tested live (no clean 8 m+ drop onto ground found on the way); unit-tested; session 7's `landed at 12.7 m/s -> fall damage 10%` is the same code path for the damage | -- |
| Ladder top | not tested (no ladder near the save's graces: `geom ladders 150` found none at the Roundtable Hold and Gatefront) | -- |
| Jump (12) | **works** | flat: rise 1.20 m at 0.52 s, down at 1.02 s, camera +1.17 m with him (`jump_camera_hold`), MGSV's lift visible (gamescope frame mid-air, Snake's shadow offset); onto 1.1-1.3 m ledges (above); refused under a walkway's edge (`no jump -- a ceiling lower than him`), crouched (`jump: not now -- not standing`); LB held + R3: MGSV's buddy wheel, no jump; LT held + R3: MGSV's shoulder swap, no jump; X in the air: no dive (the guest never arms one), so never a hop in a jump. Not tried: a gap, a 1.7 m wall |
| Dive = hop (6) | **works -> on by default** | three running dives: `dive -> hop #1..3: 0.60 m up ... at 3.57 m/s`, the guest's `confirmed by 'left STAND' 18-19 ms after X`, one up a slope; landed without damage; a dive ends prone (MGSV): X then does nothing until A |
| Binocular marking (1) | **works** (on) | `marking: binoculars up` -> 0.48 s later `marked 0xa00000010000020 (npc 30001014, hostile) at 2 m`, 31 rays 0 blocked; a red triangle with "1m" over him in Elden Ring's frame. **Fixed:** MGSV's lens (UI pixel shader 0x2f38cdb182e4da4f, t0 `inScreenTexture`) painted MGSV's airport around the view; the HUD layer now always leaves it out (`lens draws of MGSV's picture left out 133`, frame checked) |
| Material colours (2) | **works** (Revalidate on, guards `log`) | D-Walker close up and D-Dog near/far after two swaps each: no orange, green or magenta; `material guard: 0 textures in a wrong role`; `!capture`: pruned 482 -> 713, restarted 0. The poster box not re-checked |
| Projectile meshes (8) | **doesn't work -> stays off** | 3 grenades: the guest tagged none (`3 throws ... 0 draws` for them: MGSV's grenade is no rigid G-buffer draw near its arc); 2 rockets: the guest tagged a 4-vertex quad 18 frames after the release, 41 m out (`first round draw tagged ... (18 frames after the release)`), the host learned nothing (`24 lists with tagged draws, 0 templates learned, 0 never flew`) |
| Muzzle flashes (8) | **looks wrong -> stays off** | one flash per shot (16 for 16 rounds, 0 over the rate cap), but `explosion_muzzle 10030001` is a 1-3 m fireball with debris over the aim; `10035001` a golden burst with shards. New live switch `pr er "boom set muzzle_flash on"` to try other looks (`boom look muzzle <bullet>`) |
| C4 (3) | **works with the backup -> C4Backup on by default** | MGSV's own detonator answered 0 of 5 taps (`a tap with C4 in hand got no detonate request from MGSV in 250 ms`), D-Walker near or not; with the backup: `placed #4: C4 set off by the guest` -> `mines: mine 4 went off ... lethal r 8.0 m where it lies`, the next tap #5 (oldest first, one per tap); a Y tap at D-Walker with C4 in hand mounted it, nothing set off; dismounting with Y set nothing off |
| Decoys (13) | **not set** | `Player.ChangeEquip` to `EQP_SWP_Decoy` showed DECOY in MGSV's HUD, but neither LT+RT nor RT set one (C4 and grenades set the same way worked); untested whether MGSV mirrors decoys into its placed list |

Also seen: the walk-off line logged real edges only (a 1 m step at the Main Gate, the walkway's side, the landing's end);
crates at the Stormhill Shack break when Snake walks into them (Elden Ring's breakables; the object-only breaker), so there is
nothing to stand on there; a jump while crouched is refused by the guest (MGSV's A toggles crouch).

## Live pass B (2026-10-15 night, both real games, Gatefront/Stormveil/Limgrave; 0.4.0-alpha)

Both games on the default mapping (`pr launch --er-headless 3840x2160`, the virtual pad; a controller was plugged in and left
alone), three launches of each (new DLLs twice); the saves taken at the first launch (`backups/20261014-234654`) restored at the
end. Keys and the mouse were sent into Elden Ring's window by the host console (`input keydown/keyup`, `input mouse`, and the new
`input wheel` / `input mbutton`). Results:

| Item | Result | Evidence |
|---|---|---|
| Keyboard and mouse (4) | **works** (opt-in: stays off) | `input kbm on` -> `DirectInput hooked (2 GetDeviceState, 2 GetDeviceData)`; Elden Ring reads both by state (60/s, 0 buffered); Ctrl tap -> Standing; W 1.5 s -> 2.6 m, Shift+S -> 5.9 m, `Snake's pad now comes from the keyboard and mouse`; 600 mouse counts -> camera +51 deg; RMB aim, LMB 2 rounds, R reload, 1/2/3 and the wheel -> primary/handgun/support, V held -> binoculars, Space -> `dive -> hop #1`, middle button -> `Snake jumps -- 1.20 m`, Q + mouse + middle button -> `the player's order 20 -- "D-Walker: Whistle"`, E at a grace -> `rested at a grace` with the glyph "E: Rest at site of grace"; Tab tap -> map, Tab held -> iDroid, Esc closes it (tap and 0.5 s hold) with no Elden Ring menu, a fresh Esc opens Elden Ring's; G as Snake: nothing; G held 1.3 s -> Tarnished mode and back; W held across the chord both ways: 0.0 m / 0.04 m of drift |
| ...Tarnished-mode G tap | **fixed** | it reached Elden Ring as G and nothing opened: the save's Elden Ring map key is **M** (System > Key Bindings). New `[Keyboard] chord_tap = view` hands Elden Ring the pad's View instead (`chord key tapped in Tarnished mode -- passed to Elden Ring as the pad's View (its map)`) |
| ...the chord after an unexplained HUD hide | **fixed** | in Tarnished mode near Gatefront's soldiers Elden Ring's HUD state hid with nothing on screen (after a map press it refused) and took the pad gate to "off": View + Start only woke the HUD, Snake came back on the second try. The gate now lets only a hide one of Elden Ring's screens explains block the chord (HOST.md "Interactions"). Its "Auto" HUD fade doesn't hide that state (100 s idle, HUD gone from the frame: the chord worked at once, before and after the fix) |
| CQC tier 1 (5) | **works -> host `[Cqc] enabled` on** | Gatefront guard: RT held -> `CQC grab -> hold ... bullet 1 poise 15`; held on -> `choke -> sleep +249 (resist 226)`, asleep (frame), then fultoned; RT + stick -> `throw -> knockdown ... poise 120 lv 4`, down (frame); held + LB -> `interrogation`; re-grab after 2 s -> `flinch (hold cooldown)`; `fired late 0, lost 0`; guest: `MGSV CQC status 0` throughout. A punch landed twice on a held/guarding soldier (~25 HP more, unlogged): Known issues |
| ...melee centre | **fixed** | a punch after an 8 m walk was placed 8 m behind Snake (`CQC kind 0 ... at ER (6.67, -0.39, -27.61)` with Snake at (13.94, -2.74, -23.79)): MGSV's attack shape still held the last strike. A centre more than 2.5 m from Snake now falls back to 1 m in front of him (GUEST.md 7); re-checked: `melee: MGSV's attack shape is 4.8 m from Snake (stale ...)`, the strike at (0.70, 6.11, -9.61) with Snake at (0.50, 5.11, -8.63) |
| Phantom Cigar (7) | **works -> `[Cigar] TimeSkip` on** | host `timeskip 900`: 07:23 -> 22:23 (53 writes); smokes: `12:00:00 -> 17:48:17: +348 min` -> Elden Ring `12:14 -> 18:02 (+5 h 48 min, 84 writes)`; `+437 min` -> `18:57 -> 02:14` (moonlit frame); `+722 min` -> `04:10 -> 16:18`; `vars.clock` frozen after; at night, after a fast travel, Renna at the Church of Elleh |
| ...R3 in MGSV's item wheel | **fixed** | the wheel's "R: Use" was taken as a jump (`Snake jumps -- 1.20 m` with D-pad left held). `[Jump] PassWith` now includes the D-pad: wheel open, stick on the cigar, R3 -> `cigar: Snake lights the Phantom Cigar` |
| ...no cigar in the save | **fixed** | the player's save has no Phantom Cigar in its items; with `TimeSkip` on the boot now puts one into the last free item slot (`boot: phantom cigar: put into item slot 7`) |
| D-Horse treadmill (9) | **works -> `HorseTreadmill` on** | `shift refused -- HorseTreadmill is off` before; `confirmed (31 checks on the skin)` 0.5 s after `!horse on`; 307 ridden moves, all followed, 0 failed, no `didn't follow`; odometer 1.24 km; a straight **299.9 m** east from Gatefront in 27 s (integrated across 9 world shifts: Elden Ring rebases its origin every ~30 m at a gallop -- it looks like the horse jumping back unless taken out); off after the ride: 3.7 m beside Snake for 12 s, no jump, no whistle. Three fall deaths galloping into Agheel Lake's ravines (Elden Ring's falls) |
| Full proxies (10) | **safe, not useful yet -> stays off** | `!proxy on` at Gatefront: 4 stand-ins `where they should be`, `auto-aim correction on`; 643 s of `phase 0, 0 alerts, 0 calms, 0 sweeps`, Snake's life unchanged (no stand-in fire), no alert line; Fulton through a stand-in `succeeded ... kGEvFulton a=1`, phase 0; MGSV's analyzer read a stand-in through the binoculars. But LT never swung onto a stand-in (3 tries, 4-11 deg off), slot 4 switched between two enemies every ~20 s, `location objects lost` reached 4 |
| Decoys (13) | **works** | the save's E-STUN decoy (406) is **thrown** (LT until the arc, RT): `throw: equip 406 (not an explosive we send)`, `placed #1: equip 406 (decoy) ... (vars slot 0)`, `decoys: shock decoy 1 set`; `npc 43111210 ... sees the shock decoy 2 14.7 m away and stops to stare (6 s)`, the guard shot at turned and stared at another (11.8 m), then walked into it: `shock decoy 3 popped ... -> kHEvDetonate`, `stun r 2.5 m`, `poise 80 lv 3`; guest `popped ... -> kGEvPlacedGone`; a fast travel / death dropped the rest and told the guest; 9 out in MGSV at once, still throwing |
| ...decoy height | **fixed** | one thrown downhill stood 5.5 m in the air (set down from 1.5 m above MGSV's flat apron height, 4 m down only); now down to 40 m below, else from 12 m above -- re-thrown from the Gatefront grace toward the camp: `decoy 12 set down on Elden Ring's ground -6.3 m from where MGSV's apron put it` |
| Regression: README How to play | **works** | stances, run (5.7 m in 1.5 s), dive (4.0 m) and hop, camera, weapons, aim, first person, fire (30 -> 28), reload, grenade (`throw 1: sleep ... 12.4 m/s`), sleep mine (`mine 2 ... set off: npc ... at 2.9 m`, sleep gas, he slept), tranq + Fulton (+64 runes, twice), D-Walker's Whistle, iDroid Buddy Support: DISMISS D-WALKER (`D-Walker dismissed -- no buddy now`) and D-Walker again (`deployed from the iDroid`), Start -> Elden Ring's menu / B, View -> map, View held -> iDroid / Start, the chord both ways (also View first), rest at a grace (refill), fast travel (5x), deaths -> revival choice / respawn with full life and ammo (5x) |
| 10 minutes of play | **0 problems** | `play10.py 600` at Stormveil Cliffside on the final DLLs: 602 s, 206 actions (47 walks, 41 camera pans, 20 stance changes, 3 dives, 12 weapon switches, 8 reloads, 12 aim-and-fire incl. 3 in first person, 9 grenades, 7 items, 10 whistles, 9 Tarnished-mode round trips, 6 Elden Ring menus, 12 maps, 10 iDroids): Snake was driving after every one; 0 warnings or errors in either log, no death, no stuck, no fall |

Also seen: Elden Ring opens no map while an enemy is after Snake (also as the Tarnished): its own rule. A save Elden Ring wrote
during a death came back after a relaunch with its black "Choose revival location" over Snake already in control (Known
issues: Tarnished mode answers it). With the camera pushed into Snake his see-through model showed magenta and green.

Still open after pass B: full proxies' aim assist (LT never swung; the slot flapping); the punch's occasional second ~25 HP on a
held or guarding soldier; the host not noticing Elden Ring's revival screen after such a reload; keyboard and mouse with a real
keyboard and mouse (feel of `mouse_sensitivity`, `virtual_pad = auto` with no controller); projectile meshes and muzzle flashes
(pass A); a 1.7 m wall and a gap for the jump, a ladder top, the box poster (pass A's list).

## Live test plan (when both games are free)

Before: build `build/` (`flock build/.buildlock cmake --build build`), `pr backup-saves`, stay offline. `pr launch`, load the
save, wait for "Snake is in control". Logs: `build/PhantomRingHost.log`, `MGS_TPP/PhantomRingGuest.log` (`pr logs`). Run the
default-on items first (11-13 below: what the player needs at Stormveil; then 1-3), then each opt-in one with its switch.

### 1. Binocular marking (on)
1. `pr er 'mark status'` -> `marking: on | binoculars auto | 0 marked`. Near the Gatefront soldiers hold RB (not aiming):
   `pr status` GuestState flags show `Binoculars` (and `FirstPerson`); host log `marking: binoculars up (0 marked)`, then
   `binoculars down` on release. No flag: `pr er 'mark binoculars on'` tests the host side alone.
2. A soldier at the view centre ~0.5 s -> `marking: marked 0x... (npc N, hostile) at D m`; `pr er 'hud shot'`: a red triangle
   over him with the right metres in `PhantomRingHost.shot.bmp`; mark it and shoot at once: the digits sit where they settle.
3. Behind a wall: the mark stays; an unmarked soldier fully behind a wall is not marked (blocked count rises). Zoom: only
   characters near the centre get marked, marks keep their size. Three soldiers: marks follow them, distances change.
4. Kill a marked one: fades in ~1 s, `mark status` one fewer. Hidden in Elden Ring's menu, MGSV's iDroid, Tarnished mode and at
   a grace; back after. With Elden Ring's HUD on "Auto" the marks stay when the HUD fades.
5. Giant (troll / mounted knight): gets marked; if not, note his npc id and the blocked count. Near a ballista: `mark status`
   shows unrendered > 0 and no triangle in the air; if real soldiers never get marked, `rendered_only = false` and report.
6. Fast travel (or title and reload) with two marks: `marking: a new world ... 2 marks forgotten`; a grace rest keeps them.

### 2. Material colours (Revalidate on, checks `log`)
1. `pr er render` -> `material guard (role_guard log, poster_check log): 0 textures in a wrong role (...), N box textures
   checked, 0 poster draws without the poster`.
2. Play 10+ minutes, swap buddies in the iDroid (D-Dog, D-Walker, twice), then `pr lua '!capture'`: `stale entries restarted
   N / pruned M`, M > 0, `known` counts stable.
3. D-Walker close to the camera (~0.5 m) and D-Dog close and far (`pr screenshot -o walker_near.png`, `dog_close.png`): no
   orange, green or magenta. If false colours remain: `grep "would be left out" PhantomRingHost.log`, then
   `pr er "render set role_guard on"`, screenshot again, back to `log`.
4. After 10+ minutes, the poster box and a plain box in daylight: `grep "texture 4" PhantomRingHost.log` gives each box's
   poster share (the poster box should be >= 15%); `pr er "render set poster_check on"`: pin-up only on the poster box, no
   black box; back to `log`.

### 3. C4's detonator (on; backup off)
1. Guest log: no Verify mismatch for "C4 ..."; `c4: detonator = PlayerPad 0x20 with C4 in hand; ...; a tap it doesn't
   answer in 250 ms is only logged ([Combat] C4Backup off); one C4 per press (the oldest)`.
2. C4 in the support slot (`pr lua 'Player.ChangeEquip{slotType=PlayerSlotType.SUPPORT, equipId=TppEquip.EQP_SWP_C4,
   stock=4, stockSub=0, ammo=4, ammoSub=0, suppressorLife=0, isSuppressorOn=false, isLightOn=false, dropPrevEquip=false,
   toActive=true}'`), place two near enemies (`placed #N: equip 391 ...`, the host's mines log).
3. ~10 m away, away from D-Walker and boxes, tap Y once. One of: `set off by MGSV's detonator (DetonateOwn)`; `MGSV's
   detonator refused the press (... bits ...) -> 1 set off by the guest` (note the bits); `a tap with C4 in hand got no
   detonate request ... nothing set off` (then `C4Backup = true`, relaunch, tap: `... -> 1 set off by the guest`). Host:
   `mines: mine N went off ... r 8.0 m where it lies`, Elden Ring's blast at the oldest C4. A second tap sets off the second.
4. `pr lua '!placed'`: `presses N (answered A, fallbacks F ..., holds H)`, nothing pending; a `MGSV's detonator came ~N ms
   after the press` line means `C4GraceMs` should go above N. An old save's C4: `an old save's: MGSV's world` then the guest
   sets yours off. `C4DetonateAll = true` with 3 placed: one press, three blasts. With `C4Backup = true`, Y held > 0.4 s at a
   sleeping enemy Fultons him and sets nothing off (`holds 1`).
5. Negatives: Y with the rifle sets nothing off; Y at D-Walker without C4 mounts; with the backup off a Y tap at D-Walker or a
   supply box with C4 in hand sets nothing off.

### 4. Keyboard and mouse (`pr er "input kbm on"`)
1. Host log `keyboard: DirectInput hooked (N GetDeviceState, M GetDeviceData)`; `pr er "input kbm"` twice: note which reads
   rise (state or buffered, keyboard and mouse) and the last sizes (256, 16/20).
2. W: `keyboard: Snake's pad now comes from the keyboard and mouse`, Snake walks, Shift+W runs; touch the pad: `...from the
   controller`. The Tarnished must not act on Space, a click or WASD. No keyboard: `pr er "input keydown 0x57"` / `keyup`.
3. Mouse (or `pr er "input mouse 300 0"`): the camera turns; tune `input kbm sens 0.8`, `accel 0.8`. RMB aims, LMB fires, V
   while aiming = first person. Ctrl tap/hold, R, Space, 1-4 and the wheel, Q held + mouse + wheel click (D-Walker's orders), E
   at a grace ("E: Rest" glyphs).
4. Interaction: E at a pickup -> within ~3.5 s `input kbm` shows `scope Elden Ring interaction ...`; W, Space, F, LMB do
   nothing to the Tarnished then; E or Enter work in an NPC dialog.
5. Tab tap = Elden Ring's map, Tab held = iDroid; Esc closes the iDroid without Elden Ring's pause menu (normal press and a
   0.5 s hold; no `menu watch: key 0x1b`); a fresh Esc opens Elden Ring's menu.
6. G held 1 s = Tarnished mode (native WASD/mouse/E/F/Space; a G tap opens the map, `chord key tapped in Tarnished mode`); G
   held again = Snake. Stuck keys: hold W (then RMB) in Tarnished mode, chord to Snake, release, chord back: no walking or
   guarding on its own; hold Down in a menu and close it: no scrolling in the next one. `input kbm off/on` right after a G tap,
   several times: no crash.
7. Unplug the pad, press a key: `keyboard: virtual pad ON...`. With `[Keyboard] enabled = false`: no `keyboard:` lines beyond
   the install line.

### 5. CQC holds and throws (`[Cqc] enabled = true` in the host ini)
1. Elden Ring + fake_mgsv first: within 1.5 m of a Gatefront soldier, facing him (`pr er "combat targets"`).
   `pr lua '!grab grab'` -> `Snake CQC grab ... -> hold on npc ... (max hp ..., bigness ...)`, ~0.15 s later `combat: CQC hold
   -> ... bullet 1 poise 15 lv 1`: he flinches and stops attacking (no flinch: `pr er "cqc set hit_correction 15"`, then 25).
   Release then grab within 8 s: `-> flinch (hold cooldown)`. Release then `!grab choke`: `no hold`. Grab then choke: `sleep
   +... (resist 226)`, he sleeps. `!grab throw`: `CQC knockdown -> ... poise 120 lv 4`, he falls (else `cqc set
   throw_dmg_level 6`, then 7). Margit / a Tree Sentinel: `-> stagger ... (boss)`, never `hold`. `pr er cqc`: `fired late 0,
   lost 0`; HP drops by a few points at most.
2. Both games: RT held next to a soldier -> guest `cqc: kGEvCqc grab (RT held; ...)` ~0.35 s after the punch's `melee:`
   line; keep holding: `choke`; LB: `interrogate` (if not, the `cqc: [holding] pad bits A -> B` lines show LB's bit: set
   `InterrogateButtons`); release: `release`; RT running at him: the punch's `combat: CQC kind ...` then `CQC knockdown`, never
   `nothing applied` or `fired into a busy one`. Note false grabs in normal play and whether a plain punch logs `MGSV CQC status
   1` / `action callback 1` (never: `MgsvHold` may go true).
3. Hold an enemy, then Fulton him (proxies on): `cqc: ... goes up on a Fulton balloon -- the lift holds him now`.

### 6. Dive = hop (`pr er "puppet hop on"`)
1. Guest: X five times on flat ground (still, walking, running): `pr lua '!dive'` counts 5; guest log `dive: X -> diving
   (confirmed by '<sign>' N ms after X ...)`: note the sign and N (answers GUEST.md's "Is a dive NORMAL_ACTION?"). X during a
   reload or weapon switch that MGSV refuses counts as "X presses with no dive". `no sign of a dive within 600 ms`: raise
   `DiveConfirmMs`.
2. Host: `puppet: dive -> hop #N: 0.60 m up ...`, `pr er status` `hops N (refused 0, no move 0)`; Snake rises ~0.6 m, lands
   ~2.5 m ahead, no damage.
3. A 1-2 m Limgrave ditch, X at the edge: `hop #N` (or `(off a rim 0.0x s ago ...)`), lands on the far side; `no hop (off the
   ground 0.3x s ...)`: compare with the guest's N ms. Knee-high ledge (0.6-0.7 m): on top. 1 m wall: back at its foot. Stairs
   (merge fix): a dive at the foot of a staircase hops (no `less than 0.15 m of room`).
4. Ceilings: under a beam below ~1.95 m `dive -- no hop (less than 0.15 m of room above his 1.80 m ...)` (always 1.80, never
   0.50); Roundtable Hold / a doorway `(lower: a ceiling)`, head never in it; the basement stairs: `unstick`/`stall` don't rise.
5. Off a cliff: hop then fall damage. `puppet hop off`: dives as before; riding D-Walker: no hops; the death-collapse dive
   doesn't count in `!dive`.

### 7. Phantom Cigar (`[Cigar] TimeSkip = true` in the guest ini)
1. Host alone, loaded world: `pr er "timeskip 360"`: the sky moves ~+6 h over ~4 s without a jump; `time skip: Elden Ring's
   clock A -> B (+6 h 00 min, N writes)` with N ~45, the corner message; then `timeskip 60`: N ~10. Also across midnight
   (`timeskip 120` late) and one sent during a fast-travel load (waits, then applies).
2. Guest log `boot: phantom cigar: watching (status check ok, mute check ok); the clock is let run at x20 unless MGSV moves it
   itself while he smokes`; `pr lua 'return vars.clock'` = 43200.
3. Smoke the cigar; during it, a few times 1 s apart: `pr lua '!cigar'` and `pr lua 'return PhantomRing.cigar.on,
   PhantomRing.cigar.runs, PhantomRing.CigarSmoking(), TppGameStatus.IsSet("", "S_IS_TIME_CIGARETTE"),
   TppSoundDaemon.CheckCurrentMuteIs("Cigarette"), TppSoundDaemon.CheckHighestMuteIs("Cigarette"), vars.clock'` (which signal is
   real). `cigar: MGSV's clock stood still 500 ms into the smoke: it runs at [Cigar] ClockScale x20` = the guest drives the
   clock; missing but the clock moves = MGSV does. Then `cigar: put out -- ... +N min`; `+0 min ... never moved`: try
   `ClockScale` / compare `TppClock.GetTime("string")`. Once with `ClockScale = 20`, once with 0: compare `vars.clock` speed.
4. After the smoke `vars.clock` stays put (10 s+, sampled over 30 s). Both games: smoke from 12:00 to night: Elden Ring's sky
   follows, "Phantom Cigar: ... passed"; Night's Cavalry / Renna (else rest at a grace and note it). `!cigar` `stuck signals
   0`; a 60-90 s smoke must not log `on for over 120 s`.

### 8. Projectile meshes and muzzle flashes (host `projectile_meshes = true`, `muzzle_flash = true`; guest `ProjectileMeshes = true`)
1. `pr er "boom muzzle"`: a short orange flash ~1.5 m in front of the Tarnished's chest; `pr er boom` `muzzle flashes on (1
   shown ...)`, the muzzle row `ready (... 0.06 s, chain cut/none)`; log `explosion: muzzle Bullet 10030001 ... as carrier
   Bullet 10302005`. No lingering, sparks, ground flames, aggro or flinch.
2. SVG-76 single and automatic: one flash per shot at the muzzle, at most ~10/s (`over muzzle_per_second` rises); tranq
   pistol: none; first person: note where flashes appear. Wrong look: `pr er "boom look muzzle 10035001"`.
3. The same frag grenade thrown 3 times: guest `projectile: first thrown draw tagged at ... (N frames after the release)`
   (N >= 8), `!capture` "K draws tagged"; host `projectiles: throw equip <id> drawn with MGSV's mesh from now on`, then
   `... measures X m across` (X ~0.1-0.2). Report `never flew`, `is X m across ... not one`, `replaced`. From the second throw
   MGSV's grenade tumbles along Elden Ring's arc, keeps its pose when it lands, no ball; Snake's gear doesn't blink.
4. FB MR R-LAUNCHER twice: `projectiles: round bullet <id> drawn ...`, under 1.2 m; nose-first from the second rocket with the
   flame and trail. With full proxies on too, a rocket aimed at a stand-in is still tagged (merge fix).
5. Host off / guest on: Snake's gear stays, `pr er throw` `N handed back to Snake (off)`; both off: the ball as before; no
   `SpawnBullet(10302005) refused` in the host log.

### 9. D-Horse treadmill (`PHANTOMRING_BUDDY=horse pr launch`, then `pr lua '!horse on'`)
1. Before `!horse on`: `pr lua '!horse shift 3 0'` -> `horse: shift refused -- [Buddy] HorseTreadmill is off`; after it,
   before `confirmed`: `... isn't confirmed`. Then `horse: location object 0x... stands on D-Horse's skin (N scanned) --
   confirming`, `... confirmed`; `!horse` shows `confirmed`. Repeated `none of N location objects stands on D-Horse's skin`:
   no such object, keep it off.
2. On foot next to it: `!horse shift 3 0` -> the horse 3 m along +X in MGSV (`GAMESCOPE_WAYLAND_DISPLAY=gamescope-0
   gamescopectl screenshot`), `!horse` `1 moves (1 followed by the skin ...)`; shift back. Running past the radius: no
   `follow: D-Horse whistled ... after a treadmill jump`, moves rise.
3. Mount, gallop north 30 s+: no `Snake's root didn't follow the ridden jump`, no `D-Horse can't be moved`; `!status` warps
   and landed rise together; `!horse` last `moved by dx,dy,dz` (small dy), the horse on the ground; Snake in the saddle
   (buddyFlags 0x7); > 300 m in one direction in Elden Ring. If the warning appears: `the saddle didn't carry Snake`, the horse
   back under him, then no more horse jumps; retry with `HorseMoveRider = true`.
4. `!horse off`, gallop > 40 m south, `!horse on`: `no ridden jump that long (until he is back within 32 m)`; riding back,
   `D-Horse can be moved now` only within 32 m, first jump ~15 m or less. Dismount after a long ride: no jump, no whistle.
5. `!horse on`, `off`, `!buddy walker`, `!buddy horse`, `!horse on`: `forgotten (HorseTreadmill off)`, a fresh scan, no
   `confirmed` before ~0.5 s. If all passes, `HorseTreadmill` may default to true.

### 10. Inert full proxies (`[Proxies] Enabled = false` in the ini; `pr lua '!proxy on'` for the session)
1. `pr logs`: `proxy: pool of N soldiers`, no `not where this build has them`, `soldier 0x... realized ... standing in for
   0x...`, `N of N slot soldiers where they should be (0 not)`, `boot: proxies: auto-aim correction on`.
2. `pr lua '!proxy'` each minute: `inert true (sites ok), height ground`, slots `active ... placed (where it should be)`,
   `phase lock true: phase 0, 0 alerts`; the "where they should be" line doesn't flap. MGSV screenshot: stand-ins still, no
   other soldier, no alert music, no `alert: MGSV's alert phase 0 -> ...`.
3. An enemy 15-25 m away on a ledge or in a dip, LT: the reticle sits on his stand-in (expected); fire without touching the
   aim: `proxy: round aimed at a proxy turned to its enemy's height (+x.xx m)`, one host hit per `kGEvFire`. Aim by hand at
   his real chest: no `turned` line, still one hit each. One on a ledge in front, one on the ground behind: a round at the one
   behind hits him.
4. Ten minutes of play: phase stays 0 or each alert is calmed within seconds (`phase lock: sweep (keeping ...) (#n)` ~every
   3 s during one); MGSV never shoots Snake; `location objects lost` doesn't climb. After a give-up, `!proxy on` again:
   `gave up` gone, alerts back to 0; the warning names the case (`the lock keeps calming` with `[Fulton] Method = proxy`, `the
   phase lock stops with them` with `anchor`).
5. Fulton through a full proxy: `kGEvFulton a=1 ... (its proxy)` with v0 at the enemy's real height, phase 0 throughout.
   Finish with `pr lua '!proxy off'`; if all passes, a stability hour before any default change.

## Round 1b live test plan (ground, jump, decoys: all on by default)

### 11. Solid ground (on)
1. Puppet start: host `puppet: Snake's body rays: ground_filter 0x2000016, wall_filter 0x2000058 + 0x2000016, ground under the
   whole body (5 probes); below the start: ... (ground_filter), ... (ray_filter 0x2000058)`. A `... need the collision layer
   table ... -> ray_filter 0x2000058 alone` warning means the exe check failed: report it (Snake then uses the old filter).
2. Stormveil's side wooden scaffolding (session 7: Snake fell through it from ER feet ~(-3.15, 3.34, 0.69); coordinates move
   with Elden Ring's origin): climb it as the Tarnished, Tarnished mode off on the planks. Snake stays up: no `Snake left the
   ground`, no `landed at ... -> fall damage`. Stand still 10 s over a gap between boards, walk across, along and diagonally
   over the planks; `pr er "hud shot"`: feet on the boards. At the feet, compare `pr er "call ray x y+0.5 z 0 -3 0 0x2000016"`
   with `... 0x2000058` (x y z from `pr er status`).
3. Railings: walk at the scaffold's railing: he stops a body radius short, never off the edge. Any invisible wall the
   Tarnished walks through: note where, set `[Puppet] wall_filter_also = 0`, retry.
4. Edges still drop him: walk off the open end, off a 2 m ledge: one `Snake left the ground at ... his five ground rays
   (centre first) down to 3 m: ...` line each, an ordinary landing. Fall through something the Tarnished stands on: that line
   shows which filter sees it (`- - - - -`: none of the five rays).
5. Slopes and stairs: walk up and down a 20-30 degree hillside (no floating in `hud shot`), Stormveil's broken side stairs and
   the Roundtable Hold's basement stairs (they climb, `ceilings` doesn't rise in `pr er status`). Fort Haight's tower top off
   the ladder: `no ground under the Tarnished's centre ... Snake starts N m away on ground`.
6. The player's two `falls through the world` (fall watchdog at 19:42:01 and 19:58:24 in session 7): if one comes again, the
   walk-off line just before it says which filter saw no floor; `ground_filter = 0x2000058` is the way back.

### 12. Jump (on: R3)
1. Guest log at boot `ini: [Jump] button 0x0080 (MGSV's own with 0x0300 or LT held), lift 1.00 m`. Standing on flat ground,
   R3 (or `pr lua '!jump'`): guest `jump: Snake lifted 1.00 m at ... -- kGuestJumping until he lands (jump #1)`, host
   `puppet: Snake jumps -- 1.20 m up at 0.0 m/s ... (jump #1)`; `pr er "hud shot"` mid-air: Snake ~1.2 m up in Elden Ring, the
   camera's height steady; he lands without damage. `pr er status`: `Snake jumped 1 (0 refused)`, `jumping` while in the air.
2. Running (stick fully pushed): `... at ~5-6 m/s`; over a 2 m gap, onto a knee-to-waist-high ledge or crate (~1 m), not onto
   a 1.7 m wall (he comes down in front of it). Off a ~5 m drop: the same fall damage as walking off it.
3. The player's goal: into Stormveil by the side path without Tarnished mode (the gaps and ledges he couldn't cross).
4. Refused or MGSV's: crouched (A), R3 -> guest `jump: not now -- not standing (crouched or prone)`; LT held + R3: MGSV's
   shoulder change, no jump line; LB held + R3: the call menu's select; riding D-Walker: no jump; under a low beam: host
   `puppet: no jump -- a ceiling right over his head`. Note any `jump: no landing seen in MGSV within 2000 ms` or `the lift
   never showed`.
5. With `pr er "puppet hop on"`: X during a jump -> `dive -- no hop (in a jump)`; X on the ground still hops (plan 6).
6. With `[Keyboard]` on: the middle mouse button jumps.

### 13. Decoys (on)
1. A decoy in the support slot: `pr lua 'Player.ChangeEquip{slotType=PlayerSlotType.SUPPORT, equipId=TppEquip.EQP_SWP_Decoy,
   stock=4, stockSub=0, ammo=4, ammoSub=0, suppressorLife=0, isSuppressorOn=false, isLightOn=false, dropPrevEquip=false,
   toActive=true}'`; LT+RT sets one: guest `placed #N: equip 397 (decoy) at ...` (none: MGSV doesn't mirror decoys into its
   placed vars -- report, the feature can't work), host `decoys: decoy N set (equip 397) at ER (...)`; `hud shot`: a grey-olive
   balloon soldier inflates there.
2. A Gatefront soldier ~10 m away facing it: `decoys: <name> sees the decoy N 10.x m away and stops to stare (6 s)`; he stands
   still; walk behind him. The same one again only after 15 s. One fighting Snake within 4 m ignores it.
3. Pop: an enemy walking into it (or `pr er "decoys pop N"`): host `decoys: decoy N popped: ... -> kHEvDetonate`, guest
   `placed #N: decoy (equip 397) popped (or dropped) in Elden Ring -> kGEvPlacedGone; in MGSV left standing
   (DecoyPopInMgsv = false)`; the balloon collapses. `pr lua '!placed'`: `decoys popped 1`.
4. The active decoy (`EQP_SWP_ActiveDecoy`, 401): enemies within 25 m behind it stop too (`hears`); the shock decoy
   (`EQP_SWP_ShockDecoy`, 405) popped: a stun blast, the enemy staggers.
5. Without MGSV: `pr er "decoys add 6 plain"` (or `active`, `shock`) sets a test decoy 6 m ahead of the Tarnished; `pr er
   decoys` lists decoys and starers.
6. Tarnished mode and back: kept (`decoys: puppet stopped; N decoy(s) kept`). Fast travel: `... dropped ... (N told to the
   guest)`, and no `announced again but unknown` lines at every later switch. `turn = true` only as an experiment.

